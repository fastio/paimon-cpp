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

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "arrow/api.h"
#include "gtest/gtest.h"
#include "paimon/core/bucket/default_bucket_function.h"
#include "paimon/core/bucket/mod_bucket_function.h"
#include "paimon/core/schema/table_schema.h"
#include "paimon/defs.h"
#include "paimon/memory/memory_pool.h"
#include "paimon/predicate/literal.h"
#include "paimon/predicate/predicate_builder.h"
#include "paimon/testing/utils/binary_row_generator.h"
#include "paimon/testing/utils/testharness.h"

namespace paimon::test {

class BucketSelectorTest : public ::testing::Test {
 protected:
    std::shared_ptr<TableSchema> MakeSchema(const std::string& bucket_key) const {
        auto schema = arrow::schema({arrow::field("p", arrow::int32()),
                                     arrow::field("id", arrow::int32()),
                                     arrow::field("name", arrow::utf8())});
        std::map<std::string, std::string> options = {{Options::BUCKET, "4"},
                                                      {Options::BUCKET_KEY, bucket_key}};
        auto table_schema =
            TableSchema::Create(/*schema_id=*/0, schema, /*partition_keys=*/{"p"},
                                /*primary_keys=*/{}, options);
        EXPECT_TRUE(table_schema.ok()) << table_schema.status().ToString();
        return std::move(table_schema).value();
    }

    std::unique_ptr<BucketSelector> MakeSelector(
        const std::shared_ptr<Predicate>& predicate, const std::string& bucket_key = "id",
        BucketFunctionType type = BucketFunctionType::DEFAULT) const {
        auto selector = BucketSelector::Create(predicate, MakeSchema(bucket_key), type, pool_);
        EXPECT_TRUE(selector.ok()) << selector.status().ToString();
        return std::move(selector).value();
    }

    static std::shared_ptr<Predicate> Id(int32_t id) {
        return PredicateBuilder::Equal(/*field_index=*/1, "id", FieldType::INT, Literal(id));
    }

    static std::shared_ptr<Predicate> P(int32_t p) {
        return PredicateBuilder::Equal(/*field_index=*/0, "p", FieldType::INT, Literal(p));
    }

    static std::shared_ptr<Predicate> Name(const std::string& name) {
        return PredicateBuilder::Equal(/*field_index=*/2, "name", FieldType::STRING,
                                       Literal(FieldType::STRING, name.c_str(), name.size()));
    }

    int32_t BucketOf(const BinaryRowGenerator::ValueType& values, int32_t total_buckets) const {
        DefaultBucketFunction function;
        return function.Bucket(BinaryRowGenerator::GenerateRow(values, pool_.get()),
                               total_buckets);
    }

    BinaryRow Partition(int32_t p) const {
        return BinaryRowGenerator::GenerateRow({p}, pool_.get());
    }

    std::set<int32_t> Selected(const BucketSelector& selector, const BinaryRow& partition,
                               int32_t total_buckets) const {
        std::set<int32_t> buckets;
        for (int32_t bucket = 0; bucket < total_buckets; bucket++) {
            auto selected = selector.Select(partition, bucket, total_buckets);
            EXPECT_TRUE(selected.ok());
            if (selected.value()) {
                buckets.insert(bucket);
            }
        }
        return buckets;
    }

    std::set<int32_t> AllBuckets(int32_t total_buckets) const {
        std::set<int32_t> buckets;
        for (int32_t bucket = 0; bucket < total_buckets; bucket++) {
            buckets.insert(bucket);
        }
        return buckets;
    }

