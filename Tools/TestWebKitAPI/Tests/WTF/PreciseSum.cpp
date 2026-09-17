/*
 * Copyright (C) 2025 Keita Nonaka <iKonnyaku40@gmail.com>.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/PreciseSum.h>

#include <array>

namespace TestWebKitAPI {

static constexpr double Infinity = std::numeric_limits<double>::infinity();
static constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
static constexpr double MaxValue = std::numeric_limits<double>::max();
static constexpr double MinNormal = std::numeric_limits<double>::min();
static constexpr double MinSubnormal = std::numeric_limits<double>::denorm_min();

static const std::array<std::pair<std::vector<double>, double>, 68> TEST_CASES = { {
    { { 1, 2, 3 }, 6 },
    { { 1e308 }, 1e308 },
    { { 1e308, -1e308 }, 0 },
    { { 0.1 }, 0.1 },
    { { 0.1, 0.1 }, 0.2 },
    { { 0.1, -0.1 }, 0 },
    { { 1e308, 1e308, 0.1, 0.1, 1e30, 0.1, -1e30, -1e308, -1e308 }, 0.30000000000000004 },
    { { 1e30, 0.1, -1e30 }, 0.1 },

    { { 8.98846567431158e+307, 8.988465674311579e+307, -1.7976931348623157e+308 }, 9.9792015476736e+291 },
    { { -5.630637621603525e+255, 9.565271205476345e+307, 2.9937604643020797e+292 }, 9.565271205476347e+307 },
    { { 6.739986666787661e+66, 2, -1.2689709186578243e-116, 1.7046015739467354e+308, -9.979201547673601e+291, 6.160926733208294e+307, -3.179557053031852e+234, -7.027282978772846e+307, -0.7500000000000001 }, 1.61796594939028e+308 },
    { { 0.31150493246968836, -8.988465674311582e+307, 1.8315037361673755e-270, -15.999999999999996, 2.9999999999999996, 7.345200721499384e+164, -2.033582473639399, -8.98846567431158e+307, -3.5737295155405993e+292, 4.13894772383715e-124, -3.6111186457260667e-35, 2.387234887098013e+180, 7.645295562778372e-298, 3.395189016861822e-103, -2.6331611115768973e-149 }, -Infinity },
    { { -1.1442589134409902e+308, 9.593842098384855e+138, 4.494232837155791e+307, -1.3482698511467367e+308, 4.494232837155792e+307 }, -1.5936821971565685e+308 },
    { { -1.1442589134409902e+308, 4.494232837155791e+307, -1.3482698511467367e+308, 4.494232837155792e+307 }, -1.5936821971565687e+308 },
    { { 9.593842098384855e+138, -6.948356297254111e+307, -1.3482698511467367e+308, 4.494232837155792e+307 }, -1.5936821971565685e+308 },
    { { -2.534858246857893e+115, 8.988465674311579e+307, 8.98846567431158e+307 }, 1.7976931348623157e+308 },
    { { 1.3588124894186193e+308, 1.4803986201152006e+223, 6.741349255733684e+307 }, Infinity },
    { { 6.741349255733684e+307, 1.7976931348623155e+308, -7.388327292663961e+41 }, Infinity },
    { { -1.9807040628566093e+28, 1.7976931348623157e+308, 9.9792015476736e+291 }, 1.7976931348623157e+308 },
    { { -1.0214557991173964e+61, 1.7976931348623157e+308, 8.98846567431158e+307, -8.988465674311579e+307 }, 1.7976931348623157e+308 },
    { { 1.7976931348623157e+308, 7.999999999999999, -1.908963895403937e-230, 1.6445950082320264e+292, 2.0734856707605806e+205 }, Infinity },
    { { 6.197409167220438e-223, -9.979201547673601e+291, -1.7976931348623157e+308 }, -Infinity },
    { { 4.49423283715579e+307, 8.944251746776101e+307, -0.0002441406250000001, 1.1752060710043817e+308, 4.940846717201632e+292, -1.6836699406454528e+308 }, 8.353845887521184e+307 },
    { { 8.988465674311579e+307, 7.999999999999998, 7.029158107234023e-308, -2.2303483759420562e-172, -1.7976931348623157e+308, -8.98846567431158e+307 }, -1.7976931348623157e+308 },
    { { 8.98846567431158e+307, 8.98846567431158e+307 }, Infinity },

    // Exactly representable negative sums. The rounding step used to round all of these one ulp away from zero.
    { { -1 }, -1 },
    { { -0.5 }, -0.5 },
    { { -1, -1 }, -2 },
    { { 1, -2 }, -1 },
    { { -0.1 }, -0.1 },
    { { -0x1p53 }, -0x1p53 },
    { { -MaxValue }, -MaxValue },
    { { -MaxValue, -MaxValue, MaxValue }, -MaxValue },
    { { -MinNormal }, -MinNormal },
    { { -MinNormal, -MinSubnormal }, -0x1.0000000000001p-1022 },
    { { -MinSubnormal }, -MinSubnormal },
    { { -1, 0x1p-52 }, -0x1.ffffffffffffep-1 },
    { { -2, 0x1p-52 }, -0x1.fffffffffffffp0 },

    // Rounding of negative sums: to nearest, ties to even, like the positive ones. The ulp of the doubles in [1, 2) is 2^-52.
    { { -1, -0x1p-54 }, -1 }, // A quarter of an ulp beyond -1.
    { { -1, -0x1.8p-53 }, -0x1.0000000000001p0 }, // Three quarters of an ulp beyond -1.
    { { -1, -0x1p-53 }, -1 }, // A tie; -1 has the even mantissa.
    { { -1, -0x1p-53, -MinSubnormal }, -0x1.0000000000001p0 }, // Just past that tie.
    { { -0x1.0000000000001p0, -0x1p-53 }, -0x1.0000000000002p0 }, // A tie; the neighbour further from zero has the even mantissa.
    { { -0x1.0000000000001p0, -0x1p-53, MinSubnormal }, -0x1.0000000000001p0 }, // Just short of that tie.
    { { -2, 0x1p-53 }, -2 }, // A tie just inside a power of two; -2 has the even mantissa.
    { { -2, 0x1.8p-53 }, -0x1.fffffffffffffp0 }, // Three quarters of an ulp inside a power of two.
    { { -MaxValue, -0x1p969 }, -MaxValue }, // A quarter of an ulp beyond -MaxValue.
    { { -MaxValue, -0x1p970, 0x1p900 }, -MaxValue }, // Just short of half an ulp beyond -MaxValue.
    { { -MaxValue, -0x1p970 }, -Infinity }, // A tie; MaxValue has an odd mantissa.
    { { MaxValue, 0x1p969 }, MaxValue },
    { { MaxValue, 0x1p970 }, Infinity },
    { { 1, 0x1p-53 }, 1 },
    { { 0x1.0000000000001p0, 0x1p-53 }, 0x1.0000000000002p0 },
    { { 0x1.0000000000001p0, 0x1p-53, -MinSubnormal }, 0x1.0000000000001p0 },

    { { NaN }, NaN },
    { { Infinity, -Infinity }, NaN },
    { { -Infinity, Infinity }, NaN },

    { { Infinity }, Infinity },
    { { Infinity, Infinity }, Infinity },
    { { -Infinity }, -Infinity },
    { { -Infinity, -Infinity }, -Infinity },

    { { }, -0.0 },
    { { 0 }, 0 },
    { { -0.0 }, -0.0 },
    { { -0.0, -0.0 }, -0.0 },
    { { -0.0, 0 }, 0 },
    { { 0, 0 }, 0 },
    { { -1, 1 }, 0 },
} };

// The result has to be the correctly rounded sum, so compare exactly (EXPECT_DOUBLE_EQ allows 4 ulps), sign of zero included.
static void shouldBeEqual(double actual, double expected)
{
    if (std::isnan(expected)) {
        EXPECT_TRUE(std::isnan(actual));
        return;
    }
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(std::signbit(actual), std::signbit(expected));
}

TEST(WTF_PreciseSum, XsumSmall_add)
{
    for (const auto& [input, expected] : TEST_CASES) {
        PreciseSum<WTF::Xsum::XsumSmall> sum;
        for (const auto v : input)
            sum.add(v);
        shouldBeEqual(sum.compute(), expected);
    }
}

TEST(WTF_PreciseSum, XsumSmall_addList)
{
    for (const auto& [input, expected] : TEST_CASES) {
        PreciseSum<WTF::Xsum::XsumSmall> sum;
        sum.addList(input);
        shouldBeEqual(sum.compute(), expected);
    }
}

TEST(WTF_PreciseSum, XsumLarge_add)
{
    for (const auto& [input, expected] : TEST_CASES) {
        PreciseSum<WTF::Xsum::XsumLarge> sum;
        for (const auto v : input)
            sum.add(v);
        shouldBeEqual(sum.compute(), expected);
    }
}

TEST(WTF_PreciseSum, XsumLarge_addList)
{
    for (const auto& [input, expected] : TEST_CASES) {
        PreciseSum<WTF::Xsum::XsumLarge> sum;
        sum.addList(input);
        shouldBeEqual(sum.compute(), expected);
    }
}

} // namespace TestWebKitAPI
