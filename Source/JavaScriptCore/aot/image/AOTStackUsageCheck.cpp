/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTConvention.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTVector.h"
#include "CallFrame.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

using Typed::StronglyConnectedComponents;

static_assert(defaultLimitValue(LimitID::UncheckedStackBytes, hostArchitecture()) == maxStackUsedWithoutCheck);

namespace {

struct UncheckedCall {
    unsigned index() const { return callee; }

    uint32_t callee;
    uint32_t stackUsedAtEntry;
};

using UncheckedCalls = Vector<Vector<UncheckedCall, 0>>;

} // anonymous namespace

static uint64_t frameUsageOf(const FrameDescription& frame)
{
    return static_cast<uint64_t>(frame.stackUsedAtCalls) + (frame.hasFrame() ? frame.frameSizeInBytes + sizeof(CallerFrameAndPC) : 0);
}

static Vector<uint32_t> computeOwnCodeEnds(const LinkedFunctions& functions, std::span<const EntryAlias> aliases, const CodeIndex& index)
{
    Vector<uint32_t> ownCodeEnds(functions.size(), [&](size_t position) {
        return static_cast<uint32_t>(functions[position].body.bytes.size());
    });
    for (const EntryAlias& alias : aliases) {
        if (alias.placement != AliasPlacement::AfterBody)
            continue;
        if (auto position = index.indexOf(alias.body))
            ownCodeEnds[*position] = std::min(ownCodeEnds[*position], alias.start);
    }
    return ownCodeEnds;
}

static UncheckedCalls collectUncheckedCalls(const LinkedFunctions& functions, const ResolvedCallees& callees, std::span<const uint32_t> ownCodeEnds, BuildContext& context)
{
    UncheckedCalls calls(functions.size());
    forEachIndexInParallel(context.parallelLoops(), static_cast<unsigned>(functions.size()), [&](unsigned position) {
        for (const Relocation& relocation : functions[position].body.relocations) {
            if (relocation.offset >= ownCodeEnds[position] || relocation.space != SymbolSpace::Function || !transfersControl(relocation.kind))
                continue;
            OptionalIndex callee = callees.at(position, relocation.symbol).body;
            if (!callee)
                continue;
            const FrameDescription& frame = functions[*callee].frame;
            if (!frame.isEnteredWithoutStackCheck)
                continue;
            uint32_t stackUsedAtEntry = !frame.hasFrame() && relocation.kind == RelocationKind::Call ? static_cast<uint32_t>(sizeof(void*)) : 0;
            calls[position].append(UncheckedCall { *callee, stackUsedAtEntry });
        }
    });
    return calls;
}

static BitVector findCoveredBodies(const LinkedFunctions& functions, const UncheckedCalls& calls)
{
    BitVector isCovered(functions.size());
    Vector<uint32_t> worklist;
    for (uint32_t position = 0; position < functions.size(); ++position) {
        if (!functions[position].frame.isProvenToFitOnStack)
            continue;
        isCovered.quickSet(position);
        worklist.append(position);
    }
    while (!worklist.isEmpty()) {
        for (const UncheckedCall& call : calls[worklist.takeLast()]) {
            if (!isCovered.quickSet(call.callee))
                worklist.append(call.callee);
        }
    }
    return isCovered;
}

static std::optional<uint32_t> findCycleUsingStack(const LinkedFunctions& functions, const UncheckedCalls& calls, const StronglyConnectedComponents& components)
{
    BitVector isCovered = findCoveredBodies(functions, calls);
    BitVector usesStack(components.numberOfComponents);
    for (uint32_t position = 0; position < functions.size(); ++position) {
        if (!components.isInCycle.get(position) || !isCovered.get(position))
            continue;
        uint32_t component = components.componentOf[position];
        bool usesStackInCycle = frameUsageOf(functions[position].frame) || calls[position].containsIf([&](const UncheckedCall& call) {
            return call.stackUsedAtEntry && components.componentOf[call.callee] == component;
        });
        if (usesStackInCycle)
            usesStack.quickSet(component);
    }
    for (uint32_t position = 0; position < functions.size(); ++position) {
        if (components.isInCycle.get(position) && usesStack.quickGet(components.componentOf[position]))
            return position;
    }
    return std::nullopt;
}

static Vector<uint32_t> sortByComponent(const StronglyConnectedComponents& components)
{
    Vector<uint32_t> nextIndex(FillWith { }, components.numberOfComponents + 1, 0);
    for (uint32_t component : components.componentOf)
        ++nextIndex[component + 1];
    for (size_t component = 1; component < nextIndex.size(); ++component)
        nextIndex[component] += nextIndex[component - 1];
    Vector<uint32_t> positions(FillWith { }, components.componentOf.size(), 0);
    for (uint32_t position = 0; position < positions.size(); ++position)
        positions[nextIndex[components.componentOf[position]]++] = position;
    return positions;
}

static Vector<uint64_t> computeUsageByComponent(const LinkedFunctions& functions, const UncheckedCalls& calls, const StronglyConnectedComponents& components)
{
    Vector<uint64_t> usage(FillWith { }, components.numberOfComponents, 0);
    for (uint32_t position : sortByComponent(components)) {
        uint32_t component = components.componentOf[position];
        uint64_t calleeUsage = 0;
        for (const UncheckedCall& call : calls[position]) {
            uint32_t calleeComponent = components.componentOf[call.callee];
            if (calleeComponent != component)
                calleeUsage = std::max(calleeUsage, call.stackUsedAtEntry + usage[calleeComponent]);
        }
        usage[component] = std::max(usage[component], frameUsageOf(functions[position].frame) + calleeUsage);
    }
    return usage;
}

void checkUncheckedStackUsage(const LinkedFunctions& functions, const ResolvedCallees& callees, std::span<const EntryAlias> aliases, const CodeIndex& index, BuildContext& context)
{
    Vector<uint32_t> ownCodeEnds = computeOwnCodeEnds(functions, aliases, index);
    UncheckedCalls calls = collectUncheckedCalls(functions, callees, ownCodeEnds.span(), context);
    StronglyConnectedComponents components = Typed::computeStronglyConnectedComponents(static_cast<unsigned>(functions.size()), [&](unsigned position) -> const Vector<UncheckedCall, 0>& {
        return calls[position];
    });

    if (auto position = findCycleUsingStack(functions, calls, components); position && !context.recordedOptions().has(TestingSwitch::CheckProofsAtRunTime)) {
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::UncheckedStackCycle, .function = functions[*position].function });
        return;
    }

    Vector<uint64_t> usage = computeUsageByComponent(functions, calls, components);
    for (uint32_t position = 0; position < functions.size(); ++position) {
        if (!functions[position].frame.isProvenToFitOnStack)
            continue;
        uint64_t needed = usage[components.componentOf[position]];
        if (needed > context.limit(LimitID::UncheckedStackBytes)) {
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::UncheckedStackTooDeep, .function = functions[position].function, .limit = LimitID::UncheckedStackBytes, .count = clampTo<uint32_t>(needed) });
            return;
        }
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
