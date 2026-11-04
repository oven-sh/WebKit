/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeReporter.h"

#if ENABLE(AOT)

#include "AOTCompiledFrame.h"
#include "AOTImage.h"
#include "AOTModuleTable.h"
#include "AOTObject.h"
#include "AOTRuntimeTable.h"
#include "AOTSort.h"
#include "AOTVMState.h"
#include "JSCInlines.h"
#include "LineColumn.h"
#include "Options.h"
#include "StackVisitor.h"
#include <cstdlib>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <wtf/FilePrintStream.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ProcessID.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

#if OS(WINDOWS)
#include <windows.h>
#else
#include <errno.h>
#include <unistd.h>
#endif

#if OS(WINDOWS)
extern "C" __attribute__((weak)) void Bun__atexit(void (*)());
#endif

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT {

void writeToStandardError(const String& line)
{
    ASSERT(line.endsWith('\n'));
    auto bytes = line.utf8();
    for (char8_t& byte : bytes.mutableSpan().first(bytes.length() - 1)) {
        if (byte < ' ')
            byte = ' ';
    }
#if OS(WINDOWS)
    DWORD numberOfBytesWritten;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), bytes.data(), static_cast<DWORD>(bytes.length()), &numberOfBytesWritten, nullptr);
#else
    std::span<const char8_t> rest = bytes.span();
    while (!rest.empty()) {
        ssize_t numberOfBytesWritten = ::write(STDERR_FILENO, rest.data(), rest.size());
        if (numberOfBytesWritten > 0)
            rest = rest.subspan(static_cast<size_t>(numberOfBytesWritten));
        else if (!numberOfBytesWritten || errno != EINTR)
            return;
    }
#endif
}

static std::optional<std::pair<String, LineColumn>> shownPositionOf(const Image& image, const CompiledFrame& frame)
{
    if (image.isObfuscated())
        return { { emptyString(), frame.lineColumn() } };
    auto position = image.positionOf(frame.code.index(), frame.callSite);
    if (!position)
        return std::nullopt;
    String path = position->source ? ModuleTable(image.programTables()).pathTextForLineColumns(position->source) : String();
    if (path.isEmpty())
        path = "unknown"_s;
    return { { WTF::move(path), position->lineColumn } };
}

