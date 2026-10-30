/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "paimon/core/schema/schema_manager.h"

#include <algorithm>
#include <utility>

#include "arrow/api.h"
#include "fmt/format.h"
#include "paimon/common/data/variant/variant_type_utils.h"
#include "paimon/common/utils/checked_cast.h"
#include "paimon/common/utils/date_time_utils.h"
#include "paimon/common/utils/field_type_utils.h"
#include "paimon/common/utils/path_util.h"
#include "paimon/common/utils/string_utils.h"
#include "paimon/core/casting/cast_executor_factory.h"
#include "paimon/core/schema/schema_validation.h"
#include "paimon/core/utils/branch_manager.h"
#include "paimon/core/utils/file_utils.h"
#include "paimon/defs.h"
#include "paimon/fs/file_system.h"
#include "paimon/status.h"

namespace arrow {
class Schema;
}  // namespace arrow

namespace paimon {

SchemaManager::SchemaManager(const std::shared_ptr<FileSystem>& file_system,
                             const std::string& table_root)
    : SchemaManager(file_system, table_root, BranchManager::DEFAULT_MAIN_BRANCH) {}

SchemaManager::SchemaManager(const std::shared_ptr<FileSystem>& file_system,
                             const std::string& table_root, const std::string& branch)
    : file_system_(file_system),
      table_root_(table_root),
      branch_(BranchManager::NormalizeBranch(branch)) {}

std::string SchemaManager::BranchPath() const {
    return BranchManager::BranchPath(table_root_, branch_);
}

std::string SchemaManager::ToSchemaPath(int64_t schema_id) const {
    return PathUtil::JoinPath(BranchPath(),
                              "/schema/" + std::string(SCHEMA_PREFIX) + std::to_string(schema_id));
}
Result<std::optional<std::shared_ptr<TableSchema>>> SchemaManager::Latest() const {
    std::vector<int64_t> versions;
    PAIMON_RETURN_NOT_OK(FileUtils::ListVersionedFiles(file_system_, SchemaDirectory(),
                                                       std::string(SCHEMA_PREFIX), &versions));
    if (versions.empty()) {
        return std::optional<std::shared_ptr<TableSchema>>();
    }
    int64_t max_schema_id = versions[0];
    for (const auto& version : versions) {
        max_schema_id = std::max(max_schema_id, version);
    }
    PAIMON_ASSIGN_OR_RAISE(std::shared_ptr<TableSchema> schema, ReadSchema(max_schema_id));
    return std::optional<std::shared_ptr<TableSchema>>(schema);
}

Result<std::shared_ptr<TableSchema>> SchemaManager::ReadSchema(int64_t schema_id) const {
    auto iter = schema_cache_.find(schema_id);
    if (iter != schema_cache_.end()) {
        return iter->second;
    }
    auto path = ToSchemaPath(schema_id);
    std::string content;
    PAIMON_RETURN_NOT_OK(file_system_->ReadFile(path, &content));
    PAIMON_ASSIGN_OR_RAISE(std::shared_ptr<TableSchema> schema,
                           TableSchema::CreateFromJson(content));
    schema_cache_[schema_id] = schema;
    return schema;
}

std::string SchemaManager::SchemaDirectory() const {
    return PathUtil::JoinPath(BranchPath(), "/schema");
}

Result<bool> SchemaManager::SchemaExists(int64_t id) const {
    std::string schema_path = ToSchemaPath(id);
    return file_system_->Exists(schema_path);
}

Result<std::vector<int64_t>> SchemaManager::ListAllIds() const {
    std::vector<int64_t> versions;
    PAIMON_RETURN_NOT_OK(FileUtils::ListVersionedFiles(file_system_, SchemaDirectory(),
                                                       std::string(SCHEMA_PREFIX), &versions));
    return versions;
}

Result<std::unique_ptr<TableSchema>> SchemaManager::CreateTable(
    const std::shared_ptr<arrow::Schema>& schema, const std::vector<std::string>& partition_keys,
    const std::vector<std::string>& primary_keys,
    const std::map<std::string, std::string>& options) {
    while (true) {
        PAIMON_ASSIGN_OR_RAISE(std::optional<std::shared_ptr<TableSchema>> latest_schema, Latest());
        if (latest_schema) {
            return Status::Invalid("Schema in filesystem exists, creation is not allowed.");
        }
        PAIMON_ASSIGN_OR_RAISE(
            std::unique_ptr<TableSchema> table_schema,
            TableSchema::Create(/*schema_id=*/0, schema, partition_keys, primary_keys, options));
        PAIMON_RETURN_NOT_OK(SchemaValidation::ValidateTableSchema(*table_schema));
        std::string schema_path = ToSchemaPath(0);
        PAIMON_ASSIGN_OR_RAISE(std::string content, table_schema->ToJsonString());
        auto status = file_system_->AtomicStore(schema_path, content);
        if (status.ok()) {
            return table_schema;
        }
    }
    return Status::Invalid("create table failed, should not be here");
}

namespace {

bool IsSchemaFileConflict(const Status& status) {
    return !status.ok() && status.message().find("dst file already exist") != std::string::npos;
}

bool SameSchemaContent(const TableSchema& left, const TableSchema& right) {
    return left.Fields() == right.Fields() && left.PartitionKeys() == right.PartitionKeys() &&
           left.PrimaryKeys() == right.PrimaryKeys() && left.Options() == right.Options();
}

int FindColumn(const std::vector<DataField>& fields, const std::string& name) {
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].Name() == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ContainsName(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

void ReplaceName(std::vector<std::string>* names, const std::string& old_name,
                 const std::string& new_name) {
    for (auto& name : *names) {
        if (name == old_name) {
            name = new_name;
        }
    }
}

bool IsBucketKey(const std::map<std::string, std::string>& options, const std::string& name) {
    for (const auto& entry : options) {
        if (!StringUtils::EqualsIgnoreCase(entry.first, Options::BUCKET_KEY)) {
            continue;
        }
        std::vector<std::string> parts = StringUtils::Split(entry.second, ",", false);
        for (std::string part : parts) {
            StringUtils::Trim(&part);
            if (part == name) {
                return true;
            }
        }
    }
    return false;
}

bool IsStructuralKey(const std::vector<std::string>& primary_keys,
                     const std::vector<std::string>& partition_keys,
                     const std::map<std::string, std::string>& options, const std::string& name) {
    return ContainsName(primary_keys, name) || ContainsName(partition_keys, name) ||
           IsBucketKey(options, name);
}

bool IsFrozenOption(const std::string& key) {
    return StringUtils::EqualsIgnoreCase(key, Options::BUCKET) ||
           StringUtils::EqualsIgnoreCase(key, Options::BUCKET_KEY);
}

bool IsNestedType(const std::shared_ptr<arrow::DataType>& type) {
    if (!type) {
        return true;
    }
    switch (type->id()) {
        case arrow::Type::STRUCT:
        case arrow::Type::LIST:
        case arrow::Type::MAP:
        case arrow::Type::FIXED_SIZE_LIST:
            return true;
        default:
            return false;
    }
}

std::shared_ptr<const arrow::KeyValueMetadata> MetadataWithoutFieldId(
    const std::shared_ptr<const arrow::KeyValueMetadata>& metadata) {
    if (!metadata || !metadata->Contains(DataField::FIELD_ID)) {
        return metadata;
    }
    std::vector<std::string> keys;
    std::vector<std::string> values;
    keys.reserve(metadata->size());
    values.reserve(metadata->size());
    for (int64_t i = 0; i < metadata->size(); ++i) {
        if (metadata->key(i) == DataField::FIELD_ID) {
            continue;
        }
        keys.push_back(metadata->key(i));
        values.push_back(metadata->value(i));
    }
    if (keys.empty()) {
        return nullptr;
    }
    return arrow::key_value_metadata(keys, values);
}

std::shared_ptr<arrow::Field> StripFieldIds(const std::shared_ptr<arrow::Field>& field) {
    std::shared_ptr<const arrow::KeyValueMetadata> metadata =
        MetadataWithoutFieldId(field->metadata());
    if (VariantTypeUtils::IsVariantField(field)) {
        return arrow::field(field->name(), field->type(), field->nullable(), metadata);
    }
    const std::shared_ptr<arrow::DataType>& type = field->type();
    if (type->id() == arrow::Type::STRUCT) {
        std::shared_ptr<arrow::StructType> struct_type =
            checked_pointer_cast<arrow::StructType>(type);
        arrow::FieldVector children;
        children.reserve(struct_type->num_fields());
        for (const auto& child : struct_type->fields()) {
            children.push_back(StripFieldIds(child));
        }
        return arrow::field(field->name(), arrow::struct_(children), field->nullable(), metadata);
    }
    if (type->id() == arrow::Type::LIST) {
        std::shared_ptr<arrow::ListType> list_type = checked_pointer_cast<arrow::ListType>(type);
        return arrow::field(field->name(), arrow::list(StripFieldIds(list_type->value_field())),
                            field->nullable(), metadata);
    }
    if (type->id() == arrow::Type::FIXED_SIZE_LIST) {
        std::shared_ptr<arrow::FixedSizeListType> list_type =
            checked_pointer_cast<arrow::FixedSizeListType>(type);
        return arrow::field(
            field->name(),
            arrow::fixed_size_list(StripFieldIds(list_type->value_field()), list_type->list_size()),
            field->nullable(), metadata);
    }
    if (type->id() == arrow::Type::MAP) {
        std::shared_ptr<arrow::MapType> map_type = checked_pointer_cast<arrow::MapType>(type);
        auto new_map = std::make_shared<arrow::MapType>(StripFieldIds(map_type->key_field()),
                                                        StripFieldIds(map_type->item_field()),
                                                        /*keys_sorted=*/false);
        return arrow::field(field->name(), new_map, field->nullable(), metadata);
    }
    return arrow::field(field->name(), type, field->nullable(), metadata);
}

void RenameBucketKey(std::map<std::string, std::string>* options, const std::string& old_name,
                     const std::string& new_name) {
    for (auto& entry : *options) {
        if (!StringUtils::EqualsIgnoreCase(entry.first, Options::BUCKET_KEY)) {
            continue;
        }
        std::vector<std::string> parts = StringUtils::Split(entry.second, ",", false);
        bool changed = false;
        for (std::string& part : parts) {
            std::string trimmed = part;
            StringUtils::Trim(&trimmed);
            if (trimmed == old_name) {
                part = new_name;
                changed = true;
            }
        }
        if (!changed) {
            continue;
        }
        std::string joined;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) {
                joined += ",";
            }
            joined += parts[i];
        }
        entry.second = std::move(joined);
    }
}

Status DropColumn(std::vector<DataField>* fields, const std::vector<std::string>& primary_keys,
                  const std::vector<std::string>& partition_keys,
                  const std::map<std::string, std::string>& options, const std::string& name) {
    int index = FindColumn(*fields, name);
    if (index < 0) {
        return Status::NotExist(fmt::format("Column '{}' does not exist", name));
    }
    if (IsStructuralKey(primary_keys, partition_keys, options, name)) {
        return Status::Invalid(
            fmt::format("Cannot drop primary key, partition key, or bucket key column '{}'", name));
    }
    if (fields->size() == 1) {
        return Status::Invalid("Cannot drop the last column");
    }
    fields->erase(fields->begin() + index);
    return Status::OK();
}

Status RenameColumn(std::vector<DataField>* fields, std::vector<std::string>* primary_keys,
                    std::vector<std::string>* partition_keys,
                    std::map<std::string, std::string>* options, const std::string& old_name,
                    const std::string& new_name) {
    int index = FindColumn(*fields, old_name);
    if (index < 0) {
        return Status::NotExist(fmt::format("Column '{}' does not exist", old_name));
    }
    if (old_name == new_name) {
        return Status::OK();
    }
    if (new_name.empty() || FindColumn(*fields, new_name) >= 0) {
        return Status::Invalid(fmt::format("Column '{}' already exists", new_name));
    }
    const DataField& field = (*fields)[index];
    std::shared_ptr<arrow::Field> renamed =
        arrow::field(new_name, field.Type(), field.Nullable(), field.ArrowField()->metadata());
    (*fields)[index] = DataField(field.Id(), renamed, field.Description());
    ReplaceName(primary_keys, old_name, new_name);
    ReplaceName(partition_keys, old_name, new_name);
    RenameBucketKey(options, old_name, new_name);
    return Status::OK();
}

Status UpdateColumnType(std::vector<DataField>* fields,
                        const std::vector<std::string>& primary_keys,
                        const std::vector<std::string>& partition_keys,
                        const std::map<std::string, std::string>& options, const std::string& name,
                        const std::shared_ptr<arrow::DataType>& new_type) {
    int index = FindColumn(*fields, name);
    if (index < 0) {
        return Status::NotExist(fmt::format("Column '{}' does not exist", name));
    }
    if (!new_type) {
        return Status::Invalid(fmt::format("Column '{}' requires a type", name));
    }
    const DataField& field = (*fields)[index];
    if (IsNestedType(field.Type()) || IsNestedType(new_type)) {
        return Status::Invalid(fmt::format("Cannot change nested column '{}'", name));
    }
    if (field.Type()->Equals(new_type)) {
        return Status::OK();
    }
    if (IsStructuralKey(primary_keys, partition_keys, options, name)) {
        return Status::Invalid(fmt::format(
            "Cannot change type of primary key, partition key, or bucket key column '{}'", name));
    }
    PAIMON_ASSIGN_OR_RAISE(FieldType source_type,
                           FieldTypeUtils::ConvertToFieldType(field.ArrowField()));
    PAIMON_ASSIGN_OR_RAISE(FieldType target_type,
                           FieldTypeUtils::ConvertToFieldType(new_type->id()));
    if (!CastExecutorFactory::GetCastExecutorFactory()->GetCastExecutor(source_type, target_type)) {
        return Status::Invalid(
            fmt::format("Cannot change column '{}' from type {} to {}: no cast executor", name,
                        FieldTypeUtils::FieldTypeToString(source_type),
                        FieldTypeUtils::FieldTypeToString(target_type)));
    }
    std::shared_ptr<arrow::Field> updated =
        arrow::field(field.Name(), new_type, field.Nullable(), field.ArrowField()->metadata());
    (*fields)[index] = DataField(field.Id(), updated, field.Description());
    return Status::OK();
}

Status SetOption(std::map<std::string, std::string>* options, const std::string& key,
                 const std::string& value) {
    if (IsFrozenOption(key)) {
        return Status::Invalid(fmt::format("Cannot set or reset Paimon option '{}'", key));
    }
    (*options)[key] = value;
    return Status::OK();
}

Status RemoveOption(std::map<std::string, std::string>* options, const std::string& key) {
    if (IsFrozenOption(key)) {
        return Status::Invalid(fmt::format("Cannot set or reset Paimon option '{}'", key));
    }
    bool removed = false;
    for (auto iter = options->begin(); iter != options->end();) {
        if (StringUtils::EqualsIgnoreCase(iter->first, key)) {
            iter = options->erase(iter);
            removed = true;
        } else {
            ++iter;
        }
    }
    if (!removed) {
        return Status::NotExist(fmt::format("Option '{}' does not exist", key));
    }
    return Status::OK();
}

}  // namespace

