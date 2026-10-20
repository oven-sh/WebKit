/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTPerFunction.h"
#include <limits>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallGraph;
class FlowResults;
class FunctionAnalyses;
struct FunctionPassContext;

enum class HeatClass : uint8_t {
    Unknown,
    RunsOnce,
    Cold,
    Warm,
    Hot,
};

constexpr bool isHot(HeatClass heatClass) { return heatClass == HeatClass::Hot; }
constexpr bool isCold(HeatClass heatClass) { return heatClass == HeatClass::Cold || heatClass == HeatClass::RunsOnce; }

ASCIILiteral nameOf(HeatClass);

namespace HeatModel {

inline constexpr uint8_t coldPercentile = 25;
inline constexpr uint8_t hotPercentile = 80;

} // namespace HeatModel

struct HeatThresholds {
    double cold { 0 };
    double hot { std::numeric_limits<double>::infinity() };
};

class Heat {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Heat);
public:
    Heat() = default;

    double activationsOf(const Program&, FunctionID) const;
    HeatClass of(const Program&, FunctionID) const;
    uint8_t percentileOf(const Program&, FunctionID) const;
    HeatClass classOf(double heat) const;
    std::span<const FunctionID> hottestFirst() const LIFETIME_BOUND { return m_hottestFirst.span(); }

    void dump(PrintStream&, const Program&) const;

private:
    friend Heat estimateHeat(const Program&, const CallGraph&, const FlowResults&, BuildContext&);

    struct Entry {
        double activations { 1 };
        HeatClass heatClass { HeatClass::Unknown };
        uint8_t percentile { 50 };
    };

    PerFunction<Entry> m_entries;
    std::optional<HeatThresholds> m_thresholds;
    std::optional<HeatClass> m_classOfEverything;
    Vector<FunctionID> m_hottestFirst;
};

Heat estimateHeat(const Program&, const CallGraph&, const FlowResults&, BuildContext&);

HeatClass heatClassOf(const Heat&, FunctionAnalyses&, BlockID);
HeatClass heatClassOf(FunctionPassContext&, BlockID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
