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
#include "AOTStringTable.h"
#include "JSCInlines.h"
#include "Options.h"
#include "StackVisitor.h"
#include <initializer_list>
#include <mutex>
#include <stdlib.h>
#include <wtf/FilePrintStream.h>
#include <wtf/HexNumber.h>
#include <wtf/ProcessID.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

#if ENABLE(WEBASSEMBLY)
#include "AOTWasmInterface.h"
#endif

#if OS(WINDOWS)
#include <windows.h>
#else
#include <errno.h>
#include <unistd.h>
#endif

#if OS(WINDOWS)
extern "C" __attribute__((weak)) void Bun__atexit(void (*)());
#endif

namespace JSC { namespace AOT {

using Callers = Vector<String, RuntimeReporter::maxNumberOfCallers>;

static Lock s_lock;
static bool s_isDestinationSet;
static intptr_t s_descriptor;
static bool s_isJSON;
static RuntimeReporter* s_firstReporter;

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
    if (!s_isDestinationSet)
        return { standardError(), s_isJSON, false };
    if (s_descriptor < 0)
        return { std::nullopt, false, true };
    return { s_descriptor, s_isJSON, true };
}

void setMismatchLog(intptr_t descriptor, bool isJSON)
{
    Locker locker { s_lock };
    s_isDestinationSet = true;
    s_descriptor = descriptor;
    s_isJSON = isJSON;
}

void setMismatchLogFormat(bool isJSON)
{
    Locker locker { s_lock };
    s_isJSON = isJSON;
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

static String truncated(const String& text, unsigned maxLength)
{
    if (text.length() > maxLength)
        return makeString(StringView(text).left(maxLength - 3), "..."_s);
    return text;
}

static String nonEmptyOr(const String& text, ASCIILiteral fallback)
{
    return text.isEmpty() ? String(fallback) : text;
}

static bool acceptsOnlyIntegerNumbers(const RuntimeType& type)
{
    constexpr auto numbers = OptionSet<PrimitiveSet>::fromRaw(Typed::Primitive::Number);
    return (type.primitiveSet() & numbers) == OptionSet<PrimitiveSet> { PrimitiveSet::Integer };
}

// The test of describe(), which takes a class of a built-in module for a function, like a native one.
static bool isDescribedAsClass(JSCell* callable)
{
    return CompiledCallee::isCompiledCallee(callable) && CompiledCallee::kind(asObject(callable)) == CompiledCallee::FunctionKind::Class;
}

static ASCIILiteral kindWordOf(JSValue value, const RuntimeType& expected)
{
    if (!value || value.isUndefined())
        return "undefined"_s;
    if (value.isNull())
        return "null"_s;
    if (value.isBoolean())
        return "boolean"_s;
    if (value.isNumber()) {
        if (!acceptsOnlyIntegerNumbers(expected))
            return "number"_s;
        return value.isInt32AsAnyInt() ? "number, out of range"_s : "number, not an int32"_s;
    }
    if (value.isString())
        return "string"_s;
    if (value.isCallable() && !isDescribedAsClass(value.asCell()))
        return "function"_s;
    return { };
}

static String displayPath(const Image& image, ModuleID module)
{
    if (image.isObfuscated())
        return emptyString();
    if (!module)
        return "unknown"_s;
    return nonEmptyOr(ModuleTable(image.programTables()).displayPathText(module), "unknown"_s);
}

static String pathForLineColumns(const Image& image, ModuleID module)
{
    if (image.isObfuscated())
        return emptyString();
    if (!module)
        return "unknown"_s;
    return nonEmptyOr(ModuleTable(image.programTables()).pathTextForLineColumns(module), "unknown"_s);
}

static String pathForCheckID(const Image& image, ModuleID module)
{
    if (!module)
        return { };
    if (image.isObfuscated())
        return modulePath(module);
    return ModuleTable(image.programTables()).displayPathText(module);
}

struct CheckSite {
    String file;
    LineColumn position;
    String function;
};

static CheckSite checkSiteWithoutPosition(const Image& image, ModuleID module)
{
    return { displayPath(image, module), { }, image.isObfuscated() ? emptyString() : "unknown"_str };
}

static uint64_t idOf(const Image& image, const TypeSiteData& site)
{
    Vector<uint8_t> path = Typed::encodeProgramString(pathForCheckID(image, ModuleID(site.module)));
    return typeCheckIDOf(path.span(), site.position, static_cast<TypeSiteKind>(site.kind));
}

static uint64_t idOf(const CheckSite& site)
{
    Vector<uint8_t> path = Typed::encodeProgramString(site.file);
    return computeCheckID(path.span(), { site.position.line, site.position.column }, TypeSiteKind::Unknown);
}

static std::optional<CompiledFrame> findTopCompiledFrame(VM& vm)
{
    auto frames = CompiledFrameIterator::tryCreateAtTop(vm);
    if (!frames || frames->isAtEnd())
        return std::nullopt;
    return frames->frame();
}

static uint64_t callSiteKeyOf(const CompiledFrame& frame)
{
    return static_cast<uint64_t>(frame.code.index()) << 32 | frame.callSite.bits();
}

static std::optional<CheckSite> checkSiteOf(VM& vm, const Image& image, const CompiledFrame& frame)
{
    if (image.isObfuscated())
        return CheckSite { emptyString(), frame.lineColumn(), emptyString() };
    auto position = image.positionOf(frame.code.index(), frame.callSite);
    if (!position)
        return std::nullopt;
    FunctionDescription description(frame.function);
    String function = description.codeType() == ModuleCode ? "module"_str : nonEmptyOr(description.name(vm), "anonymous"_s);
    return CheckSite { pathForLineColumns(image, position->source), position->lineColumn, WTF::move(function) };
}

static String describeCaller(const Image& image, const CompiledFrame& frame)
{
    if (image.isObfuscated()) {
        LineColumn position = frame.lineColumn();
        return makeString(position.line, ':', position.column);
    }
    auto position = image.positionOf(frame.code.index(), frame.callSite);
    if (!position)
        return { };
    return makeString(pathForLineColumns(image, position->source), ':', position->lineColumn.line);
}

static Callers findCallers(VM& vm)
{
    Callers callers;
    const Image* image = vm.aotState().image;
    if (!vm.topCallFrame || !image)
        return callers;
    bool isFirstFrame = true;
    StackVisitor::visit(vm.topCallFrame, vm, [&](StackVisitor& visitor) {
        if (std::exchange(isFirstFrame, false) || !visitor->isCompiledFrame())
            return IterationStatus::Continue;
        if (String caller = describeCaller(*image, visitor->compiledFrame()); !caller.isNull())
            callers.append(WTF::move(caller));
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
        m_next = s_firstReporter;
        if (m_next)
            m_next->m_previous = this;
        s_firstReporter = this;
    }
    if (Options::aotCountsPath())
        installExitHook();
}

RuntimeReporter::~RuntimeReporter()
{
    Locker locker { s_lock };
    if (m_previous)
        m_previous->m_next = m_next;
    else
        s_firstReporter = m_next;
    if (m_next)
        m_next->m_previous = m_previous;
}

void RuntimeReporter::installExitHook()
{
    static std::once_flag once;
    std::call_once(once, [] {
        void (*writeAllAtExit)() = [] {
            Locker locker { s_lock };
            for (auto* reporter = s_firstReporter; reporter; reporter = reporter->m_next)
                reporter->writeAtExit();
        };
#if OS(WINDOWS)
        if (Bun__atexit) {
            Bun__atexit(writeAllAtExit);
            return;
        }
#endif
        atexit(writeAllAtExit);
#if !OS(DARWIN) && !OS(WINDOWS)
        at_quick_exit(writeAllAtExit);
#endif
    });
}

RuntimeReporter::Site* RuntimeReporter::addSite(uint64_t id, NOESCAPE const Invocable<std::unique_ptr<Site>()> auto& create)
{
    Site* site = m_sites.get(id);
    if (!site && m_sites.size() < maxNumberOfSites) {
        site = m_sites.add(id, create()).iterator->value.get();
        m_sitesInOrder.append(site);
        installExitHook();
    }
    if (!site && !std::exchange(m_hasOverflowed, true))
        writeLine(m_destination, m_destination.isJSON ? makeString("{\"aot\":\"type-mismatch\",\"kind\":\"overflow\",\"sites\":"_s, maxNumberOfSites, "}\n"_s) : makeString("[aot] type-mismatch log is full ("_s, maxNumberOfSites, " sites). New sites are not logged\n"_s));
    return site;
}

uint64_t RuntimeReporter::bump(Site& site, ASCIILiteral word, const String& name)
{
    for (auto& [kind, count] : site.counts) {
        if (word.isNull() ? kind == name : kind == word)
            return ++count;
    }
    if (site.counts.size() < maxNumberOfKindsAtSite || word == "other"_s) {
        site.counts.append({ word.isNull() ? name.isolatedCopy() : String(word), 1 });
        return 1;
    }
    return 0;
}

void RuntimeReporter::reportFailedCheck(Instance& instance, std::optional<uint32_t> typeSite, const RuntimeType& expected, JSValue actual)
{
    if (!isEnabled())
        return;
    Locker locker { m_lock };
    VM& vm = *instance.vm;

    Site* site = nullptr;
    std::optional<CompiledFrame> frame = findTopCompiledFrame(vm);
    auto known = frame ? m_sitesByCallSite.find(callSiteKeyOf(*frame)) : m_sitesByCallSite.end();
    if (known != m_sitesByCallSite.end())
        site = known->value;
    else {
        const Image& image = *vm.aotState().image;
        const ProgramTables& tables = instance.tables();
        const TypeSiteData* data = typeSite ? tables.findTypeSite(*typeSite) : nullptr;
        if (typeSite && !data) {
            ASSERT_NOT_REACHED();
            return;
        }
        CheckSite checkSite = checkSiteWithoutPosition(image, data ? ModuleID(data->module) : ModuleID());
        if (auto compiledSite = frame ? checkSiteOf(vm, image, *frame) : std::nullopt)
            checkSite = WTF::move(*compiledSite);
        uint64_t id = data ? idOf(image, *data) : idOf(checkSite);
        site = addSite(id, [&] {
            auto newSite = makeUnique<Site>();
            newSite->id = id;
            newSite->kind = logWordOf(data ? static_cast<TypeSiteKind>(data->kind) : TypeSiteKind::Unknown);
            newSite->file = checkSite.file.isolatedCopy();
            newSite->position = checkSite.position;
            newSite->function = checkSite.function.isolatedCopy();
            newSite->expected = truncated(describe(tables, expected, typeSite ? TypeSiteID(*typeSite) : TypeSiteID()), maxLengthOfType).isolatedCopy();
            return newSite;
        });
        if (frame)
            m_sitesByCallSite.add(callSiteKeyOf(*frame), site);
    }
    if (!site)
        return;
    ASCIILiteral word = kindWordOf(actual, expected);
    countAndWrite(vm, *site, word, word.isNull() ? describe(actual, ValueDetail::KindOnly) : String());
}

void RuntimeReporter::countAndWrite(VM& vm, Site& site, ASCIILiteral word, const String& name)
{
    uint64_t count = bump(site, word, name);
    if (!count) {
        word = "other"_s;
        count = bump(site, word, name);
    }
    if (!isPowerOfTen(count))
        return;
    write(site, word.isNull() ? name : String(word), count, findCallers(vm), false);
}

void RuntimeReporter::write(const Site& site, const String& got, uint64_t count, const Callers& callers, bool isTotal)
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
            line.append(&caller == callers.begin() ? ",\"from\":["_s : ","_s);
            line.appendQuotedJSONString(caller);
        }
        line.append(callers.isEmpty() ? ""_s : "]"_s, isTotal ? ",\"total\":true}\n"_s : "}\n"_s);
    } else {
        line.append("[aot] type-mismatch "_s, hex(site.id, 12, Lowercase), ' ', site.kind, ' ', site.file, ':', site.position.line, ':', site.position.column, " ("_s, site.function, "): "_s);
        line.append("expected "_s, site.expected, ", got "_s, got, "; continued; count "_s, count);
        for (auto& caller : callers)
            line.append(&caller == callers.begin() ? "; from "_s : " < "_s, caller);
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
    writeTotals();
    writeCounts();
}

void RuntimeReporter::writeTotals()
{
    for (Site* site : m_sitesInOrder) {
        for (auto& [kind, count] : site->counts) {
            if (count && !isPowerOfTen(count))
                write(*site, kind, count, { }, true);
            count = 0;
        }
    }
}

void RuntimeReporter::writeCounts()
{
    const char* path = byteCast<char>(Options::aotCountsPath());
    if (!path)
        return;
    String name = makeStringByReplacingAll(String::fromUTF8(path), "%p"_s, String::number(getCurrentProcessID()));
    // Every VM of the process writes to one file, and a reader adds up the lines of one name.
    static Lock lock;
    static bool hasWrittenInThisProcess;
    Locker locker { lock };
    auto file = FilePrintStream::open(name.utf8(), std::exchange(hasWrittenInThisProcess, true) ? "a"_s : "w"_s);
    if (!file)
        return;
    dumpCounts(m_vm, *file);
}

static RuntimeReporter& reporterOf(VM& vm)
{
    return *vm.aotState().reporter;
}

static std::optional<uint32_t> indexOf(TypeSiteID site)
{
    if (!site)
        return std::nullopt;
    return site.index();
}

void reportFailedCheck(Instance& instance, uint32_t typeSite, const RuntimeType& expected, JSValue actual)
{
    reporterOf(*instance.vm).reportFailedCheck(instance, indexOf(TypeSiteID(typeSite)), expected, actual);
}

#if ENABLE(WEBASSEMBLY)

struct InterpreterFallbackText {
    ASCIILiteral word;
    ASCIILiteral cause;
};

static InterpreterFallbackText textOf(Wasm::InterpreterFallbackReason reason)
{
    switch (reason) {
    case Wasm::InterpreterFallbackReason::MemoryChecksBounds:
        return { "no-guard-pages"_s, "its memory has no guard pages"_s };
    case Wasm::InterpreterFallbackReason::VectorsAreDisabled:
        return { "no-simd"_s, "SIMD isn't available on this CPU"_s };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool currentImageReportsMismatches()
{
    const Image* image = Image::current();
    return image && image->header().options.policy() == MismatchPolicy::Report;
}

void reportInterpreterFallback(Wasm::InterpreterFallbackReason reason)
{
    ReportDestination destination = currentDestination();
    if (!destination.descriptor)
        return;
    if (!destination.isExplicit && !currentImageReportsMismatches())
        return;
    auto [word, cause] = textOf(reason);
    writeLine(destination, destination.isJSON ? makeString("{\"aot\":\"interpreter-fallback\",\"reason\":\""_s, word, "\"}\n"_s) : makeString("[aot] interpreter-fallback "_s, word, ": a compiled WebAssembly module is running in the interpreter because "_s, cause, '\n'));
}

#endif // ENABLE(WEBASSEMBLY)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