    std::shared_ptr<MemoryPool> pool_ = GetDefaultPool();
};

TEST_F(BucketSelectorTest, EqualSelectsOneBucket) {
    auto selector = MakeSelector(Id(1));
    ASSERT_TRUE(selector);
    ASSERT_EQ(Selected(*selector, Partition(1), 4), (std::set<int32_t>{BucketOf({1}, 4)}));
    ASSERT_EQ(Selected(*selector, Partition(7), 4), (std::set<int32_t>{BucketOf({1}, 4)}));
}

TEST_F(BucketSelectorTest, InAndOrSelectEachValue) {
    std::set<int32_t> expected = {BucketOf({1}, 16), BucketOf({2}, 16), BucketOf({3}, 16)};
    auto in = PredicateBuilder::In(/*field_index=*/1, "id", FieldType::INT,
                                   {Literal(1), Literal(2), Literal(3), Literal(FieldType::INT)});
    auto in_selector = MakeSelector(in);
    ASSERT_TRUE(in_selector);
    ASSERT_EQ(Selected(*in_selector, Partition(1), 16), expected);

    ASSERT_OK_AND_ASSIGN(auto or_predicate, PredicateBuilder::Or({Id(1), Id(2), Id(3)}));
    auto or_selector = MakeSelector(or_predicate);
    ASSERT_TRUE(or_selector);
    ASSERT_EQ(Selected(*or_selector, Partition(1), 16), expected);
}

TEST_F(BucketSelectorTest, ConjunctsIntersect) {
    auto in = PredicateBuilder::In(/*field_index=*/1, "id", FieldType::INT,
                                   {Literal(1), Literal(2), Literal(3)});
    ASSERT_OK_AND_ASSIGN(auto or_predicate, PredicateBuilder::Or({Id(2), Id(3), Id(4)}));
    ASSERT_OK_AND_ASSIGN(auto predicate, PredicateBuilder::And({in, or_predicate}));
    auto selector = MakeSelector(predicate);
    ASSERT_TRUE(selector);
    ASSERT_EQ(Selected(*selector, Partition(1), 16),
              (std::set<int32_t>{BucketOf({2}, 16), BucketOf({3}, 16)}));

    // an empty intersection leaves the rows to the row filters
    ASSERT_OK_AND_ASSIGN(auto disjoint, PredicateBuilder::And({Id(1), Id(2)}));
    ASSERT_FALSE(MakeSelector(disjoint));
    ASSERT_OK_AND_ASSIGN(auto rows, BucketSelector::CollectBucketKeyRows(
                                        disjoint, {"id"}, {FieldType::INT}, {arrow::int32()},
                                        pool_.get()));
    ASSERT_FALSE(rows);
}

TEST_F(BucketSelectorTest, UnusablePredicatesKeepEveryBucket) {
    ASSERT_OK_AND_ASSIGN(auto mixed_or, PredicateBuilder::Or({Id(1), Name("x")}));
    ASSERT_FALSE(MakeSelector(mixed_or));
    auto range = PredicateBuilder::GreaterThan(/*field_index=*/1, "id", FieldType::INT,
                                               Literal(1));
    ASSERT_FALSE(MakeSelector(range));
    ASSERT_FALSE(MakeSelector(Name("x")));
    ASSERT_FALSE(MakeSelector(nullptr));
    // only one of two bucket keys is restricted
    ASSERT_FALSE(MakeSelector(Id(1), "id,name"));
    auto null_equal = PredicateBuilder::Equal(/*field_index=*/1, "id", FieldType::INT,
                                              Literal(FieldType::INT));
    ASSERT_FALSE(MakeSelector(null_equal));
}

TEST_F(BucketSelectorTest, ConditionsBesideBucketKeysAreIgnored) {
    ASSERT_OK_AND_ASSIGN(auto predicate, PredicateBuilder::And({Id(1), Name("x"), P(3)}));
    auto selector = MakeSelector(predicate);
    ASSERT_TRUE(selector);
    ASSERT_EQ(Selected(*selector, Partition(3), 4), (std::set<int32_t>{BucketOf({1}, 4)}));
}

TEST_F(BucketSelectorTest, CompositeBucketKey) {
    auto ids = PredicateBuilder::In(/*field_index=*/1, "id", FieldType::INT,
                                    {Literal(1), Literal(2)});
    ASSERT_OK_AND_ASSIGN(auto names, PredicateBuilder::Or({Name("a"), Name("b")}));
    ASSERT_OK_AND_ASSIGN(auto predicate, PredicateBuilder::And({ids, names}));
    auto selector = MakeSelector(predicate, "id,name");
    ASSERT_TRUE(selector);
    std::set<int32_t> expected;
    for (int32_t id : {1, 2}) {
        for (const char* name : {"a", "b"}) {
            expected.insert(BucketOf({id, std::string(name)}, 64));
        }
    }
    ASSERT_EQ(Selected(*selector, Partition(1), 64), expected);
}

TEST_F(BucketSelectorTest, PartitionValuesRewriteThePredicate) {
    ASSERT_OK_AND_ASSIGN(auto p1, PredicateBuilder::And({P(1), Id(1)}));
    ASSERT_OK_AND_ASSIGN(auto p2, PredicateBuilder::And({P(2), Id(2)}));
    ASSERT_OK_AND_ASSIGN(auto predicate, PredicateBuilder::Or({p1, p2}));
    auto selector = MakeSelector(predicate);
    ASSERT_TRUE(selector);
    ASSERT_EQ(Selected(*selector, Partition(1), 16), (std::set<int32_t>{BucketOf({1}, 16)}));
    ASSERT_EQ(Selected(*selector, Partition(2), 16), (std::set<int32_t>{BucketOf({2}, 16)}));
    // no row of partition 3 matches, which is left to the partition and row filters
    ASSERT_EQ(Selected(*selector, Partition(3), 16), AllBuckets(16));
    // the partition values are unknown for a whole manifest
    for (int32_t bucket = 0; bucket < 16; bucket++) {
        ASSERT_OK_AND_ASSIGN(bool may_contain, selector->MayContain(16, bucket, bucket));
        ASSERT_TRUE(may_contain);
    }
}

TEST_F(BucketSelectorTest, RewriteWithPartition) {
    std::map<std::string, int32_t> partition_name_to_idx = {{"p", 0}};
    auto partition_schema = arrow::schema({arrow::field("p", arrow::int32())});
    auto rewrite = [&](const std::shared_ptr<Predicate>& predicate, int32_t p) {
        auto result = BucketSelector::RewriteWithPartition(predicate, partition_name_to_idx,
                                                           partition_schema, Partition(p));
        EXPECT_TRUE(result.ok());
        return std::move(result).value();
    };

    auto leaf = rewrite(P(1), 1);
    ASSERT_EQ(leaf.constant, std::optional<bool>(true));
    leaf = rewrite(P(1), 2);
    ASSERT_EQ(leaf.constant, std::optional<bool>(false));
    leaf = rewrite(Id(1), 2);
    ASSERT_FALSE(leaf.constant);
    ASSERT_EQ(*leaf.predicate, *Id(1));

    ASSERT_OK_AND_ASSIGN(auto and_predicate, PredicateBuilder::And({P(1), Id(1), Name("x")}));
    auto and_true = rewrite(and_predicate, 1);
    ASSERT_FALSE(and_true.constant);
    ASSERT_OK_AND_ASSIGN(auto expected_and, PredicateBuilder::And({Id(1), Name("x")}));
    ASSERT_EQ(*and_true.predicate, *expected_and);
    ASSERT_EQ(rewrite(and_predicate, 2).constant, std::optional<bool>(false));

    ASSERT_OK_AND_ASSIGN(auto or_predicate, PredicateBuilder::Or({P(1), Id(1)}));
    ASSERT_EQ(rewrite(or_predicate, 1).constant, std::optional<bool>(true));
    auto or_false = rewrite(or_predicate, 2);
    ASSERT_FALSE(or_false.constant);
    ASSERT_EQ(*or_false.predicate, *Id(1));

    auto is_null = PredicateBuilder::IsNull(/*field_index=*/0, "p", FieldType::INT);
    auto null_partition = BinaryRowGenerator::GenerateRow({NullType()}, pool_.get());
    ASSERT_OK_AND_ASSIGN(auto null_rewrite,
                         BucketSelector::RewriteWithPartition(is_null, partition_name_to_idx,
                                                              partition_schema, null_partition));
    ASSERT_EQ(null_rewrite.constant, std::optional<bool>(true));
}

TEST_F(BucketSelectorTest, BucketsFollowTotalBucketsOfEachEntry) {
    auto selector = MakeSelector(Id(5));
    ASSERT_TRUE(selector);
    ASSERT_EQ(Selected(*selector, Partition(1), 2), (std::set<int32_t>{BucketOf({5}, 2)}));
    ASSERT_EQ(Selected(*selector, Partition(1), 4), (std::set<int32_t>{BucketOf({5}, 4)}));
    ASSERT_EQ(Selected(*selector, Partition(1), 7), (std::set<int32_t>{BucketOf({5}, 7)}));

    // postpone and unaware entries, and entries without a total bucket number
    for (int32_t bucket : {-2, -1}) {
        ASSERT_OK_AND_ASSIGN(bool selected, selector->Select(Partition(1), bucket, 4));
        ASSERT_TRUE(selected);
    }
    for (int32_t total_buckets : {0, -1, -2}) {
        ASSERT_OK_AND_ASSIGN(bool selected, selector->Select(Partition(1), 3, total_buckets));
        ASSERT_TRUE(selected);
    }
}

TEST_F(BucketSelectorTest, ManifestMayContain) {
    auto selector = MakeSelector(Id(5));
    ASSERT_TRUE(selector);
    int32_t bucket = BucketOf({5}, 8);
    ASSERT_OK_AND_ASSIGN(bool contains, selector->MayContain(8, bucket, bucket));
    ASSERT_TRUE(contains);
    ASSERT_OK_AND_ASSIGN(contains, selector->MayContain(8, 0, 7));
    ASSERT_TRUE(contains);
    if (bucket > 0) {
        ASSERT_OK_AND_ASSIGN(contains, selector->MayContain(8, 0, bucket - 1));
        ASSERT_FALSE(contains);
    }
    if (bucket < 7) {
        ASSERT_OK_AND_ASSIGN(contains, selector->MayContain(8, bucket + 1, 7));
        ASSERT_FALSE(contains);
    }
    ASSERT_OK_AND_ASSIGN(contains, selector->MayContain(8, -2, -2));
    ASSERT_TRUE(contains);
    ASSERT_OK_AND_ASSIGN(contains, selector->MayContain(0, 100, 100));
    ASSERT_TRUE(contains);
}

TEST_F(BucketSelectorTest, MaxValues) {
    auto in = [](int32_t count) {
        std::vector<Literal> literals;
        for (int32_t i = 0; i < count; i++) {
            literals.emplace_back(i);
        }
        return PredicateBuilder::In(/*field_index=*/1, "id", FieldType::INT, literals);
    };
    ASSERT_OK_AND_ASSIGN(auto rows, BucketSelector::CollectBucketKeyRows(
                                        in(BucketSelector::MAX_VALUES), {"id"}, {FieldType::INT},
                                        {arrow::int32()}, pool_.get()));
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->size(), static_cast<size_t>(BucketSelector::MAX_VALUES));
    ASSERT_OK_AND_ASSIGN(rows, BucketSelector::CollectBucketKeyRows(
                                   in(BucketSelector::MAX_VALUES + 1), {"id"}, {FieldType::INT},
                                   {arrow::int32()}, pool_.get()));
    ASSERT_FALSE(rows);
    ASSERT_FALSE(MakeSelector(in(BucketSelector::MAX_VALUES + 1)));
}

TEST_F(BucketSelectorTest, ModBucketFunction) {
    auto selector = MakeSelector(Id(-7), "id", BucketFunctionType::MOD);
    ASSERT_TRUE(selector);
    ASSERT_OK_AND_ASSIGN(auto function, ModBucketFunction::Create(FieldType::INT));
    int32_t expected = function->Bucket(BinaryRowGenerator::GenerateRow({-7}, pool_.get()), 4);
    ASSERT_EQ(Selected(*selector, Partition(1), 4), (std::set<int32_t>{expected}));
}

}  // namespace paimon::test
