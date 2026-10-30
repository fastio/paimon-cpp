/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "paimon/core/bucket/bucket_selector.h"

#include <algorithm>
#include <utility>

#include "arrow/type.h"
#include "paimon/common/data/binary_row_writer.h"
#include "paimon/common/predicate/predicate_filter.h"
#include "paimon/common/types/data_field.h"
#include "paimon/common/utils/field_type_utils.h"
#include "paimon/core/bucket/bucket_function.h"
#include "paimon/core/bucket/bucket_select_converter.h"
#include "paimon/core/schema/table_schema.h"
#include "paimon/memory/memory_pool.h"
#include "paimon/predicate/compound_predicate.h"
#include "paimon/predicate/function.h"
#include "paimon/predicate/leaf_predicate.h"
#include "paimon/predicate/literal.h"
#include "paimon/predicate/predicate.h"
#include "paimon/predicate/predicate_builder.h"
#include "paimon/predicate/predicate_utils.h"

namespace paimon {

Result<std::unique_ptr<BucketSelector>> BucketSelector::Create(
    const std::shared_ptr<Predicate>& predicate, const std::shared_ptr<TableSchema>& table_schema,
    BucketFunctionType bucket_function_type, const std::shared_ptr<MemoryPool>& pool) {
    const std::vector<std::string>& bucket_keys = table_schema->BucketKeys();
    if (predicate == nullptr || bucket_keys.empty()) {
        return std::unique_ptr<BucketSelector>();
    }
    PAIMON_ASSIGN_OR_RAISE(std::vector<DataField> bucket_fields,
                           table_schema->GetFields(bucket_keys));
    std::vector<FieldType> bucket_key_types;
    std::vector<std::shared_ptr<arrow::DataType>> bucket_key_arrow_types;
    for (const auto& field : bucket_fields) {
        Result<FieldType> field_type = FieldTypeUtils::ConvertToFieldType(field.Type()->id());
        if (!field_type.ok()) {
            return std::unique_ptr<BucketSelector>();
        }
        bucket_key_types.push_back(field_type.value());
        bucket_key_arrow_types.push_back(field.Type());
    }

    const std::vector<std::string>& partition_keys = table_schema->PartitionKeys();
    std::map<std::string, int32_t> partition_name_to_idx;
    for (size_t i = 0; i < partition_keys.size(); i++) {
        partition_name_to_idx.emplace(partition_keys[i], static_cast<int32_t>(i));
    }
    PAIMON_ASSIGN_OR_RAISE(std::vector<DataField> partition_fields,
                           table_schema->GetFields(partition_keys));
    std::shared_ptr<arrow::Schema> partition_schema =
        DataField::ConvertDataFieldsToArrowSchema(partition_fields);
    std::set<std::string> partition_names(partition_keys.begin(), partition_keys.end());
    PAIMON_ASSIGN_OR_RAISE(bool has_partition_fields,
                           PredicateUtils::ContainAnyField(predicate, partition_names));

    Selection manifest_selection;
    PAIMON_ASSIGN_OR_RAISE(manifest_selection.rows,
                           CollectBucketKeyRows(predicate, bucket_keys, bucket_key_types,
                                                bucket_key_arrow_types, pool.get()));
    if (!has_partition_fields && !manifest_selection.rows) {
        return std::unique_ptr<BucketSelector>();
    }
    PAIMON_ASSIGN_OR_RAISE(std::unique_ptr<BucketFunction> bucket_function,
                           BucketSelectConverter::CreateBucketFunction(
                               bucket_function_type, bucket_key_types, bucket_key_arrow_types));
    return std::unique_ptr<BucketSelector>(new BucketSelector(
        predicate, bucket_keys, std::move(bucket_key_types), std::move(bucket_key_arrow_types),
        std::move(partition_name_to_idx), std::move(partition_schema), std::move(bucket_function),
        std::move(manifest_selection), has_partition_fields, pool));
}

BucketSelector::BucketSelector(const std::shared_ptr<Predicate>& predicate,
                               std::vector<std::string> bucket_keys,
                               std::vector<FieldType> bucket_key_types,
                               std::vector<std::shared_ptr<arrow::DataType>> bucket_key_arrow_types,
                               std::map<std::string, int32_t> partition_name_to_idx,
                               std::shared_ptr<arrow::Schema> partition_schema,
                               std::unique_ptr<BucketFunction> bucket_function,
                               Selection manifest_selection, bool has_partition_fields,
                               const std::shared_ptr<MemoryPool>& pool)
    : predicate_(predicate),
      bucket_keys_(std::move(bucket_keys)),
      bucket_key_types_(std::move(bucket_key_types)),
      bucket_key_arrow_types_(std::move(bucket_key_arrow_types)),
      partition_name_to_idx_(std::move(partition_name_to_idx)),
      partition_schema_(std::move(partition_schema)),
      bucket_function_(std::move(bucket_function)),
      has_partition_fields_(has_partition_fields),
      pool_(pool),
      manifest_selection_(std::move(manifest_selection)) {}

BucketSelector::~BucketSelector() = default;

Result<bool> BucketSelector::Select(const BinaryRow& partition, int32_t bucket,
                                    int32_t total_buckets) const {
    if (bucket < 0 || total_buckets <= 0) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Selection* selection = &manifest_selection_;
    if (has_partition_fields_) {
        PAIMON_ASSIGN_OR_RAISE(selection, PartitionSelection(partition));
    }
    if (!selection->rows) {
        return true;
    }
    const std::set<int32_t>& buckets = Buckets(selection, total_buckets);
    return buckets.find(bucket) != buckets.end();
}

Result<bool> BucketSelector::MayContain(int32_t total_buckets, int32_t min_bucket,
                                        int32_t max_bucket) const {
    if (total_buckets <= 0 || min_bucket < 0) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!manifest_selection_.rows) {
        return true;
    }
    const std::set<int32_t>& buckets = Buckets(&manifest_selection_, total_buckets);
    auto iter = buckets.lower_bound(min_bucket);
    return iter != buckets.end() && *iter <= max_bucket;
}

