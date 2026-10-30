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

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "arrow/type_fwd.h"
#include "paimon/bucket/bucket_function_type.h"
#include "paimon/common/data/binary_row.h"
#include "paimon/defs.h"
#include "paimon/result.h"

namespace paimon {

class BucketFunction;
class MemoryPool;
class Predicate;
class TableSchema;

/// Selects the buckets a scan has to read from EQUAL and IN conditions on every bucket key, as
/// Java `BucketSelectConverter` and `BucketFilter`.
///
/// Candidate buckets are computed with the total bucket number recorded for each manifest entry
/// or manifest, never with the current `bucket` option: files written before the bucket number
/// was changed keep their old distribution.
class BucketSelector {
 public:
    /// Java `BucketSelectConverter.MAX_VALUES`: with more bucket key combinations every bucket is
    /// read.
    static constexpr int32_t MAX_VALUES = 1000;

    /// The outcome of replacing the partition fields of a predicate with partition values.
    struct PartitionRewrite {
        /// Set when the partition values alone decide the predicate.
        std::optional<bool> constant;
        /// The remaining predicate when `constant` is not set.
        std::shared_ptr<Predicate> predicate;
    };

    /// Returns nullptr when the predicate can never restrict buckets, for example when it has no
    /// condition on a bucket key or the table has no bucket key.
    static Result<std::unique_ptr<BucketSelector>> Create(
        const std::shared_ptr<Predicate>& predicate,
        const std::shared_ptr<TableSchema>& table_schema, BucketFunctionType bucket_function_type,
        const std::shared_ptr<MemoryPool>& pool);

    ~BucketSelector();

    /// Whether an entry of `partition` in `bucket`, written with `total_buckets`, may hold
    /// matching rows. Entries outside a real bucket (postpone or unaware) are always kept.
    Result<bool> Select(const BinaryRow& partition, int32_t bucket, int32_t total_buckets) const;

    /// Whether a manifest whose entries were all written with `total_buckets` and lie in
    /// [`min_bucket`, `max_bucket`] may hold matching entries. Partition values are unknown at
    /// this level, so only conditions without partition fields are used.
    Result<bool> MayContain(int32_t total_buckets, int32_t min_bucket, int32_t max_bucket) const;

    /// Replaces each leaf on a partition field with its value on `partition`, then simplifies
    /// AND and OR, as Java `PartitionValuePredicateVisitor`.
    static Result<PartitionRewrite> RewriteWithPartition(
        const std::shared_ptr<Predicate>& predicate,
        const std::map<std::string, int32_t>& partition_name_to_idx,
        const std::shared_ptr<arrow::Schema>& partition_schema, const BinaryRow& partition);

    /// Collects the bucket key rows the predicate allows, or nullopt when it does not restrict
    /// every bucket key to EQUAL or IN values, or allows more than `MAX_VALUES` rows.
    static Result<std::optional<std::vector<BinaryRow>>> CollectBucketKeyRows(
        const std::shared_ptr<Predicate>& predicate, const std::vector<std::string>& bucket_keys,
        const std::vector<FieldType>& bucket_key_types,
        const std::vector<std::shared_ptr<arrow::DataType>>& bucket_key_arrow_types,
        MemoryPool* pool);

 private:
    struct Selection {
        std::optional<std::vector<BinaryRow>> rows;
        std::unordered_map<int32_t, std::set<int32_t>> buckets_by_total;
    };

    BucketSelector(const std::shared_ptr<Predicate>& predicate,
                   std::vector<std::string> bucket_keys, std::vector<FieldType> bucket_key_types,
                   std::vector<std::shared_ptr<arrow::DataType>> bucket_key_arrow_types,
                   std::map<std::string, int32_t> partition_name_to_idx,
                   std::shared_ptr<arrow::Schema> partition_schema,
                   std::unique_ptr<BucketFunction> bucket_function, Selection manifest_selection,
                   bool has_partition_fields, const std::shared_ptr<MemoryPool>& pool);

    Result<Selection*> PartitionSelection(const BinaryRow& partition) const;

    const std::set<int32_t>& Buckets(Selection* selection, int32_t total_buckets) const;

    std::shared_ptr<Predicate> predicate_;
    std::vector<std::string> bucket_keys_;
    std::vector<FieldType> bucket_key_types_;
    std::vector<std::shared_ptr<arrow::DataType>> bucket_key_arrow_types_;
    std::map<std::string, int32_t> partition_name_to_idx_;
    std::shared_ptr<arrow::Schema> partition_schema_;
    std::unique_ptr<BucketFunction> bucket_function_;
    bool has_partition_fields_;
    std::shared_ptr<MemoryPool> pool_;

    // manifest entries are filtered on executor threads
    mutable std::mutex mutex_;
    mutable Selection manifest_selection_;
    mutable std::unordered_map<BinaryRow, std::unique_ptr<Selection>> partition_selections_;
};

}  // namespace paimon
