/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeReporter.h"

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTCompiledCallee.h"
#include "AOTCompiledFrame.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTHost.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleData.h"
#include "AOTModuleHost.h"
#include "AOTModuleTable.h"
#include "AOTPrimitives.h"
#include "AOTProgramStrings.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeType.h"
#include "AOTSchemaEnums.h"
#include "AOTSort.h"
#include "AOTStringTable.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "Options.h"
#include "StackVisitor.h"
#include <initializer_list>
#include <mutex>
#include <ranges>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/FilePrintStream.h>
#include <wtf/HexNumber.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ProcessID.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

#if ENABLE(WEBASSEMBLY)
#include "AOTWasmInterface.h"
#endif

#if OS(WINDOWS)
#include <windows.h>
extern "C" __attribute__((weak)) void Bun__atexit(void (*)());
#else
#include <errno.h>
#include <unistd.h>
#endif

namespace JSC { namespace AOT {

static Lock s_lock;
static ReportDestination s_destination;

static Vector<RuntimeReporter*>& reportersUnderLock()
{
    static NeverDestroyed<Vector<RuntimeReporter*>> reporters;
    return reporters;
}

static intptr_t standardError()
{
#if OS(WINDOWS)
    return reinterpret_cast<intptr_t>(GetStdHandle(STD_ERROR_HANDLE));
#else
    return STDERR_FILENO;
#endif
}

static ReportDestination currentDestination()
{
    Locker locker { s_lock };
    ReportDestination destination = s_destination;
    if (!destination.isExplicit)
        destination.descriptor = standardError();
    return destination;
}

void setMismatchLog(intptr_t descriptor, bool isJSON)
{
    Locker locker { s_lock };
    s_destination = { descriptor < 0 ? std::nullopt : std::optional(descriptor), isJSON, true };
}

void setMismatchLogFormat(bool isJSON)
{
    Locker locker { s_lock };
    s_destination.isJSON = isJSON;
}

static void writeLine(const ReportDestination& destination, const String& line)
{
    auto bytes = line.utf8();
    for (char8_t& byte : bytes.mutableSpan().first(bytes.length() - 1)) {
        if (byte < ' ')
            byte = ' ';
    }
#if OS(WINDOWS)
    DWORD numberOfBytesWritten;
    WriteFile(reinterpret_cast<HANDLE>(*destination.descriptor), bytes.data(), static_cast<DWORD>(bytes.length()), &numberOfBytesWritten, nullptr);
#else
    std::span<const char8_t> rest = bytes.span();
    while (!rest.empty()) {
        ssize_t numberOfBytesWritten = ::write(static_cast<int>(*destination.descriptor), rest.data(), rest.size());
        if (numberOfBytesWritten > 0)
            rest = rest.subspan(static_cast<size_t>(numberOfBytesWritten));
        else if (!numberOfBytesWritten || errno != EINTR)
            return;
    }
#endif
}

void writeToStandardError(const String& line)
{
    writeLine({ standardError() }, line);
}

static uint64_t computeCheckID(std::span<const uint8_t> path, std::initializer_list<uint32_t> position, TypeSiteKind kind)
{
    constexpr unsigned numberOfIDBits = 48;
    uint64_t hash = 0xcbf29ce484222325ULL;
    auto add = [&](uint8_t byte) {
        hash = (hash ^ byte) * 0x100000001b3ULL;
    };
    for (uint8_t byte : path)
        add(byte);
    for (uint32_t word : position) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            add(static_cast<uint8_t>(word >> shift));
    }
    add(static_cast<uint8_t>(kind));
    return hash & ((1ULL << numberOfIDBits) - 1);
}

uint64_t typeCheckIDOf(std::span<const uint8_t> path, uint32_t position, TypeSiteKind kind)
{
    return computeCheckID(path, { position }, kind);
}

static bool isPowerOfTen(uint64_t count)
{
    while (count && !(count % 10))
        count /= 10;
    return count == 1;
}

static String nonEmptyOr(const String& text, ASCIILiteral fallback)
{
    return text.isEmpty() ? String(fallback) : text;
}

static String describeKindOf(JSValue value, const RuntimeType& expected)
{
    if (!value)
        return "undefined"_s;
    constexpr auto numbers = OptionSet<PrimitiveSet>::fromRaw(Typed::Primitive::Number);
    if (value.isNumber() && (expected.primitiveSet() & numbers) == OptionSet<PrimitiveSet> { PrimitiveSet::Integer })
        return value.isInt32AsAnyInt() ? "number, out of range"_s : "number, not an int32"_s;
    bool isClass = value.isCell() && CompiledCallee::isCompiledCallee(value.asCell()) && CompiledCallee::kind(asObject(value)) == CompiledCallee::FunctionKind::Class;
    if (value.isCallable() && !isClass)
        return "function"_s;
    return describe(value);
}

static String shownPath(const Image& image, ModuleID module, String (ModuleTable::*textOf)(ModuleID) const)
{
    if (image.isObfuscated())
        return emptyString();
    return nonEmptyOr(module ? (ModuleTable(image.programTables()).*textOf)(module) : String(), "unknown"_s);
}

static std::optional<std::pair<String, LineColumn>> shownPositionOf(const Image& image, const CompiledFrame& frame)
{
    if (image.isObfuscated())
        return { { emptyString(), frame.lineColumn() } };
    auto position = image.positionOf(frame.code.index(), frame.callSite);
    if (!position)
        return std::nullopt;
    return { { shownPath(image, position->source, &ModuleTable::pathTextForLineColumns), position->lineColumn } };
}

Vector<String, RuntimeReporter::maxNumberOfCallers> placesOfInnermostCompiledFrames(VM& vm)
{
    Vector<String, RuntimeReporter::maxNumberOfCallers> places;
    const Image* image = vm.aotState().image;
    if (!vm.topCallFrame || !image)
        return places;
    StackVisitor::visit(vm.topCallFrame, vm, [&](StackVisitor& visitor) {
        if (!visitor->isCompiledFrame())
            return IterationStatus::Continue;
        if (auto position = shownPositionOf(*image, visitor->compiledFrame()))
            places.append(makeString(position->first, ':', position->second.line, ':', position->second.column));
        return places.size() < RuntimeReporter::maxNumberOfCallers ? IterationStatus::Continue : IterationStatus::Done;
    });
    return places;
}

static Vector<String, RuntimeReporter::maxNumberOfCallers> findCallers(VM& vm)
{
    Vector<String, RuntimeReporter::maxNumberOfCallers> callers;
    const Image* image = vm.aotState().image;
    if (!vm.topCallFrame || !image)
        return callers;
    bool isFirstFrame = true;
    StackVisitor::visit(vm.topCallFrame, vm, [&](StackVisitor& visitor) {
        if (std::exchange(isFirstFrame, false) || !visitor->isCompiledFrame())
            return IterationStatus::Continue;
        if (auto position = shownPositionOf(*image, visitor->compiledFrame()))
            callers.append(image->isObfuscated() ? makeString(position->second.line, ':', position->second.column) : makeString(position->first, ':', position->second.line));
        return callers.size() < RuntimeReporter::maxNumberOfCallers ? IterationStatus::Continue : IterationStatus::Done;
    });
    return callers;
}

RuntimeReporter::RuntimeReporter(VM& vm)
    : m_vm(vm)
    , m_destination(currentDestination())
{
    {
        Locker locker { s_lock };
        reportersUnderLock().append(this);
    }
    if (Options::aotCountsPath())
        installExitHook();
}

RuntimeReporter::~RuntimeReporter()
{
    Locker locker { s_lock };
    reportersUnderLock().removeFirst(this);
}

RuntimeReporter* RuntimeReporter::ifExplicitlyEnabled(VM& vm)
{
    RuntimeReporter* reporter = vm.aotState().reporter.get();
    return reporter && reporter->m_destination.descriptor && reporter->m_destination.isExplicit ? reporter : nullptr;
}

void RuntimeReporter::callAtExit(void (*function)())
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

void RuntimeReporter::installExitHook()
{
    static std::once_flag once;
    std::call_once(once, [] {
        callAtExit([] {
            Locker locker { s_lock };
            for (RuntimeReporter* reporter : std::views::reverse(reportersUnderLock()))
                reporter->writeAtExit();
        });
    });
}

RuntimeReporter::Site* RuntimeReporter::findOrAddSite(Instance& instance, TypeSiteID typeSite, const RuntimeType& expected, const CompiledFrame* frame)
{
    VM& vm = *instance.vm;
    const Image& image = *vm.aotState().image;
    const TypeSiteData* data = typeSite ? instance.tables().findTypeSite(typeSite.index()) : nullptr;
    if (typeSite && !data) {
        ASSERT_NOT_REACHED();
        return nullptr;
    }
    ModuleID module = data ? ModuleID(data->module) : ModuleID();
    TypeSiteKind kind = data ? static_cast<TypeSiteKind>(data->kind) : TypeSiteKind::Unknown;
    Site site { .kind = logWordOf(kind), .file = shownPath(image, module, &ModuleTable::displayPathText), .function = image.isObfuscated() ? emptyString() : "unknown"_str };
    if (auto position = frame ? shownPositionOf(image, *frame) : std::nullopt) {
        std::tie(site.file, site.position) = *position;
        FunctionDescription description(frame->function);
        if (!image.isObfuscated())
            site.function = description.codeType() == ModuleCode ? "module"_str : nonEmptyOr(description.name(vm), "anonymous"_s);
    }
    if (data) {
        String path = !module ? String() : image.isObfuscated() ? modulePath(module) : ModuleTable(image.programTables()).displayPathText(module);
        site.id = typeCheckIDOf(Typed::encodeProgramString(path).span(), data->position, kind);
    } else
        site.id = computeCheckID(Typed::encodeProgramString(site.file).span(), { site.position.line, site.position.column }, kind);

    for (Site& known : m_sites) {
        if (known.id == site.id)
            return &known;
    }
    if (m_sites.size() == maxNumberOfSites) {
        if (!std::exchange(m_hasOverflowed, true))
            writeLine(m_destination, m_destination.isJSON ? makeString("{\"aot\":\"type-mismatch\",\"kind\":\"overflow\",\"sites\":"_s, maxNumberOfSites, "}\n"_s) : makeString("[aot] type-mismatch log is full ("_s, maxNumberOfSites, " sites). New sites are not logged\n"_s));
        return nullptr;
    }
    site.file = site.file.isolatedCopy();
    site.function = site.function.isolatedCopy();
    site.expected = describe(instance.tables(), expected, typeSite).isolatedCopy();
    if (site.expected.length() > maxLengthOfType)
        site.expected = makeString(StringView(site.expected).left(maxLengthOfType - 3), "..."_s);
    m_sites.append(WTF::move(site));
    installExitHook();
    return &m_sites.last();
}

void RuntimeReporter::reportFailedCheck(Instance& instance, TypeSiteID typeSite, const RuntimeType& expected, JSValue actual)
{
    if (!m_destination.descriptor)
        return;
    Locker locker { m_lock };
    VM& vm = *instance.vm;
    auto frames = CompiledFrameIterator::tryCreateAtTop(vm);
    const CompiledFrame* frame = frames && !frames->isAtEnd() ? &frames->frame() : nullptr;
    Site* site = frame ? m_sitesByCallSite.ensure(static_cast<uint64_t>(frame->code.index()) << 32 | frame->callSite.bits(), [&] {
        return findOrAddSite(instance, typeSite, expected, frame);
    }).iterator->value : findOrAddSite(instance, typeSite, expected, nullptr);
    if (!site)
        return;

    auto bump = [&](const String& kind) -> uint64_t {
        for (auto& [known, count] : site->counts) {
            if (known == kind)
                return ++count;
        }
        if (site->counts.size() >= maxNumberOfKindsAtSite && kind != "other"_s)
            return 0;
        site->counts.append({ kind.isolatedCopy(), 1 });
        return 1;
    };
    String got = describeKindOf(actual, expected);
    uint64_t count = bump(got);
    if (!count) {
        got = "other"_s;
        count = bump(got);
    }
    if (isPowerOfTen(count))
        write(*site, got, count, findCallers(vm).span(), false);
}

void RuntimeReporter::write(const Site& site, const String& got, uint64_t count, std::span<const String> callers, bool isTotal)
{
    StringBuilder line;
    if (m_destination.isJSON) {
        auto member = [&](ASCIILiteral name, const String& text) {
            line.append(",\""_s, name, "\":"_s);
            line.appendQuotedJSONString(text);
        };
        line.append("{\"aot\":\"type-mismatch\",\"id\":\""_s, hex(site.id, 12, Lowercase), "\",\"kind\":\""_s, site.kind, '"');
        member("file"_s, site.file);
        line.append(",\"line\":"_s, site.position.line, ",\"col\":"_s, site.position.column);
        member("fn"_s, site.function);
        member("expected"_s, site.expected);
        member("got"_s, got);
        line.append(",\"action\":\"continued\",\"count\":"_s, count);
        for (auto& caller : callers) {
            line.append(&caller == callers.data() ? ",\"from\":["_s : ","_s);
            line.appendQuotedJSONString(caller);
        }
        line.append(callers.empty() ? ""_s : "]"_s, isTotal ? ",\"total\":true}\n"_s : "}\n"_s);
    } else {
        line.append("[aot] type-mismatch "_s, hex(site.id, 12, Lowercase), ' ', site.kind, ' ', site.file, ':', site.position.line, ':', site.position.column, " ("_s, site.function, "): "_s);
        line.append("expected "_s, site.expected, ", got "_s, got, "; continued; count "_s, count);
        for (auto& caller : callers)
            line.append(&caller == callers.data() ? "; from "_s : " < "_s, caller);
        line.append(isTotal ? "; total\n"_s : "\n"_s);
    }
    writeLine(m_destination, line.toString());
}

void RuntimeReporter::reportNativeCall(ValidationFailureKind kind, const String& callee, const String& mismatch)
{
    StringBuilder line;
    if (m_destination.isJSON) {
        line.append("{\"aot\":\"native-call\",\"kind\":\""_s, nameOf(kind), "\",\"callee\":"_s);
        line.appendQuotedJSONString(callee);
        line.append(",\"mismatch\":"_s);
        line.appendQuotedJSONString(mismatch);
        line.append("}\n"_s);
    } else
        line.append("[aot] native-call "_s, nameOf(kind), ' ', callee, ' ', mismatch, '\n');

    Locker locker { m_lock };
    if (m_reportedNativeCalls.add(line.toString()).isNewEntry)
        writeLine(m_destination, line.toString());
}

void RuntimeReporter::writeAtExit()
{
    Locker locker { m_lock };
    if (std::exchange(m_hasWrittenAtExit, true))
        return;
    for (Site& site : m_sites) {
        for (auto& [kind, count] : site.counts) {
            if (count && !isPowerOfTen(count))
                write(site, kind, count, { }, true);
            count = 0;
        }
    }
    const char* path = byteCast<char>(Options::aotCountsPath());
    if (!path)
        return;
    String name = makeStringByReplacingAll(String::fromUTF8(path), "%p"_s, String::number(getCurrentProcessID()));
    // Every VM of the process writes to one file, and a reader adds up the lines of one name.
    static Lock lock;
    static bool hasWrittenInThisProcess;
    Locker fileLocker { lock };
    if (auto file = FilePrintStream::open(name.utf8(), std::exchange(hasWrittenInThisProcess, true) ? "ab"_s : "wb"_s))
        dumpCounts(m_vm, *file);
}

void reportFailedCheck(Instance& instance, uint32_t typeSite, const RuntimeType& expected, JSValue actual)
{
    instance.vm->aotState().reporter->reportFailedCheck(instance, TypeSiteID(typeSite), expected, actual);
}

void reportOrCrash(VM& vm, const ValidationFailure& failure, ASCIILiteral whatIsCalled, const String& callee, const String& mismatch)
{
    if (RuntimeReporter* reporter = RuntimeReporter::ifExplicitlyEnabled(vm)) {
        reporter->reportNativeCall(failure.kind, callee, mismatch);
        return;
    }
    dataLogLn("AOT ", whatIsCalled, ": ", callee, " ", mismatch);
    crashOnValidationFailure(vm, failure);
}

#if ENABLE(WEBASSEMBLY)
void reportInterpreterFallback(Wasm::InterpreterFallbackReason reason)
{
    ReportDestination destination = currentDestination();
    const Image* image = Image::current();
    if (!destination.descriptor || (!destination.isExplicit && !(image && image->header().options.policy() == MismatchPolicy::Report)))
        return;
    bool hasNoGuardPages = reason == Wasm::InterpreterFallbackReason::MemoryChecksBounds;
    ASSERT(hasNoGuardPages || reason == Wasm::InterpreterFallbackReason::VectorsAreDisabled);
    ASCIILiteral word = hasNoGuardPages ? "no-guard-pages"_s : "no-simd"_s;
    ASCIILiteral cause = hasNoGuardPages ? "its memory has no guard pages"_s : "SIMD isn't available on this CPU"_s;
    writeLine(destination, destination.isJSON ? makeString("{\"aot\":\"interpreter-fallback\",\"reason\":\""_s, word, "\"}\n"_s) : makeString("[aot] interpreter-fallback "_s, word, ": a compiled WebAssembly module is running in the interpreter because "_s, cause, '\n'));
}
#endif

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

void writePerfMap()
{
    const Image* image = Image::current();
    if (!image)
        return;
    // Each entry point of an executable has an image of its own, and a thread that starts one adds its functions to the file.
    static Lock lock;
    static bool hasWrittenInThisProcess;
    Locker locker { lock };
    auto file = FilePrintStream::open(makeString("/tmp/perf-"_s, getCurrentProcessID(), ".map"_s).utf8(), std::exchange(hasWrittenInThisProcess, true) ? "a"_s : "w"_s);
    if (!file)
        return;
    const uint8_t* code = image->code().data();
    Vector<std::pair<size_t, String>> symbols;
    for (uint32_t function = 0; function < image->header().counts.functions; ++function) {
        if (image->sizeOfCodeOf(function))
            symbols.append({ static_cast<size_t>(image->codeOf(function) - code), symbolNameOf(*image, function) });
    }
    for (unsigned copy = 0; copy < image->sharedCodeCopies().size(); ++copy)
        symbols.append({ static_cast<size_t>(image->sharedCodeCopy(copy) - code), String("(stubs of compiled code)"_s) });
    AOT::sort(symbols, { }, &std::pair<size_t, String>::first);
    for (size_t i = 0; i < symbols.size(); ++i) {
        size_t end = i + 1 < symbols.size() ? symbols[i + 1].first : image->header().code.functionsEnd;
        file->println(hex(reinterpret_cast<uintptr_t>(code) + symbols[i].first, Lowercase), " ", hex(end - symbols[i].first, Lowercase), " ", symbols[i].second);
    }
}
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
