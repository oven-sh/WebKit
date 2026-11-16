/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOrderRecorder.h"

#if ENABLE(AOT)

#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTModuleTable.h"
#include "JSCInlines.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <span>
#include <wtf/ASCIICType.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/PageBlock.h>
#include <wtf/Seconds.h>
#include <wtf/SetForScope.h>
#include <wtf/StringPrintStream.h>
#include <wtf/Threading.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

#if OS(LINUX) && !defined(BUN_HARDENED) && (CPU(X86_64) || CPU(ARM64))
#define AOT_CAN_RECORD_ORDER 1
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#else
#define AOT_CAN_RECORD_ORDER 0
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

bool recordsOrder()
{
    const Image* image = Image::current();
    return image && image->recordsOrder();
}

#if AOT_CAN_RECORD_ORDER

namespace {

#if CPU(X86_64)
using TrapWord = uint8_t;
constexpr TrapWord trapWord = 0xcc;
#else
using TrapWord = uint32_t;
constexpr TrapWord trapWord = 0xd43e0000;
constexpr TrapWord maskOfTrap = 0xffe0001f;
#endif

struct UnitRange {
    uint32_t start;
    uint32_t end;
};

class OrderRecorder {
    WTF_MAKE_NONCOPYABLE(OrderRecorder);
public:
    OrderRecorder(const Image& image, int file)
        : m_image(image)
        , m_code(const_cast<uint8_t*>(image.code().data()))
        , m_original(image.bytes().data() + image.header().codeOffset)
        , m_functionsEnd(image.header().code.functionsEnd)
        , m_numberOfUnits(image.header().counts.functions)
        , m_file(file)
    {
    }

    ASCIILiteral start();
    bool didTrapAt(uintptr_t pc);
    void write();
    void runWriter();
    void appendLineForRegExp(StringView pattern, uint16_t significantFlags);

    static OrderRecorder* s_current;
    static struct sigaction s_previousAction;

private:
    UnitRange rangeOfUnit(uint32_t unit) const;
    void fillWithTraps(uint8_t* code) const;
    bool mapCopies();
    void restoreUnit(uint32_t unit);
    bool isTrapInOriginal(uint32_t offset) const;

    struct UnitName {
        FunctionID source;
        char kind;
        uint32_t rank;
    };
    using NumberMap = UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    UnitName nameUnit(uint32_t unit) WTF_REQUIRES_LOCK(m_lock);
    void appendLineForUnit(Vector<uint8_t>&, const ModuleTable&, uint32_t unit) WTF_REQUIRES_LOCK(m_lock);
    void findBodies() WTF_REQUIRES_LOCK(m_lock);
    bool hasBody(uint32_t function) const WTF_REQUIRES_LOCK(m_lock) { return m_bodies[function] != noBody; }
    std::optional<SourcePosition> definitionOf(uint32_t function) const WTF_REQUIRES_LOCK(m_lock);
    uint32_t numberAmongNamesakes(uint32_t function) const WTF_REQUIRES_LOCK(m_lock);
    uint32_t numberOfFile(Vector<uint8_t>&, const ModuleTable&, ModuleID) WTF_REQUIRES_LOCK(m_lock);
    uint32_t firstFunctionOfFile(uint32_t function, ModuleID) WTF_REQUIRES_LOCK(m_lock);

    const Image& m_image;
    uint8_t* const m_code;
    const uint8_t* m_original;
    const uint32_t m_functionsEnd;
    const uint32_t m_numberOfUnits;
    const int m_file;
    size_t m_size { 0 };
    std::array<uint8_t*, 2> m_executableCopies { };
    std::array<uint8_t*, 2> m_writableCopies { };
    uint8_t* m_copyOfPage { nullptr };
    uint8_t* m_wasEntered { nullptr };
    uint32_t* m_enteredUnits { nullptr };
    std::atomic<uint32_t> m_numberOfEnteredUnits { 0 };
    std::atomic<bool> m_isRestoring { false };