Vector<String, maxNumberOfReportedFrames> placesOfInnermostCompiledFrames(VM& vm)
{
    Vector<String, maxNumberOfReportedFrames> places;
    const Image* image = vm.aotState().image;
    if (!vm.topCallFrame || !image)
        return places;
    StackVisitor::visit(vm.topCallFrame, vm, [&](StackVisitor& visitor) {
        if (!visitor->isCompiledFrame())
            return IterationStatus::Continue;
        if (auto position = shownPositionOf(*image, visitor->compiledFrame()))
            places.append(makeString(position->first, ':', position->second.line, ':', position->second.column));
        return places.size() < maxNumberOfReportedFrames ? IterationStatus::Continue : IterationStatus::Done;
    });
    return places;
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(RuntimeCounts);

static Lock s_countsLock;
static Vector<RuntimeCounts*>& countsOfProcess() WTF_REQUIRES_LOCK(s_countsLock)
{
    static NeverDestroyed<Vector<RuntimeCounts*>> counts;
    return counts.get();
}

static void callHoweverProcessEnds(void (*function)())
{
#if OS(WINDOWS)
    if (Bun__atexit) {
        Bun__atexit(function);
        return;
    }
#endif
    atexit(function);
#if !OS(DARWIN) && !OS(WINDOWS)
    at_quick_exit(function);
#endif
}

RuntimeCounts::RuntimeCounts(VM& vm)
    : m_vm(vm)
    , m_calls(FillWith { }, numberOfEntries, 0)
{
    {
        Locker locker { s_countsLock };
        countsOfProcess().append(this);
    }
    static std::once_flag once;
    std::call_once(once, [] {
        callHoweverProcessEnds([] {
            Locker locker { s_countsLock };
            for (RuntimeCounts* counts : std::views::reverse(countsOfProcess()))
                counts->write();
        });
    });
}

RuntimeCounts::~RuntimeCounts()
{
    Locker locker { s_countsLock };
    write();
    countsOfProcess().removeFirst(this);
}

static constexpr ASCIILiteral kindOf(RuntimeCounterID counter)
{
    switch (counter) {
    case RuntimeCounterID::StackListingsAfterGivenUpCaptures:
        return "made:"_s;
    case RuntimeCounterID::ModulesForRequireCache:
    case RuntimeCounterID::ModulesForWhatHasRun:
    case RuntimeCounterID::LinksBelowRoots:
    case RuntimeCounterID::StacksOfThrows:
    case RuntimeCounterID::RunsBelowListedFrames:
        return "sought:"_s;
    default:
        return "counter:"_s;
    }
}

static constexpr ASCIILiteral namesOfKindsOfImmediates[] { "nothing"_s, "undefined"_s, "null"_s, "boolean"_s, "int32"_s, "double"_s };
static constexpr uint64_t numberOfKindsOfImmediates = std::size(namesOfKindsOfImmediates);

void RuntimeCounts::dump(PrintStream& out) const
{
    for (RuntimeCounterID counter : allMembersOf<RuntimeCounterID>) {
        if (counter != RuntimeCounterID::None)
            out.println(valueOf(counter), "\t", kindOf(counter), Wire::nameOf(counter));
    }
    out.println(m_vm.hasBytecodeIntrinsicRegistry() ? 1 : 0, "\tmade:BytecodeIntrinsicRegistry");

    Vector<Entry> countedOperations;
    for (unsigned index = 0; index < numberOfEntries; ++index) {
        Entry entry = static_cast<Entry>(index);
        if (countsCallsOf(entry))
            countedOperations.append(entry);
    }
    AOT::sort(countedOperations, [](Entry a, Entry b) {
        return codePointCompareLessThan(nameOf(a), nameOf(b));
    });
    for (Entry entry : countedOperations)
        out.println(numberOfCalls(entry), "\toperation:", nameOf(entry));

    Vector<String> mockTransitions = copyToVector(m_mockTransitions.keys());
    AOT::sort(mockTransitions, [](const String& a, const String& b) {
        return codePointCompareLessThan(a, b);
    });
    for (const String& transition : mockTransitions)
        out.println(m_mockTransitions.get(transition), "\tnamed:mock:", transition);

    Vector<FieldThatLeftSlot> fields = copyToVector(m_fieldsThatLeftSlots.keys());
    AOT::sort(fields);
    for (FieldThatLeftSlot key : fields) {
        auto [field, kind] = key;
        out.print(m_fieldsThatLeftSlots.get(key), "\tfield:L", field >> 32, ".", field >> 16 & UINT16_MAX, ":", Wire::nameOf(static_cast<RuntimeCounterID>(field & UINT16_MAX)), ":");
        if (kind >> 32)
            out.println("L", (kind >> 32) - 1);
        else if (kind < numberOfKindsOfImmediates)
            out.println(namesOfKindsOfImmediates[kind]);
        else
            out.println(static_cast<JSType>(kind - numberOfKindsOfImmediates));
    }
}

void RuntimeCounts::didLeaveSlot(uint32_t layout, uint16_t field, RuntimeCounterID counter, JSValue property)
{
    uint64_t kind = 0;
    if (Object* object = compiledObjectOrNull(property))
        kind = (static_cast<uint64_t>(object->layout().index) + 1) << 32;
    else if (property && property.isCell())
        kind = numberOfKindsOfImmediates + static_cast<uint64_t>(property.asCell()->type());
    else if (property)
        kind = property.isUndefined() ? 1 : property.isNull() ? 2 : property.isBoolean() ? 3 : property.isInt32() ? 4 : 5;
    m_fieldsThatLeftSlots.add({ static_cast<uint64_t>(layout) << 32 | static_cast<uint64_t>(field) << 16 | static_cast<uint64_t>(counter), kind }, 0).iterator->value++;
}

void RuntimeCounts::write()
{
    ASSERT(s_countsLock.isHeld());
    if (std::exchange(m_hasWritten, true))
        return;
    String path = makeStringByReplacingAll(String::fromUTF8(byteCast<char>(Options::aotCountsPath())), "%p"_s, String::number(getCurrentProcessID()));
    static bool hasWrittenInThisProcess;
    if (auto file = FilePrintStream::open(path.utf8(), std::exchange(hasWrittenInThisProcess, true) ? "ab"_s : "wb"_s))
        dump(*file);
}

void countEvent(VM& vm, RuntimeCounterID counter, uint64_t times)
{
    if (RuntimeCounts* counts = vm.aotState().counts.get()) [[unlikely]]
        counts->add(counter, times);
}

void countFieldThatLeftSlot(VM& vm, uint32_t layout, uint16_t field, RuntimeCounterID counter, JSValue property)
{
    if (RuntimeCounts* counts = vm.aotState().counts.get()) [[unlikely]] {
        counts->add(counter, 1);
        counts->didLeaveSlot(layout, field, counter, property);
    }
}

} } // namespace JSC::AOT

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
