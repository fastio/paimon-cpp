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

#include "paimon/common/utils/data_converter_utils.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "arrow/type.h"
#include "arrow/type_fwd.h"
#include "gtest/gtest.h"
#include "paimon/memory/memory_pool.h"
#include "paimon/testing/utils/testharness.h"

namespace paimon::test {
TEST(DataConverterUtilsTest, TestDataToBinaryRowConverterWithLegacyPartitionName) {
    auto pool = GetDefaultPool();
    std::vector<std::pair<std::string, arrow::Type::type>> data = {
        {"true", arrow::Type::BOOL},
        {"10", arrow::Type::INT8},
        {"-20", arrow::Type::INT8},
        {"1556", arrow::Type::INT16},
        {"-2556", arrow::Type::INT16},
        {"348489", arrow::Type::INT32},
        {"-448489", arrow::Type::INT32},
        {"279039", arrow::Type::INT64},
        {"1234567", arrow::Type::INT64},
        {"abcde", arrow::Type::STRING},
        {"这是一个很长很长的中文", arrow::Type::STRING},
        {"10440", arrow::Type::DATE32}};

    std::vector<DataConverterUtils::StrToBinaryRowConverter> converters;
    std::vector<DataConverterUtils::BinaryRowFieldToStrConverter> reconverters;
    for (const auto& [value, type] : data) {
        ASSERT_OK_AND_ASSIGN(auto converter,
                             DataConverterUtils::CreateDataToBinaryRowConverter(type, pool.get()));
        converters.emplace_back(std::move(converter));
        ASSERT_OK_AND_ASSIGN(auto reconverter,
                             DataConverterUtils::CreateBinaryRowFieldToStringConverter(
                                 type, /*legacy_partition_name_enabled=*/true));
        reconverters.emplace_back(reconverter);
    }
    // test not implement type
    ASSERT_NOK(DataConverterUtils::CreateDataToBinaryRowConverter(arrow::Type::LIST, pool.get()));

    BinaryRow row(data.size());
    BinaryRowWriter writer(&row, 0, pool.get());
    for (size_t idx = 0; idx < data.size(); idx++) {
        ASSERT_OK(converters[idx](data[idx].first, idx, &writer));
    }
    // test invalid str
    ASSERT_NOK(converters[0]("abc", /*idx=*/0, &writer));
    writer.Complete();

    ASSERT_EQ(data.size(), row.GetFieldCount());
    ASSERT_EQ(true, row.GetBoolean(0));
    ASSERT_EQ(10, row.GetByte(1));
    // GetByte() returns a plain char, whose signedness the target ABI decides and
    // -fsigned-char / -funsigned-char can flip; the AArch64 Linux ABI makes it unsigned.
    ASSERT_EQ(-20, static_cast<int8_t>(row.GetByte(2)));
    ASSERT_EQ(1556, row.GetShort(3));
    ASSERT_EQ(-2556, row.GetShort(4));
    ASSERT_EQ(348489, row.GetInt(5));
    ASSERT_EQ(-448489, row.GetInt(6));
    ASSERT_EQ(279039, row.GetLong(7));
    ASSERT_EQ(1234567, row.GetLong(8));
    ASSERT_EQ("abcde", row.GetString(9).ToString());
    ASSERT_EQ("这是一个很长很长的中文", row.GetString(10).ToString());
    ASSERT_EQ(10440, row.GetDate(11));

    for (size_t idx = 0; idx < data.size(); idx++) {
        ASSERT_OK_AND_ASSIGN(auto partition_field_str, reconverters[idx](row, idx));
        ASSERT_EQ(data[idx].first, partition_field_str);
    }
}

TEST(DataConverterUtilsTest, TestDataToBinaryRowConverterWithNoLegacyPartitionName) {
    auto pool = GetDefaultPool();
    std::vector<std::pair<std::string, arrow::Type::type>> data = {
        {"true", arrow::Type::BOOL},
        {"10", arrow::Type::INT8},
        {"-20", arrow::Type::INT8},
        {"1556", arrow::Type::INT16},
        {"-2556", arrow::Type::INT16},
        {"348489", arrow::Type::INT32},
        {"-448489", arrow::Type::INT32},
        {"279039", arrow::Type::INT64},
        {"1234567", arrow::Type::INT64},
        {"abcde", arrow::Type::STRING},
        {"这是一个很长很长的中文", arrow::Type::STRING},
        {"1998-08-02", arrow::Type::DATE32}};

    std::vector<DataConverterUtils::StrToBinaryRowConverter> converters;
    std::vector<DataConverterUtils::BinaryRowFieldToStrConverter> reconverters;
    for (const auto& [value, type] : data) {
        ASSERT_OK_AND_ASSIGN(auto converter,
                             DataConverterUtils::CreateDataToBinaryRowConverter(type, pool.get()));
        converters.emplace_back(std::move(converter));
        ASSERT_OK_AND_ASSIGN(auto reconverter,
                             DataConverterUtils::CreateBinaryRowFieldToStringConverter(
                                 type, /*legacy_partition_name_enabled=*/false));
        reconverters.emplace_back(reconverter);
    }
    BinaryRow row(data.size());
    BinaryRowWriter writer(&row, 0, pool.get());
    for (size_t idx = 0; idx < data.size(); idx++) {
        ASSERT_OK(converters[idx](data[idx].first, idx, &writer));
    }
    writer.Complete();

    ASSERT_EQ(data.size(), row.GetFieldCount());
    ASSERT_EQ(true, row.GetBoolean(0));
    ASSERT_EQ(10, row.GetByte(1));
    // GetByte() returns a plain char, whose signedness the target ABI decides and
    // -fsigned-char / -funsigned-char can flip; the AArch64 Linux ABI makes it unsigned.
    ASSERT_EQ(-20, static_cast<int8_t>(row.GetByte(2)));
    ASSERT_EQ(1556, row.GetShort(3));
    ASSERT_EQ(-2556, row.GetShort(4));
    ASSERT_EQ(348489, row.GetInt(5));
    ASSERT_EQ(-448489, row.GetInt(6));
    ASSERT_EQ(279039, row.GetLong(7));
    ASSERT_EQ(1234567, row.GetLong(8));
    ASSERT_EQ("abcde", row.GetString(9).ToString());
    ASSERT_EQ("这是一个很长很长的中文", row.GetString(10).ToString());
    ASSERT_EQ(10440, row.GetDate(11));

    for (size_t idx = 0; idx < data.size(); idx++) {
        ASSERT_OK_AND_ASSIGN(auto partition_field_str, reconverters[idx](row, idx));
        ASSERT_EQ(data[idx].first, partition_field_str);
    }
}

// Partition directory names Java writes for TIMESTAMP, DECIMAL, FLOAT and DOUBLE.
TEST(DataConverterUtilsTest, TestParameterizedPartitionTypes) {
    auto pool = GetDefaultPool();
    struct Case {
        std::string legacy_str;
        std::string str;
        std::shared_ptr<arrow::DataType> type;
    };
    std::vector<Case> data = {
        {"2024-01-01T00:00", "2024-01-01 00:00:00.000", arrow::timestamp(arrow::TimeUnit::MILLI)},
        {"2024-01-01T12:30:00.123456", "2024-01-01 12:30:00.123456",
         arrow::timestamp(arrow::TimeUnit::MICRO)},
        {"1969-12-31T23:59:59", "1969-12-31 23:59:59", arrow::timestamp(arrow::TimeUnit::SECOND)},
        {"2024-01-02T00:00:00.000000001", "2024-01-02 00:00:00.000000001",
         arrow::timestamp(arrow::TimeUnit::NANO)},
        {"1.50", "1.50", arrow::decimal128(10, 2)},
        {"-3.00", "-3.00", arrow::decimal128(10, 2)},
        {"12345678901234567890.123", "12345678901234567890.123", arrow::decimal128(38, 3)},
        {"1.5", "1.5", arrow::float32()},
        {"-0.0", "-0.0", arrow::float32()},
        {"1.0E10", "1.0E10", arrow::float32()},
        {"0.25", "0.25", arrow::float64()},
        {"NaN", "NaN", arrow::float64()},
        {"abc", "abc", arrow::utf8()},
    };
    for (bool legacy : {true, false}) {
        BinaryRow row(data.size());
        BinaryRowWriter writer(&row, 0, pool.get());
        for (size_t idx = 0; idx < data.size(); idx++) {
            const auto& value = legacy ? data[idx].legacy_str : data[idx].str;
            ASSERT_OK_AND_ASSIGN(auto converter, DataConverterUtils::CreateDataToBinaryRowConverter(
                                                     data[idx].type, pool.get()));
            ASSERT_OK(converter(value, idx, &writer)) << value;
        }
        writer.Complete();
        for (size_t idx = 0; idx < data.size(); idx++) {
            ASSERT_OK_AND_ASSIGN(auto reconverter,
                                 DataConverterUtils::CreateBinaryRowFieldToStringConverter(
                                     data[idx].type, legacy));
            ASSERT_OK_AND_ASSIGN(auto str, reconverter(row, idx));
            ASSERT_EQ(legacy ? data[idx].legacy_str : data[idx].str, str);
        }
    }
    ASSERT_NOK(DataConverterUtils::CreateBinaryRowFieldToStringConverter(
        arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"), /*legacy_partition_name_enabled=*/false));
    ASSERT_OK(DataConverterUtils::CreateBinaryRowFieldToStringConverter(
        arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"), /*legacy_partition_name_enabled=*/true));
}

TEST(DataConverterUtilsTest, TestNullFieldWriterKeepsNonCompactSlots) {
    auto pool = GetDefaultPool();
    std::vector<std::shared_ptr<arrow::DataType>> types = {
        arrow::timestamp(arrow::TimeUnit::MICRO), arrow::decimal128(38, 2),
        arrow::timestamp(arrow::TimeUnit::MILLI), arrow::decimal128(10, 2)};
    BinaryRow row(types.size());
    BinaryRowWriter writer(&row, 0, pool.get());
    for (size_t idx = 0; idx < types.size(); idx++) {
        DataConverterUtils::CreateNullFieldWriter(types[idx])(idx, &writer);
    }
    writer.Complete();
    for (size_t idx = 0; idx < types.size(); idx++) {
        ASSERT_TRUE(row.IsNullAt(idx));
    }
    // Same bytes as a row written through the typed null writers of BinaryRowWriter.
    BinaryRow expected(types.size());
    BinaryRowWriter expected_writer(&expected, 0, pool.get());
    expected_writer.WriteTimestamp(0, std::nullopt, 6);
    expected_writer.WriteDecimal(1, std::nullopt, 38);
    expected_writer.SetNullAt(2);
    expected_writer.SetNullAt(3);
    expected_writer.Complete();
    ASSERT_EQ(expected, row);
}

}  // namespace paimon::test
