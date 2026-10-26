/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTNaturalLoops.h"
#include "AOTPerFunction.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <limits>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class BodyReferences;
class BodyWeights;
class CallGraph;
class EffectSummaries;
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
inline constexpr unsigned callersOfFunctionThatRunsOften = 4;
inline constexpr double activationsOfFunctionThatRunsOften = 100;
inline constexpr uint8_t percentileOfFunctionThatRunsOften = 90;
inline constexpr uint8_t percentileOfOtherFunctions = 10;
AOT_THRESHOLD(double, minWeightOfCallToFollow, 0.01);
AOT_THRESHOLD(double, minWeightOfCallNearBusyCode, 0.25);
// Behind two branches in a loop a call weighs 2.25 or more. Two calls in a row weigh 2.
AOT_THRESHOLD(double, minWeightOfCallInLoop, 2.2);
AOT_THRESHOLD(double, minWeightOfNearlyCertainCall, 0.5);
AOT_THRESHOLD(unsigned, maxCallersOfCompactFunction, 1);
AOT_THRESHOLD(unsigned, minMassPerValueWithHeavyLoop, 3);
AOT_THRESHOLD(uint32_t, maxSizeOfSmallFunction, 32);
// About what the call and the return of a function cost.
AOT_THRESHOLD(uint32_t, maxSizeOfHelper, 16);

} // namespace HeatModel

struct HeatThresholds {
    double cold { 0 };
    double hot { std::numeric_limits<double>::infinity() };
    // The heat of the coldest code that still fits MaxHotCodeValues beside all that is hotter.
    double coldestOfHottestCode { 0 };
    // What cold is without MaxWarmCodeValues.
    double coldWithoutLimit { std::numeric_limits<double>::infinity() };
    // Code in a loop of its function, or in a function that a loop calls, that is expected to run less often has no long form in line, however little there is above it.
    double leastHeatOfFastCodeInLoops { HeatModel::minWeightOfCallInLoop };
    // Whether a call in a loop within a loop of a warm caller is worth what a hot call is worth.
    bool callsInLoopsAreHot { true };
};

// Preparation: for the passes in front of the Optimize schedule, which ask for the classes of functions and of their blocks.
enum class HeatFor : uint8_t { Preparation, Everything };

class Heat {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Heat);
public:
    Heat() = default;

    double activationsOf(const Program&, FunctionID) const;
    // For weighing places of the whole program against each other. The classes of a very large program do not come from it.
    double estimatedActivationsOf(const Program&, FunctionID) const;
    // The activations of the functions that allocate the layout, once for each place that does.
    double allocationsOf(LayoutID) const;
    // Whether a function of the module allocates the layout. The literals of many modules may share one layout, which names only the first of them as its module.
    bool isAllocatedIn(LayoutID, ModuleID) const;
    // Whether some store, definition or literal of the program puts a value of the layout under the name. Only values whose type lists a few layouts count.
    bool isStoredUnder(LayoutID, PropertyKeyID) const;
    HeatClass of(const Program&, FunctionID) const;
    // The third class of function, beside hot and cold: a cold function of which nothing in the structure of the program says that it may run.
    bool isCompact(const Program& program, FunctionID id) const { return !m_classOfEverything && m_entries.of(program, id).isCompact; }
    // There the classes are read off the structure of the program.
    bool isOfVeryLargeProgram() const { return m_valuesInAllFunctions; }
    // False where HeatFor::Preparation has left something out.
    bool hasWeights() const { return m_hasWeights; }
    // Whether some block of the function is neither cold nor runs once.
    bool hasWarmCode(const Program&, FunctionID) const;
    uint8_t percentileOf(const Program&, FunctionID) const;
    HeatClass classOf(double heat) const;
    HeatClass classOf(const Program& program, FunctionID id, double frequencyInFunction) const { return classOf(program, id, activationsOf(program, id), frequencyInFunction); }
    HeatClass classOf(const Program&, FunctionID, double activations, double frequencyInFunction) const;
    // Whether code of this heat fits MaxHotCodeValues beside all that is hotter. In a program smaller than that, all code does.
    bool isAmongHottestCode(double heat) const { return !m_thresholds || heat >= m_thresholds->coldestOfHottestCode; }
    // For a block that runs so many times for each time that its function is entered.
    double leastHeatOfFastCode(const Program&, FunctionID, double frequencyInFunction) const;
    bool callsInLoopsAreHot() const { return m_thresholds && m_thresholds->callsInLoopsAreHot; }
    // Whether code of this heat is cold only because it does not fit MaxWarmCodeValues. Such code is expected to run.
    bool isColdByLimitAlone(double heat) const { return m_thresholds && heat >= m_thresholds->coldWithoutLimit && heat < m_thresholds->cold; }
    std::span<const FunctionID> hottestFirst() const LIFETIME_BOUND { return m_hottestFirst.span(); }

    void dump(PrintStream&, const Program&) const;

private:
    friend Heat estimateHeat(const Program&, const CallGraph&, const FlowResults&, BuildContext&, const EffectSummaries*, const BodyReferences*, const BodyWeights*, BodyWeights*, HeatFor);

    struct Entry {
        double activations { 1 };
        HeatClass heatClass { HeatClass::Unknown };
        uint8_t percentile { 50 };
        double estimatedActivations { 1 };
        bool hasWarmCode { true };
        bool isCompact { false };
        bool isCalledInLoop { false };
        bool isCalledPerEvent { false };
        double rank { 0 };
    };

    PerFunction<Entry> m_entries;
    bool m_hasWeights { true };
    bool m_hotIsByRank { false };
    std::optional<HeatThresholds> m_thresholds;
    std::optional<HeatClass> m_classOfEverything;
    uint64_t m_valuesInCompactFunctions { 0 };
    uint64_t m_valuesInAllFunctions { 0 };
    Vector<FunctionID> m_hottestFirst;
    Vector<double> m_allocationsOfLayouts;
    // Ascending: the layout in the upper half, the module in the lower.
    Vector<uint64_t> m_modulesThatAllocate;
    // Ascending: the name in the upper half, the layout in the lower.
    Vector<uint64_t> m_layoutsStoredUnderNames;
};

// summariesFromFlow is the result of createSummariesFromFlow() for the current program.
Heat estimateHeat(const Program&, const CallGraph&, const FlowResults&, BuildContext&, const EffectSummaries* summariesFromFlow = nullptr, const BodyReferences* = nullptr, const BodyWeights* kept = nullptr, BodyWeights* made = nullptr, HeatFor = HeatFor::Everything);

HeatClass heatClassOf(const Heat&, FunctionAnalyses&, BlockID);

// By how often the code of the function is taken to run for its size.
bool isInColderPartOfProgram(const Heat&, const Program&, FunctionID);
HeatClass heatClassOf(FunctionPassContext&, BlockID);
bool isHotLoop(FunctionPassContext&, const NaturalLoop&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
