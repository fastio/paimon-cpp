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

#include <set>
#include <utility>

#include "arrow/type.h"
#include "gtest/gtest.h"
#include "paimon/catalog/schema_change.h"
#include "paimon/fs/local/local_file_system.h"
#include "paimon/status.h"
#include "paimon/testing/utils/testharness.h"

namespace paimon::test {

TEST(SchemaManagerTest, TestSimple) {
    auto fs = std::make_shared<LocalFileSystem>();
    std::string table_root =
        paimon::test::GetDataDir() + "/orc/pk_table_with_alter_table.db/pk_table_with_alter_table/";
    SchemaManager manager(fs, table_root);
    ASSERT_EQ(manager.ToSchemaPath(/*schema_id=*/0),
              paimon::test::GetDataDir() +
                  "/orc/pk_table_with_alter_table.db/pk_table_with_alter_table/schema/schema-0");

    ASSERT_OK_AND_ASSIGN(std::shared_ptr<TableSchema> ret, manager.ReadSchema(/*schema_id=*/1));
    std::string schema_json = R"({
        "version" : 3,
        "id" : 1,
        "fields" : [ {
                "id" : 1,
                "name" : "key1",
                "type" : "INT NOT NULL"
        }, {
                "id" : 7,
                "name" : "k",
                "type" : "STRING"
        }, {
                "id" : 2,
                "name" : "key_2",
                "type" : "INT NOT NULL"
        }, {
                "id" : 4,
                "name" : "c",
                "type" : "INT"
        }, {
                "id" : 8,
                "name" : "d",
                "type" : "INT",
                "description" : ""
        }, {
                "id" : 6,
                "name" : "a",
                "type" : "INT"
        }, {
                "id" : 0,
                "name" : "key0",
                "type" : "INT NOT NULL"
        }, {
                "id" : 9,
                "name" : "e",
                "type" : "INT"
        } ],
        "highestFieldId" : 9,
        "partitionKeys" : [ "key0", "key1" ],
        "primaryKeys" : [ "key0", "key1", "key_2" ],
        "options" : {
                "bucket" : "1",
                "manifest.format" : "orc",
                "file.format" : "orc",
                "deletion-vectors.enabled" : "true",
                "commit.force-compact" : "true"
        },
        "timeMillis" : 1730516111087
    })";
    ASSERT_OK_AND_ASSIGN(auto expected_schema, TableSchema::CreateFromJson(schema_json));
    ASSERT_EQ(*ret, *expected_schema);
    ASSERT_FALSE(manager.schema_cache_.empty());
    ASSERT_EQ(*manager.ReadSchema(/*schema_id=*/1).value(), *expected_schema);
    ASSERT_EQ(*(manager.Latest().value().value()), *expected_schema);
}

TEST(SchemaManagerTest, TestNonExistTable) {
    auto fs = std::make_shared<LocalFileSystem>();
    std::string table_root = paimon::test::GetDataDir() + "/non-exist.db/non-exist/";
    SchemaManager manager(fs, table_root);
    ASSERT_OK_AND_ASSIGN(std::optional<std::shared_ptr<TableSchema>> latest, manager.Latest());
    ASSERT_EQ(latest, std::nullopt);
    auto ret = manager.ReadSchema(/*schema_id=*/100);
    ASSERT_FALSE(ret.ok());
}

TEST(SchemaManagerTest, TestSchemaDirectory) {
    auto fs = std::make_shared<LocalFileSystem>();
    std::string table_root = paimon::test::GetDataDir() + "/sample_table/";
    SchemaManager manager(fs, table_root);
    ASSERT_EQ(manager.SchemaDirectory(), paimon::test::GetDataDir() + "/sample_table/schema");
}