Result<BucketSelector::Selection*> BucketSelector::PartitionSelection(
    const BinaryRow& partition) const {
    auto iter = partition_selections_.find(partition);
    if (iter != partition_selections_.end()) {
        return iter->second.get();
    }
    auto selection = std::make_unique<Selection>();
    PAIMON_ASSIGN_OR_RAISE(
        PartitionRewrite rewrite,
        RewriteWithPartition(predicate_, partition_name_to_idx_, partition_schema_, partition));
    // a predicate the partition makes false selects nothing, but keeping the entries is safe
    if (!rewrite.constant) {
        PAIMON_ASSIGN_OR_RAISE(selection->rows, CollectBucketKeyRows(
                                                    rewrite.predicate, bucket_keys_,
                                                    bucket_key_types_, bucket_key_arrow_types_,
                                                    pool_.get()));
    }
    Selection* result = selection.get();
    partition_selections_.emplace(partition.Copy(pool_.get()), std::move(selection));
    return result;
}

const std::set<int32_t>& BucketSelector::Buckets(Selection* selection,
                                                 int32_t total_buckets) const {
    auto iter = selection->buckets_by_total.find(total_buckets);
    if (iter != selection->buckets_by_total.end()) {
        return iter->second;
    }
    std::set<int32_t> buckets;
    for (const auto& row : selection->rows.value()) {
        buckets.insert(bucket_function_->Bucket(row, total_buckets));
    }
    return selection->buckets_by_total.emplace(total_buckets, std::move(buckets)).first->second;
}

Result<BucketSelector::PartitionRewrite> BucketSelector::RewriteWithPartition(
    const std::shared_ptr<Predicate>& predicate,
    const std::map<std::string, int32_t>& partition_name_to_idx,
    const std::shared_ptr<arrow::Schema>& partition_schema, const BinaryRow& partition) {
    if (auto leaf = std::dynamic_pointer_cast<LeafPredicate>(predicate)) {
        if (partition_name_to_idx.find(leaf->FieldName()) == partition_name_to_idx.end()) {
            return PartitionRewrite{std::nullopt, predicate};
        }
        PAIMON_ASSIGN_OR_RAISE(
            std::shared_ptr<Predicate> partition_leaf,
            PredicateUtils::CreatePickedFieldFilter(predicate, partition_name_to_idx));
        auto filter = std::dynamic_pointer_cast<PredicateFilter>(partition_leaf);
        if (!filter) {
            return Status::Invalid("invalid partition predicate, cannot cast to PredicateFilter");
        }
        PAIMON_ASSIGN_OR_RAISE(bool value, filter->Test(partition_schema, partition));
        return PartitionRewrite{value, nullptr};
    }
    auto compound = std::dynamic_pointer_cast<CompoundPredicate>(predicate);
    if (!compound) {
        return PartitionRewrite{std::nullopt, predicate};
    }
    Function::Type type = compound->GetFunction().GetType();
    if (type != Function::Type::AND && type != Function::Type::OR) {
        return PartitionRewrite{std::nullopt, predicate};
    }
    // AND is decided by a false child, OR by a true one; the other constant drops out
    const bool deciding = type == Function::Type::OR;
    std::vector<std::shared_ptr<Predicate>> children;
    for (const auto& child : compound->Children()) {
        PAIMON_ASSIGN_OR_RAISE(PartitionRewrite rewritten,
                               RewriteWithPartition(child, partition_name_to_idx,
                                                    partition_schema, partition));
        if (rewritten.constant) {
            if (rewritten.constant.value() == deciding) {
                return PartitionRewrite{deciding, nullptr};
            }
            continue;
        }
        children.push_back(std::move(rewritten.predicate));
    }
    if (children.empty()) {
        return PartitionRewrite{!deciding, nullptr};
    }
    if (type == Function::Type::AND) {
        PAIMON_ASSIGN_OR_RAISE(std::shared_ptr<Predicate> result, PredicateBuilder::And(children));
        return PartitionRewrite{std::nullopt, std::move(result)};
    }
    PAIMON_ASSIGN_OR_RAISE(std::shared_ptr<Predicate> result, PredicateBuilder::Or(children));
    return PartitionRewrite{std::nullopt, std::move(result)};
}

