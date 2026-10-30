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

#include "paimon/common/utils/java_string_utils.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "paimon/testing/utils/testharness.h"

namespace paimon::test {

// Expected strings are what Java prints for the same values.
TEST(JavaStringUtilsTest, FloatToString) {
    std::vector<std::pair<float, std::string>> cases = {
        {1.5f, "1.5"},
        {-2.5f, "-2.5"},
        {0.25f, "0.25"},
        {0.1f, "0.1"},
        {100.0f, "100.0"},
        {1234567.0f, "1234567.0"},
        {1.0e7f, "1.0E7"},
        {1.0e10f, "1.0E10"},
        {0.001f, "0.001"},
        {1.0e-4f, "1.0E-4"},
        {1.25e-5f, "1.25E-5"},
        {std::numeric_limits<float>::max(), "3.4028235E38"},
        {std::numeric_limits<float>::denorm_min(), "1.4E-45"},
        {0.0f, "0.0"},
        {-0.0f, "-0.0"},
        {std::numeric_limits<float>::infinity(), "Infinity"},
        {-std::numeric_limits<float>::infinity(), "-Infinity"},
        {std::numeric_limits<float>::quiet_NaN(), "NaN"},
    };
    for (const auto& [value, expected] : cases) {
        ASSERT_EQ(expected, JavaStringUtils::FloatToString(value));
        ASSERT_OK_AND_ASSIGN(float parsed, JavaStringUtils::ParseFloat(expected));
        if (std::isnan(value)) {
            ASSERT_TRUE(std::isnan(parsed));
        } else {
            ASSERT_EQ(value, parsed);
            ASSERT_EQ(std::signbit(value), std::signbit(parsed));
        }
    }
}

TEST(JavaStringUtilsTest, DoubleToString) {
    std::vector<std::pair<double, std::string>> cases = {
        {0.25, "0.25"},
        {-2.5, "-2.5"},
        {0.1, "0.1"},
        {1.0e7, "1.0E7"},
        {9999999.0, "9999999.0"},
        {1.0e-3, "0.001"},
        {1.0e-4, "1.0E-4"},
        {123456789.125, "1.23456789125E8"},
        {std::numeric_limits<double>::max(), "1.7976931348623157E308"},
        {std::numeric_limits<double>::denorm_min(), "4.9E-324"},
        {-0.0, "-0.0"},
    };
    for (const auto& [value, expected] : cases) {
        ASSERT_EQ(expected, JavaStringUtils::DoubleToString(value));
        ASSERT_OK_AND_ASSIGN(double parsed, JavaStringUtils::ParseDouble(expected));
        ASSERT_EQ(value, parsed);
    }
    ASSERT_NOK(JavaStringUtils::ParseDouble(""));
    ASSERT_NOK(JavaStringUtils::ParseDouble("1.5x"));
}

TEST(JavaStringUtilsTest, TimestampToLocalDateTimeString) {
    // 2024-01-01T00:00:00Z
    const int64_t base = 1704067200000L;
    std::vector<std::pair<Timestamp, std::string>> cases = {
        {Timestamp(base, 0), "2024-01-01T00:00"},
        {Timestamp(base + 45000000L, 0), "2024-01-01T12:30"},
        {Timestamp(base + 5000L, 0), "2024-01-01T00:00:05"},
        {Timestamp(base + 123L, 0), "2024-01-01T00:00:00.123"},
        {Timestamp(base + 123L, 456000), "2024-01-01T00:00:00.123456"},
        {Timestamp(base + 123L, 456789), "2024-01-01T00:00:00.123456789"},
        {Timestamp(-1L, 0), "1969-12-31T23:59:59.999"},
        {Timestamp(-62135596800000L, 0), "0001-01-01T00:00"},
        {Timestamp(253402300799999L, 0), "9999-12-31T23:59:59.999"},
    };
    for (const auto& [timestamp, expected] : cases) {
        ASSERT_EQ(expected, JavaStringUtils::TimestampToLocalDateTimeString(timestamp));
        ASSERT_OK_AND_ASSIGN(Timestamp parsed, JavaStringUtils::ParseTimestamp(expected));
        ASSERT_EQ(timestamp, parsed) << expected;
    }
}

TEST(JavaStringUtilsTest, TimestampToStringWithPrecision) {
    const int64_t base = 1704067200000L;
    ASSERT_EQ("2024-01-01 00:00:00", JavaStringUtils::TimestampToString(Timestamp(base, 0), 0));
    ASSERT_EQ("2024-01-01 00:00:00.000",
              JavaStringUtils::TimestampToString(Timestamp(base, 0), 3));
    ASSERT_EQ("2024-01-01 00:00:00.123000",
              JavaStringUtils::TimestampToString(Timestamp(base + 123L, 0), 6));
    ASSERT_EQ("2024-01-01 00:00:00.123456789",
              JavaStringUtils::TimestampToString(Timestamp(base + 123L, 456789), 6));
    ASSERT_OK_AND_ASSIGN(Timestamp parsed,
                         JavaStringUtils::ParseTimestamp("2024-01-01 00:00:00.123000"));
    ASSERT_EQ(Timestamp(base + 123L, 0), parsed);
    ASSERT_OK_AND_ASSIGN(parsed, JavaStringUtils::ParseTimestamp("2024-01-01"));
    ASSERT_EQ(Timestamp(base, 0), parsed);
}

TEST(JavaStringUtilsTest, ParseTimestampRejectsMalformed) {
    for (const char* str :
         {"", "2024-1-01", "2024-02-30", "2024-01-01T24:00", "2024-01-01T00:00:00.", "2024-01-01X",
          "2024-01-01T00:00:00.1234567890"}) {
        ASSERT_NOK(JavaStringUtils::ParseTimestamp(str)) << str;
    }
}

TEST(JavaStringUtilsTest, DecimalToPlainString) {
    ASSERT_EQ("1.50", JavaStringUtils::DecimalToPlainString(Decimal(10, 2, 150)));
    ASSERT_EQ("-3.00", JavaStringUtils::DecimalToPlainString(Decimal(10, 2, -300)));
    ASSERT_EQ("0.00", JavaStringUtils::DecimalToPlainString(Decimal(10, 2, 0)));
    ASSERT_EQ("123", JavaStringUtils::DecimalToPlainString(Decimal(5, 0, 123)));
    ASSERT_EQ("0.0000001", JavaStringUtils::DecimalToPlainString(Decimal(10, 7, 1)));
    ASSERT_EQ("-0.05", JavaStringUtils::DecimalToPlainString(Decimal(4, 2, -5)));
    Decimal::int128_t big = 1;
    for (int i = 0; i < 37; ++i) {
        big *= 10;
    }
    ASSERT_EQ("1000000000000000000000000000000000.0000",
              JavaStringUtils::DecimalToPlainString(Decimal(38, 4, big)));
}

TEST(JavaStringUtilsTest, ParseDecimal) {
    ASSERT_OK_AND_ASSIGN(Decimal value, JavaStringUtils::ParseDecimal("1.50", 10, 2));
    ASSERT_EQ(150, value.Value());
    ASSERT_OK_AND_ASSIGN(value, JavaStringUtils::ParseDecimal("-3.00", 10, 2));
    ASSERT_EQ(-300, value.Value());
    ASSERT_OK_AND_ASSIGN(value, JavaStringUtils::ParseDecimal("2.5", 10, 2));
    ASSERT_EQ(250, value.Value());
    ASSERT_OK_AND_ASSIGN(value, JavaStringUtils::ParseDecimal("1.505", 10, 2));
    ASSERT_EQ(151, value.Value());
    ASSERT_OK_AND_ASSIGN(value, JavaStringUtils::ParseDecimal("-1.505", 10, 2));
    ASSERT_EQ(-151, value.Value());
    ASSERT_OK_AND_ASSIGN(value, JavaStringUtils::ParseDecimal("0.0000001", 10, 7));
    ASSERT_EQ(1, value.Value());
    ASSERT_NOK(JavaStringUtils::ParseDecimal("123456", 5, 0));
    ASSERT_NOK(JavaStringUtils::ParseDecimal("9.995", 3, 2));
    ASSERT_NOK(JavaStringUtils::ParseDecimal("1e5", 10, 0));
    ASSERT_NOK(JavaStringUtils::ParseDecimal(".", 10, 0));
}

}  // namespace paimon::test