TEST(SchemaManagerTest, TestSchemaDirectoryWithBranch) {
    auto fs = std::make_shared<LocalFileSystem>();
    std::string table_root = paimon::test::GetDataDir() + "/sample_table/";
    {
        SchemaManager manager(fs, table_root, /*branch=*/"data");
        ASSERT_EQ(manager.SchemaDirectory(),
                  paimon::test::GetDataDir() + "/sample_table/branch/branch-data/schema");
    }
    {
        SchemaManager manager(fs, table_root, /*branch=*/"main");
        ASSERT_EQ(manager.SchemaDirectory(), paimon::test::GetDataDir() + "/sample_table/schema");
    }
}

TEST(SchemaManagerTest, TestCreateTableWithInvalidInput) {
    auto fs = std::make_shared<LocalFileSystem>();
    auto dir = UniqueTestDirectory::Create();
    SchemaManager manager(fs, dir->Str());

    // Create an Arrow schema
    auto field1 = std::make_shared<arrow::Field>("id", arrow::int32(), false);
    auto field2 = std::make_shared<arrow::Field>("name", arrow::utf8());
    auto field3 = std::make_shared<arrow::Field>("value", arrow::int64());
    auto schema = arrow::schema(arrow::FieldVector{field1, field2, field3});

    std::vector<std::string> partition_keys = {"id"};
    std::vector<std::string> primary_keys = {"id"};
    std::map<std::string, std::string> options = {{"file.format", "orc"},
                                                  {"commit.force-compact", "true"}};

    // Create table
    auto result = manager.CreateTable(schema, partition_keys, primary_keys, options);
    ASSERT_NOK(result);
}

TEST(SchemaManagerTest, TestCreateTable) {
    auto fs = std::make_shared<LocalFileSystem>();
    auto dir = UniqueTestDirectory::Create();
    SchemaManager manager(fs, dir->Str());

    // Create an Arrow schema
    auto field1 = std::make_shared<arrow::Field>("id", arrow::int32(), false);
    auto field2 = std::make_shared<arrow::Field>("name", arrow::utf8());
    auto field3 = std::make_shared<arrow::Field>("value", arrow::int64());
    auto schema = arrow::schema(arrow::FieldVector{field1, field2, field3});

    std::vector<std::string> partition_keys = {"name"};
    std::vector<std::string> primary_keys = {"id"};
    std::map<std::string, std::string> options = {{"file.format", "orc"},
                                                  {"commit.force-compact", "true"}};

    // Create table
    ASSERT_OK_AND_ASSIGN([[maybe_unused]] std::unique_ptr<TableSchema> result,
                         manager.CreateTable(schema, partition_keys, primary_keys, options));

    // Verify schema was created
    ASSERT_OK_AND_ASSIGN(std::optional<std::shared_ptr<TableSchema>> latest_result,
                         manager.Latest());
    ASSERT_TRUE(latest_result);
    auto created_schema = latest_result.value();
    ASSERT_EQ(created_schema->Id(), 0);
    ASSERT_EQ(created_schema->PartitionKeys(), partition_keys);
    ASSERT_EQ(created_schema->PrimaryKeys(), primary_keys);
}

TEST(SchemaManagerTest, TestCreateTableAlreadyExists) {
    auto fs = std::make_shared<LocalFileSystem>();
    std::string table_root =
        paimon::test::GetDataDir() + "/orc/pk_table_with_alter_table.db/pk_table_with_alter_table/";
    SchemaManager manager(fs, table_root);

    // Create an Arrow schema
    auto field = std::make_shared<arrow::Field>("dummy", arrow::int32());
    auto schema = arrow::schema(arrow::FieldVector{field});

    // Try to create table where schema already exists
    ASSERT_NOK_WITH_MSG(manager.CreateTable(schema, {}, {}, {}), "Schema in filesystem exists");
}

TEST(SchemaManagerTest, TestListAllIds) {
    auto fs = std::make_shared<LocalFileSystem>();
    std::string table_root =
        paimon::test::GetDataDir() + "/orc/pk_table_with_mor.db/pk_table_with_mor/";
    SchemaManager manager(fs, table_root);
    ASSERT_OK_AND_ASSIGN(auto ids, manager.ListAllIds());
    ASSERT_EQ(std::set<int64_t>(ids.begin(), ids.end()), std::set<int64_t>({0, 1, 2, 3, 4}));
}

