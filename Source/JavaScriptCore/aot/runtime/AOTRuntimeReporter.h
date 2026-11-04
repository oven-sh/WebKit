/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "AOTWireEnums.h"
#include "CodeType.h"
#include "JSCJSValue.h"
#include "JSExportMacros.h"
#include <array>
#include <wtf/Forward.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class VM;

namespace AOT {

enum class Entry : uint16_t;
using RuntimeCounterID = Wire::RuntimeCounterID;

inline constexpr unsigned maxNumberOfReportedFrames = 3;

// A VM has one only under Options::aotCountsPath().
class RuntimeCounts {
    WTF_MAKE_TZONE_ALLOCATED(RuntimeCounts);
    WTF_MAKE_NONCOPYABLE(RuntimeCounts);
public:
    explicit RuntimeCounts(VM&);
    ~RuntimeCounts();

    void didCall(Entry entry) { ++m_calls[static_cast<unsigned>(entry)]; }
    void add(RuntimeCounterID counter, uint64_t times) { m_values[static_cast<unsigned>(counter)] += times; }
    // The field, by the number of its layout and its place in the record of the layout, has left its slot in one more object. `counter`: why. `property`: what it is from then on, empty behind a deletion.
    void didLeaveSlot(uint32_t layout, uint16_t field, RuntimeCounterID counter, JSValue property);
    // Modules::describeMockState() in front of a door between the host and the engine, the door, and the state behind it. A line for each, "<count> TAB named:mock:<transition>".
    void didPassMockDoor(const String& transition) { m_mockTransitions.add(transition, 0).iterator->value++; }
    uint64_t numberOfCalls(Entry entry) const { return m_calls[static_cast<unsigned>(entry)]; }
    uint64_t valueOf(RuntimeCounterID counter) const { return m_values[static_cast<unsigned>(counter)]; }

    // One line for each counter, "<count> TAB <kind>:<name>", and one for each operation whose calls are counted, "<count> TAB operation:<name>".
    // A name without a line is one that nothing counts. Then a line for each field that has left its slot and each kind of value that took it out, "<count> TAB field:L<layout>.<field>:<counter>:<kind>": which types the run time held in vain.
    // A stub repeats what the run time did to the last object in the same state and counts nothing: each field that left has its line, and its count is a lower bound.
    JS_EXPORT_PRIVATE void dump(PrintStream&) const;
    // Once, when the VM goes or the process ends. Every VM of the process writes to one file, and a reader adds up the lines of one name.
    void write();

private:
    VM& m_vm;
    bool m_hasWritten { false };
    std::array<uint64_t, numberOfEnumerators<RuntimeCounterID>> m_values { };
    // By layout, field and counter, then by the kind of the value: a JSType, above it the number of the layout of a compiled object plus one.
    using FieldThatLeftSlot = std::pair<uint64_t, uint64_t>;
    UncheckedKeyHashMap<FieldThatLeftSlot, uint64_t, DefaultHash<FieldThatLeftSlot>, PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> m_fieldsThatLeftSlots;
    UncheckedKeyHashMap<String, uint64_t> m_mockTransitions;
    Vector<uint64_t> m_calls;
};

JS_EXPORT_PRIVATE void countEvent(VM&, RuntimeCounterID, uint64_t times = 1);
void countFieldThatLeftSlot(VM&, uint32_t layout, uint16_t field, RuntimeCounterID, JSValue property = { });
// In a run that counts, the run-time table holds a function that counts in front of the operation.
JS_EXPORT_PRIVATE bool countsCallsOf(Entry);

constexpr RuntimeCounterID counterFor(CodeType codeType)
{
    using enum RuntimeCounterID;
    return std::array { InterpretedGlobalCode, InterpretedEvalCode, InterpretedFunctionCode, InterpretedModuleCode }[codeType];
}
static_assert(counterFor(GlobalCode) == RuntimeCounterID::InterpretedGlobalCode && counterFor(EvalCode) == RuntimeCounterID::InterpretedEvalCode);
static_assert(counterFor(FunctionCode) == RuntimeCounterID::InterpretedFunctionCode && counterFor(ModuleCode) == RuntimeCounterID::InterpretedModuleCode);

void writeToStandardError(const String& line);
// The program's current position, on its own thread: the positions of the innermost compiled frames.
Vector<String, maxNumberOfReportedFrames> placesOfInnermostCompiledFrames(VM&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
