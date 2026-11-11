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
#include <span>
#include <wtf/Forward.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class VM;

namespace AOT {

class Image;
enum class Entry : uint16_t;
using RuntimeCounterID = Wire::RuntimeCounterID;

inline constexpr unsigned maxNumberOfReportedFrames = 3;

class RuntimeCounts {
    WTF_MAKE_TZONE_ALLOCATED(RuntimeCounts);
    WTF_MAKE_NONCOPYABLE(RuntimeCounts);
public:
    explicit RuntimeCounts(VM&);
    ~RuntimeCounts();

    void didCall(Entry entry) { ++m_calls[static_cast<unsigned>(entry)]; }
    void add(RuntimeCounterID counter, uint64_t times) { m_values[static_cast<unsigned>(counter)] += times; }
    void didLeaveSlot(uint32_t layout, uint16_t field, RuntimeCounterID counter, JSValue property);
    void didPassMockDoor(const String& transition) { m_mockTransitions.add(transition, 0).iterator->value++; }
    uint64_t numberOfCalls(Entry entry) const { return m_calls[static_cast<unsigned>(entry)]; }
    uint64_t valueOf(RuntimeCounterID counter) const { return m_values[static_cast<unsigned>(counter)]; }

    JS_EXPORT_PRIVATE void dump(PrintStream&) const;
    void write();

private:
    VM& m_vm;
    bool m_hasWritten { false };
    std::array<uint64_t, numberOfEnumerators<RuntimeCounterID>> m_values { };
    using FieldThatLeftSlot = std::pair<uint64_t, uint64_t>;
    UncheckedKeyHashMap<FieldThatLeftSlot, uint64_t, DefaultHash<FieldThatLeftSlot>, PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> m_fieldsThatLeftSlots;
    UncheckedKeyHashMap<String, uint64_t> m_mockTransitions;
    Vector<uint64_t> m_calls;
};

std::span<uint8_t> makeEffectsNetExercised();
std::span<const uint8_t> effectsNetExercised();
bool noteFalseRowOfEffects(const String&);
void printRowOfEffectsNet(PrintStream&, unsigned row);

JS_EXPORT_PRIVATE void countEvent(VM&, RuntimeCounterID, uint64_t times = 1);
void countFieldThatLeftSlot(VM&, uint32_t layout, uint16_t field, RuntimeCounterID, JSValue property = { });
JS_EXPORT_PRIVATE bool countsCallsOf(Entry);

constexpr RuntimeCounterID counterFor(CodeType codeType)
{
    using enum RuntimeCounterID;
    return std::array { InterpretedGlobalCode, InterpretedEvalCode, InterpretedFunctionCode, InterpretedModuleCode }[codeType];
}
static_assert(counterFor(GlobalCode) == RuntimeCounterID::InterpretedGlobalCode && counterFor(EvalCode) == RuntimeCounterID::InterpretedEvalCode);
static_assert(counterFor(FunctionCode) == RuntimeCounterID::InterpretedFunctionCode && counterFor(ModuleCode) == RuntimeCounterID::InterpretedModuleCode);

#if !defined(BUN_HARDENED)
void writePerfMap(const Image&);
#endif
void writeToStandardError(const String& line);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