    Lock m_lock;
    uint32_t m_numberOfListedUnits WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    Vector<uint8_t> m_bytesToWrite WTF_GUARDED_BY_LOCK(m_lock);
    bool m_hasWrittenNumberOfFunctions WTF_GUARDED_BY_LOCK(m_lock) { false };
    UncheckedKeyHashMap<String, uint32_t> m_numbersOfFiles WTF_GUARDED_BY_LOCK(m_lock);
    NumberMap m_firstFunctionsOfModules WTF_GUARDED_BY_LOCK(m_lock);
    Vector<uint32_t> m_ranksOfOtherUnits WTF_GUARDED_BY_LOCK(m_lock);
    static constexpr uint32_t noBody = UINT32_MAX;
    Vector<uint32_t> m_bodies WTF_GUARDED_BY_LOCK(m_lock);
};

OrderRecorder* OrderRecorder::s_current;
struct sigaction OrderRecorder::s_previousAction;

static void* mapMemoryForRecorder(size_t size)
{
    void* memory = mmap(nullptr, std::max<size_t>(size, 1), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return memory == MAP_FAILED ? nullptr : memory;
}

UnitRange OrderRecorder::rangeOfUnit(uint32_t unit) const
{
    const FunctionStarts& starts = m_image.functionStarts();
    uint32_t start = starts[unit];
    uint32_t end = unit + 1 < m_numberOfUnits ? starts[unit + 1] : m_functionsEnd;
    std::span<const uint32_t> copies = m_image.sharedCodeCopies();
    auto copy = std::ranges::lower_bound(copies, start);
    return { start, copy != copies.end() ? std::min(end, *copy) : end };
}

void OrderRecorder::fillWithTraps(uint8_t* code) const
{
    for (uint32_t unit = 0; unit < m_numberOfUnits; ++unit) {
        UnitRange range = rangeOfUnit(unit);
        std::fill(reinterpret_cast<TrapWord*>(code + range.start), reinterpret_cast<TrapWord*>(code + range.end), trapWord);
    }
}

bool OrderRecorder::mapCopies()
{
    constexpr int executable = PROT_READ | PROT_EXEC;
    for (unsigned copy = 0; copy < m_executableCopies.size(); ++copy) {
        void* code = mmap(nullptr, m_size, executable, MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (code == MAP_FAILED)
            return false;
        void* window = mremap(code, 0, m_size, MREMAP_MAYMOVE);
        if (window == MAP_FAILED || mprotect(window, m_size, PROT_READ | PROT_WRITE))
            return false;
        m_executableCopies[copy] = static_cast<uint8_t*>(code);
        m_writableCopies[copy] = static_cast<uint8_t*>(window);
    }
    return true;
}

void OrderRecorder::restoreUnit(uint32_t unit)
{
    UnitRange range = rangeOfUnit(unit);
    size_t bytesInPage = pageSize();
    for (size_t pageEnd = WTF::roundUpToMultipleOf(bytesInPage, static_cast<size_t>(range.end)); pageEnd > range.start; pageEnd -= bytesInPage) {
        size_t pageStart = pageEnd - bytesInPage;
        size_t first = std::max<size_t>(range.start, pageStart);
        size_t end = std::min<size_t>(range.end, pageEnd);
        uint8_t& current = m_copyOfPage[pageStart / bytesInPage];
        unsigned next = !current;
        uint8_t* target = m_writableCopies[next] + pageStart;
        memcpy(target, m_writableCopies[current] + pageStart, bytesInPage);
        memcpy(target + (first - pageStart), m_original + first, end - first);
        __builtin___clear_cache(reinterpret_cast<char*>(target), reinterpret_cast<char*>(target + bytesInPage));
        if (mremap(m_executableCopies[next] + pageStart, 0, bytesInPage, MREMAP_MAYMOVE | MREMAP_FIXED, m_code + pageStart) == MAP_FAILED)
            memcpy(m_writableCopies[current] + first, m_original + first, end - first);
        else
            current = next;
        __builtin___clear_cache(reinterpret_cast<char*>(m_code + pageStart), reinterpret_cast<char*>(m_code + pageEnd));
    }
}

bool OrderRecorder::isTrapInOriginal(uint32_t offset) const
{
#if CPU(X86_64)
    return m_original[offset] == trapWord;
#else
    TrapWord word;
    memcpy(&word, m_original + offset, sizeof(word));
    return (word & maskOfTrap) == (trapWord & maskOfTrap);
#endif
}

bool OrderRecorder::didTrapAt(uintptr_t pc)
{
    uintptr_t offset = pc - reinterpret_cast<uintptr_t>(m_code);
    if (offset >= m_functionsEnd)
        return false;
    CodeLocation location = m_image.locate(reinterpret_cast<const void*>(pc));
    if (location.kind != CodeKind::Function)
        return false;
    while (m_isRestoring.exchange(true, std::memory_order_acquire)) { }
    bool isFirstEntry = !m_wasEntered[location.function];
    if (isFirstEntry) {
        restoreUnit(location.function);
        m_wasEntered[location.function] = true;
        uint32_t count = m_numberOfEnteredUnits.load(std::memory_order_relaxed);
        m_enteredUnits[count] = location.function;
        m_numberOfEnteredUnits.store(count + 1, std::memory_order_release);
    }
    m_isRestoring.store(false, std::memory_order_release);
    return isFirstEntry || !isTrapInOriginal(static_cast<uint32_t>(offset));
}

static void handleTrap(int signal, siginfo_t* info, void* context)
{
    int errorBefore = errno;
    mcontext_t& machine = static_cast<ucontext_t*>(context)->uc_mcontext;
#if CPU(X86_64)
    auto& instruction = machine.gregs[REG_RIP];
    uintptr_t pc = static_cast<uintptr_t>(instruction) - sizeof(TrapWord);
#else
    auto& instruction = machine.pc;
    uintptr_t pc = static_cast<uintptr_t>(instruction);
#endif
    bool isSentByProcess = info->si_code <= 0;
    if (!isSentByProcess && OrderRecorder::s_current->didTrapAt(pc)) {
        instruction = pc;
        errno = errorBefore;
        return;
    }
    const struct sigaction& previous = OrderRecorder::s_previousAction;
    if (previous.sa_flags & SA_SIGINFO)
        previous.sa_sigaction(signal, info, context);
    else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN)
        previous.sa_handler(signal);
    else {
        sigaction(signal, &previous, nullptr);
        if (!isSentByProcess)
            instruction = pc;
        else
            raise(signal);
    }
    errno = errorBefore;
}

ASCIILiteral OrderRecorder::start()
{
    m_size = WTF::roundUpToMultipleOf(pageSize(), static_cast<size_t>(m_functionsEnd));
    if (!m_numberOfUnits || m_size > m_image.header().executableSize || reinterpret_cast<uintptr_t>(m_code) % pageSize())
        return "the program has no code that could be recorded"_s;
    if (m_original == m_code) {
        void* copy = mapMemoryForRecorder(m_size);
        if (!copy)
            return "there is not enough memory"_s;
        memcpy(copy, m_code, m_size);
        m_original = static_cast<const uint8_t*>(copy);
    }
    m_copyOfPage = static_cast<uint8_t*>(mapMemoryForRecorder(m_size / pageSize()));
    m_wasEntered = static_cast<uint8_t*>(mapMemoryForRecorder(m_numberOfUnits));
    m_enteredUnits = static_cast<uint32_t*>(mapMemoryForRecorder(m_numberOfUnits * sizeof(uint32_t)));
    if (!m_copyOfPage || !m_wasEntered || !m_enteredUnits)
        return "there is not enough memory"_s;
    if (!mapCopies())
        return "the system does not allow the mappings that the recorder needs"_s;
    memcpy(m_writableCopies[0], m_original, m_size);
    fillWithTraps(m_writableCopies[0]);
    __builtin___clear_cache(reinterpret_cast<char*>(m_writableCopies[0]), reinterpret_cast<char*>(m_writableCopies[0] + m_size));

    s_current = this;
    struct sigaction action { };
    action.sa_sigaction = handleTrap;
    action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigfillset(&action.sa_mask);
    for (int fault : { SIGSEGV, SIGBUS, SIGILL, SIGFPE })
        sigdelset(&action.sa_mask, fault);
    RELEASE_ASSERT(!sigaction(SIGTRAP, &action, &s_previousAction));
    if (mremap(m_executableCopies[0], 0, m_size, MREMAP_MAYMOVE | MREMAP_FIXED, m_code) == MAP_FAILED) {
        RELEASE_ASSERT(!sigaction(SIGTRAP, &s_previousAction, nullptr));
        s_current = nullptr;
        return "the system does not allow the mappings that the recorder needs"_s;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(m_code), reinterpret_cast<char*>(m_code + m_size));
    Thread::create("AOT order"_s, [this] {
        runWriter();
    });
    return { };
}

static void appendTextToOrder(Vector<uint8_t>& out, StringView text)
{
    for (uint8_t byte : byteCast<uint8_t>(text.utf8().span()))
        out.append(std::max<uint8_t>(byte, ' '));
}

OrderRecorder::UnitName OrderRecorder::nameUnit(uint32_t unit)
{
    const ProgramTables& tables = m_image.programTables();
    uint32_t firstDerived = tables.closureFunctions.first;
    uint32_t endOfEntries = tables.closureFunctions.end;
    uint32_t number = m_image.rootFunctionOf(unit).index();
    FunctionID source(tables.sourceFunctionOf(number));
    if (number < firstDerived)
        return { source, 'b', 0 };
    if (number < endOfEntries) {
        uint32_t rank = 0;
        while (number - rank > firstDerived && tables.sourceFunctionOf(number - rank - 1) == source.index())
            ++rank;
        return { source, 'e', rank };
    }
    if (m_ranksOfOtherUnits.isEmpty()) {
        NumberMap counts;
        for (uint32_t other = endOfEntries; other < tables.numberOfFunctions(); ++other)
            m_ranksOfOtherUnits.append(counts.add(tables.sourceFunctionOf(other), 0).iterator->value++);
    }
    return { source, 'o', m_ranksOfOtherUnits[number - endOfEntries] };
}

void OrderRecorder::findBodies()
{
    m_bodies.fill(noBody, m_image.programTables().closureFunctions.first);
    for (uint32_t unit = 0; unit < m_numberOfUnits; ++unit) {
        uint32_t number = m_image.rootFunctionOf(unit).index();
        if (number < m_bodies.size())
            m_bodies[number] = unit;
    }
}

std::optional<SourcePosition> OrderRecorder::definitionOf(uint32_t function) const
{
    if (!hasBody(function))
        return std::nullopt;
    std::optional<SourcePosition> position = m_image.positionOf(m_bodies[function], CallSiteIndex(0));
    return position && position->source ? position : std::nullopt;
}

uint32_t OrderRecorder::numberAmongNamesakes(uint32_t function) const
{
    const ProgramTables& tables = m_image.programTables();
    std::optional<uint32_t> name = tables.nameOfFunction(function);
    uint32_t number = 0;
    for (uint32_t other = 0; other < function; ++other)
        number += tables.nameOfFunction(other) == name && hasBody(other) && !definitionOf(other);
    return number;
}

uint32_t OrderRecorder::numberOfFile(Vector<uint8_t>& out, const ModuleTable& modules, ModuleID module)
{
    String path = modules.displayPathText(module);
    auto added = m_numbersOfFiles.add(path, m_numbersOfFiles.size());
    if (added.isNewEntry) {
        appendTextToOrder(out, toString("M ", added.iterator->value, " ", path));
        out.append('\n');
    }
    return added.iterator->value;
}

uint32_t OrderRecorder::firstFunctionOfFile(uint32_t function, ModuleID module)
{
    auto added = m_firstFunctionsOfModules.add(module.index(), function);
    uint32_t& first = added.iterator->value;
    if (!added.isNewEntry && first <= function)
        return first;
    first = function;
    for (uint32_t other = function; other--;) {
        std::optional<SourcePosition> definition = definitionOf(other);
        if (!definition)
            continue;
        if (definition->source != module)
            break;
        first = other;
    }
    return first;
}

void OrderRecorder::appendLineForUnit(Vector<uint8_t>& out, const ModuleTable& modules, uint32_t unit)
{
    UnitName name = nameUnit(unit);
    uint32_t function = name.source.index();
    if (m_image.isObfuscated()) {
        if (!m_hasWrittenNumberOfFunctions) {
            m_hasWrittenNumberOfFunctions = true;
            appendTextToOrder(out, toString("P ", m_image.programTables().closureFunctions.first));
            out.append('\n');
        }
        appendTextToOrder(out, toString("F = ", function, " ", name.kind, name.rank, " - 0:0 "));
        out.append('\n');
        return;
    }
    std::optional<SourcePosition> definition = definitionOf(function);
    for (uint32_t other = function; !definition && !hasBody(function) && other;)
        definition = definitionOf(--other);

    StringPrintStream line;
    if (definition)
        line.print("F ", numberOfFile(out, modules, definition->source), " ", function - firstFunctionOfFile(function, definition->source), " ", name.kind, name.rank, " - ", definition->lineColumn.line, ":", definition->lineColumn.column, " ");
    else if (hasBody(function))
        line.print("F - ", numberAmongNamesakes(function), " ", name.kind, name.rank, " - 0:0 ");
    else
        return;
    appendTextToOrder(out, line.toString());
    appendTextToOrder(out, writtenFunctionName(name.source));
    out.append('\n');
}

void OrderRecorder::appendLineForRegExp(StringView pattern, uint16_t significantFlags)
{
    Locker locker { m_lock };
    appendTextToOrder(m_bytesToWrite, toString("R ", significantFlags, " "));
    for (char16_t unit : pattern.codeUnits()) {
        if (unit >= ' ' && unit <= '~' && unit != '%') {
            m_bytesToWrite.append(static_cast<uint8_t>(unit));
            continue;
        }
        m_bytesToWrite.append('%');
        for (unsigned shift : { 12, 8, 4, 0 })
            m_bytesToWrite.append(lowerNibbleToASCIIHexDigit(static_cast<uint8_t>(unit >> shift)));
    }
    m_bytesToWrite.append('\n');
}

void OrderRecorder::write()
{
    Locker locker { m_lock };
    uint32_t count = m_numberOfEnteredUnits.load(std::memory_order_acquire);
    if (m_numberOfListedUnits == count && m_bytesToWrite.isEmpty())
        return;
    SetForScope runningImage(*Image::addressOfCurrentOfThread(), &m_image);
    ModuleTable modules(m_image.programTables());
    if (m_bodies.isEmpty())
        findBodies();
    for (; m_numberOfListedUnits < count; ++m_numberOfListedUnits)
        appendLineForUnit(m_bytesToWrite, modules, m_enteredUnits[m_numberOfListedUnits]);
    size_t written = 0;
    while (written < m_bytesToWrite.size()) {
        ssize_t result = ::write(m_file, m_bytesToWrite.span().data() + written, m_bytesToWrite.size() - written);
        if (result < 0 && errno != EINTR)
            break;
        written += std::max<ssize_t>(result, 0);
    }
    m_bytesToWrite.removeAt(0, written);
}

void OrderRecorder::runWriter()
{
    constexpr Seconds timeBetweenWrites = 50_ms;
    for (;;) {
        WTF::sleep(timeBetweenWrites);
        write();
    }
}

} // anonymous namespace

ASCIILiteral startOrderRecording(const char* path)
{
    RELEASE_ASSERT(!OrderRecorder::s_current);
    const Image* image = Image::current();
    if (!image)
        return "there is no compiled program"_s;
    int file = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (file < 0)
        return "the file cannot be written"_s;
    if (flock(file, LOCK_EX | LOCK_NB)) {
        close(file);
        return { };
    }
    constexpr auto head = "aot-order v1\n"_s;
    if (ftruncate(file, 0) || ::write(file, head.characters(), head.length()) != static_cast<ssize_t>(head.length())) {
        close(file);
        return "the file cannot be written"_s;
    }
    static LazyNeverDestroyed<OrderRecorder> recorder;
    recorder.construct(*image, file);
    ASCIILiteral failure = recorder->start();
    if (!failure.isNull())
        close(file);
    return failure;
}

void finishOrderRecording()
{
    if (OrderRecorder::s_current)
        OrderRecorder::s_current->write();
}

bool isRecordingOrder()
{
    return OrderRecorder::s_current;
}

void recordUseOfRegExp(StringView pattern, uint16_t significantFlags)
{
    OrderRecorder::s_current->appendLineForRegExp(pattern, significantFlags);
}

#else // AOT_CAN_RECORD_ORDER

ASCIILiteral startOrderRecording(const char*)
{
    return "this build of Bun does not record an order on this system"_s;
}

void finishOrderRecording()
{
}

bool isRecordingOrder()
{
    return false;
}

void recordUseOfRegExp(StringView, uint16_t)
{
}

#endif

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
