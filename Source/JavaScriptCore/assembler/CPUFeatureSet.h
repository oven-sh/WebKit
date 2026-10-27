/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSExportMacros.h"
#include <stdint.h>
#include <wtf/Platform.h>

#if CPU(X86_64)

#if defined(__SSE3__)
#define JSC_BUILD_TARGET_HAS_SSE3 1
#else
#define JSC_BUILD_TARGET_HAS_SSE3 0
#endif

#if defined(__SSSE3__)
#define JSC_BUILD_TARGET_HAS_SUPPLEMENTAL_SSE3 1
#else
#define JSC_BUILD_TARGET_HAS_SUPPLEMENTAL_SSE3 0
#endif

#if defined(__SSE4_1__)
#define JSC_BUILD_TARGET_HAS_SSE4_1 1
#else
#define JSC_BUILD_TARGET_HAS_SSE4_1 0
#endif

#if defined(__SSE4_2__)
#define JSC_BUILD_TARGET_HAS_SSE4_2 1
#else
#define JSC_BUILD_TARGET_HAS_SSE4_2 0
#endif

#if defined(__POPCNT__)
#define JSC_BUILD_TARGET_HAS_POPCNT 1
#else
#define JSC_BUILD_TARGET_HAS_POPCNT 0
#endif

#if defined(__AVX__)
#define JSC_BUILD_TARGET_HAS_AVX 1
#else
#define JSC_BUILD_TARGET_HAS_AVX 0
#endif

#if defined(__AVX2__)
#define JSC_BUILD_TARGET_HAS_AVX2 1
#else
#define JSC_BUILD_TARGET_HAS_AVX2 0
#endif

#if defined(__LZCNT__)
#define JSC_BUILD_TARGET_HAS_LZCNT 1
#else
#define JSC_BUILD_TARGET_HAS_LZCNT 0
#endif

#if defined(__BMI__)
#define JSC_BUILD_TARGET_HAS_BMI1 1
#else
#define JSC_BUILD_TARGET_HAS_BMI1 0
#endif

#define FOR_EACH_CPU_FEATURE(macro) \
    macro(SSE3, JSC_BUILD_TARGET_HAS_SSE3) \
    macro(SupplementalSSE3, JSC_BUILD_TARGET_HAS_SUPPLEMENTAL_SSE3) \
    macro(SSE4_1, JSC_BUILD_TARGET_HAS_SSE4_1) \
    macro(SSE4_2, JSC_BUILD_TARGET_HAS_SSE4_2) \
    macro(Popcnt, JSC_BUILD_TARGET_HAS_POPCNT) \
    macro(AVX, JSC_BUILD_TARGET_HAS_AVX) \
    macro(AVX2, JSC_BUILD_TARGET_HAS_AVX2) \
    macro(LZCNT, JSC_BUILD_TARGET_HAS_LZCNT) \
    macro(BMI1, JSC_BUILD_TARGET_HAS_BMI1)

#elif CPU(ARM64)

#if HAVE(LSE_INSTRUCTION)
#define JSC_BUILD_TARGET_HAS_LSE 1
#else
#define JSC_BUILD_TARGET_HAS_LSE 0
#endif

#if HAVE(FJCVTZS_INSTRUCTION)
#define JSC_BUILD_TARGET_HAS_JSCVT 1
#else
#define JSC_BUILD_TARGET_HAS_JSCVT 0
#endif

#if HAVE(FLOAT16_INSTRUCTION)
#define JSC_BUILD_TARGET_HAS_FLOAT16 1
#else
#define JSC_BUILD_TARGET_HAS_FLOAT16 0
#endif

// Every ARM64 CPU that runs macOS has FRINT and SHA3.
#if HAVE(FRINT_INSTRUCTION) || OS(MACOS)
#define JSC_BUILD_TARGET_HAS_FRINT 1
#else
#define JSC_BUILD_TARGET_HAS_FRINT 0
#endif

#if HAVE(SHA3_INSTRUCTION) || OS(MACOS)
#define JSC_BUILD_TARGET_HAS_SHA3 1
#else
#define JSC_BUILD_TARGET_HAS_SHA3 0
#endif

#if defined(__ARM_FEATURE_DOTPROD)
#define JSC_BUILD_TARGET_HAS_DOTPROD 1
#else
#define JSC_BUILD_TARGET_HAS_DOTPROD 0
#endif

#define FOR_EACH_CPU_FEATURE(macro) \
    macro(LSE, JSC_BUILD_TARGET_HAS_LSE) \
    macro(JSCVT, JSC_BUILD_TARGET_HAS_JSCVT) \
    macro(Float16, JSC_BUILD_TARGET_HAS_FLOAT16) \
    macro(FRINT, JSC_BUILD_TARGET_HAS_FRINT) \
    macro(SHA3, JSC_BUILD_TARGET_HAS_SHA3) \
    macro(DotProd, JSC_BUILD_TARGET_HAS_DOTPROD)

#else

#define FOR_EACH_CPU_FEATURE(macro)

#endif

namespace JSC {

enum class CPUFeature : uint8_t {
#define JSC_DECLARE_CPU_FEATURE(name, isInBuildTarget) name,
    FOR_EACH_CPU_FEATURE(JSC_DECLARE_CPU_FEATURE)
#undef JSC_DECLARE_CPU_FEATURE
};

#define JSC_COUNT_CPU_FEATURE(name, isInBuildTarget) + 1
static constexpr unsigned numberOfCPUFeatures = 0 FOR_EACH_CPU_FEATURE(JSC_COUNT_CPU_FEATURE);
#undef JSC_COUNT_CPU_FEATURE
static_assert(numberOfCPUFeatures <= 32);

class CPUFeatureSet {
public:
    constexpr CPUFeatureSet() = default;

    constexpr bool contains(CPUFeature feature) const { return m_bits & bit(feature); }
    constexpr CPUFeatureSet with(CPUFeature feature) const { return CPUFeatureSet(m_bits | bit(feature)); }
    constexpr CPUFeatureSet unionWith(CPUFeatureSet other) const { return CPUFeatureSet(m_bits | other.m_bits); }
    constexpr bool isSubsetOf(CPUFeatureSet other) const { return !(m_bits & ~other.m_bits); }
    constexpr uint32_t bits() const { return m_bits; }

    friend constexpr bool operator==(CPUFeatureSet, CPUFeatureSet) = default;

    static constexpr CPUFeatureSet fromBits(uint32_t bits) { return CPUFeatureSet(bits & allBits); }
    static constexpr CPUFeatureSet buildTarget();
    JS_EXPORT_PRIVATE static CPUFeatureSet host();

private:
    static constexpr uint32_t allBits = static_cast<uint32_t>((1ull << numberOfCPUFeatures) - 1);

    explicit constexpr CPUFeatureSet(uint32_t bits)
        : m_bits(bits)
    {
    }

    static constexpr uint32_t bit(CPUFeature feature) { return 1u << static_cast<unsigned>(feature); }

    uint32_t m_bits { 0 };
};

constexpr CPUFeatureSet CPUFeatureSet::buildTarget()
{
    CPUFeatureSet features;
#define JSC_ADD_BUILD_TARGET_CPU_FEATURE(name, isInBuildTarget) \
    if constexpr (isInBuildTarget) \
        features = features.with(CPUFeature::name);
    FOR_EACH_CPU_FEATURE(JSC_ADD_BUILD_TARGET_CPU_FEATURE)
#undef JSC_ADD_BUILD_TARGET_CPU_FEATURE
    return features;
}

} // namespace JSC
