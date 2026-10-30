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

#pragma once

#include <cstdint>
#include <string>

#include "paimon/data/decimal.h"
#include "paimon/data/timestamp.h"
#include "paimon/result.h"

namespace paimon {

/// String conversions that reproduce the Java implementation byte for byte. Partition directory
/// names are written by Java, so reading them back requires the same rendering.
class JavaStringUtils {
 public:
    JavaStringUtils() = delete;
    ~JavaStringUtils() = delete;

    /// `Float.toString`: shortest round-trip digits, plain notation in [1e-3, 1e7), otherwise
    /// `d.dddE<exp>`; `NaN`, `Infinity`, `-Infinity`, `-0.0`.
    static std::string FloatToString(float value);
    /// `Double.toString`, with the same rules as `FloatToString`.
    static std::string DoubleToString(double value);

    /// `Float.parseFloat` for the strings `FloatToString` produces, plus plain decimals.
    static Result<float> ParseFloat(const std::string& str);
    /// `Double.parseDouble` for the strings `DoubleToString` produces, plus plain decimals.
    static Result<double> ParseDouble(const std::string& str);

    /// Paimon `Timestamp.toString`, i.e. `LocalDateTime.toString` in UTC: seconds are omitted
    /// when seconds and nanos are zero, and the fraction has 3, 6 or 9 digits.
    static std::string TimestampToLocalDateTimeString(const Timestamp& timestamp);
    /// Paimon `DateTimeUtils.formatTimestamp(ts, precision)`: `yyyy-MM-dd HH:mm:ss`, then the
    /// nanosecond fraction with trailing zeros removed down to `precision` digits.
    static std::string TimestampToString(const Timestamp& timestamp, int32_t precision);
    /// Parses `yyyy-MM-dd[( |T)HH:mm[:ss[.fffffffff]]]` as a UTC timestamp.
    static Result<Timestamp> ParseTimestamp(const std::string& str);

    /// `BigDecimal.toPlainString` of the unscaled value and scale.
    static std::string DecimalToPlainString(const Decimal& decimal);
    /// Parses a plain decimal, rounding HALF_UP to `scale` like `Decimal.fromBigDecimal`.
    /// Fails when the result does not fit `precision`.
    static Result<Decimal> ParseDecimal(const std::string& str, int32_t precision, int32_t scale);
};

}  // namespace paimon