namespace {

Result<std::unique_ptr<TableSchema>> CreateBaseTable(SchemaManager* manager) {
    auto schema = arrow::schema(
        {arrow::field("id", arrow::int32(), false), arrow::field("dt", arrow::utf8(), false),
         arrow::field("name", arrow::utf8()), arrow::field("val", arrow::int32())});
    std::map<std::string, std::string> options = {{"bucket", "1"}, {"bucket-key", "id"}};
    return manager->CreateTable(schema, {"dt"}, {"id", "dt"}, options);
}

const DataField* FindField(const TableSchema& schema, const std::string& name) {
    for (const auto& field : schema.Fields()) {
        if (field.Name() == name) {
            return &field;
        }
    }
    return nullptr;
}

class ConflictOnceFileSystem : public LocalFileSystem {
 public:
    Status AtomicStore(const std::string& path, const std::string& content) override {
        if (!failed_) {
            failed_ = true;
            return Status::Invalid("dst file already exist");
        }
        return LocalFileSystem::AtomicStore(path, content);
    }

 private:
    bool failed_ = false;
};

class WriteThenConflictFileSystem : public LocalFileSystem {
 public:
    Status AtomicStore(const std::string& path, const std::string& content) override {
        if (!conflicted_) {
            conflicted_ = true;
            PAIMON_RETURN_NOT_OK(LocalFileSystem::AtomicStore(path, content));
            return Status::Invalid("dst file already exist");
        }
        return LocalFileSystem::AtomicStore(path, content);
    }

 private:
    bool conflicted_ = false;
};

}  // namespace

