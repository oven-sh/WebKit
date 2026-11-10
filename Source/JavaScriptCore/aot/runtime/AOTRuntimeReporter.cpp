/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeReporter.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCompiledFrame.h"
#include "AOTEffectsNet.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTModuleTable.h"
#include "AOTObject.h"
#include "AOTRuntimeTable.h"
#include "AOTSort.h"
#include "AOTStubDescriptor.h"
#include "AOTVMState.h"
#include "JSCInlines.h"
#include "LineColumn.h"
#include "Options.h"
#include "StackVisitor.h"
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/FilePrintStream.h>
#include <wtf/HexNumber.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ProcessID.h>
#include <wtf/SetForScope.h>
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

WTF_MAKE_TZONE_ALLOCATED_IMPL(RuntimeCounts);

void printRowOfEffectsNet(PrintStream& out, unsigned row)
{
    if (row == Typed::noRowInEffectsNet)
        out.print("the callee's effect summary");
    else if (row >= Typed::firstBuiltinRowInEffectsNet)
        out.print("built-in:", BuiltinTable::singleton().textOf(BuiltinID(row - Typed::firstBuiltinRowInEffectsNet)));
    else if (row >= Typed::firstStubRowInEffectsNet)
        out.print("stub:", descriptorOf(static_cast<StubFamily>(row - Typed::firstStubRowInEffectsNet)).name);
    else if (row >= Typed::firstRuntimeOperationRowInEffectsNet)
        out.print("runtime:", nameOf(static_cast<Entry>(row - Typed::firstRuntimeOperationRowInEffectsNet)));
    else
        out.print("operation:", Typed::nameOf(static_cast<Typed::Opcode>(row)));
}

static Lock s_falseRowsOfEffectsLock;
static Vector<String>& falseRowsOfEffects() WTF_REQUIRES_LOCK(s_falseRowsOfEffectsLock)
{
    static NeverDestroyed<Vector<String>> rows;
    return rows;
}

bool noteFalseRowOfEffects(const String& said)
{
    Locker locker { s_falseRowsOfEffectsLock };
    if (falseRowsOfEffects().contains(said))
        return false;
    falseRowsOfEffects().append(said.isolatedCopy());
    return true;
}

static constexpr size_t sizeOfEffectsNetExercised = Typed::numberOfRowsInEffectsNet * Typed::bytesOfRowInEffectsNet;
static std::atomic<uint8_t*> s_effectsNetExercised;

std::span<uint8_t> makeEffectsNetExercised()
{
    uint8_t* bytes = s_effectsNetExercised.load(std::memory_order_acquire);
    if (!bytes) {
        uint8_t* made = static_cast<uint8_t*>(fastZeroedMalloc(sizeOfEffectsNetExercised));
        if (s_effectsNetExercised.compare_exchange_strong(bytes, made, std::memory_order_acq_rel))
            bytes = made;
        else
            fastFree(made);
    }
    return { bytes, sizeOfEffectsNetExercised };
}

std::span<const uint8_t> effectsNetExercised()
{
    const uint8_t* bytes = s_effectsNetExercised.load(std::memory_order_acquire);
    return { bytes, bytes ? sizeOfEffectsNetExercised : 0 };
}

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
    case RuntimeCounterID::ReturnAddressTraceFallbacks:
        return "made:"_s;
    case RuntimeCounterID::ModulesScannedForRequireCache:
    case RuntimeCounterID::ModulesScannedForEvaluationStatus:
    case RuntimeCounterID::ModulesScannedForPendingCycles:
    case RuntimeCounterID::RecordsForStatesOfLoads:
    case RuntimeCounterID::ModuleLinkWalksCreated:
    case RuntimeCounterID::ThrowStackTracesCaptured:
    case RuntimeCounterID::StackWalksToCompiledFrames:
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

    std::span<const uint8_t> exercised = effectsNetExercised();
    static constexpr std::array<ASCIILiteral, Typed::bytesOfRowInEffectsNet> parts { "runs-no-other-code"_s, "throws-nothing"_s, "leaves-heaps-alone"_s, "does-not-collect"_s };
    for (unsigned byte = 0; byte < exercised.size(); ++byte) {
        if (!exercised[byte])
            continue;
        out.print("1	effects:", parts[byte % Typed::bytesOfRowInEffectsNet], ":");
        printRowOfEffectsNet(out, byte / Typed::bytesOfRowInEffectsNet);
        out.println();
    }
    {
        Locker locker { s_falseRowsOfEffectsLock };
        for (const String& said : falseRowsOfEffects())
            out.println("1\tfalse-effects:", said);
    }

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

#if !defined(BUN_HARDENED)
static String symbolNameOf(const Image& image, uint32_t function)
{
    if (image.isObfuscated())
        return makeString('f', function);
    FunctionDescription description(image.rootFunctionOf(function));
    String name = description.codeType() == ModuleCode ? String("(module)"_s) : writtenFunctionName(description.sourceFunction());
    if (name.isEmpty())
        name = "(anonymous)"_s;
    auto position = image.positionOf(function, CallSiteIndex(0));
    if (!position || !position->source)
        return name;
    return makeString(name, ' ', ModuleTable(image.programTables()).pathTextForLineColumns(position->source), ':', position->lineColumn.line);
}

void writePerfMap(const Image& image)
{
    static Lock lock;
    static bool hasWrittenInThisProcess;
    Locker locker { lock };
    auto file = FilePrintStream::open(makeString("/tmp/perf-"_s, getCurrentProcessID(), ".map"_s).utf8(), std::exchange(hasWrittenInThisProcess, true) ? "a"_s : "w"_s);
    if (!file)
        return;
    SetForScope runningImage(*Image::addressOfCurrentOfThread(), &image);
    const uint8_t* code = image.code().data();
    Vector<std::pair<size_t, String>> symbols;
    for (uint32_t function = 0; function < image.header().counts.functions; ++function) {
        if (image.sizeOfCodeOf(function))
            symbols.append({ static_cast<size_t>(image.codeOf(function) - code), symbolNameOf(image, function) });
    }
    for (unsigned copy = 0; copy < image.sharedCodeCopies().size(); ++copy)
        symbols.append({ static_cast<size_t>(image.sharedCodeCopy(copy) - code), String("(stubs of compiled code)"_s) });
    AOT::sort(symbols, { }, &std::pair<size_t, String>::first);
    for (size_t i = 0; i < symbols.size(); ++i) {
        size_t end = i + 1 < symbols.size() ? symbols[i + 1].first : image.header().code.functionsEnd;
        file->println(hex(reinterpret_cast<uintptr_t>(code) + symbols[i].first, Lowercase), " ", hex(end - symbols[i].first, Lowercase), " ", symbols[i].second);
    }
}
#endif

} } // namespace JSC::AOT

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
