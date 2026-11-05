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
#include "CallFrame.h"
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

using Typed::StronglyConnectedComponents;

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

static UncheckedCalls collectUncheckedCalls(const LinkedFunctions& functions, const ResolvedCallees& callees, BuildContext& context)
{
    UncheckedCalls calls(functions.size());
    forEachIndexInParallel(context.parallelLoops(), static_cast<unsigned>(functions.size()), [&](unsigned position) {
        for (const Relocation& relocation : functions[position].body.relocations) {
            if (relocation.space != SymbolSpace::Function || !transfersControl(relocation.kind))
                continue;
            OptionalIndex callee = callees.at(position, relocation.symbol);
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

static std::optional<uint32_t> findCycleUsingStack(const LinkedFunctions& functions, const UncheckedCalls& calls, const StronglyConnectedComponents& components)
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

static Vector<uint64_t> computeUsageByComponent(const LinkedFunctions& functions, const UncheckedCalls& calls, const StronglyConnectedComponents& components)
{
    Vector<uint64_t> usage;
    usage.fill(0, components.numberOfComponents);
    for (uint32_t position : components.members) {
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

static void reportUncheckedStackUsage(InternalErrorKind kind, const CompiledCode& code, uint64_t bytes, BuildContext& context)
{
    context.diagnostics().add(InternalError {
        .kind = kind,
        .function = code.unit.function,
        .count = clampTo<uint32_t>(bytes),
        .detail = makeString("found at the "_s, nameOf(code.unit.role), " of function f"_s, code.unit.function.index()),
    });
}

void checkUncheckedStackUsage(const LinkedFunctions& functions, const ResolvedCallees& callees, BuildContext& context)
{
    UncheckedCalls calls = collectUncheckedCalls(functions, callees, context);
    StronglyConnectedComponents components = Typed::computeStronglyConnectedComponents(static_cast<unsigned>(functions.size()), [&](unsigned position) -> const Vector<UncheckedCall, 0>& {
        return calls[position];
    });

    if (auto position = findCycleUsingStack(functions, calls, components)) {
        reportUncheckedStackUsage(InternalErrorKind::UncheckedStackCycle, functions[*position], 0, context);
        return;
    }

    Vector<uint64_t> usage = computeUsageByComponent(functions, calls, components);
    for (uint32_t position = 0; position < functions.size(); ++position) {
        if (!functions[position].frame.isProvenToFitOnStack)
            continue;
        uint64_t needed = usage[components.componentOf[position]];
        if (needed > maxStackUsedWithoutCheck) {
            reportUncheckedStackUsage(InternalErrorKind::UncheckedStackTooDeep, functions[position], needed, context);
            return;
        }
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