TEST(SchemaManagerTest, CommitSchemaChangesEvolvesFieldsAndOptions) {
    auto fs = std::make_shared<LocalFileSystem>();
    auto dir = UniqueTestDirectory::Create();
    SchemaManager manager(fs, dir->Str());
    ASSERT_NOK_WITH_MSG(manager.CommitSchemaChanges({}), "does not exist");

    ASSERT_OK_AND_ASSIGN(std::unique_ptr<TableSchema> created, CreateBaseTable(&manager));
    ASSERT_EQ(created->Id(), 0);
    ASSERT_EQ(FindField(*created, "id")->Id(), 0);
    ASSERT_EQ(FindField(*created, "name")->Id(), 2);
    ASSERT_EQ(created->HighestFieldId(), 3);

    ASSERT_OK_AND_ASSIGN(
        int64_t same_type,
        manager.CommitSchemaChanges({SchemaChange::UpdateColumnType("val", arrow::int32())}));
    ASSERT_EQ(same_type, 0);
    ASSERT_OK_AND_ASSIGN(std::vector<int64_t> ids_after_same_type, manager.ListAllIds());
    ASSERT_EQ(ids_after_same_type, std::vector<int64_t>({0}));

    auto extra = arrow::field("extra", arrow::int32());
    extra = extra->WithMetadata(arrow::key_value_metadata({DataField::FIELD_ID}, {"99"}));
    ASSERT_OK_AND_ASSIGN(int64_t added,
                         manager.CommitSchemaChanges({SchemaChange::AddColumn(extra)}));
    ASSERT_EQ(added, 1);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<TableSchema> schema1, manager.ReadSchema(1));
    ASSERT_EQ(FindField(*schema1, "extra")->Id(), 4);
    ASSERT_EQ(schema1->HighestFieldId(), 4);
    ASSERT_EQ(FindField(*schema1, "name")->Id(), 2);

    ASSERT_OK_AND_ASSIGN(int64_t renamed, manager.CommitSchemaChanges(
                                              {SchemaChange::RenameColumn("name", "title")}));
    ASSERT_EQ(renamed, 2);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<TableSchema> schema2, manager.ReadSchema(2));
    ASSERT_EQ(FindField(*schema2, "title")->Id(), 2);
    ASSERT_EQ(schema2->PrimaryKeys(), std::vector<std::string>({"id", "dt"}));
    ASSERT_EQ(schema2->Options().at("bucket-key"), "id");
    ASSERT_EQ(schema2->Comment(), created->Comment());

    ASSERT_OK_AND_ASSIGN(int64_t renamed_key,
                         manager.CommitSchemaChanges({SchemaChange::RenameColumn("dt", "day")}));
    ASSERT_EQ(renamed_key, 3);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<TableSchema> schema3, manager.ReadSchema(3));
    ASSERT_EQ(schema3->PartitionKeys(), std::vector<std::string>({"day"}));
    ASSERT_EQ(schema3->PrimaryKeys(), std::vector<std::string>({"id", "day"}));
    ASSERT_EQ(FindField(*schema3, "day")->Id(), 1);

    ASSERT_OK_AND_ASSIGN(int64_t renamed_bucket_key,
                         manager.CommitSchemaChanges({SchemaChange::RenameColumn("id", "pk")}));
    ASSERT_EQ(renamed_bucket_key, 4);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<TableSchema> renamed_pk, manager.ReadSchema(4));
    ASSERT_EQ(renamed_pk->PrimaryKeys(), std::vector<std::string>({"pk", "day"}));
    ASSERT_EQ(renamed_pk->Options().at("bucket-key"), "pk");
    ASSERT_EQ(FindField(*renamed_pk, "pk")->Id(), 0);

    ASSERT_OK_AND_ASSIGN(
        int64_t widened,
        manager.CommitSchemaChanges({SchemaChange::UpdateColumnType("val", arrow::int64())}));
    ASSERT_EQ(widened, 5);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<TableSchema> schema4, manager.ReadSchema(5));
    ASSERT_EQ(FindField(*schema4, "val")->Id(), 3);
    ASSERT_TRUE(FindField(*schema4, "val")->Type()->Equals(arrow::int64()));

    ASSERT_NOK(manager.CommitSchemaChanges({SchemaChange::UpdateColumnType("pk", arrow::int64())}));
    ASSERT_NOK(manager.CommitSchemaChanges({SchemaChange::UpdateColumnType("day", arrow::utf8())}));
    ASSERT_NOK(manager.CommitSchemaChanges({SchemaChange::UpdateColumnType(
        "val", arrow::struct_({arrow::field("x", arrow::int32())}))}));
    ASSERT_NOK(
        manager.CommitSchemaChanges({SchemaChange::UpdateColumnType("val", arrow::date32())}));
    ASSERT_NOK(manager.CommitSchemaChanges({SchemaChange::DropColumn("pk")}));
    ASSERT_NOK(manager.CommitSchemaChanges({SchemaChange::DropColumn("day")}));
    ASSERT_NOK(manager.CommitSchemaChanges({SchemaChange::SetOption("bucket", "1")}));
    ASSERT_NOK(manager.CommitSchemaChanges({SchemaChange::SetOption("BUCKET", "4")}));
    ASSERT_NOK(manager.CommitSchemaChanges({SchemaChange::RemoveOption("bucket-key")}));
    ASSERT_NOK(
        manager.CommitSchemaChanges({SchemaChange::SetOption("snapshot.num-retained.min", "0")}));
    ASSERT_OK_AND_ASSIGN(std::vector<int64_t> ids_after_reject, manager.ListAllIds());
    ASSERT_EQ(std::set<int64_t>(ids_after_reject.begin(), ids_after_reject.end()),
              std::set<int64_t>({0, 1, 2, 3, 4, 5}));

    ASSERT_OK_AND_ASSIGN(int64_t option_set, manager.CommitSchemaChanges(
                                                 {SchemaChange::SetOption("file.format", "orc")}));
    ASSERT_EQ(option_set, 6);
    ASSERT_OK_AND_ASSIGN(int64_t option_same, manager.CommitSchemaChanges(
                                                  {SchemaChange::SetOption("file.format", "orc")}));
    ASSERT_EQ(option_same, 6);
    ASSERT_NOK(manager.CommitSchemaChanges({SchemaChange::RemoveOption("missing-option")}));
    ASSERT_OK_AND_ASSIGN(int64_t option_reset,
                         manager.CommitSchemaChanges({SchemaChange::RemoveOption("FILE.FORMAT")}));
    ASSERT_EQ(option_reset, 7);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<TableSchema> schema6, manager.ReadSchema(7));
    ASSERT_EQ(schema6->Options().count("file.format"), 0);
    ASSERT_EQ(schema6->Options().at("bucket"), "1");

    ASSERT_OK_AND_ASSIGN(int64_t dropped,
                         manager.CommitSchemaChanges({SchemaChange::DropColumn("val")}));
    ASSERT_EQ(dropped, 8);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<TableSchema> schema7, manager.ReadSchema(8));
    ASSERT_EQ(FindField(*schema7, "val"), nullptr);
    ASSERT_EQ(schema7->HighestFieldId(), 4);
    ASSERT_OK_AND_ASSIGN(int64_t reused, manager.CommitSchemaChanges({SchemaChange::AddColumn(
                                             arrow::field("again", arrow::utf8()))}));
    ASSERT_EQ(reused, 9);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<TableSchema> schema8, manager.ReadSchema(9));
    ASSERT_EQ(FindField(*schema8, "again")->Id(), 5);
    ASSERT_EQ(schema8->HighestFieldId(), 5);

    ASSERT_OK_AND_ASSIGN(bool snapshot_dir, fs->Exists(dir->Str() + "/snapshot"));
    ASSERT_FALSE(snapshot_dir);
}