Status SchemaManager::AddEvolvedColumn(std::vector<DataField>* fields, int32_t* highest_field_id,
                                       const std::shared_ptr<arrow::Field>& field) const {
    if (!field || field->name().empty()) {
        return Status::Invalid("ADD COLUMN requires a named field");
    }
    if (FindColumn(*fields, field->name()) >= 0) {
        return Status::Invalid(fmt::format("Column '{}' already exists", field->name()));
    }
    int32_t next_field_id = *highest_field_id + 1;
    std::shared_ptr<arrow::Field> stripped = StripFieldIds(field);
    PAIMON_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Field> field_with_id,
                           TableSchema::AssignFieldIdsRecursively(
                               stripped, /*assign_id_to_self=*/true, &next_field_id));
    PAIMON_ASSIGN_OR_RAISE(DataField data_field,
                           DataField::ConvertArrowFieldToDataField(field_with_id));
    fields->push_back(data_field);
    *highest_field_id = next_field_id - 1;
    return Status::OK();
}

Result<std::unique_ptr<TableSchema>> SchemaManager::ApplySchemaChanges(
    const TableSchema& latest, const std::vector<SchemaChange>& changes) const {
    std::vector<DataField> fields = latest.Fields();
    int32_t highest_field_id = latest.HighestFieldId();
    std::vector<std::string> partition_keys = latest.PartitionKeys();
    std::vector<std::string> primary_keys = latest.PrimaryKeys();
    std::map<std::string, std::string> options = latest.Options();
    for (const auto& change : changes) {
        switch (change.GetKind()) {
            case SchemaChange::Kind::kAddColumn:
                PAIMON_RETURN_NOT_OK(AddEvolvedColumn(&fields, &highest_field_id, change.Field()));
                break;
            case SchemaChange::Kind::kDropColumn:
                PAIMON_RETURN_NOT_OK(
                    DropColumn(&fields, primary_keys, partition_keys, options, change.Name()));
                break;
            case SchemaChange::Kind::kRenameColumn:
                PAIMON_RETURN_NOT_OK(RenameColumn(&fields, &primary_keys, &partition_keys, &options,
                                                  change.Name(), change.NewName()));
                break;
            case SchemaChange::Kind::kUpdateColumnType:
                PAIMON_RETURN_NOT_OK(UpdateColumnType(&fields, primary_keys, partition_keys,
                                                      options, change.Name(), change.NewType()));
                break;
            case SchemaChange::Kind::kSetOption:
                PAIMON_RETURN_NOT_OK(SetOption(&options, change.OptionKey(), change.OptionValue()));
                break;
            case SchemaChange::Kind::kRemoveOption:
                PAIMON_RETURN_NOT_OK(RemoveOption(&options, change.OptionKey()));
                break;
        }
    }
    return TableSchema::InitSchema(latest.Id() + 1, fields, highest_field_id, partition_keys,
                                   primary_keys, options, latest.Comment(),
                                   DateTimeUtils::GetCurrentUTCTimeUs() / 1000);
}

Result<int64_t> SchemaManager::CommitSchemaChanges(const std::vector<SchemaChange>& changes) {
    while (true) {
        PAIMON_ASSIGN_OR_RAISE(std::optional<std::shared_ptr<TableSchema>> latest_schema, Latest());
        if (!latest_schema || !latest_schema.value()) {
            return Status::NotExist("Schema does not exist");
        }
        const std::shared_ptr<TableSchema>& latest = latest_schema.value();
        if (changes.empty()) {
            return latest->Id();
        }
        PAIMON_ASSIGN_OR_RAISE(std::unique_ptr<TableSchema> updated,
                               ApplySchemaChanges(*latest, changes));
        if (SameSchemaContent(*latest, *updated)) {
            return latest->Id();
        }
        PAIMON_RETURN_NOT_OK(SchemaValidation::ValidateTableSchema(*updated));
        PAIMON_ASSIGN_OR_RAISE(std::string content, updated->ToJsonString());
        std::string schema_path = ToSchemaPath(updated->Id());
        Status status = file_system_->AtomicStore(schema_path, content);
        if (status.ok()) {
            return updated->Id();
        }
        if (!IsSchemaFileConflict(status)) {
            return status;
        }
    }
}

}  // namespace paimon
