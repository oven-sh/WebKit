/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeCounters.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "VM.h"
#include <algorithm>
#include <span>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

enum class CounterKind : uint8_t { Counter, StubExit };

static CounterKind kindOf(RuntimeCounterID counter)
{
    return countsStubExit(counter) ? CounterKind::StubExit : CounterKind::Counter;
}

template<typename Function>
static void forEachInstance(VM& vm, const Function& function)
{
    VMState& state = vm.aotState();
    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances)
        function(*instance);
    for (Instance* instance : state.instancesToDestroy)
        function(*instance);
}

void RuntimeCounters::add(const CounterWords& words)
{
    std::span source { words.values };
    for (size_t index = 0; index < values.size(); ++index)
        values[index] += source[index];
}

void count(VM& vm, RuntimeCounterID counter)
{
    if (RuntimeCounters* counters = vm.aotState().counters.get())
        ++counters->values[static_cast<size_t>(counter)];
}

void count(Instance& instance, RuntimeCounterID counter)
{
    ++std::span { instance.runtime.counters.values }[static_cast<size_t>(counter)];
}

void countLimitHit(VM& vm, LimitID limit)
{
    if (RuntimeCounters* counters = vm.aotState().counters.get())
        ++counters->limitHits[static_cast<size_t>(limit)];
}

uint64_t valueOf(VM& vm, RuntimeCounterID counter)
{
    size_t index = static_cast<size_t>(counter);
    RuntimeCounters* counters = vm.aotState().counters.get();
    uint64_t value = counters ? counters->values[index] : 0;
    forEachInstance(vm, [&](const Instance& instance) {
        value += std::span { instance.runtime.counters.values }[index];
    });
    return value;
}

uint64_t limitHitsOf(VM& vm, LimitID limit)
{
    RuntimeCounters* counters = vm.aotState().counters.get();
    return counters ? counters->limitHits[static_cast<size_t>(limit)] : 0;
}

uint64_t runtimeLimitValue(VM& vm, LimitID limit)
{
    const Image* image = vm.aotState().image;
    if (image && image->header().options.loweredLimit == limit)
        return image->header().options.loweredLimitValue;
    return defaultLimitValue(limit, hostArchitecture());
}

static void dumpCounters(VM& vm, PrintStream& out, CounterKind kind, ASCIILiteral prefix)
{
    for (RuntimeCounterID counter : allMembersOf<RuntimeCounterID>) {
        if (counter != RuntimeCounterID::None && kindOf(counter) == kind)
            out.println(valueOf(vm, counter), "\t", prefix, Wire::nameOf(counter));
    }
}

static void dumpLimitHits(VM& vm, PrintStream& out)
{
    // The run time also meets limits of the build, such as the number of key orders of a layout.
    for (LimitID limit : allMembersOf<LimitID>) {
        uint64_t hits = limitHitsOf(vm, limit);
        if (stageOf(limit) == Stage::RunTime || hits)
            out.println(hits, "\tlimit:", Wire::nameOf(limit));
    }
}

static void dumpOperationCalls(VM& vm, PrintStream& out)
{
    const RuntimeTable* table = vm.aotState().runtimeTable.get();
    if (!table)
        return;
    Vector<Entry> calledOperations;
    for (unsigned index = 0; index < numberOfEntries; ++index) {
        Entry entry = static_cast<Entry>(index);
        if (isOperation(entry) && table->callCount(entry))
            calledOperations.append(entry);
    }
    std::sort(calledOperations.begin(), calledOperations.end(), [](Entry a, Entry b) {
        return codePointCompareLessThan(nameOf(a), nameOf(b));
    });
    for (Entry entry : calledOperations)
        out.println(table->callCount(entry), "\toperation:", nameOf(entry));
}

static void dumpBranchCounts(VM& vm, const Instance& instance, PrintStream& out)
{
    if (!instance.runtime.counters.branchCounts)
        return;
    auto branchCounts = unsafeMakeSpan(instance.runtime.counters.branchCounts, static_cast<size_t>(runtimeLimitValue(vm, LimitID::BranchCounters)));
    for (size_t index = 0; index < branchCounts.size(); ++index) {
        if (uint64_t branchCount = branchCounts[index])
            out.println(branchCount, "\tbranch:", index);
    }
}

void dumpCounts(VM& vm, PrintStream& out)
{
    dumpCounters(vm, out, CounterKind::Counter, "counter:"_s);
    dumpCounters(vm, out, CounterKind::StubExit, "exit:"_s);
    dumpLimitHits(vm, out);
    dumpOperationCalls(vm, out);
    forEachInstance(vm, [&](const Instance& instance) {
        dumpBranchCounts(vm, instance, out);
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