Result<std::optional<std::vector<BinaryRow>>> BucketSelector::CollectBucketKeyRows(
    const std::shared_ptr<Predicate>& predicate, const std::vector<std::string>& bucket_keys,
    const std::vector<FieldType>& bucket_key_types,
    const std::vector<std::shared_ptr<arrow::DataType>>& bucket_key_arrow_types,
    MemoryPool* pool) {
    std::vector<std::optional<std::vector<Literal>>> values(bucket_keys.size());
    for (const auto& and_child : PredicateUtils::SplitAnd(predicate)) {
        std::optional<size_t> reference;
        std::vector<Literal> literals;
        bool usable = true;
        for (const auto& or_child : PredicateUtils::SplitOr(and_child)) {
            auto leaf = std::dynamic_pointer_cast<LeafPredicate>(or_child);
            if (!leaf) {
                usable = false;
                break;
            }
            Function::Type type = leaf->GetFunction().GetType();
            auto key = std::find(bucket_keys.begin(), bucket_keys.end(), leaf->FieldName());
            if (key == bucket_keys.end() ||
                (type != Function::Type::EQUAL && type != Function::Type::IN)) {
                usable = false;
                break;
            }
            auto index = static_cast<size_t>(key - bucket_keys.begin());
            if ((reference && reference.value() != index) ||
                leaf->GetFieldType() != bucket_key_types[index]) {
                usable = false;
                break;
            }
            reference = index;
            for (const auto& literal : leaf->Literals()) {
                if (!literal.IsNull()) {
                    literals.push_back(literal);
                }
            }
        }
        if (!usable || !reference) {
            continue;
        }
        auto& key_values = values[reference.value()];
        if (!key_values) {
            key_values = std::move(literals);
            continue;
        }
        std::vector<Literal> intersection;
        for (const auto& literal : key_values.value()) {
            if (std::find(literals.begin(), literals.end(), literal) != literals.end()) {
                intersection.push_back(literal);
            }
        }
        key_values = std::move(intersection);
    }

    int64_t row_count = 1;
    for (const auto& key_values : values) {
        // an empty set means no row can match; selecting nothing is left to the row filters
        if (!key_values || key_values->empty()) {
            return std::optional<std::vector<BinaryRow>>();
        }
        row_count *= static_cast<int64_t>(key_values->size());
        if (row_count > MAX_VALUES) {
            return std::optional<std::vector<BinaryRow>>();
        }
    }

    auto num_fields = static_cast<int32_t>(bucket_keys.size());
    std::vector<BinaryRow> rows;
    rows.reserve(static_cast<size_t>(row_count));
    std::vector<size_t> positions(bucket_keys.size(), 0);
    for (int64_t n = 0; n < row_count; n++) {
        BinaryRow row(num_fields);
        BinaryRowWriter writer(&row, /*initial_size=*/1024, pool);
        writer.Reset();
        for (int32_t i = 0; i < num_fields; i++) {
            Status status = BucketSelectConverter::WriteLiteralToRow(
                i, values[i].value()[positions[i]], bucket_key_types[i], bucket_key_arrow_types[i],
                &writer);
            if (!status.ok()) {
                // a bucket key type that cannot be encoded does not restrict buckets
                return std::optional<std::vector<BinaryRow>>();
            }
        }
        writer.Complete();
        rows.push_back(std::move(row));
        for (int32_t i = num_fields - 1; i >= 0; i--) {
            if (++positions[i] < values[i]->size()) {
                break;
            }
            positions[i] = 0;
        }
    }
    return std::optional<std::vector<BinaryRow>>(std::move(rows));
}

}  // namespace paimon