TEST(SchemaManagerTest, CommitSchemaChangesRetriesAtomicStoreConflict) {
    auto fs = std::make_shared<LocalFileSystem>();
    auto dir = UniqueTestDirectory::Create();
    SchemaManager creator(fs, dir->Str());
    ASSERT_OK(CreateBaseTable(&creator));

    auto conflict_fs = std::make_shared<ConflictOnceFileSystem>();
    SchemaManager manager(conflict_fs, dir->Str());
    ASSERT_OK_AND_ASSIGN(int64_t added, manager.CommitSchemaChanges({SchemaChange::AddColumn(
                                            arrow::field("extra", arrow::int32()))}));
    ASSERT_EQ(added, 1);
    ASSERT_OK_AND_ASSIGN(std::vector<int64_t> ids, manager.ListAllIds());
    ASSERT_EQ(std::set<int64_t>(ids.begin(), ids.end()), std::set<int64_t>({0, 1}));
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<TableSchema> schema1, manager.ReadSchema(1));
    ASSERT_NE(FindField(*schema1, "extra"), nullptr);
}

TEST(SchemaManagerTest, CommitSchemaChangesKeepsWinnerWhenReplayIsInvalid) {
    auto fs = std::make_shared<LocalFileSystem>();
    auto dir = UniqueTestDirectory::Create();
    SchemaManager creator(fs, dir->Str());
    ASSERT_OK(CreateBaseTable(&creator));

    auto conflict_fs = std::make_shared<WriteThenConflictFileSystem>();
    SchemaManager manager(conflict_fs, dir->Str());
    ASSERT_NOK(manager.CommitSchemaChanges(
        {SchemaChange::AddColumn(arrow::field("extra", arrow::int32()))}));
    ASSERT_OK_AND_ASSIGN(std::vector<int64_t> ids, manager.ListAllIds());
    ASSERT_EQ(std::set<int64_t>(ids.begin(), ids.end()), std::set<int64_t>({0, 1}));
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<TableSchema> schema1, manager.ReadSchema(1));
    ASSERT_NE(FindField(*schema1, "extra"), nullptr);
    ASSERT_OK_AND_ASSIGN(bool schema2, fs->Exists(dir->Str() + "/schema/schema-2"));
    ASSERT_FALSE(schema2);
}

}  // namespace paimon::test
