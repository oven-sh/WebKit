/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include <map>
#include <tuple>
#include "AOTImage.h"


#if ENABLE(AOT)

#include "AOTCompiler.h"
#include "AOTInstructionsInCode.h"
#include "AOTOutliner.h"
#include "AOTTypedBuiltins.h"
#include "AOTTypedFormat.h"
#include "AOTTypedOperations.h"
#include "AOTTypedThreads.h"
#include "CachedTypes.h"
#include "CodeBlock.h"
#include "CodeCache.h"
#include "ExecutableAllocator.h"
#include "FunctionExecutable.h"
#include "GetterSetter.h"
#include "JSCBytecodeCacheVersion.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSModuleEnvironment.h"
#include "JSModuleRecord.h"
#include "MachineContext.h"
#include "MarkedBlockCage.h"
#include "Options.h"
#include "ParseInt.h"
#include "SourceProvider.h"
#include "YarrSyntaxChecker.h"
#include <wtf/FilePrintStream.h>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SHA1.h>
#include <wtf/StackCheck.h>
#include <wtf/Scope.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/AccessibleAddress.h>
#include <wtf/threads/Signals.h>

#if OS(DARWIN) || OS(LINUX) || OS(FREEBSD)
#include <sys/mman.h>
#if OS(DARWIN)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#elif OS(WINDOWS)
#include <windows.h>
#endif

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageBuilder);
WTF_MAKE_TZONE_ALLOCATED_IMPL(Image);

uint64_t imageStamp()
{
    uint64_t stamp = computeJSCBytecodeCacheVersion();
    auto mix = [&](uint64_t value) {
        stamp = (stamp ^ value) * 0x100000001b3ULL;
        stamp ^= stamp >> 29;
    };
    mix(numberOfEntries);
    mix(Instance::offsetOfStates());
    mix(Instance::stateOfLinkedWithoutData());
    mix(FunctionInfo::offsetOfBitsAndStateNumber() << 8 | FunctionInfo::stateNumberShift);
    mix(Instance::offsetOfOwnData());
    mix(BiasedDataPointer::bias);
    mix(Instance::offsetOfTypedStatics());
    mix(Instance::offsetOfTypedCards());
    mix(static_cast<uint64_t>(adapterSaveAreaSize) << 8 | static_cast<uint64_t>(-offsetOfInstanceInAdapter));
#if CPU(ARM64)
    mix(static_cast<uint64_t>(cellCageBaseGPR) << 16 | Instance::offsetOfCellCageBase());
#endif
    mix(Instance::roomForEnvironments);
    mix(Instance::granuleBelowInstance);
    mix(Instance::typedCardShift);
    mix(Instance::offsetOfTypedTexts());
    mix(Instance::offsetOfTypedData());
    mix(OBJECT_OFFSETOF(Instance::TypedAllocation, allocator) << 16 | OBJECT_OFFSETOF(Instance::TypedAllocation, words) << 8 | sizeof(Instance::TypedAllocation));
    mix(Instance::minStateWithData);
    mix(numberOfStubs);
    mix(sizeof(ImageHeader));
    mix(sizeof(ImageFunction));
    mix(numOpcodeIDs);
    mix(sizeof(VM));
    mix(sizeof(JSGlobalObject));
    mix(sizeof(CodeBlock));
    mix(sizeof(Data));
    mix(sizeof(Typed::Data));
    mix(VM::offsetOfSoftStackLimit());
    mix(Instance::offsetOfSoftStackLimit());
    mix(VM::offsetOfHeapBarrierThreshold());
    mix(Instance::offsetOfBarrierThreshold());
    mix(CodeBlock::offsetOfJITData());
    mix(JSGlobalObject::offsetOfGlobalLexicalBindingEpoch());
#if CPU(ARM64)
    mix(1 | MacroAssembler::featuresOfBuildTarget() << 8);
#elif CPU(X86_64)
    mix(2 | MacroAssembler::featuresOfBuildTarget() << 8);
#endif
    mix(Typed::builtinsHash);
    mix(Typed::Format::version);
    return stamp;
}

static std::span<const uint8_t> bytesOf(const StringImpl& string) { return string.is8Bit() ? asBytes(string.span8()) : asBytes(string.span16()); }

static constexpr unsigned maxUntruncatedQuoteLength = 64;
static constexpr unsigned maxCalleeQuoteLength = 96;
static constexpr unsigned truncatedQuoteEndLength = 40;

static size_t argumentListStartIndex(StringView text)
{
    unsigned length = text.length();
    if (length < 2 || text[length - 1] != ')')
        return notFound;
    unsigned depth = 1;
    bool isInComment = false;
    for (unsigned i = length - 2; i; --i) {
        char16_t c = text[i];
        if (isInComment) {
            if (c == '*' && text[i - 1] == '/') {
                isInComment = false;
                --i;
            }
        } else if (c == '(') {
            if (!--depth)
                return i;
        } else if (c == ')')
            ++depth;
        else if (c == '/' && text[i - 1] == '*') {
            isInComment = true;
            --i;
        }
        if (!i)
            break;
    }
    return notFound;
}

void collectQuotes(CompiledFunctionInfo& info, UnlinkedCodeBlock* codeBlock, StringView text, unsigned sourceOffset)
{
    if (!codeBlock->hasExpressionInfo())
        return;
    int length = text.length();
    for (uint32_t offset : info.quotableSites) {
        auto entry = codeBlock->expressionInfoForBytecodeIndex(BytecodeIndex(offset));
        int divot = entry.divot + sourceOffset;
        int start = divot - entry.startOffset;
        int stop = divot + entry.endOffset;
        if (stop <= 0 || start < 0 || start > length || stop > length)
            continue;
        Quote quote { offset, static_cast<uint32_t>(start), Quote::Exact, { } };
        if (start < stop) {
            StringView said = text.substring(start, stop - start);
            if (said.length() <= maxUntruncatedQuoteLength)
                quote.text = said.utf8();
            else if (size_t open = argumentListStartIndex(said); open != notFound && open < maxCalleeQuoteLength) {
                quote.kind = Quote::Call;
                quote.text = said.left(open + 1).utf8();
            } else {
                quote.start = std::numeric_limits<uint32_t>::max();
                quote.text = makeString(said.left(truncatedQuoteEndLength), "..."_s, said.right(truncatedQuoteEndLength)).utf8();
            }
        } else {
            int from = start;
            int to = start;
            while (from > 0 && start - from < 20 && text[from - 1] != '\n')
                from--;
            while (from < start - 1 && isStrWhiteSpace(text[from]))
                from++;
            while (to < length && to - start < 20 && text[to] != '\n')
                to++;
            while (to > start && isStrWhiteSpace(text[to - 1]))
                to--;
            quote.kind = Quote::Approximate;
            quote.start = from;
            quote.text = text.substring(from, to - from).utf8();
        }
        info.quotes.append(WTF::move(quote));
    }
}

static void appendVarint(Vector<uint8_t>& bytes, uint64_t value)
{
    while (value >= 0x80) {
        bytes.append(static_cast<uint8_t>(value) | 0x80);
        value >>= 7;
    }
    bytes.append(static_cast<uint8_t>(value));
}

static constexpr uint32_t faultingAccessesBetweenCheckpoints = 64;

static uint64_t readVarint(const uint8_t*& at)
{
    uint64_t value = 0;
    for (unsigned shift = 0;; shift += 7) {
        uint8_t byte = *at++;
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return value;
    }
}

struct SiteCheckpoint {
    static constexpr unsigned sitesBetween = 64;
    static uint64_t numberFor(uint64_t numberOfSites) { return numberOfSites ? (numberOfSites - 1) / sitesBetween : 0; }
    static SiteCheckpoint at(const uint8_t* first, uint64_t index)
    {
        SiteCheckpoint result;
        memcpy(&result, first + index * sizeof(SiteCheckpoint), sizeof(SiteCheckpoint));
        return result;
    }

    uint32_t offset;
    uint32_t positionAndFrame;
};

static Compress s_compress;
static Decompress s_decompress;

void setCodec(Compress compress, Decompress decompress)
{
    s_compress = compress;
    s_decompress = decompress;
}

static std::optional<Vector<uint8_t>> compressInBlocks(std::span<const uint8_t> bytes, size_t sizeOfBlock)
{
    if (!s_compress || !s_decompress || bytes.size() < 4 * KB)
        return std::nullopt;
    size_t numberOfBlocks = (bytes.size() + sizeOfBlock - 1) / sizeOfBlock;
    Vector<uint32_t> words;
    words.append(safeCast<uint32_t>(numberOfBlocks));
    words.append(safeCast<uint32_t>(bytes.size() - (numberOfBlocks - 1) * sizeOfBlock));
    Vector<uint8_t> blocks;
    Vector<uint8_t> packed(2 * sizeOfBlock);
    for (size_t block = 0; block < numberOfBlocks; ++block) {
        auto source = bytes.subspan(block * sizeOfBlock, std::min(sizeOfBlock, bytes.size() - block * sizeOfBlock));
        size_t size = s_compress(source.data(), source.size(), packed.mutableSpan().data(), packed.size());
        if (!size)
            return std::nullopt;
        words.append(safeCast<uint32_t>(blocks.size()));
        blocks.append(packed.span().first(size));
    }
    words.append(safeCast<uint32_t>(blocks.size()));
    Vector<uint8_t> result;
    result.append(asBytes(words.span()));
    result.appendVector(blocks);
    return result;
}

class BlockReader {
public:
    BlockReader(const uint8_t* bytes, size_t sizeOfBlock, uint64_t start)
        : m_bytes(bytes)
        , m_sizeOfBlock(sizeOfBlock)
        , m_at(start)
    {
    }

    bool failed() const { return m_failed; }

    uint8_t next()
    {
        if (!m_sizeOfBlock)
            return m_bytes[m_at++];
        size_t block = m_at / m_sizeOfBlock;
        if (block != m_block && !unpack(block))
            return 0;
        size_t inBlock = m_at++ % m_sizeOfBlock;
        RELEASE_ASSERT(inBlock < m_unpacked.size());
        return m_unpacked[inBlock];
    }

    uint64_t varint()
    {
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            uint8_t byte = next();
            value |= static_cast<uint64_t>(byte & 0x7f) << shift;
            if (!(byte & 0x80))
                break;
        }
        return value;
    }

private:
    bool unpack(size_t block)
    {
        if (m_failed)
            return false;
        const uint32_t* words = reinterpret_cast<const uint32_t*>(m_bytes);
        size_t numberOfBlocks = words[0];
        const uint32_t* starts = words + 2;
        const uint8_t* blocks = reinterpret_cast<const uint8_t*>(starts + numberOfBlocks + 1);
        m_failed = true;
        if (!s_decompress || block >= numberOfBlocks)
            return false;
        m_unpacked.resize(block + 1 == numberOfBlocks ? words[1] : m_sizeOfBlock);
        if (!s_decompress(blocks + starts[block], starts[block + 1] - starts[block], m_unpacked.mutableSpan().data(), m_unpacked.size()))
            return false;
        m_failed = false;
        m_block = block;
        return true;
    }

    const uint8_t* m_bytes;
    size_t m_sizeOfBlock;
    uint64_t m_at;
    size_t m_block { std::numeric_limits<size_t>::max() };
    Vector<uint8_t> m_unpacked;
    bool m_failed { false };
};

String Image::quoteText(uint64_t start, size_t length) const
{
    BlockReader reader(this->at<uint8_t>(header().quotesTextOffset), header().quoteTextBlockSize, start);
    Vector<uint8_t> bytes(length, [&](size_t) { return reader.next(); });
    if (reader.failed())
        return { };
    return String::fromUTF8(bytes.span());
}

std::optional<std::pair<String, bool>> Image::quoteAt(const ImageFunction& function, unsigned bytecodeOffset) const
{
    if (!function.quotes())
        return std::nullopt;
    BlockReader reader(this->at<uint8_t>(header().quotesOffset), header().quoteBlockSize, function.quotes());
    uint64_t offset = 0;
    int64_t start = 0;
    for (uint64_t count = reader.varint(); count--;) {
        offset += reader.varint();
        uint64_t step = reader.varint();
        start += static_cast<int64_t>(step >> 1) ^ -static_cast<int64_t>(step & 1);
        uint64_t lengthAndKind = reader.varint();
        if (reader.failed())
            return std::nullopt;
        if (offset < bytecodeOffset)
            continue;
        if (offset > bytecodeOffset)
            break;
        String text = quoteText(start, static_cast<size_t>(lengthAndKind >> 2));
        if (text.isNull())
            return std::nullopt;
        auto kind = static_cast<Quote::Kind>(lengthAndKind & 3);
        if (kind == Quote::Call)
            text = makeString(text, "...)"_s);
        return std::pair { WTF::move(text), kind != Quote::Approximate };
    }
    return std::nullopt;
}

void collectConstructSites(CompiledFunctionInfo& info, UnlinkedCodeBlock* codeBlock, StringView text, unsigned sourceOffset)
{
    for (const auto& instruction : codeBlock->instructions()) {
        switch (instruction->opcodeID()) {
        case op_construct:
        case op_construct_varargs:
        case op_super_construct:
        case op_super_construct_varargs: {
            info.constructSites.append(instruction.offset());
            std::pair<uint32_t, uint32_t> start { 0, 0 };
            if (!text.isNull() && codeBlock->hasExpressionInfo()) {
                auto entry = codeBlock->expressionInfoForBytecodeIndex(BytecodeIndex(instruction.offset()));
                int64_t divot = static_cast<int64_t>(entry.divot) + sourceOffset;
                int64_t at = divot - entry.startOffset;
                if (at >= 0 && divot < text.length()) {
                    for (int64_t i = at + 1; i <= divot; ++i)
                        start.first += text[i] == '\n';
                    start.second = entry.startOffset;
                    if (start.first) {
                        start.second = 1;
                        for (int64_t i = at - 1; i > 0 && text[i] != '\n'; --i)
                            start.second++;
                    }
                }
            }
            info.constructionStarts.append(start);
            break;
        }
        default:
            break;
        }
    }
}

Vector<FunctionReportableSites> ImageBuilder::reportableSites()
{
    Vector<FunctionReportableSites> all(m_functions.size());
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        auto& info = m_functions[index].code.info;
        FunctionReportableSites result;
        result.offsets = WTF::move(info.callSites);
        result.identifierIndices = WTF::move(info.identifierIndices);
        result.typedIdentifierIndices = WTF::move(info.typedIdentifierIndices);
        result.typedIdentifierNames = WTF::move(info.typedIdentifierNames);
        result.typedCatchEntrypointsForCallSites = WTF::move(info.typedCatchEntrypointsForCallSites);
        result.typedFunction = info.typedFunction;
        result.stateNumber = info.stateNumber;
        result.typedPositions = WTF::move(info.typedPositions);
        result.numberOfSitesWithoutCode = info.numberOfSitesWithoutCode;
        result.typedSourceNames = WTF::move(info.typedSourceNames);
        result.constantIndices = WTF::move(info.constantIndices);
        for (unsigned i = 0; i < info.constructSites.size(); ++i) {
            if (info.constructionStarts[i].first || info.constructionStarts[i].second)
                result.constructions.append({ info.constructSites[i], info.constructionStarts[i].first, info.constructionStarts[i].second });
        }
        all[index] = WTF::move(result);
    });
    return all;
}

bool Image::constructsAt(const ImageFunction& function, unsigned bytecodeOffset) const
{
    if (!function.quotes())
        return false;
    BlockReader reader(this->at<uint8_t>(header().quotesOffset), header().quoteBlockSize, function.quotes());
    for (uint64_t count = reader.varint(); count-- && !reader.failed();) {
        reader.varint();
        reader.varint();
        reader.varint();
    }
    uint64_t offset = 0;
    for (uint64_t count = reader.varint(); count-- && !reader.failed();) {
        offset += reader.varint();
        if (offset >= bytecodeOffset)
            return offset == bytecodeOffset && !reader.failed();
    }
    return false;
}

static std::atomic<uint64_t> s_linkTimeConstantsUsed[4];
static_assert(numberOfLinkTimeConstants <= 256);

static std::atomic<bool> s_usesNumberedIntrinsics;

void ImageBuilder::clearStubCalls(CompiledCode& code)
{
    if (!std::exchange(code.stubCallsAreCleared, true)) {
        for (auto& call : code.info.stubCalls)
            memset(code.bytes.mutableSpan().data() + call.offset, 0, sizeOfNearCall);
    }
    for (auto& continuation : code.continuations)
        clearStubCalls(continuation);
}

static String onOneLineOfMap(StringView text)
{
    String result = text.toString();
    for (auto [character, written] : { std::pair { "\\"_s, "\\\\"_s }, std::pair { "\t"_s, "\\t"_s }, std::pair { "\n"_s, "\\n"_s }, std::pair { "\r"_s, "\\r"_s } })
        result = makeStringByReplacingAll(result, character, written);
    return result;
}

void ImageBuilder::add(ImageKey key, uint64_t rank, CompiledCode&& code, String&& nameForMap)
{
    clearStubCalls(code);
    Locker locker { m_lock };
    m_functions.append(Function { key, rank, WTF::move(code), WTF::move(nameForMap) });
}

template<typename T>
static void growOnPagesTouchedByPool(Vector<T>& vector, size_t newSize)
{
#if !ASAN_ENABLED
    size_t oldSize = vector.size();
    if (newSize > oldSize) {
        vector.reserveCapacity(newSize);
        auto* bytes = reinterpret_cast<volatile uint8_t*>(vector.begin() + oldSize);
        size_t size = (newSize - oldSize) * sizeof(T);
        constexpr size_t stride = 4 * KB;
        constexpr size_t bytesInChunk = MB;
        Typed::forEachIndexInParallel(static_cast<unsigned>((size + bytesInChunk - 1) / bytesInChunk), [&](unsigned chunk) {
            size_t end = std::min(size, (static_cast<size_t>(chunk) + 1) * bytesInChunk);
            for (size_t at = static_cast<size_t>(chunk) * bytesInChunk; at < end; at += stride)
                bytes[at] = 0;
        });
    }
#endif
    vector.grow(newSize);
}

size_t ImageBuilder::growFunctions(size_t number)
{
    Locker locker { m_lock };
    size_t first = m_functions.size();
    growOnPagesTouchedByPool(m_functions, first + number);
    return first;
}

void ImageBuilder::set(size_t place, ImageKey key, uint64_t rank, CompiledCode&& code, String&& nameForMap)
{
    clearStubCalls(code);
    m_functions[place] = Function { key, rank, WTF::move(code), WTF::move(nameForMap) };
}

static constexpr size_t stackOfRegExpCompiler = 448 * KB;

static size_t stackLeftOnThisThread()
{
    return static_cast<char*>(currentStackPointer()) - static_cast<char*>(Thread::currentSingleton().stack().end());
}

static bool hasStackForRegExpCompiler()
{
    return stackLeftOnThisThread() >= stackOfRegExpCompiler + 16 * KB;
}

void ImageBuilder::requestRegExp(VM& vm, const String& pattern, OptionSet<Yarr::Flags> flags, bool isFoundByEvaluator)
{
    flags = ImageRegExp::significantFlags(flags);
    String key = makeString(flags.toRaw(), '/', pattern);
    RegExpRequest request { pattern.isolatedCopy(), flags, isFoundByEvaluator };
    Locker locker { m_lock };
    if (!m_requestedRegExps.add(WTF::move(key)).isNewEntry)
        return;
    m_vmOfRegExps = &vm;
    m_regExpRequests.append(WTF::move(request));
    m_numberOfRegExpsWaiting.fetch_add(1, std::memory_order_relaxed);
    m_regExpIsRequested.notifyOne();
}

NEVER_INLINE auto ImageBuilder::compileRegExp(VM& vm, const RegExpRequest& request, bool& isLeftToInterpreter) -> std::optional<RegExpCode>
{
    size_t left = stackLeftOnThisThread();
    RELEASE_ASSERT(left >= stackOfRegExpCompiler);
    StackCheck stackCheck(Thread::currentSingleton().stack(), left - stackOfRegExpCompiler + StackBounds::DefaultReservedZone);
    auto& pattern = request.pattern;
    auto flags = request.flags;
    RegExpCode result { pattern, flags, { } };
    result.isFoundByEvaluator = request.isFoundByEvaluator;
    auto notCompiled = [&](ASCIILiteral reason) -> std::optional<RegExpCode> {
        isLeftToInterpreter = true;
        if (Options::verboseAOTCompilation()) [[unlikely]]
            dataLogLn("AOT: regular expression not compiled (", reason, "): /", pattern, "/ flags ", flags.toRaw());
        return std::nullopt;
    };
    auto withoutCode = [&]() -> std::optional<RegExpCode> {
        for (auto& code : result.code)
            code = { };
        auto summary = Yarr::checkSyntax(pattern, flags);
        if (Yarr::hasError(summary.error) || summary.hasNamedCaptureGroups || summary.isOnlyPatternCharacters || summary.maxParenthesesDepth > ImageRegExp::maxParenthesesDepthWithoutPattern)
            return notCompiled("too long for code, and its creation wants more than its syntax"_s);
        result.hasNoCode = true;
        result.numSubpatterns = summary.numSubpatterns;
        result.sourceIsPattern = RegExp::sourceIsPattern(pattern);
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: a regular expression of ", pattern.length(), " characters: no code: valid, ", result.numSubpatterns, " groups", result.sourceIsPattern ? ", nothing to escape" : "");
        return WTF::move(result);
    };
    for (auto charSize : { Yarr::CharSize::Char8, Yarr::CharSize::Char16 }) {
        Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
        Yarr::YarrPattern yarrPattern(pattern, flags, error, Yarr::ExecutionMode::IncludeSubpatterns, &stackCheck);
        if (Yarr::hasError(error) || yarrPattern.containsUnsignedLengthPattern() || (yarrPattern.m_containsLookbehinds && !Options::useRegExpLookbehindJIT()))
            return notCompiled("the JIT does not support it either"_s);
        if (!yarrPattern.m_atom.isNull())
            return std::nullopt;
        auto code = Yarr::jitCompileForImage(yarrPattern, pattern, charSize, &vm, Yarr::ExecutionMode::IncludeSubpatterns, stackCheck);
        if (!code && pattern.length() > ImageRegExp::maxLengthWithCode)
            return withoutCode();
        if (!code)
            return notCompiled(charSize == Yarr::CharSize::Char8 ? "no code for 8-bit subjects"_s : "no code for 16-bit subjects"_s);
        if (pattern.length() > 4096)
            dataLogLnIf(Options::verboseAOTCompilation(), "AOT: a regular expression of ", pattern.length(), " characters: ", code->bytes.size(), " bytes of code for ", charSize == Yarr::CharSize::Char8 ? "8" : "16", "-bit subjects");
        result.code[charSize == Yarr::CharSize::Char16] = WTF::move(*code);
        result.numSubpatterns = yarrPattern.m_numSubpatterns;
        result.minimumSize = yarrPattern.m_body->m_minimumSize;
        result.specificPattern = yarrPattern.m_specificPattern;
        result.keepsMoreOfPattern = !yarrPattern.m_captureGroupNames.isEmpty() || !yarrPattern.m_namedGroupToParenIndices.isEmpty();
    }
    result.sourceIsPattern = RegExp::sourceIsPattern(pattern);
    return WTF::move(result);
}

bool ImageBuilder::compileOneRequestedRegExp()
{
    return m_numberOfRegExpsWaiting.load(std::memory_order_relaxed) && hasStackForRegExpCompiler() && takeAndCompileOneRegExp();
}

bool ImageBuilder::takeAndCompileOneRegExp()
{
    RegExpRequest request;
    VM* vm;
    {
        Locker locker { m_lock };
        if (m_nextRegExpRequest == m_regExpRequests.size())
            return false;
        request = WTF::move(m_regExpRequests[m_nextRegExpRequest++]);
        m_numberOfRegExpsWaiting.fetch_sub(1, std::memory_order_relaxed);
        vm = m_vmOfRegExps;
    }
    MonotonicTime start = MonotonicTime::now();
    bool isLeftToInterpreter = false;
    auto result = compileRegExp(*vm, request, isLeftToInterpreter);
    request = { };
    Locker locker { m_lock };
    if (result)
        m_regExps.append(WTF::move(*result));
    m_numberOfRegExpsNotCompiled += isLeftToInterpreter;
    if (Options::verboseAOTCompilation()) [[unlikely]] {
        m_timeInRegExpCompiler += MonotonicTime::now() - start;
        m_threadsThatCompiledRegExps.add(&Thread::currentSingleton());
    }
    return true;
}

void ImageBuilder::didRequestAllRegExps()
{
    Locker locker { m_lock };
    m_didRequestAllRegExps = true;
    m_regExpIsRequested.notifyAll();
}

void ImageBuilder::compileRegExpsUntilAllAreRequested()
{
    if (!hasStackForRegExpCompiler())
        return;
    for (;;) {
        if (takeAndCompileOneRegExp())
            continue;
        Locker locker { m_lock };
        if (m_nextRegExpRequest != m_regExpRequests.size())
            continue;
        if (m_didRequestAllRegExps)
            return;
        m_regExpIsRequested.wait(m_lock);
    }
}

void ImageBuilder::compileRegExpsThatAreLeft()
{
    size_t numberLeft = m_numberOfRegExpsWaiting.load();
    bool hasStack = hasStackForRegExpCompiler();
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: regular expressions left for finish(): ", numberLeft, ", compiled on ", hasStack ? "this thread" : "a thread made for them");
    if (!numberLeft)
        return;
    if (hasStack) {
        while (takeAndCompileOneRegExp()) { }
    } else {
        Thread::create("AOT compiler"_s, [this] {
            while (takeAndCompileOneRegExp()) { }
        })->waitForCompletion();
    }
    RELEASE_ASSERT(!m_numberOfRegExpsWaiting.load());
}

Vector<uint8_t> ImageBuilder::finish()
{
    compileRegExpsThatAreLeft();
    Locker locker { m_lock };
    if (Options::verboseAOTCompilation()) [[unlikely]] {
        dataLogLn("AOT: compiled ", m_regExps.size(), " regular expressions, and left ", m_numberOfRegExpsNotCompiled, " to the interpreter");
        static constexpr ASCIILiteral lengths[] = { "below 64"_s, "64 to 255"_s, "256 to 1,023"_s, "1,024 to 4,095"_s, "4,096 and more"_s };
        std::array<std::array<size_t, 3>, 5> weights { };
        std::array<std::array<size_t, 3>, 2> sums { };
        for (auto& regExp : m_regExps) {
            size_t code = regExp.code[0].bytes.size() + regExp.code[1].bytes.size();
            size_t text = regExp.pattern.sizeInBytes();
            unsigned length = regExp.pattern.length();
            auto add = [&](std::array<size_t, 3>& weight) {
                weight[0] += 1;
                weight[1] += code;
                weight[2] += text;
            };
            add(sums[regExp.isFoundByEvaluator]);
            if (regExp.isFoundByEvaluator)
                add(weights[length < 64 ? 0 : length < 256 ? 1 : length < 1024 ? 2 : length < 4096 ? 3 : 4]);
        }
        for (unsigned i = 0; i < 5; ++i)
            dataLogLn("AOT: evaluator of texts: handed over and taken, of ", lengths[i], " characters: ", weights[i][0], " expressions, ", weights[i][1], " B of code, ", weights[i][2], " B of text");
        dataLogLn("AOT: regular expressions written as constants: ", sums[0][0], ": ", sums[0][1], " B of code, ", sums[0][2], " B of text; by the evaluator of texts: ", sums[1][0], ": ", sums[1][1], " B of code, ", sums[1][2], " B of text");
        dataLogLn("AOT: Yarr's compiler took ", static_cast<unsigned>(m_timeInRegExpCompiler.milliseconds()), " ms in all, on ", m_threadsThatCompiledRegExps.size(), " threads");
    }
    MonotonicTime stepStart = MonotonicTime::now();
    auto endStep = [&](ASCIILiteral name) {
        if (!Typed::saysClocks()) [[likely]]
            return;
        MonotonicTime now = MonotonicTime::now();
        dataLogLn("AOT: - ", name, " took ", static_cast<unsigned>((now - stepStart).milliseconds()), " ms");
        stepStart = now;
    };
    auto takeInOrder = [&](const Vector<uint32_t>& order) {
        Vector<Function> taken;
        growOnPagesTouchedByPool(taken, order.size());
        Typed::forEachIndexInParallel(order.size(), [&](unsigned index) {
            taken[index] = WTF::move(m_functions[order[index]]);
        });
        Typed::destroyBeside(std::exchange(m_functions, WTF::move(taken)));
    };
    {
        struct PlaceInOrder {
            uint64_t rank;
            uint32_t module;
            uint32_t start;
            uint32_t kind;
            uint32_t index;
        };
        Vector<PlaceInOrder> places(m_functions.size());
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            auto& function = m_functions[index];
            places[index] = { function.rank, function.key.module, function.key.start, function.key.kind, index };
        });
        auto isAhead = [](const PlaceInOrder& a, const PlaceInOrder& b) {
            return std::tie(a.rank, a.module, a.start, a.kind) < std::tie(b.rank, b.module, b.start, b.kind);
        };
        bool standsInOrder = true;
        for (size_t index = 1; index < places.size() && standsInOrder; ++index)
            standsInOrder = isAhead(places[index - 1], places[index]);
        if (!standsInOrder) {
            std::ranges::sort(places, [](const PlaceInOrder& a, const PlaceInOrder& b) {
                return std::tie(a.rank, a.module, a.start, a.kind, a.index) < std::tie(b.rank, b.module, b.start, b.kind, b.index);
            });
            takeInOrder(WTF::map(places, [](const PlaceInOrder& place) {
                return place.index;
            }));
        }
    }

    endStep("the functions in order"_s);
    unsigned capacity = 16;
    while (capacity * 3 < m_functions.size() * 4)
        capacity *= 2;

    const IdentifierIndices* identifierIndices = m_programIdentifierIndices;
    for (auto& function : m_functions) {
        auto& info = function.code.info;
        RELEASE_ASSERT(info.siteConstants.size() == info.numSlots);
        for (unsigned slot = 0; slot < info.numSlots; ++slot) {
            uint32_t& constant = info.siteConstants[slot];
            if (!constant)
                continue;
            RELEASE_ASSERT(constant & CompiledFunctionInfo::siteConstantIsPlan);
            constant &= ~CompiledFunctionInfo::siteConstantIsPlan;
            if (identifierIndices) {
                RELEASE_ASSERT(!info.sites[slot].identifierAndExtra);
                info.sites[slot].identifierAndExtra = constant;
            }
        }
    }
    endStep("the plans at the sites of the functions"_s);
    struct EntryInsideFunction {
        ImageKey key;
        Convention convention;
        ImageKey function;
        size_t offset;
    };
    Vector<EntryInsideFunction> entriesInsideFunctions;
    if (m_numberOfProgramIdentifiers) {
#if CPU(ARM64)
        constexpr size_t sizeOfJump = 4;
#else
        constexpr size_t sizeOfJump = 5;
#endif
        constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
        Vector<std::array<uint32_t, 4>> keysAndIndices(m_functions.size(), [&](size_t index) {
            auto& key = m_functions[index].key;
            return std::array<uint32_t, 4> { key.module, key.start, key.kind, static_cast<uint32_t>(index) };
        });
        std::ranges::sort(keysAndIndices);
        auto hasKey = [](const std::array<uint32_t, 4>& entry, const ImageKey& key) {
            return entry[0] == key.module && entry[1] == key.start && entry[2] == key.kind;
        };
        BitVector isDuplicated(m_functions.size());
        for (size_t i = 1; i < keysAndIndices.size(); ++i) {
            if (hasKey(keysAndIndices[i], m_functions[keysAndIndices[i - 1][3]].key)) {
                isDuplicated.set(keysAndIndices[i - 1][3]);
                isDuplicated.set(keysAndIndices[i][3]);
            }
        }
        Vector<uint32_t> whereTypedFunctionStands;
        for (auto& entry : keysAndIndices | std::views::reverse) {
            if (entry[0] || entry[2] != ImageKey::typedFunction)
                continue;
            if (whereTypedFunctionStands.isEmpty())
                whereTypedFunctionStands.fill(none, entry[1] + 1);
            whereTypedFunctionStands[entry[1]] = entry[3];
        }
        auto indexOf = [&](const ImageKey& key) {
            if (!key.module && key.kind == ImageKey::typedFunction)
                return key.start < whereTypedFunctionStands.size() ? whereTypedFunctionStands[key.start] : none;
            auto found = std::ranges::lower_bound(keysAndIndices, std::array<uint32_t, 4> { key.module, key.start, key.kind, 0 });
            return found != keysAndIndices.end() && hasKey(*found, key) ? (*found)[3] : none;
        };
        Vector<uint32_t> jumpsTo(FillWith { }, m_functions.size(), none);
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            auto& function = m_functions[index];
            auto& info = function.code.info;
            if (function.key.kind != ImageKey::typedFunction || isDuplicated.get(index) || !info.hasNoFrame || info.stubCalls.size() != 1 || info.numSlots || info.typedPositions.size() != 1
                || !info.callSites.isEmpty() || !info.inlineFrames.isEmpty() || !info.indexReferences.isEmpty() || !info.faultingAccesses.isEmpty() || !info.spreadSites.isEmpty() || !info.plans.isEmpty())
                return;
            auto& jump = info.stubCalls[0];
            if (!jump.isTailCall || jump.thunk || jump.function == StubCall::noFunction || jump.offset + sizeOfJump > function.code.bytes.size())
                return;
            auto behindJump = function.code.bytes.span().subspan(jump.offset + sizeOfJump);
#if CPU(X86_64)
            while (!behindJump.empty() && behindJump.front() == 0xcc)
                behindJump = behindJump.subspan(1);
            std::array<uint8_t, imageFunctionAlignment> padding;
            if (behindJump.size() >= padding.size())
                return;
            X86Assembler::fillNops(padding.data(), behindJump.size());
            if (!equalSpans(behindJump, std::span<const uint8_t> { padding }.first(behindJump.size())))
                return;
#else
            if (!behindJump.empty())
                return;
#endif
            uint32_t callee = indexOf(info.knownCallees[jump.function]);
            if (callee != none && callee != index && !isDuplicated.get(callee))
                jumpsTo[index] = callee;
        });
        auto isOneJump = [&](uint32_t index) {
            return jumpsTo[index] != none && !m_functions[index].code.info.stubCalls[0].offset;
        };
        auto behindJumps = [&](uint32_t index) {
            for (unsigned steps = 0; steps < 16 && isOneJump(index); ++steps)
                index = jumpsTo[index];
            return index;
        };
        Vector<uint32_t> uses(FillWith { }, m_functions.size(), 0u);
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            for (auto& key : m_functions[index].code.info.knownCallees) {
                uint32_t callee = indexOf(key);
                if (callee == none)
                    continue;
                if (uint32_t last = behindJumps(callee); last != callee && !isOneJump(last)) {
                    key = m_functions[last].key;
                    callee = last;
                }
                WTF::atomicExchangeAdd(&uses[callee], 1u, std::memory_order_relaxed);
            }
        });
        BitVector isRemoved(m_functions.size());
        size_t numberOfStarts = 0;
        uint32_t ahead = none;
        for (uint32_t index = 0; index < m_functions.size(); ++index) {
            uint32_t function = jumpsTo[index] == none || uses[index] ? none : behindJumps(jumpsTo[index]);
            if (function == none || jumpsTo[function] != none || (!isOneJump(index) && (ahead == none || isDuplicated.get(ahead)))) {
                ahead = index;
                continue;
            }
            auto& entry = m_functions[index];
            isRemoved.set(index);
            if (isOneJump(index)) {
                ++numberOfStarts;
                entriesInsideFunctions.append({ entry.key, entry.code.info.convention, m_functions[function].key, 0 });
                continue;
            }
            auto& host = m_functions[ahead];
            size_t offset = WTF::roundUpToMultipleOf<imageFunctionAlignment>(host.code.bytes.size());
            if (!host.code.instructions.isEmpty() && host.code.bytes.size() < offset)
                host.code.instructions.append(static_cast<uint8_t>((offset - host.code.bytes.size()) | 0x80));
            static_assert(imageFunctionAlignment <= 16);
            host.code.startOfGuests = std::min(host.code.startOfGuests, safeCast<uint32_t>(offset));
            host.code.instructions.appendVector(entry.code.instructions);
            host.code.hasNotes &= entry.code.hasNotes;
            for (DataInCode data : entry.code.dataInCode) {
                data.offset += offset;
                host.code.dataInCode.append(data);
            }
            while (host.code.bytes.size() < offset)
                host.code.bytes.append(0);
            host.code.bytes.appendVector(entry.code.bytes);
            StubCall jump = entry.code.info.stubCalls[0];
            jump.offset += offset;
            size_t callee = host.code.info.knownCallees.findIf([&](const ImageKey& key) {
                return key.sameFunction(m_functions[function].key);
            });
            if (callee == notFound) {
                callee = host.code.info.knownCallees.size();
                host.code.info.knownCallees.append(m_functions[function].key);
            }
            jump.function = safeCast<uint32_t>(callee);
            host.code.info.stubCalls.append(jump);
            entriesInsideFunctions.append({ entry.key, entry.code.info.convention, host.key, offset });
        }
        if (!entriesInsideFunctions.isEmpty()) {
            Vector<uint32_t> kept;
            kept.reserveInitialCapacity(m_functions.size());
            for (uint32_t index = 0; index < m_functions.size(); ++index) {
                if (!isRemoved.get(index))
                    kept.append(index);
            }
            takeInOrder(kept);
        }
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", entriesInsideFunctions.size(), " functions that only jump to another have no rows: ", numberOfStarts, " are the start of that one, ", entriesInsideFunctions.size() - numberOfStarts, " are the tail of the one ahead of them");
    }

    endStep("entries inside functions"_s);
    struct FoldedFunction {
        ImageKey key;
        ImageKey keptFunction;
        size_t size;
        String nameForMap;
    };
    Vector<FoldedFunction> foldedFunctions;
    if (m_numberOfProgramIdentifiers) {
        constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
        Vector<std::array<uint32_t, 4>> keysAndIndices(m_functions.size(), [&](size_t index) {
            auto& key = m_functions[index].key;
            return std::array<uint32_t, 4> { key.module, key.start, key.kind, static_cast<uint32_t>(index) };
        });
        std::ranges::sort(keysAndIndices);
        auto hasKey = [](const std::array<uint32_t, 4>& entry, const ImageKey& key) {
            return entry[0] == key.module && entry[1] == key.start && entry[2] == key.kind;
        };
        BitVector isDuplicated(m_functions.size());
        for (size_t i = 1; i < keysAndIndices.size(); ++i) {
            if (hasKey(keysAndIndices[i], m_functions[keysAndIndices[i - 1][3]].key)) {
                isDuplicated.set(keysAndIndices[i - 1][3]);
                isDuplicated.set(keysAndIndices[i][3]);
            }
        }
        Vector<uint32_t> whereTypedFunctionStands;
        for (auto& entry : keysAndIndices | std::views::reverse) {
            if (entry[0] || entry[2] != ImageKey::typedFunction)
                continue;
            if (whereTypedFunctionStands.isEmpty())
                whereTypedFunctionStands.fill(none, entry[1] + 1);
            whereTypedFunctionStands[entry[1]] = entry[3];
        }
        auto indexOf = [&](const ImageKey& key) {
            if (!key.module && key.kind == ImageKey::typedFunction)
                return key.start < whereTypedFunctionStands.size() ? whereTypedFunctionStands[key.start] : none;
            auto found = std::ranges::lower_bound(keysAndIndices, std::array<uint32_t, 4> { key.module, key.start, key.kind, 0 });
            return found != keysAndIndices.end() && hasKey(*found, key) ? (*found)[3] : none;
        };
        Vector<uint32_t> candidates;
        Vector<Vector<uint64_t, 8>> factsOf(m_functions.size());
        Vector<uint8_t> isCandidate(FillWith { }, m_functions.size(), static_cast<uint8_t>(0));
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            auto& function = m_functions[index];
            auto& info = function.code.info;
            if (function.key.kind != ImageKey::typedFunction || isDuplicated.get(index) || (!info.hasNoFrame && !info.isGenericEntrypoint) || info.numSlots || !function.code.continuations.isEmpty() || !info.callSites.isEmpty()
                || !info.typedCatchEntrypointsForCallSites.isEmpty() || !info.inlineFrames.isEmpty() || !info.indexReferences.isEmpty() || !info.faultingAccesses.isEmpty() || !info.spreadSites.isEmpty() || !info.plans.isEmpty())
                return;
            bool isNeverAsked = std::ranges::all_of(info.stubCalls, [&](const StubCall& call) {
                return call.callSite == StubCall::noCallSite || (info.isGenericEntrypoint && call.function != StubCall::noFunction);
            });
            if (!isNeverAsked)
                return;
            isCandidate[index] = 1;
            auto& facts = factsOf[index];
            facts.append(info.hasNoFrame);
            if (!info.hasNoFrame) {
                facts.appendList(std::initializer_list<uint64_t> { info.frameSizeInBytes, info.numberOfSitesWithoutCode, static_cast<uint64_t>(info.convention.signature), info.convention.numberOfParameters, info.convention.usesThis, info.kindByte, info.calleeSaveRegisters.registerCount() });
                for (auto& entry : info.calleeSaveRegisters)
                    facts.appendList(std::initializer_list<uint64_t> { entry.reg().index(), static_cast<uint64_t>(entry.offset()) });
            }
            for (auto& call : info.stubCalls)
                facts.appendList(std::initializer_list<uint64_t> { call.offset, static_cast<uint64_t>(call.stub), call.isTailCall, call.thunk, call.function == StubCall::noFunction, call.callSite, call.argumentMoves == StubCall::noArgumentMoves ? 0 : info.argumentMoves[call.argumentMoves].size(), call.firstOmittedGPR, call.endOfOmittedGPRs });
            for (auto& call : info.stubCalls) {
                if (call.argumentMoves != StubCall::noArgumentMoves)
                    facts.appendVector(info.argumentMoves[call.argumentMoves]);
            }
        });
        for (uint32_t index = 0; index < m_functions.size(); ++index) {
            if (isCandidate[index])
                candidates.append(index);
        }
        auto compareWithoutCallees = [&](uint32_t first, uint32_t second) {
            auto& a = m_functions[first].code;
            auto& b = m_functions[second].code;
            if (a.bytes.size() != b.bytes.size())
                return a.bytes.size() < b.bytes.size() ? -1 : 1;
            if (int order = memcmp(a.bytes.span().data(), b.bytes.span().data(), a.bytes.size()))
                return order;
            auto order = std::lexicographical_compare_three_way(factsOf[first].begin(), factsOf[first].end(), factsOf[second].begin(), factsOf[second].end());
            return order < 0 ? -1 : order > 0;
        };
        Vector<uint32_t> classOf(m_functions.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        std::ranges::sort(candidates, [&](uint32_t first, uint32_t second) {
            int order = compareWithoutCallees(first, second);
            return order ? order < 0 : first < second;
        });
        for (size_t i = 1; i < candidates.size(); ++i) {
            if (!compareWithoutCallees(candidates[i - 1], candidates[i]))
                classOf[candidates[i]] = classOf[candidates[i - 1]];
        }
        Vector<Vector<uint32_t, 2>> calleesOfCandidate(m_functions.size());
        Typed::forEachIndexInParallel(candidates.size(), [&](unsigned candidate) {
            uint32_t index = candidates[candidate];
            auto& info = m_functions[index].code.info;
            for (auto& call : info.stubCalls) {
                if (call.function != StubCall::noFunction)
                    calleesOfCandidate[index].append(indexOf(info.knownCallees[call.function]));
            }
        });
        for (bool changed = true; changed;) {
            changed = false;
            auto compare = [&](uint32_t first, uint32_t second) {
                if (classOf[first] != classOf[second])
                    return classOf[first] < classOf[second] ? -1 : 1;
                for (size_t i = 0; i < calleesOfCandidate[first].size(); ++i) {
                    uint32_t a = calleesOfCandidate[first][i];
                    uint32_t b = calleesOfCandidate[second][i];
                    a = a == none ? none : classOf[a];
                    b = b == none ? none : classOf[b];
                    if (a != b)
                        return a < b ? -1 : 1;
                }
                return 0;
            };
            std::ranges::sort(candidates, [&](uint32_t first, uint32_t second) {
                int order = compare(first, second);
                return order ? order < 0 : first < second;
            });
            Vector<uint32_t> next = classOf;
            for (size_t i = 0; i < candidates.size(); ++i) {
                next[candidates[i]] = i && !compare(candidates[i - 1], candidates[i]) ? next[candidates[i - 1]] : candidates[i];
                changed |= next[candidates[i]] != classOf[candidates[i]];
            }
            classOf = WTF::move(next);
        }
        auto keptKeyFor = [&](ImageKey& key) {
            if (uint32_t index = indexOf(key); index != none && classOf[index] != index)
                key = m_functions[classOf[index]].key;
        };
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            for (auto& key : m_functions[index].code.info.knownCallees)
                keptKeyFor(key);
        });
        for (auto& entry : entriesInsideFunctions)
            keptKeyFor(entry.function);
        size_t bytes = 0;
        Vector<ImageKey> keyOf = WTF::map(m_functions, [](const Function& function) {
            return function.key;
        });
        uint32_t numberKept = 0;
        for (uint32_t index = 0; index < m_functions.size(); ++index) {
            auto& function = m_functions[index];
            if (classOf[index] == index) {
                if (numberKept != index)
                    m_functions[numberKept] = WTF::move(function);
                ++numberKept;
                continue;
            }
            ImageKey keptFunction = keyOf[classOf[index]];
            bytes += function.code.bytes.size();
            entriesInsideFunctions.append({ function.key, function.code.info.convention, keptFunction, 0 });
            foldedFunctions.append({ function.key, keptFunction, function.code.bytes.size(), WTF::move(function.nameForMap) });
        }
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", foldedFunctions.size(), " functions have the code of another: ", bytes, " bytes");
        m_functions.shrink(numberKept);
    }

    endStep("folding functions"_s);
    Vector<uint32_t> functionWithKey;
    functionWithKey.fill(std::numeric_limits<uint32_t>::max(), capacity);
    for (size_t index = 0; index < m_functions.size(); ++index) {
        unsigned bucket = m_functions[index].key.hash() & (capacity - 1);
        while (functionWithKey[bucket] != std::numeric_limits<uint32_t>::max() && !m_functions[functionWithKey[bucket]].key.sameFunction(m_functions[index].key))
            bucket = (bucket + 1) & (capacity - 1);
        if (functionWithKey[bucket] == std::numeric_limits<uint32_t>::max())
            functionWithKey[bucket] = index;
    }
    endStep("the functions by key"_s);
    // By function: where each of its known callees is among the functions.
    Vector<Vector<uint32_t>> calleesOf(m_functions.size());
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        calleesOf[index] = WTF::map(m_functions[index].code.info.knownCallees, [&](const ImageKey& key) {
            for (unsigned bucket = key.hash() & (capacity - 1); functionWithKey[bucket] != std::numeric_limits<uint32_t>::max(); bucket = (bucket + 1) & (capacity - 1)) {
                if (m_functions[functionWithKey[bucket]].key.sameFunction(key))
                    return functionWithKey[bucket];
            }
            return ImageFunction::noSuchFunction;
        });
    });
    endStep("the callees of each function"_s);
    auto directTargetOf = [&](size_t caller, const StubCall& call) -> std::optional<size_t> {
        if (call.function == StubCall::noFunction)
            return std::nullopt;
        if (uint32_t target = calleesOf[caller][call.function]; target != ImageFunction::noSuchFunction)
            return target;
        const ImageKey& key = m_functions[caller].code.info.knownCallees[call.function];
        dataLogLn("AOT: there is no code for @", key.module, ":", key.start, ":", key.kind, ", which something calls without asking");
        RELEASE_ASSERT_NOT_REACHED();
        return std::nullopt;
    };

    {
        constexpr uint32_t notSeen = std::numeric_limits<uint32_t>::max();
        constexpr uint32_t isUnderWay = notSeen - 1;
        Vector<uint32_t> stackTaken(FillWith { }, m_functions.size(), notSeen);
        struct Visit {
            uint32_t function;
            uint32_t nextCall;
            uint32_t below;
            uint32_t own;
            bool isOnRingWithoutStack;
        };
        Vector<Visit> visits;
        auto refuse = [&](uint32_t function, ASCIILiteral why) {
            auto& key = m_functions[function].key;
            m_whyFailed = makeString("internal error: @"_s, key.module, ':', key.start, ':', key.kind, " is reached from a function that does not check for stack overflow, and "_s, why);
        };
        auto frameOf = [&](uint32_t function) {
            auto& info = m_functions[function].code.info;
            return info.hasNoFrame ? 0 : info.frameSizeInBytes + static_cast<uint32_t>(sizeof(CallerFrameAndPC));
        };
        auto takenAtEntry = [&](uint32_t function, bool isJump) {
            return m_functions[function].code.info.hasNoFrame && !isJump ? static_cast<uint32_t>(sizeof(void*)) : 0;
        };
        for (uint32_t root = 0; root < m_functions.size(); ++root) {
            if (!m_functions[root].code.info.fitsOnStackByProof || stackTaken[root] != notSeen)
                continue;
            stackTaken[root] = isUnderWay;
            visits.append({ root, 0, 0, frameOf(root), false });
            while (!visits.isEmpty()) {
                Visit visit = visits.last();
                auto& code = m_functions[visit.function].code;
                if (visit.nextCall < code.info.stubCalls.size()) {
                    ++visits.last().nextCall;
                    auto& call = code.info.stubCalls[visit.nextCall];
                    if (call.offset >= code.startOfGuests)
                        continue;
                    auto target = directTargetOf(visit.function, call);
                    if (!target || !m_functions[*target].code.info.isEnteredWithoutStackCheck)
                        continue;
                    uint32_t atEntry = takenAtEntry(static_cast<uint32_t>(*target), call.isTailCall);
                    uint32_t taken = stackTaken[*target];
                    if (taken == isUnderWay) {
                        uint32_t aroundOnce = atEntry;
                        size_t head = visits.size();
                        while (visits[--head].function != *target)
                            aroundOnce += visits[head].own;
                        aroundOnce += frameOf(static_cast<uint32_t>(*target));
                        if (!aroundOnce) {
                            for (size_t i = head + 1; i < visits.size(); ++i)
                                visits[i].isOnRingWithoutStack = true;
                            continue;
                        }
                        refuse(static_cast<uint32_t>(*target), "neither does any function on a way from it back to it"_s);
                        return { };
                    }
                    if (taken == notSeen) {
                        stackTaken[*target] = isUnderWay;
                        visits.append({ static_cast<uint32_t>(*target), 0, 0, frameOf(static_cast<uint32_t>(*target)) + atEntry, false });
                        continue;
                    }
                    visits.last().below = std::max(visits.last().below, taken + atEntry);
                    continue;
                }
                uint32_t taken = visit.below + visit.own;
                if (taken > maxStackTakenWithoutCheck) {
                    refuse(visit.function, "the frames from it on take more than is set aside for them"_s);
                    return { };
                }
                stackTaken[visit.function] = visit.isOnRingWithoutStack ? notSeen : visit.below + frameOf(visit.function);
                visits.removeLast();
                if (!visits.isEmpty())
                    visits.last().below = std::max(visits.last().below, taken);
            }
        }
    }
    endStep("the stack that is taken without asking"_s);

    if (Options::verboseAOTCompilation()) [[unlikely]] {
        unsigned inPlace = 0;
        unsigned byStub = 0;
        for (auto& function : m_functions) {
            inPlace += function.code.info.numberOfEmptyArrayLiteralsMadeInPlace;
            byStub += function.code.info.numberOfEmptyArrayLiteralsMadeByStub;
        }
        dataLogLn("AOT: empty array literals that are pushed to and born contiguous: ", inPlace, " are made in place, ", byStub, " by the stub");
    }

    Vector<std::array<uint8_t, 2>> omittedArgumentsOf(FillWith { }, m_functions.size(), std::array<uint8_t, 2> { });
    for (size_t index = 0; index < m_functions.size(); ++index) {
        for (auto& call : m_functions[index].code.info.stubCalls) {
            if (call.firstOmittedGPR == call.endOfOmittedGPRs)
                continue;
            auto& [first, end] = omittedArgumentsOf[*directTargetOf(index, call)];
            first = end ? std::min(first, call.firstOmittedGPR) : call.firstOmittedGPR;
            end = std::max(end, call.endOfOmittedGPRs);
        }
    }
    auto sizeAheadOf = [&](size_t index) {
        return sizeOfOmittedArguments(omittedArgumentsOf[index][0], omittedArgumentsOf[index][1]);
    };
    auto sizeAheadFor = [&](size_t target, const StubCall& call) -> size_t {
        return call.firstOmittedGPR == call.endOfOmittedGPRs ? 0 : sizeOfOmittedArguments(call.firstOmittedGPR, omittedArgumentsOf[target][1]);
    };
    constexpr unsigned shiftOfEntryInFarCallee = 27;
    RELEASE_ASSERT(!(m_functions.size() >> shiftOfEntryInFarCallee));
    auto farCalleeOf = [&](size_t target, const StubCall& call) {
        return static_cast<uint32_t>(target) | (call.firstOmittedGPR == call.endOfOmittedGPRs ? 0 : call.firstOmittedGPR + 1u) << shiftOfEntryInFarCallee;
    };
    endStep("inlining"_s);
    Outlining outlining;
    auto indexOfFunctionWithKey = [&](const ImageKey& key) -> uint32_t {
        for (unsigned bucket = key.hash() & (capacity - 1); functionWithKey[bucket] != std::numeric_limits<uint32_t>::max(); bucket = (bucket + 1) & (capacity - 1)) {
            if (m_functions[functionWithKey[bucket]].key.sameFunction(key))
                return functionWithKey[bucket];
        }
        return ImageFunction::noSuchFunction;
    };
    Vector<Vector<uint32_t, 0>> entriesInsideOf;
    auto findEntriesInside = [&] {
        if (!entriesInsideOf.isEmpty())
            return;
        entriesInsideOf.grow(m_functions.size());
        for (auto& entry : entriesInsideFunctions) {
            if (uint32_t index = indexOfFunctionWithKey(entry.function); entry.offset && index != ImageFunction::noSuchFunction)
                entriesInsideOf[index].append(safeCast<uint32_t>(entry.offset));
        }
    };
    auto firstGuestIn = [&](unsigned index) {
        uint32_t first = std::numeric_limits<uint32_t>::max();
        for (uint32_t offset : entriesInsideOf[index])
            first = std::min(first, offset);
        return first;
    };
    {
        Vector<std::pair<uint32_t, uint32_t>> entries;
        for (auto& entry : entriesInsideFunctions) {
            if (uint32_t index = indexOfFunctionWithKey(entry.function); index != ImageFunction::noSuchFunction)
                entries.append({ index, safeCast<uint32_t>(entry.offset) });
        }
        outlining = chooseWindows(WTF::map(m_functions, [](const Function& function) {
            return &function.code;
        }), WTF::move(entries));
        endStep("choosing what is outlined"_s);
    }
    static const char* const outlinesForTesting = byteCast<char>(Options::aotOutlineForTesting());
    if (outlinesForTesting && outlining.bodies.isEmpty()) {
        const bool takesCalls = !!strchr(outlinesForTesting, 'k');
        struct Found {
            OutlinedPlace place;
            bool endsInCall;
            Vector<uint64_t> words;
        };
        struct WordsAreInOrder {
            bool operator()(const Vector<uint64_t>& a, const Vector<uint64_t>& b) const { return std::ranges::lexicographical_compare(a, b); }
        };
        findEntriesInside();
        Vector<Vector<Found>> foundIn(m_functions.size());
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            auto& code = m_functions[index].code;
            auto& info = code.info;
            if (!canBeWalked(code) || info.typedFunction == FunctionReportableSites::notTyped || (isARM64() && info.hasNoFrame))
                return;
            static const char* const forgetsForTesting = byteCast<char>(Options::aotOutlinerFaultForTesting());
            const uint32_t firstGuest = isARM64() && !(forgetsForTesting && !strcmp(forgetsForTesting, "guest")) ? firstGuestIn(index) : std::numeric_limits<uint32_t>::max();
            BitVector isTarget(code.bytes.size() + 1);
            BitVector isBarred(code.bytes.size() + 8);
            forEachInstructionInCode(code, [&](const InstructionInCode& instruction) {
                using Kind = InstructionInCode::Kind;
                if (instruction.kind == Kind::Jump || instruction.kind == Kind::ConditionalJump || instruction.kind == Kind::AddressOfTable || instruction.kind == Kind::TableEntry)
                    isTarget.set(instruction.target);
            });
            for (auto& entry : info.typedCatchEntrypointsForCallSites)
                isTarget.set(entry.second);
            for (uint32_t offset : entriesInsideOf[index])
                isTarget.set(offset);
            for (auto& reference : info.indexReferences) {
                if (isARM64()) {
                    isBarred.set(reference.offset);
                    isBarred.set(reference.offset + sizeof(uint32_t));
                } else
                    isBarred.set(reference.offset - 1);
            }
            for (auto& access : info.faultingAccesses)
                isBarred.set(access.codeOffset);
            Vector<std::pair<uint32_t, uint32_t>, 16> run;
            auto take = [&](size_t first, size_t count, const StubCall* call) {
                uint32_t offset = run[first].first;
                uint32_t length = run[first + count - 1].first + run[first + count - 1].second - offset + (call ? sizeOfNearCall : 0);
                if (length <= sizeOfNearCall)
                    return;
                Found found { { offset, length, 0 }, !!call, { } };
                found.words.append(static_cast<uint64_t>(length) << 1 | !!call);
                for (uint32_t i = 0; i < length; ++i)
                    found.words.append(code.bytes[offset + i]);
                if (call) {
                    found.words.append(static_cast<uint64_t>(call->stub) << 32 | static_cast<uint64_t>(call->thunk) << 2 | (call->function != StubCall::noFunction) << 1 | (call->argumentMoves != StubCall::noArgumentMoves));
                    found.words.append(static_cast<uint64_t>(call->firstOmittedGPR) << 8 | call->endOfOmittedGPRs);
                    if (call->function != StubCall::noFunction) {
                        auto& key = info.knownCallees[call->function];
                        found.words.append(static_cast<uint64_t>(key.module) << 32 | key.start);
                        found.words.append(static_cast<uint64_t>(key.kind) << 1 | 1);
                    }
                    if (call->argumentMoves != StubCall::noArgumentMoves) {
                        for (uint64_t word : info.argumentMoves[call->argumentMoves])
                            found.words.append(word);
                    }
                }
                foundIn[index].append(WTF::move(found));
            };
            auto flush = [&](const StubCall* call) {
                size_t forCall = call ? std::min<size_t>(run.size(), 2) : 0;
                for (size_t first = 0; first < run.size() - forCall; first += 3)
                    take(first, std::min<size_t>(3, run.size() - forCall - first), nullptr);
                if (forCall)
                    take(run.size() - forCall, forCall, call);
                run.shrink(0);
            };
            forEachInstructionInCode(code, [&](const InstructionInCode& instruction) {
                using Kind = InstructionInCode::Kind;
                bool isBarredHere = instruction.offset >= firstGuest;
                for (uint32_t i = 0; i < instruction.length; ++i)
                    isBarredHere |= isBarred.get(instruction.offset + i);
                if (isTarget.get(instruction.offset))
                    flush(nullptr);
                if ((instruction.kind == Kind::Plain || (instruction.kind == Kind::Pinned && forgetsForTesting && !strcmp(forgetsForTesting, "pinned"))) && !isBarredHere) {
                    run.append({ instruction.offset, instruction.length });
                    return;
                }
                const StubCall* call = instruction.kind == Kind::StubCall && takesCalls && !isBarredHere ? &info.stubCalls[instruction.target] : nullptr;
                if (call && (call->isTailCall || call->body || (isARM64() && call->function != StubCall::noFunction)))
                    call = nullptr;
                flush(call);
            });
            flush(nullptr);
        });
        outlining.places.grow(m_functions.size());
        std::map<Vector<uint64_t>, uint32_t, WordsAreInOrder> bodyOfWords;
        for (uint32_t index = 0; index < m_functions.size(); ++index) {
            for (auto& found : foundIn[index]) {
                auto [entry, isNew] = bodyOfWords.try_emplace(WTF::move(found.words), static_cast<uint32_t>(outlining.bodies.size()));
                if (isNew)
                    outlining.bodies.append({ index, found.place.offset, found.place.length, 0, index, found.endsInCall });
                auto& body = outlining.bodies[entry->second];
                ++body.numberOfPlaces;
                if (body.onlyCaller != index)
                    body.onlyCaller = Outlining::none;
                found.place.body = entry->second;
                outlining.places[index].append(found.place);
            }
        }
    }
    OutlinedBodies sharedBodies;
    OutlinedBodies ownBodies;
    Vector<uint32_t> numberOfBodyAmongItsLike;
    Vector<Vector<uint32_t, 0>> ownBodiesOf(m_functions.size());
    Vector<ImageKey> calleesOfBodies;
    Vector<ArgumentMoves> argumentMovesOfBodies;
    Vector<Vector<std::pair<uint32_t, uint32_t>, 0>> fillingsAheadOfEntries(m_functions.size());
    if (!outlining.bodies.isEmpty()) {
        RELEASE_ASSERT(outlining.places.size() == m_functions.size());
        findEntriesInside();
        for (auto& body : outlining.bodies) {
            auto& from = m_functions[body.function].code;
            RELEASE_ASSERT(body.length >= sizeOfNearCall && body.offset + body.length <= from.bytes.size());
            auto& like = body.onlyCaller == Outlining::none ? sharedBodies : ownBodies;
            numberOfBodyAmongItsLike.append(like.offsets.size());
            if (body.onlyCaller != Outlining::none)
                ownBodiesOf[body.onlyCaller].append(like.offsets.size());
            uint32_t at = safeCast<uint32_t>(like.bytes.size());
            like.offsets.append(at);
            like.bytes.append(from.bytes.span().subspan(body.offset, body.length));
            StubCall jump { };
            jump.offset = OutlinedBodies::noJump;
            if (body.endsInCall) {
                size_t call = from.info.stubCalls.findIf([&](const StubCall& call) {
                    return call.offset + sizeOfNearCall == body.offset + body.length;
                });
                RELEASE_ASSERT(call != notFound && !from.info.stubCalls[call].isTailCall && !from.info.stubCalls[call].body);
                jump = from.info.stubCalls[call];
                jump.offset = at + body.length - static_cast<uint32_t>(sizeOfNearCall);
                jump.isTailCall = true;
                jump.callSite = StubCall::noCallSite;
                RELEASE_ASSERT(!isARM64() || jump.function == StubCall::noFunction);
                if (jump.function != StubCall::noFunction) {
                    calleesOfBodies.append(from.info.knownCallees[jump.function]);
                    jump.function = calleesOfBodies.size() - 1;
                }
                if (jump.argumentMoves != StubCall::noArgumentMoves) {
                    argumentMovesOfBodies.append(from.info.argumentMoves[jump.argumentMoves]);
                    jump.argumentMoves = argumentMovesOfBodies.size() - 1;
                }
            } else {
#if CPU(X86_64)
                like.bytes.append(0xc3);
#else
                constexpr uint32_t ret = 0xd65f03c0;
                like.bytes.append(std::span { reinterpret_cast<const uint8_t*>(&ret), sizeof(ret) });
#endif
            }
            like.jumps.append(jump);
        }
        struct Shift {
            uint32_t from;
            uint32_t shift;
        };
        static const char* const breaksForTesting = byteCast<char>(Options::aotOutlinerFaultForTesting());
        auto breaks = [](const char* what) {
            return breaksForTesting && !strcmp(breaksForTesting, what);
        };
        Vector<Vector<Shift>> shiftsOf(m_functions.size());
        auto mapped = [](const Vector<Shift>& shifts, uint32_t offset) {
            auto* found = std::upper_bound(shifts.begin(), shifts.end(), offset, [](uint32_t offset, const Shift& entry) {
                return offset < entry.from;
            });
            return offset - (found == shifts.begin() ? 0 : found[-1].shift);
        };
        auto placeWith = [](const Vector<OutlinedPlace>& places, uint32_t firstByte, uint32_t lastByte) -> const OutlinedPlace* {
            auto* found = std::upper_bound(places.begin(), places.end(), firstByte, [](uint32_t byte, const OutlinedPlace& place) {
                return byte < place.offset + place.length;
            });
            return found != places.end() && found->offset <= lastByte ? found : nullptr;
        };
        struct AsItWas {
            Vector<uint8_t> bytes;
            Vector<uint8_t> comparableAt;
            Vector<uint32_t> callsOfBodies;
        };
        constexpr uint8_t isSetAnewOnARM64 = 0xff;
        auto standsWhereItStood = [&](const Vector<uint8_t>& oldBytes, const Vector<uint8_t>& comparableAt, const Vector<uint32_t>& callsOfBodies, const Vector<uint8_t>& newBytes, const Vector<OutlinedPlace>& places, uint32_t oldOffset, uint32_t newOffset) {
            if (auto* place = placeWith(places, oldOffset, oldOffset))
                return place->offset == oldOffset && newOffset == callsOfBodies[place - places.begin()];
            uint8_t comparable = oldOffset < comparableAt.size() ? comparableAt[oldOffset] : 0;
            if (comparable == isSetAnewOnARM64) {
                if (newOffset + sizeof(uint32_t) > newBytes.size())
                    return false;
                uint32_t oldWord;
                uint32_t newWord;
                memcpy(&oldWord, oldBytes.span().data() + oldOffset, sizeof(oldWord));
                memcpy(&newWord, newBytes.span().data() + newOffset, sizeof(newWord));
                uint32_t allButDisplacement = (oldWord & 0x7c000000) == 0x14000000 ? 0xfc000000 : (oldWord & 0x7e000000) == 0x36000000 ? 0xfff8001f : (oldWord & 0x9f000000) == 0x10000000 ? 0x9f00001f : 0xff00001f;
                return !((oldWord ^ newWord) & allButDisplacement);
            }
            return comparable && newOffset + comparable <= newBytes.size() && !memcmp(newBytes.span().data() + newOffset, oldBytes.span().data() + oldOffset, comparable);
        };
        Vector<AsItWas> hostsAsTheyWere(m_functions.size());
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            auto& places = outlining.places[index];
            if (places.isEmpty())
                return;
            auto& code = m_functions[index].code;
            auto& info = code.info;
            auto& shifts = shiftsOf[index];
            RELEASE_ASSERT(info.typedFunction != FunctionReportableSites::notTyped);
            RELEASE_ASSERT(!isARM64() || (!info.hasNoFrame && places.last().offset + places.last().length <= firstGuestIn(index)));
            Vector<uint8_t> bytes;
            bytes.reserveInitialCapacity(code.bytes.size());
            Vector<StubCall> calls;
            Vector<uint32_t> callsOfBodies;
            Vector<uint8_t> comparableAt(FillWith { }, code.bytes.size() + 1, static_cast<uint8_t>(0));
            uint32_t copied = 0;
            uint32_t shift = 0;
            Vector<uint32_t, 0> entries = entriesInsideOf[index];
            std::ranges::sort(entries);
            size_t nextEntry = 0;
            auto copyUpTo = [&](uint32_t end) {
                for (; nextEntry < entries.size() && entries[nextEntry] <= end; ++nextEntry) {
                    uint32_t entry = entries[nextEntry];
                    RELEASE_ASSERT(entry >= copied && !(entry % imageFunctionAlignment));
                    bytes.append(code.bytes.span().subspan(copied, entry - copied));
                    copied = entry;
                    uint32_t filling = breaks("align") ? 0 : static_cast<uint32_t>(WTF::roundUpToMultipleOf<imageFunctionAlignment>(bytes.size()) - bytes.size());
                    RELEASE_ASSERT(filling <= shift);
                    fillingsAheadOfEntries[index].append({ entry, filling });
                    if (!filling)
                        continue;
                    bytes.grow(bytes.size() + filling);
                    memset(bytes.mutableSpan().data() + bytes.size() - filling, 0xcc, filling);
                    shift -= filling;
                    shifts.append({ entry, shift });
                }
                bytes.append(code.bytes.span().subspan(copied, end - copied));
                copied = end;
            };
            for (auto& place : places) {
                auto& body = outlining.bodies[place.body];
                RELEASE_ASSERT(place.offset >= copied && place.length == body.length && place.offset + place.length <= code.bytes.size());
                RELEASE_ASSERT(body.onlyCaller == Outlining::none || body.onlyCaller == index);
                copyUpTo(place.offset);
                StubCall call { };
                call.offset = safeCast<uint32_t>(bytes.size());
                call.isTailCall = false;
                call.body = place.body + 1;
                calls.append(call);
                callsOfBodies.append(call.offset);
                bytes.grow(bytes.size() + sizeOfNearCall);
                memset(bytes.mutableSpan().data() + call.offset, 0, sizeOfNearCall);
                copied = place.offset + place.length;
                shift += place.length - static_cast<uint32_t>(sizeOfNearCall);
                shifts.append({ copied, shift });
            }
            copyUpTo(safeCast<uint32_t>(code.bytes.size()));
            auto map = [&](uint32_t offset) {
                return mapped(shifts, offset);
            };
            auto isAtHeadOrOutside = [&](uint32_t offset) {
                auto* place = placeWith(places, offset, offset);
                return !place || place->offset == offset;
            };
            uint32_t startOfTable = 0;
            bool isInTable = false;
            size_t numberOfHeads = 0;
            forEachInstructionInCode(code, [&](const InstructionInCode& instruction) {
                using Kind = InstructionInCode::Kind;
                if (auto* place = placeWith(places, instruction.offset, instruction.offset + instruction.length - 1)) {
                    bool endsPlace = instruction.offset + instruction.length == place->offset + place->length;
                    bool endsInCall = endsPlace && outlining.bodies[place->body].endsInCall;
                    RELEASE_ASSERT(instruction.offset >= place->offset && instruction.offset + instruction.length <= place->offset + place->length);
                    RELEASE_ASSERT(instruction.kind == (endsInCall ? Kind::StubCall : Kind::Plain));
                    numberOfHeads += instruction.offset == place->offset;
                }
                bool isSetAnew = instruction.kind == Kind::Jump || instruction.kind == Kind::ConditionalJump || instruction.kind == Kind::AddressOfTable;
                comparableAt[instruction.offset] = instruction.kind == Kind::TableEntry ? 0 : !isSetAnew ? instruction.length : isARM64() ? isSetAnewOnARM64 : instruction.length - (instruction.length == 2 ? 1 : sizeof(int32_t));
                if (instruction.kind == Kind::TableEntry && !isInTable)
                    startOfTable = instruction.offset;
                isInTable = instruction.kind == Kind::TableEntry;
                if (instruction.kind != Kind::Jump && instruction.kind != Kind::ConditionalJump && instruction.kind != Kind::AddressOfTable && instruction.kind != Kind::TableEntry)
                    return;
                RELEASE_ASSERT(isAtHeadOrOutside(instruction.target));
                if (instruction.kind == Kind::TableEntry) {
                    int32_t entry;
                    memcpy(&entry, code.bytes.span().data() + instruction.offset, sizeof(entry));
                    RELEASE_ASSERT(instruction.target == startOfTable + entry);
                }
                if ((breaks("jump") && instruction.kind == Kind::ConditionalJump) || (breaks("table") && (instruction.kind == Kind::TableEntry || instruction.kind == Kind::AddressOfTable)))
                    return;
                setTarget(bytes.mutableSpan(), instruction, map(instruction.offset), map(instruction.target), map(startOfTable));
            });
            RELEASE_ASSERT(numberOfHeads == places.size());
            auto stands = [&](uint32_t oldOffset, uint32_t newOffset) {
                return standsWhereItStood(code.bytes, comparableAt, callsOfBodies, bytes, places, oldOffset, newOffset);
            };
            auto referenceStands = [&](uint32_t oldOffset, uint32_t newOffset) {
                if (isARM64())
                    return stands(oldOffset, newOffset) && stands(oldOffset + sizeof(uint32_t), newOffset + sizeof(uint32_t));
                for (uint32_t length = 1; length <= 15 && length <= oldOffset && length <= newOffset; ++length) {
                    if (comparableAt[oldOffset - length] == length)
                        return stands(oldOffset - length, newOffset - length);
                }
                return false;
            };
            for (auto& call : info.stubCalls) {
                if (auto* place = placeWith(places, call.offset, call.offset + sizeOfNearCall - 1)) {
                    RELEASE_ASSERT(outlining.bodies[place->body].endsInCall && !call.isTailCall && !call.body && call.offset + sizeOfNearCall == place->offset + place->length);
                    calls[place - places.begin()].callSite = breaks("site") ? StubCall::noCallSite : call.callSite;
                    continue;
                }
                StubCall kept = call;
                kept.offset = breaks("call") ? call.offset : map(call.offset);
                calls.append(kept);
            }
            std::ranges::sort(calls, { }, &StubCall::offset);
            info.stubCalls = WTF::move(calls);
            for (auto& reference : info.indexReferences) {
                RELEASE_ASSERT(isARM64() ? !placeWith(places, reference.offset, reference.offset + 2 * sizeof(uint32_t) - 1) : !placeWith(places, reference.offset - sizeof(int32_t), reference.offset - 1));
                uint32_t was = reference.offset;
                reference.offset = breaks("reference") ? reference.offset : map(reference.offset);
                RELEASE_ASSERT(referenceStands(was, reference.offset));
            }
            for (auto& access : info.faultingAccesses) {
                RELEASE_ASSERT(!placeWith(places, access.codeOffset, access.codeOffset));
                uint32_t was = access.codeOffset;
                access.codeOffset = breaks("fault") ? access.codeOffset : map(access.codeOffset);
                RELEASE_ASSERT(stands(was, access.codeOffset));
            }
            for (auto& entry : info.typedCatchEntrypointsForCallSites) {
                RELEASE_ASSERT(isAtHeadOrOutside(entry.second));
                uint32_t was = entry.second;
                entry.second = breaks("catch") ? entry.second : map(entry.second);
                RELEASE_ASSERT(stands(was, entry.second));
            }
            info.codeSize = bytes.size();
            if (!entriesInsideOf[index].isEmpty())
                hostsAsTheyWere[index] = { WTF::move(code.bytes), WTF::move(comparableAt), WTF::move(callsOfBodies) };
            code.bytes = WTF::move(bytes);
            code.isRewritten = true;
        });
        for (auto& entry : entriesInsideFunctions) {
            uint32_t index = entry.offset ? indexOfFunctionWithKey(entry.function) : ImageFunction::noSuchFunction;
            if (index == ImageFunction::noSuchFunction || shiftsOf[index].isEmpty())
                continue;
            auto* place = placeWith(outlining.places[index], safeCast<uint32_t>(entry.offset), safeCast<uint32_t>(entry.offset));
            RELEASE_ASSERT(!place || place->offset == entry.offset);
            uint32_t was = safeCast<uint32_t>(entry.offset);
            entry.offset = breaks("entry") ? entry.offset : mapped(shiftsOf[index], safeCast<uint32_t>(entry.offset));
            auto& host = hostsAsTheyWere[index];
            RELEASE_ASSERT(standsWhereItStood(host.bytes, host.comparableAt, host.callsOfBodies, m_functions[index].code.bytes, outlining.places[index], was, safeCast<uint32_t>(entry.offset)));
        }
    }
    endStep("outlining"_s);
    Vector<size_t> endsOfFunctions;
    for (auto& function : m_functions)
        endsOfFunctions.append(WTF::roundUpToMultipleOf<imageFunctionAlignment>(endsOfFunctions.isEmpty() ? 0 : endsOfFunctions.last()) + function.code.bytes.size());
    auto walkIsShort = [&](unsigned shift) {
        size_t startsPassed = 0;
        size_t lookups = 0;
        size_t atStartOfGranule = 0;
        for (size_t index = 0; index < endsOfFunctions.size(); ++index) {
            if (m_functions[index].code.bytes.isEmpty())
                continue;
            size_t startOfGranule = (endsOfFunctions[index] - 1) >> shift << shift;
            while (endsOfFunctions[atStartOfGranule] <= startOfGranule)
                ++atStartOfGranule;
            startsPassed += index - atStartOfGranule;
            ++lookups;
        }
        return 2 * startsPassed <= 5 * lookups;
    };
    unsigned codeGranuleShift = maxCodeGranuleShift;
    while (codeGranuleShift > minCodeGranuleShift && !walkIsShort(codeGranuleShift))
        --codeGranuleShift;
    endStep("the size of a granule of code"_s);
    const StubBlob& stubs = stubBlob(codeGranuleShift, !m_typedData.isEmpty() && reinterpret_cast<const Typed::Data*>(m_typedData.span().data())->functionLengthsAreBytes);
    Vector<const ArgumentMoves*> argumentMoves;
    auto argumentMovesAreInOrder = [](const ArgumentMoves* a, const ArgumentMoves* b) {
        return std::ranges::lexicographical_compare(*a, *b);
    };
    auto sortAndKeepOneOfEach = [&](Vector<const ArgumentMoves*>& list) {
        std::ranges::sort(list, argumentMovesAreInOrder);
        list.shrink(std::unique(list.begin(), list.end(), [](const ArgumentMoves* a, const ArgumentMoves* b) {
            return *a == *b;
        }) - list.begin());
    };
    constexpr size_t functionsInChunk = 1024;
    constexpr unsigned bitsOfShard = 6;
    constexpr unsigned numberOfShards = 1 << bitsOfShard;
    Vector<std::array<Vector<const ArgumentMoves*>, numberOfShards>> argumentMovesOfChunks((m_functions.size() + functionsInChunk - 1) / functionsInChunk);
    Typed::forEachIndexInParallel(argumentMovesOfChunks.size(), [&](unsigned chunk) {
        Vector<const ArgumentMoves*> own;
        for (auto& function : m_functions.span().subspan(chunk * functionsInChunk, std::min(functionsInChunk, m_functions.size() - chunk * functionsInChunk))) {
            for (auto& call : function.code.info.stubCalls) {
                if (call.argumentMoves != StubCall::noArgumentMoves)
                    own.append(&function.code.info.argumentMoves[call.argumentMoves]);
            }
        }
        sortAndKeepOneOfEach(own);
        for (auto* moves : own) {
            uint64_t hash = moves->size();
            for (uint64_t word : *moves)
                hash = (hash ^ word) * 0x9e3779b97f4a7c15ULL;
            argumentMovesOfChunks[chunk][hash >> (64 - bitsOfShard)].append(moves);
        }
    });
    std::array<Vector<const ArgumentMoves*>, numberOfShards> argumentMovesOfShards;
    Typed::forEachIndexInParallel(numberOfShards, [&](unsigned shard) {
        for (auto& ofChunk : argumentMovesOfChunks)
            argumentMovesOfShards[shard].appendVector(ofChunk[shard]);
        sortAndKeepOneOfEach(argumentMovesOfShards[shard]);
    });
    for (auto& ofShard : argumentMovesOfShards)
        argumentMoves.appendVector(ofShard);
    for (auto& moves : argumentMovesOfBodies)
        argumentMoves.append(&moves);
    sortAndKeepOneOfEach(argumentMoves);
    auto numberOfArgumentMoves = [&](const ArgumentMoves& moves) {
        return static_cast<size_t>(std::ranges::lower_bound(argumentMoves, &moves, argumentMovesAreInOrder) - argumentMoves.begin());
    };
    ArgumentThunks argumentThunks = generateArgumentThunks(argumentMoves);
    Vector<size_t> sizeOfOwnBodiesOf(FillWith { }, m_functions.size(), static_cast<size_t>(0));
    for (size_t index = 0; index < m_functions.size(); ++index) {
        for (uint32_t number : ownBodiesOf[index])
            sizeOfOwnBodiesOf[index] += ownBodies.sizeOf(number);
    }
    size_t argumentThunksAt = WTF::roundUpToMultipleOf<imageStubsAlignment>(stubs.bytes.size());
    size_t sizeOfStubs = argumentThunks.bytes.isEmpty() ? stubs.bytes.size() : argumentThunksAt + argumentThunks.bytes.size();
    size_t sharedBodiesAt = WTF::roundUpToMultipleOf<sizeof(uint32_t)>(sizeOfStubs);
    if (!sharedBodies.bytes.isEmpty())
        sizeOfStubs = sharedBodiesAt + sharedBodies.bytes.size();
    size_t reach = stubCallReach;
    if (unsigned copies = Options::numberOfAOTStubCopiesForTesting()) [[unlikely]] {
        size_t sizeOfFunctions = 0;
        for (auto& function : m_functions)
            sizeOfFunctions += WTF::roundUpToMultipleOf<imageFunctionAlignment>(function.code.bytes.size());
        sizeOfFunctions += ownBodies.bytes.size();
        reach = sizeOfStubs + imageStubsAlignment + sizeOfFunctions / copies;
    }
    endStep("the stubs and the argument thunks"_s);
    Vector<size_t> stubsAt;
    Vector<std::pair<size_t, size_t>> placement;
    Vector<Vector<uint32_t, 0>> farCallees(m_functions.size());
    auto sizeWithVeneers = [&](size_t index) {
        size_t size = m_functions[index].code.bytes.size();
        return (farCallees[index].isEmpty() ? size : WTF::roundUpToMultipleOf<sizeof(uint32_t)>(size) + farCallees[index].size() * sizeOfVeneer) + sizeOfOwnBodiesOf[index];
    };
    {
        Vector<size_t> at;
        size_t end = 0;
        size_t lastStubs = std::numeric_limits<size_t>::max();
        for (auto& function : m_functions) {
            end = WTF::roundUpToMultipleOf<imageFunctionAlignment>(end + sizeAheadOf(at.size()));
            if (lastStubs == std::numeric_limits<size_t>::max() || end + function.code.bytes.size() + sizeOfOwnBodiesOf[at.size()] - lastStubs > reach) {
                end = lastStubs = WTF::roundUpToMultipleOf<imageStubsAlignment>(end);
                end = WTF::roundUpToMultipleOf<imageFunctionAlignment>(end + sizeOfStubs + sizeAheadOf(at.size()));
            }
            at.append(end);
            end += function.code.bytes.size() + sizeOfOwnBodiesOf[at.size() - 1];
        }
        std::atomic<size_t> sizeOfVeneers { 0 };
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            auto& info = m_functions[index].code.info;
            for (auto& call : info.stubCalls) {
                auto target = directTargetOf(index, call);
                if (!target)
                    continue;
                size_t from = at[index] + call.offset;
                size_t to = at[*target];
                if (((from > to ? from - to : to - from) <= reach && !Options::forceAOTVeneers()) || farCallees[index].contains(farCalleeOf(*target, call)))
                    continue;
                farCallees[index].append(farCalleeOf(*target, call));
                sizeOfVeneers += sizeOfVeneer + sizeof(uint32_t) + imageStubsAlignment;
            }
        });
        if (sizeOfVeneers + sizeOfStubs + imageStubsAlignment >= stubCallReachSlack) {
            m_whyFailed = makeString("the program has too much machine code: calls between functions that are too far apart need "_s, sizeOfVeneers.load(), " bytes, and the limit is "_s, stubCallReachSlack - stubs.bytes.size() - imageStubsAlignment);
            return { };
        }
    }
    endStep("where the copies of the stubs go"_s);
    BitVector fallsIntoNext(m_functions.size());
    BitVector sharesItsKey(m_functions.size());
    for (size_t index = 0; index < m_functions.size(); ++index) {
        if (uint32_t first = indexOfFunctionWithKey(m_functions[index].key); first != index) {
            sharesItsKey.quickSet(first);
            sharesItsKey.quickSet(index);
        }
    }
    for (size_t index = 0, sizeOfRun = 0; index + 1 < m_functions.size(); ++index) {
        constexpr size_t maxSizeOfRun = 1 << 20;
        auto& function = m_functions[index].code;
        auto& calls = function.info.stubCalls;
        size_t size = function.bytes.size();
        sizeOfRun += size;
        size_t last = calls.findIf([&](const StubCall& call) {
            return call.isTailCall && !call.thunk && !call.body && call.function != StubCall::noFunction && call.argumentMoves == StubCall::noArgumentMoves && call.firstOmittedGPR == call.endOfOmittedGPRs
                && call.offset + sizeOfJump <= size && size - call.offset - sizeOfJump < imageFunctionAlignment && *directTargetOf(index, call) == index + 1;
        });
        bool isCut = last != notFound && !sharesItsKey.quickGet(index) && farCallees[index].isEmpty() && !sizeOfOwnBodiesOf[index] && !sizeAheadOf(index + 1) && sizeOfRun <= maxSizeOfRun;
#if CPU(X86_64)
        if (isCut) {
            auto behindJump = function.bytes.span().subspan(calls[last].offset + sizeOfJump);
            while (!behindJump.empty() && behindJump.front() == 0xcc)
                behindJump = behindJump.subspan(1);
            std::array<uint8_t, imageFunctionAlignment> padding;
            X86Assembler::fillNops(padding.data(), behindJump.size());
            isCut = equalSpans(behindJump, std::span<const uint8_t> { padding }.first(behindJump.size()));
        }
#endif
        if (!isCut) {
            sizeOfRun = 0;
            continue;
        }
        size_t offset = calls[last].offset;
        size_t filling = WTF::roundUpToMultipleOf<imageFunctionAlignment>(offset) - offset;
        calls.removeAt(last);
        function.bytes.shrink(offset);
        if (!function.instructions.isEmpty()) {
            size_t count = 0;
            for (size_t at = 0; at < offset; ++count)
                at += function.instructions[count] & 15;
            function.instructions.shrink(count);
            if (filling)
                function.instructions.append(static_cast<uint8_t>(filling));
        }
#if CPU(X86_64)
        function.bytes.grow(offset + filling);
        X86Assembler::fillNops(function.bytes.mutableSpan().data() + offset, filling);
#else
        RELEASE_ASSERT(!filling);
#endif
        fallsIntoNext.quickSet(index);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", fallsIntoNext.bitCount(), " functions fall into the function that is placed next; ", sharesItsKey.bitCount(), " share their key with another and are left as they are");
    endStep("functions that fall into the next"_s);
    size_t recordsSize = 0;
    size_t codeSize = 0;
    for (size_t functionIndex = 0; functionIndex < m_functions.size(); ++functionIndex) {
        auto& function = m_functions[functionIndex];
        recordsSize += sizeof(ImageFunction) + function.code.info.sites.size() * (sizeof(Site) + (identifierIndices ? 0 : sizeof(uint32_t))) + function.code.info.plans.sizeInBytes() + (function.code.info.quotes.isEmpty() && function.code.info.constructSites.isEmpty() ? 0 : sizeof(uint32_t));
        RELEASE_ASSERT(function.code.info.sites.size() == function.code.info.numSlots);
        codeSize = WTF::roundUpToMultipleOf<imageFunctionAlignment>(codeSize + sizeAheadOf(functionIndex));
        bool isFallenInto = functionIndex && fallsIntoNext.quickGet(functionIndex - 1);
        RELEASE_ASSERT(!isFallenInto || codeSize == placement.last().first + m_functions[functionIndex - 1].code.bytes.size());
        size_t sizeOfRun = sizeWithVeneers(functionIndex);
        for (size_t last = functionIndex; !isFallenInto && fallsIntoNext.quickGet(last); ++last)
            sizeOfRun += sizeWithVeneers(last + 1);
        if (stubsAt.isEmpty() || (!isFallenInto && codeSize + sizeOfRun - stubsAt.last() > reach)) {
            codeSize = WTF::roundUpToMultipleOf<imageStubsAlignment>(codeSize);
            stubsAt.append(codeSize);
            static_assert(!((1 << minCodeGranuleShift) % imageFunctionAlignment));
            codeSize = WTF::roundUpToMultipleOf(static_cast<size_t>(1) << codeGranuleShift, codeSize + sizeOfStubs + sizeAheadOf(functionIndex));
        }
        placement.append({ codeSize, stubsAt.isEmpty() ? 0 : stubsAt.last() });
        if (Options::logAOTTypeInference()) [[unlikely]]
            dataLogLn("PLACED @", m_functions[placement.size() - 1].key.module, ":", m_functions[placement.size() - 1].key.start, ":", m_functions[placement.size() - 1].key.kind, " ", codeSize);
        codeSize += sizeWithVeneers(functionIndex);
    }
    Vector<size_t> placeOfOwnBody(FillWith { }, ownBodies.offsets.size(), static_cast<size_t>(0));
    auto targetOfJumpOfBody = [&](const StubCall& jump, size_t copyOfStubs) -> size_t {
        if (jump.function != StubCall::noFunction) {
            uint32_t callee = indexOfFunctionWithKey(calleesOfBodies[jump.function]);
            RELEASE_ASSERT(callee != ImageFunction::noSuchFunction);
            return placement[callee].first - sizeAheadFor(callee, jump);
        }
        if (jump.argumentMoves != StubCall::noArgumentMoves)
            return copyOfStubs + argumentThunksAt + argumentThunks.offsets[numberOfArgumentMoves(argumentMovesOfBodies[jump.argumentMoves])];
        return copyOfStubs + (jump.thunk ? stubs.thunkOffsets[jump.thunk - 1] : stubs.offsets[static_cast<unsigned>(jump.stub)]);
    };
    for (size_t index = 0; index < m_functions.size(); ++index) {
        size_t at = placement[index].first + sizeWithVeneers(index) - sizeOfOwnBodiesOf[index];
        for (uint32_t number : ownBodiesOf[index]) {
            placeOfOwnBody[number] = at;
            at += ownBodies.sizeOf(number);
        }
    }
    size_t functionsEnd = codeSize;
    // As many as there can be in what the offsets of the header can address.
    static_assert(maxStubCopiesPerImage * stubCallReach > std::numeric_limits<uint32_t>::max());
    if (stubsAt.size() > maxStubCopiesPerImage) {
        m_whyFailed = makeString("the program has too much machine code: "_s, codeSize, " bytes need "_s, stubsAt.size(), " copies of the runtime's stubs, and the limit is "_s, maxStubCopiesPerImage);
        return { };
    }

    endStep("the sizes of records and code"_s);
    Vector<uint32_t> functionStarts;
    for (auto& [at, stubsAtOffset] : placement)
        functionStarts.append(safeCast<uint32_t>(at));
    functionStarts.append(std::numeric_limits<uint32_t>::max());
    std::unique_ptr<FilePrintStream> map;
    if (Options::aotMapFilePath()) [[unlikely]] {
        map = FilePrintStream::open(UTF8CStringView::unsafeFromUTF8(byteCast<char>(Options::aotMapFilePath())), "w"_s);
        RELEASE_ASSERT(map);
        auto& out = map;
        for (unsigned index = 0; index < m_functions.size(); ++index) {
            auto& function = m_functions[index];
            out->println("F\t", index, "\t", functionStarts[index], "\t", function.code.bytes.size(), "\t", function.key.module, "\t", function.key.start, "\t", function.key.kind, "\t", function.nameForMap);
            if (!function.code.bytesByOperation.isNull())
                out->println("O\t", index, function.code.bytesByOperation);
            if (!function.code.instructions.isEmpty()) {
                out->print("J\t", index, "\t");
                for (uint8_t record : function.code.instructions)
                    out->print("0123456789abcdef"[record >> 4], "0123456789abcdef"[record & 15]);
                out->println();
            }
            if (!function.code.dataInCode.isEmpty()) {
                out->print("G\t", index);
                for (auto& data : function.code.dataInCode)
                    out->print("\t", data.offset, ":", data.numberOfWords, data.isTable ? ":t" : ":d");
                out->println();
            }
            if (function.code.startOfRareCode != std::numeric_limits<uint32_t>::max())
                out->println("H\t", index, "\t", function.code.startOfRareCode);
        }
        for (auto& folded : foldedFunctions) {
            for (unsigned bucket = folded.keptFunction.hash() & (capacity - 1); functionWithKey[bucket] != std::numeric_limits<uint32_t>::max(); bucket = (bucket + 1) & (capacity - 1)) {
                if (!m_functions[functionWithKey[bucket]].key.sameFunction(folded.keptFunction))
                    continue;
                out->println("A\t", functionStarts[functionWithKey[bucket]], "\t", folded.size, "\t", folded.key.module, "\t", folded.key.start, "\t", folded.key.kind, "\t", folded.nameForMap);
                break;
            }
        }
        for (size_t at : stubsAt)
            out->println("C\t", at);
        for (auto& [name, offset] : stubs.names)
            out->println("T\t", offset, "\t", name);
        for (size_t i = 0; i < argumentMoves.size(); ++i) {
            auto& name = stubs.names[argumentMoves[i]->at(1) == StubCall::noImmediate ? static_cast<size_t>(argumentMoves[i]->at(0)) : numberOfStubs + *thunkFor(static_cast<Stub>(argumentMoves[i]->at(0)), static_cast<uint32_t>(argumentMoves[i]->at(1)))].first;
            out->println("T\t", argumentThunksAt + argumentThunks.offsets[i], "\t", name, " with arguments");
        }
        if (!outlining.bodies.isEmpty())
            out->println("Y\tS, O, J and G hold the offsets and the bytes of the functions as they were ahead of the outlining");
        for (uint32_t number = 0; number < outlining.bodies.size(); ++number) {
            uint32_t amongItsLike = numberOfBodyAmongItsLike[number];
            if (outlining.bodies[number].onlyCaller == Outlining::none)
                out->println("T\t", sharedBodiesAt + sharedBodies.offsets[amongItsLike], "\toutlined ", number);
            else
                out->println("U\t", placeOfOwnBody[amongItsLike], "\t", ownBodies.sizeOf(amongItsLike), "\t", outlining.bodies[number].onlyCaller, "\toutlined ", number);
        }
        for (size_t index = 0; index < outlining.places.size(); ++index) {
            if (outlining.places[index].isEmpty())
                continue;
            out->print("P\t", index);
            for (auto& place : outlining.places[index])
                out->print("\t", place.offset, "\t", place.length, "\t", place.body);
            out->println();
            if (fillingsAheadOfEntries[index].isEmpty())
                continue;
            out->print("E\t", index);
            for (auto [entry, filling] : fillingsAheadOfEntries[index])
                out->print("\t", entry, "\t", filling);
            out->println();
        }
        dumpRegistersChangedByStubs(*out);
        out->println("R\t", functionsEnd);
        if (identifierIndices) {
            out->println("D\t", sizeof(Data), "\t", sizeof(Slot));
            Vector<std::pair<uint32_t, const StringImpl*>> identifiers;
            for (auto& entry : *identifierIndices)
                identifiers.append({ entry.value, entry.key });
            std::ranges::sort(identifiers);
            for (auto& [number, identifier] : identifiers)
                out->println("I\t", number, "\t", onOneLineOfMap(identifier));
            for (unsigned index = 0; index < m_functions.size(); ++index) {
                auto& sites = m_functions[index].code.info.sites;
                for (unsigned slot = 0; slot < sites.size(); ++slot) {
                    if (uint32_t identifier = sites[slot].identifierAndExtra & ((1u << Site::identifierBits) - 1))
                        out->println("S\t", index, "\t", slot, "\t", identifier);
                }
            }
        }
    }
    endStep("the starts of the functions, and the map"_s);
    Vector<uint32_t> codeGranules;
    {
        uint32_t function = 0;
        for (size_t start = 0; start < functionsEnd; start += 1 << codeGranuleShift) {
            while (functionStarts[function + 1] <= start)
                ++function;
            codeGranules.append(function);
        }
    }
    Vector<uint32_t> faultingAccesses;
    Vector<uint32_t> throwSitesOfFaultingAccesses;
    Vector<uint32_t> wideNamesOfFaultingAccesses;
    uint32_t numberOfTrapsAtThrowSites = 0;
    for (unsigned index = 0; index < m_functions.size(); ++index) {
        auto& info = m_functions[index].code.info;
        RELEASE_ASSERT(FaultingAccessInImage::hasNoFrameBit || !info.hasNoFrame || info.faultingAccesses.isEmpty());
        RELEASE_ASSERT(!info.hasNoFrame || !info.calleeSaveRegisters.registerCount());
        for (auto& access : info.faultingAccesses) {
            if (access.throwSite.isEmpty()) {
                ++numberOfTrapsAtThrowSites;
                continue;
            }
            uint32_t offset = functionStarts[index] + access.codeOffset;
            uint32_t word = access.throwSite[0];
            RELEASE_ASSERT(!(offset & FaultingAccessInImage::hasNoFrameBit));
            RELEASE_ASSERT(faultingAccesses.isEmpty() || (faultingAccesses.last() & ~FaultingAccessInImage::hasNoFrameBit) < offset);
            RELEASE_ASSERT(!(word & ThrowSite::hasB) && access.throwSite.size() == (word & ThrowSite::hasWideA ? 2 : 1));
            if (word & ThrowSite::hasWideA && access.throwSite[1] != ThrowSite::none) {
                wideNamesOfFaultingAccesses.append(faultingAccesses.size());
                wideNamesOfFaultingAccesses.append(access.throwSite[1]);
            }
            faultingAccesses.append(offset | (info.hasNoFrame ? FaultingAccessInImage::hasNoFrameBit : 0));
            throwSitesOfFaultingAccesses.append(word);
        }
    }
    uint32_t numberOfFaultingAccesses = faultingAccesses.size();
    uint32_t numberOfThrowSitesOfFaultingAccesses = 0;
    {
        Vector<uint32_t> checkpoints;
        Vector<uint8_t> steps;
        uint32_t placeOfLast = 0;
        for (uint32_t i = 0; i < numberOfFaultingAccesses; ++i) {
            if (!(i % faultingAccessesBetweenCheckpoints))
                checkpoints.appendList({ placeOfLast, safeCast<uint32_t>(steps.size()) });
            uint32_t place = faultingAccesses[i] & ~FaultingAccessInImage::hasNoFrameBit;
            appendVarint(steps, static_cast<uint64_t>(place - placeOfLast) << 1 | !!(faultingAccesses[i] & FaultingAccessInImage::hasNoFrameBit));
            placeOfLast = place;
        }
        Vector<uint32_t> distinct;
        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numberOfWord;
        Vector<uint32_t> numbers;
        for (uint32_t word : throwSitesOfFaultingAccesses) {
            auto added = numberOfWord.add(word, distinct.size());
            if (added.isNewEntry)
                distinct.append(word);
            numbers.append(added.iterator->value);
        }
        numberOfThrowSitesOfFaultingAccesses = distinct.size();
        Vector<uint8_t> bytes;
        bytes.append(asBytes(checkpoints.span()));
        bytes.append(asBytes(distinct.span()));
        bytes.append(asBytes(wideNamesOfFaultingAccesses.span()));
        if (distinct.size() <= std::numeric_limits<uint16_t>::max() + 1u) {
            for (uint32_t number : numbers) {
                uint16_t narrow = static_cast<uint16_t>(number);
                bytes.append(asBytes(singleElementSpan(narrow)));
            }
        } else
            bytes.append(asBytes(numbers.span()));
        bytes.appendVector(steps);
        while (bytes.size() % sizeof(uint32_t))
            bytes.append(0);
        faultingAccesses.resize(bytes.size() / sizeof(uint32_t));
        memcpySpan(asMutableByteSpan(faultingAccesses.mutableSpan()), bytes.span());
    }
    endStep("granules and faulting accesses"_s);
    Vector<Vector<uint8_t>> callSitesOf(m_functions.size());
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        auto& function = m_functions[index];
        auto& callSites = callSitesOf[index];
        Vector<std::pair<uint32_t, uint32_t>> all;
        BitVector constructs;
        for (uint32_t number : function.code.info.sitesThatConstruct)
            constructs.set(number);
        for (auto& call : function.code.info.stubCalls) {
            if (call.callSite != StubCall::noCallSite && !call.isTailCall) {
                all.append({ call.offset + static_cast<uint32_t>(sizeOfNearCall), call.callSite });
                if ((call.stub == Stub::ConstructClosure || call.stub == Stub::ConstructClosureList) && function.code.info.typedFunction != FunctionReportableSites::notTyped)
                    constructs.set(CallSiteIndex(PackedSite::bits(call.callSite)).bytecodeIndex().offset());
            }
        }
        for (auto& access : function.code.info.faultingAccesses)
            all.append({ access.codeOffset + static_cast<uint32_t>(access.throwSite.isEmpty() ? offsetOfReturnAddressInTrap : sizeOfNearCall), access.callSite });
        if (all.isEmpty())
            return;
        std::ranges::sort(all);
        auto& spreadSites = function.code.info.spreadSites;
        auto& inlineFrames = function.code.info.inlineFrames;
        // The typed compiler numbers its sites so: behind those that have no code.
        bool isInCodeOrder = function.code.info.typedFunction != FunctionReportableSites::notTyped;
        appendVarint(callSites, all.size() << 3 | isInCodeOrder << 2 | !inlineFrames.isEmpty() << 1 | !spreadSites.isEmpty());
        if (isInCodeOrder)
            appendVarint(callSites, function.code.info.numberOfSitesWithoutCode);
        if (!inlineFrames.isEmpty()) {
            Vector<uint8_t> frames;
            for (unsigned frame = 1; frame < inlineFrames.size(); ++frame) {
                appendVarint(frames, inlineFrames[frame].parent << 2 | inlineFrames[frame].isConstruction << 1 | inlineFrames[frame].isTailCall);
                if (isInCodeOrder) {
                    RELEASE_ASSERT(inlineFrames[frame].callSite == function.code.info.numberOfSitesWithoutCode - (inlineFrames.size() - 1) + frame);
                    appendVarint(frames, inlineFrames[frame].knownCallee);
                    continue;
                }
                appendVarint(frames, inlineFrames[frame].callSite);
                StubCall call { };
                call.function = inlineFrames[frame].knownCallee;
                appendVarint(frames, *directTargetOf(index, call));
            }
            appendVarint(callSites, inlineFrames.size() - 1);
            appendVarint(callSites, frames.size());
            callSites.appendVector(frames);
        }
        uint32_t previousOffset = 0;
        int64_t previousSite = 0;
        unsigned previousFrame = 0;
        uint32_t number = function.code.info.numberOfSitesWithoutCode;
        Vector<uint8_t> sitesInCodeOrder;
        unsigned numberWritten = 0;
        for (auto& [offset, site] : all) {
            if (isInCodeOrder) {
                RELEASE_ASSERT(CallSiteIndex(PackedSite::bits(site)).bytecodeIndex().offset() == ++number && !PackedSite::isTailCallSite(site));
                if (numberWritten && !(numberWritten % SiteCheckpoint::sitesBetween)) {
                    RELEASE_ASSERT(!(sitesInCodeOrder.size() >> 24));
                    SiteCheckpoint checkpoint { static_cast<uint32_t>(previousOffset / codeOffsetUnit), static_cast<uint32_t>(sitesInCodeOrder.size() << 8 | previousFrame) };
                    callSites.append(std::span { reinterpret_cast<const uint8_t*>(&checkpoint), sizeof(checkpoint) });
                }
                ++numberWritten;
                unsigned frame = PackedSite::inlineFrame(site);
                bool saysMore = frame != previousFrame || constructs.get(number);
                appendVarint(sitesInCodeOrder, static_cast<uint64_t>((offset - previousOffset) / codeOffsetUnit) << 1 | saysMore);
                if (saysMore)
                    appendVarint(sitesInCodeOrder, frame << 1 | constructs.get(number));
                previousOffset = offset;
                previousFrame = frame;
                continue;
            }
            appendVarint(callSites, (offset - previousOffset) / codeOffsetUnit);
            int64_t step = static_cast<int64_t>(site) - previousSite;
            appendVarint(callSites, static_cast<uint64_t>(step << 1) ^ static_cast<uint64_t>(step >> 63));
            previousOffset = offset;
            previousSite = site;
        }
        callSites.appendVector(sitesInCodeOrder);
        if (!spreadSites.isEmpty()) {
            appendVarint(callSites, spreadSites.size());
            for (auto& entry : spreadSites) {
                appendVarint(callSites, entry.callSite);
                appendVarint(callSites, entry.item);
                appendVarint(callSites, entry.site);
            }
        }
    });
    endStep("the call sites of each function"_s);
    Vector<uint8_t> callSites { 0 };
    Vector<uint32_t> functionCallSites;
    for (auto& ofFunction : callSitesOf) {
        functionCallSites.append(ofFunction.isEmpty() ? 0 : safeCast<uint32_t>(callSites.size()));
        callSites.appendVector(ofFunction);
    }
    callSitesOf.clear();

    endStep("the call sites together"_s);
    std::ranges::sort(m_regExps, [](const RegExpCode& a, const RegExpCode& b) {
        uint32_t hashOfA = ImageRegExp::hashOf(a.pattern, a.flags);
        uint32_t hashOfB = ImageRegExp::hashOf(b.pattern, b.flags);
        if (hashOfA != hashOfB)
            return hashOfA < hashOfB;
        if (a.flags != b.flags)
            return a.flags.toRaw() < b.flags.toRaw();
        return codePointCompareLessThan(a.pattern, b.pattern);
    });
    if (Options::verboseAOTCompilation()) [[unlikely]] {
        size_t numberWithHashOfAnother = 0;
        for (size_t i = 0; i < m_regExps.size(); ++i) {
            auto hashAt = [&](size_t index) {
                return ImageRegExp::hashOf(m_regExps[index].pattern, m_regExps[index].flags);
            };
            numberWithHashOfAnother += (i && hashAt(i - 1) == hashAt(i)) || (i + 1 < m_regExps.size() && hashAt(i + 1) == hashAt(i));
        }
        dataLogLn("AOT: regular expressions that have the hash of another: ", numberWithHashOfAnother);
    }
    UncheckedKeyHashMap<const uint8_t*, size_t> regExpTables;
    Vector<std::array<size_t, 2>> regExpPlacement;
    UncheckedKeyHashMap<String, size_t> ownedRegExpTablesAt;
    Vector<std::pair<size_t, const Vector<uint8_t>*>> ownedRegExpTablesPlaced;
    Vector<std::array<Vector<size_t>, 2>> ownedRegExpTableTargets;
    UncheckedKeyHashMap<String, size_t> regExpRoutinesAt;
    Vector<std::pair<size_t, const Vector<uint8_t>*>> regExpRoutinesPlaced;
    Vector<std::array<Vector<size_t>, 2>> regExpRoutineTargets;
    Vector<ImageRegExp> imageRegExps;
    Vector<uint8_t> regExpText;
    BitVector regExpTextIsInTypedData;
    auto* typedData = m_typedData.isEmpty() ? nullptr : reinterpret_cast<const Typed::Data*>(m_typedData.span().data());
    constexpr uint32_t isNotInTypedData = std::numeric_limits<uint32_t>::max();
    UncheckedKeyHashMap<String, uint32_t> offsetsInTypedData;
    BitVector lengthsOfPatterns;
    auto textOf = [](const String& pattern) {
        return pattern.isNull() ? emptyString() : pattern;
    };
    for (auto& regExp : m_regExps) {
        if (!typedData || !regExp.pattern.containsOnlyASCII() || !regExp.pattern.is8Bit())
            continue;
        offsetsInTypedData.add(textOf(regExp.pattern), isNotInTypedData);
        lengthsOfPatterns.set(regExp.pattern.length());
    }
    for (uint32_t index = 0; !offsetsInTypedData.isEmpty() && index < typedData->numberOfStrings; ++index) {
        if (typedData->hasOnlyDecodedForm(index) || !lengthsOfPatterns.get(typedData->sizeOfString(index)))
            continue;
        auto text = typedData->string(index);
        auto found = offsetsInTypedData.find<StringViewHashTranslator>(StringView { byteCast<Latin1Character>(text) });
        if (found != offsetsInTypedData.end() && found->value == isNotInTypedData)
            found->value = safeCast<uint32_t>(text.data() - m_typedData.span().data());
    }
    auto offsetInTypedData = [&](const String& pattern) -> std::optional<uint32_t> {
        auto found = offsetsInTypedData.find(textOf(pattern));
        if (found == offsetsInTypedData.end() || found->value == isNotInTypedData)
            return std::nullopt;
        return found->value;
    };
    {
        size_t before = codeSize;
        for (auto& regExp : m_regExps) {
            for (auto& code : regExp.code) {
                for (auto& reference : code.tables) {
                    regExpTables.ensure(reference.table.data(), [&] {
                        codeSize = WTF::roundUpToMultipleOf<imageStubsAlignment>(codeSize);
                        size_t at = codeSize;
                        codeSize += reference.table.size();
                        return at;
                    });
                }
            }
        }
        size_t sizeOfTables = codeSize - before;
        UncheckedKeyHashMap<String, std::pair<size_t, unsigned>> regExpBodiesPlaced;
        regExpPlacement.grow(m_regExps.size());
        ownedRegExpTableTargets.grow(m_regExps.size());
        regExpRoutineTargets.grow(m_regExps.size());
        for (unsigned i = 0; i < 2; ++i) {
            for (size_t number = 0; number < m_regExps.size(); ++number) {
                auto& regExp = m_regExps[number];
                auto& at = regExpPlacement[number];
                auto& targets = ownedRegExpTableTargets[number];
                auto& routineTargets = regExpRoutineTargets[number];
                if (!regExp.code[i].bytes.isEmpty()) {
                    auto& body = regExp.code[i];
                    Vector<uint8_t> content;
                    auto addNumber = [&](uint64_t number) {
                        content.append(asByteSpan(number));
                    };
                    auto addBytes = [&](std::span<const uint8_t> bytes) {
                        addNumber(bytes.size());
                        content.append(bytes);
                    };
                    addBytes(body.bytes.span());
                    addNumber(body.tables.size());
                    for (auto& reference : body.tables) {
                        addNumber(reference.offset);
                        addNumber(reference.reg);
                        addNumber(std::bit_cast<uintptr_t>(reference.table.data()));
                    }
                    addNumber(body.ownedTableReferences.size());
                    for (auto& reference : body.ownedTableReferences) {
                        addNumber(reference.offset);
                        addNumber(reference.reg);
                        addNumber(reference.isFar);
                        addBytes(body.ownedTables[reference.table].span());
                    }
                    addNumber(body.routineCalls.size());
                    for (auto& call : body.routineCalls) {
                        addNumber(call.offset);
                        addBytes(body.routines[call.routine].span());
                    }
                    auto placed = regExpBodiesPlaced.add(String { byteCast<Latin1Character>(content.span()) }, std::pair<size_t, unsigned> { number, i });
                    if (!placed.isNewEntry) {
                        auto [index, width] = placed.iterator->value;
                        at[i] = regExpPlacement[index][width];
                        targets[i] = ownedRegExpTableTargets[index][width];
                        routineTargets[i] = regExpRoutineTargets[index][width];
                        if (map) [[unlikely]]
                            map->println("XD\t", at[i], "\t", body.bytes.size(), "\t", i ? 16 : 8, "\t", regExp.flags.toRaw(), "\t", onOneLineOfMap(regExp.pattern));
                        continue;
                    }
                }
                codeSize = WTF::roundUpToMultipleOf<imageStubsAlignment>(codeSize);
                at[i] = codeSize;
                codeSize += regExp.code[i].bytes.size();
                for (auto& reference : regExp.code[i].ownedTableReferences) {
                    auto& table = regExp.code[i].ownedTables[reference.table];
                    String content { byteCast<Latin1Character>(table.span()) };
                    auto found = ownedRegExpTablesAt.find(content);
                    bool isInReach = found != ownedRegExpTablesAt.end();
#if !CPU(X86_64)
                    size_t instructionAt = at[i] + reference.offset;
                    if (isInReach && !reference.isFar)
                        isInReach = (found->value < instructionAt ? instructionAt - found->value : found->value - instructionAt) < (1 << 20) - 16;
#endif
                    if (isInReach) {
                        targets[i].append(found->value);
                        continue;
                    }
                    codeSize = WTF::roundUpToMultipleOf<8>(codeSize);
                    ownedRegExpTablesAt.set(content, codeSize);
                    ownedRegExpTablesPlaced.append({ codeSize, &table });
                    if (map) [[unlikely]]
                        map->println("XT\t", codeSize, "\t", table.size());
                    targets[i].append(codeSize);
                    codeSize += table.size();
                }
                for (auto& call : regExp.code[i].routineCalls) {
                    auto& routine = regExp.code[i].routines[call.routine];
                    RELEASE_ASSERT(!routine.isEmpty());
                    String content { byteCast<Latin1Character>(routine.span()) };
                    auto found = regExpRoutinesAt.find(content);
                    bool isInReach = found != regExpRoutinesAt.end();
#if !CPU(X86_64)
                    size_t instructionAt = at[i] + call.offset;
                    if (isInReach)
                        isInReach = (found->value < instructionAt ? instructionAt - found->value : found->value - instructionAt) < (1 << 27) - 16;
#endif
                    if (isInReach) {
                        routineTargets[i].append(found->value);
                        continue;
                    }
                    codeSize = WTF::roundUpToMultipleOf<imageStubsAlignment>(codeSize);
                    regExpRoutinesAt.set(content, codeSize);
                    regExpRoutinesPlaced.append({ codeSize, &routine });
                    routineTargets[i].append(codeSize);
                    if (map) [[unlikely]]
                        map->println("XR\t", codeSize, "\t", routine.size());
                    codeSize += routine.size();
                }
                if (map && !regExp.hasNoCode) [[unlikely]]
                    map->println("X\t", at[i], "\t", regExp.code[i].bytes.size(), "\t", i ? 16 : 8, "\t", regExp.flags.toRaw(), "\t", onOneLineOfMap(regExp.pattern));
            }
        }
        for (size_t number = 0; number < m_regExps.size(); ++number) {
            auto& regExp = m_regExps[number];
            auto& at = regExpPlacement[number];
            ImageRegExp record { };
            record.hash = ImageRegExp::hashOf(regExp.pattern, regExp.flags);
            record.length = regExp.pattern.length();
            record.is8Bit = regExp.pattern.is8Bit();
            record.flags = regExp.flags.toRaw();
            record.codeFor8Bit = safeCast<uint32_t>(at[0]);
            record.codeFor16Bit = safeCast<uint32_t>(at[1]);
            record.numSubpatterns = regExp.numSubpatterns;
            record.minimumSize = regExp.minimumSize;
            record.specificPattern = static_cast<uint8_t>(regExp.specificPattern);
            record.keepsMoreOfPattern = regExp.keepsMoreOfPattern;
            record.hasNoCode = regExp.hasNoCode;
            record.sourceIsPattern = regExp.sourceIsPattern;
            imageRegExps.append(record);
        }
        for (bool isKeptAsText : { false, true }) {
            for (size_t number = 0; number < m_regExps.size(); ++number) {
                auto& regExp = m_regExps[number];
                if ((regExp.pattern.length() > ImageRegExp::minLengthKeptAsTextOfImage) != isKeptAsText)
                    continue;
                if (auto offset = regExp.pattern.containsOnlyASCII() && regExp.pattern.is8Bit() ? offsetInTypedData(regExp.pattern) : std::nullopt) {
                    imageRegExps[number].text = *offset;
                    regExpTextIsInTypedData.set(number);
                    continue;
                }
                if (!regExp.pattern.is8Bit())
                    regExpText.insertFill(regExpText.size(), 0, regExpText.size() % 2);
                imageRegExps[number].text = safeCast<uint32_t>(regExpText.size());
                regExpText.append(regExp.pattern.is8Bit() ? asBytes(regExp.pattern.span8()) : asBytes(regExp.pattern.span16()));
            }
        }
    }
    endStep("placing the regular expressions"_s);
    RELEASE_ASSERT(recordsSize < std::numeric_limits<uint32_t>::max());

    ImageHeader header { };
    header.magic = imageMagic;
    header.stamp = imageStamp();
    header.tableOffset = sizeof(ImageHeader);
    Vector<uint8_t> quotes { 0 };
    Vector<uint8_t> quotesText;
    Vector<uint32_t> functionQuotes;
    functionQuotes.fill(0, m_functions.size());
    {
        UncheckedKeyHashMap<UTF8CString, uint32_t> knownLocation;
        size_t numberOfQuotes = 0;
        for (size_t index = 0; index < m_functions.size(); ++index) {
            auto& all = m_functions[index].code.info.quotes;
            auto& constructSites = m_functions[index].code.info.constructSites;
            if (all.isEmpty() && constructSites.isEmpty())
                continue;
            numberOfQuotes += all.size();
            struct Within {
                unsigned quote;
                uint32_t offset;
            };
            Vector<Within, 16> within(all.size(), [](size_t i) { return Within { static_cast<unsigned>(i), 0 }; });
            {
                Vector<unsigned, 16> inOrder;
                for (unsigned i = 0; i < all.size(); ++i) {
                    if (all[i].start != std::numeric_limits<uint32_t>::max() && charactersAreAllASCII(byteCast<Latin1Character>(all[i].text.span())))
                        inOrder.append(i);
                }
                std::ranges::sort(inOrder, [&](unsigned a, unsigned b) {
                    if (all[a].start != all[b].start)
                        return all[a].start < all[b].start;
                    return all[a].text.length() > all[b].text.length();
                });
                std::optional<unsigned> outer;
                for (unsigned i : inOrder) {
                    if (outer) {
                        auto& longer = all[*outer];
                        uint64_t offset = all[i].start - longer.start;
                        if (offset + all[i].text.length() <= longer.text.length() && !memcmp(longer.text.data() + offset, all[i].text.data(), all[i].text.length())) {
                            within[i] = { *outer, static_cast<uint32_t>(offset) };
                            continue;
                        }
                    }
                    if (!outer || static_cast<uint64_t>(all[i].start) + all[i].text.length() > static_cast<uint64_t>(all[*outer].start) + all[*outer].text.length())
                        outer = i;
                }
            }
            functionQuotes[index] = safeCast<uint32_t>(quotes.size());
            appendVarint(quotes, all.size());
            uint32_t previousOffset = 0;
            int64_t previousStart = 0;
            for (unsigned i = 0; i < all.size(); ++i) {
                auto& quote = all[i];
                const UTF8CString* kept = &all[within[i].quote].text;
                uint32_t start = within[i].offset + knownLocation.ensure(*kept, [&] {
                    uint32_t result = safeCast<uint32_t>(quotesText.size());
                    quotesText.append(kept->span());
                    return result;
                }).iterator->value;
                appendVarint(quotes, quote.bytecodeOffset - previousOffset);
                int64_t step = static_cast<int64_t>(start) - previousStart;
                appendVarint(quotes, static_cast<uint64_t>(step << 1) ^ static_cast<uint64_t>(step >> 63));
                appendVarint(quotes, static_cast<uint64_t>(quote.text.length()) << 2 | quote.kind);
                previousOffset = quote.bytecodeOffset;
                previousStart = start;
            }
            appendVarint(quotes, constructSites.size());
            uint32_t previous = 0;
            for (uint32_t offset : constructSites) {
                appendVarint(quotes, offset - previous);
                previous = offset;
            }
        }
    }
    endStep("the quotes"_s);
    constexpr size_t compressionBlockSize = 64 * KB;
    uint32_t quoteTextBlockSize = 0;
    if (auto packed = compressInBlocks(quotesText.span(), compressionBlockSize)) {
        quotesText = WTF::move(*packed);
        quoteTextBlockSize = compressionBlockSize;
    }
    uint32_t quoteBlockSize = 0;
    if (auto packed = compressInBlocks(quotes.span(), compressionBlockSize)) {
        quotes = WTF::move(*packed);
        quoteBlockSize = compressionBlockSize;
    }

    endStep("compressing the quotes"_s);
    bool keysAreOmitted = !!m_numberOfProgramIdentifiers;
    header.tableCapacity = keysAreOmitted ? 0 : capacity;
    header.recordsOffset = header.tableOffset + header.tableCapacity * sizeof(ImageKey);
    header.recordsSize = recordsSize;
    size_t totalDataSize = 0;
    for (auto& function : m_functions)
        totalDataSize += roundUpToMultipleOf<16>(Data::allocationSize(function.code.info.numSlots, true));
    header.totalDataSizeIn16Bytes = safeCast<uint32_t>(totalDataSize / 16);
    RELEASE_ASSERT_WITH_MESSAGE(!m_typedData.size() || m_environmentsSize <= Instance::roomForEnvironments, "AOT: the program has more environments than there is room for below the instance");
    header.environmentsSize = m_environmentsSize;
    header.environmentsOffset = WTF::roundUpToMultipleOf<8>(static_cast<size_t>(header.recordsOffset) + recordsSize);
    header.numberOfEnvironments = m_environments.size();
    size_t tablesEnd = static_cast<size_t>(header.environmentsOffset) + m_environments.size() * sizeof(ImageEnvironment);
    auto place = [&](size_t size) {
        tablesEnd = WTF::roundUpToMultipleOf<8>(tablesEnd);
        uint32_t result = safeCast<uint32_t>(tablesEnd);
        tablesEnd += size;
        return result;
    };
    header.quotesOffset = place(quotes.size());
    header.quotesTextOffset = place(quotesText.size());
    header.quoteTextBlockSize = quoteTextBlockSize;
    header.quoteBlockSize = quoteBlockSize;
    header.numberOfProgramIdentifiers = m_numberOfProgramIdentifiers;
    header.numberOfProgramConstants = m_numberOfProgramConstants;
    header.regExpsOffset = place(imageRegExps.sizeInBytes());
    header.numberOfRegExps = imageRegExps.size();
    header.regExpTextOffset = place(regExpText.size());
    header.functionStartsOffset = place(functionStarts.sizeInBytes());
    header.codeGranulesOffset = place(codeGranules.sizeInBytes());
    header.codeGranuleShift = codeGranuleShift;
    header.callSitesOffset = place(callSites.size());
    header.faultingAccessesOffset = place(faultingAccesses.sizeInBytes());
    header.numberOfFaultingAccesses = numberOfFaultingAccesses;
    header.numberOfFaultingAccessesWithWideName = wideNamesOfFaultingAccesses.size() / 2;
    header.numberOfThrowSitesOfFaultingAccesses = numberOfThrowSitesOfFaultingAccesses;
    header.numberOfTrapsAtThrowSites = numberOfTrapsAtThrowSites;
    Vector<ImageFrame> frames;
    Vector<uint16_t> functionFrame;
    {
        UncheckedKeyHashMap<uint64_t, uint16_t, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> numbers;
        for (auto& function : m_functions) {
            auto& info = function.code.info;
            RELEASE_ASSERT(!(info.frameSizeInBytes % stackAlignmentBytes()));
            ImageFrame frame { 0, 0, safeCast<uint16_t>(info.frameSizeInBytes / stackAlignmentBytes()) };
            uint64_t calleeSaveRegisters = 0;
            for (unsigned i = 0; i < info.calleeSaveRegisters.registerCount(); ++i) {
                const RegisterAtOffset& entry = info.calleeSaveRegisters.at(i);
                RELEASE_ASSERT(entry.reg().index() < 64 && entry.offset() == info.calleeSaveRegisters.at(0).offset() + static_cast<ptrdiff_t>(i * sizeof(CPURegister)) && (!i || entry.reg().index() > info.calleeSaveRegisters.at(i - 1).reg().index()));
                calleeSaveRegisters |= 1ULL << entry.reg().index();
            }
            if (info.calleeSaveRegisters.registerCount()) {
                ptrdiff_t offset = info.calleeSaveRegisters.at(0).offset();
                RELEASE_ASSERT(offset < 0 && !(offset % static_cast<ptrdiff_t>(sizeof(CPURegister))) && -offset / static_cast<ptrdiff_t>(sizeof(CPURegister)) <= static_cast<ptrdiff_t>(ImageFrame::maxCalleeSavesStart));
                frame.calleeSavesStart = static_cast<uint16_t>(-offset / static_cast<ptrdiff_t>(sizeof(CPURegister)));
            }
            frame.calleeSaveRegisters = ImageFunction::packRegisters(calleeSaveRegisters);
            functionFrame.append(numbers.ensure(frame.bits(), [&] {
                RELEASE_ASSERT(frames.size() < 1u << 15);
                frames.append(frame);
                return static_cast<uint16_t>(frames.size() - 1);
            }).iterator->value);
        }
    }
    header.framesOffset = place(frames.sizeInBytes());
    header.typedDataOffset = place(m_typedData.size());
    header.typedDataSize = safeCast<uint32_t>(m_typedData.size());
    for (size_t index = 0; index < imageRegExps.size(); ++index)
        imageRegExps[index].text = safeCast<uint32_t>(static_cast<uint64_t>(imageRegExps[index].text) + (regExpTextIsInTypedData.get(index) ? header.typedDataOffset : header.regExpTextOffset));
    header.functionsEnd = safeCast<uint32_t>(functionsEnd);
    header.stubsSize = safeCast<uint32_t>(sizeOfStubs);
    for (unsigned i = 0; i < 4; ++i)
        header.linkTimeConstantsUsed[i] = s_linkTimeConstantsUsed[i].load();
    header.numberOfStubCopies = stubsAt.size();
    for (unsigned i = 0; i < stubsAt.size(); ++i)
        header.stubCopies[i] = safeCast<uint32_t>(stubsAt[i]);
    RELEASE_ASSERT(stubs.returnsIntoAdapters.size() == numberOfAdapters);
    for (unsigned i = 0; i < numberOfAdapters; ++i)
        header.returnsIntoAdapters[i] = stubs.returnsIntoAdapters[i];
    RELEASE_ASSERT(stubs.returnsFromFindingTargets.size() == maxReturnsIntoFramesOfHostFunctions);
    for (unsigned i = 0; i < maxReturnsIntoFramesOfHostFunctions; ++i)
        header.returnsFromFindingTargets[i] = stubs.returnsFromFindingTargets[i];
    // 0: no code of the image loads an intrinsic by its number, and no realm numbers them for it.
    header.intrinsicHash = ImmutableIntrinsics::shared() && s_usesNumberedIntrinsics.load() ? ImmutableIntrinsics::shared()->hash() : 0;
    header.codeOffset = WTF::roundUpToMultipleOf<imagePageSize>(tablesEnd);
    header.codeSize = codeSize;
    header.size = WTF::roundUpToMultipleOf<imagePageSize>(header.codeOffset + codeSize);
    if (map) [[unlikely]]
        map->println("B\t", codeSize);
    header.numberOfFunctions = m_functions.size();
    for (unsigned i = 0; i < numberOfStubs; ++i)
        header.stubOffsets[i] = safeCast<uint32_t>(stubs.offsets[i] + (stubsAt.isEmpty() ? 0 : stubsAt.first()));

    endStep("the header and the frames"_s);
    Vector<uint8_t> image;
    image.grow(header.size + (keysAreOmitted ? capacity * sizeof(ImageKey) : 0));
    constexpr size_t bytesAtATime = MB;
    Typed::forEachIndexInParallel((image.size() + bytesAtATime - 1) / bytesAtATime, [&](unsigned i) {
        size_t start = i * bytesAtATime;
        zeroSpan(image.mutableSpan().subspan(start, std::min(bytesAtATime, image.size() - start)));
    });
    endStep("room for the image"_s);
    uint8_t* base = image.mutableSpan().data();
    memcpy(base, &header, sizeof(header));
    auto* table = reinterpret_cast<ImageKey*>(base + (keysAreOmitted ? header.size : header.tableOffset));
    uint8_t* records = base + header.recordsOffset;
    uint8_t* code = base + header.codeOffset;

    memcpy(base + header.environmentsOffset, m_environments.span().data(), m_environments.size() * sizeof(ImageEnvironment));
    memcpy(base + header.quotesOffset, quotes.span().data(), quotes.size());
    memcpy(base + header.quotesTextOffset, quotesText.span().data(), quotesText.size());
    memcpy(base + header.regExpsOffset, imageRegExps.span().data(), imageRegExps.sizeInBytes());
    memcpy(base + header.regExpTextOffset, regExpText.span().data(), regExpText.size());
    memcpy(base + header.functionStartsOffset, functionStarts.span().data(), functionStarts.sizeInBytes());
    memcpy(base + header.codeGranulesOffset, codeGranules.span().data(), codeGranules.sizeInBytes());
    memcpy(base + header.callSitesOffset, callSites.span().data(), callSites.size());
    memcpy(base + header.faultingAccessesOffset, faultingAccesses.span().data(), faultingAccesses.sizeInBytes());
    memcpy(base + header.framesOffset, frames.span().data(), frames.sizeInBytes());
    memcpy(base + header.typedDataOffset, m_typedData.span().data(), m_typedData.size());
    endStep("copying the tables"_s);
    for (auto& [table, at] : regExpTables) {
        for (auto& regExp : m_regExps) {
            bool copied = false;
            for (auto& regExpCode : regExp.code) {
                for (auto& reference : regExpCode.tables) {
                    if (reference.table.data() == table && !copied) {
                        memcpy(code + at, table, reference.table.size());
                        copied = true;
                    }
                }
            }
            if (copied)
                break;
        }
    }
    for (size_t index = 0; index < m_regExps.size(); ++index) {
        for (unsigned i = 0; i < 2; ++i) {
            auto& regExpCode = m_regExps[index].code[i];
            size_t codeAt = regExpPlacement[index][i];
            memcpy(code + codeAt, regExpCode.bytes.span().data(), regExpCode.bytes.size());
            for (size_t k = 0; k < regExpCode.ownedTableReferences.size(); ++k) {
                auto& reference = regExpCode.ownedTableReferences[k];
                size_t instructionAt = codeAt + reference.offset;
                size_t tableAt = ownedRegExpTableTargets[index][i][k];
#if CPU(X86_64)
                constexpr size_t sizeOfInstruction = 7;
                int64_t distance = static_cast<int64_t>(tableAt) - static_cast<int64_t>(instructionAt + sizeOfInstruction);
                RELEASE_ASSERT(distance == static_cast<int32_t>(distance));
                int32_t displacement = static_cast<int32_t>(distance);
                memcpy(code + instructionAt + sizeOfInstruction - sizeof(displacement), &displacement, sizeof(displacement));
#else
                if (reference.isFar) {
                    int64_t pages = static_cast<int64_t>(tableAt >> 12) - static_cast<int64_t>(instructionAt >> 12);
                    RELEASE_ASSERT(pages >= -(1 << 20) && pages < (1 << 20));
                    uint32_t adrp = 0x90000000u | (static_cast<uint32_t>(pages) & 3u) << 29 | (static_cast<uint32_t>(pages >> 2) & 0x7ffffu) << 5 | reference.reg;
                    uint32_t add = 0x91000000u | static_cast<uint32_t>(tableAt & 0xfff) << 10 | static_cast<uint32_t>(reference.reg) << 5 | reference.reg;
                    memcpy(code + instructionAt, &adrp, sizeof(adrp));
                    memcpy(code + instructionAt + sizeof(adrp), &add, sizeof(add));
                    continue;
                }
                int64_t delta = static_cast<int64_t>(tableAt) - static_cast<int64_t>(instructionAt);
                RELEASE_ASSERT(delta >= -(1 << 20) && delta < (1 << 20));
                uint32_t adr = 0x10000000u | (static_cast<uint32_t>(delta) & 3u) << 29 | (static_cast<uint32_t>(delta >> 2) & 0x7ffffu) << 5 | reference.reg;
                memcpy(code + instructionAt, &adr, sizeof(adr));
#endif
            }
            for (size_t k = 0; k < regExpCode.routineCalls.size(); ++k) {
                size_t instructionAt = codeAt + regExpCode.routineCalls[k].offset;
                size_t routineAt = regExpRoutineTargets[index][i][k];
#if CPU(X86_64)
                constexpr size_t sizeOfCall = 5;
                RELEASE_ASSERT(regExpCode.routineCalls[k].offset + sizeOfCall <= regExpCode.bytes.size() && code[instructionAt] == 0xe8);
                int64_t distance = static_cast<int64_t>(routineAt) - static_cast<int64_t>(instructionAt + sizeOfCall);
                RELEASE_ASSERT(distance == static_cast<int32_t>(distance));
                int32_t displacement = static_cast<int32_t>(distance);
                memcpy(code + instructionAt + sizeOfCall - sizeof(displacement), &displacement, sizeof(displacement));
#else
                RELEASE_ASSERT(regExpCode.routineCalls[k].offset + sizeof(uint32_t) <= regExpCode.bytes.size());
                int64_t delta = static_cast<int64_t>(routineAt) - static_cast<int64_t>(instructionAt);
                RELEASE_ASSERT(!(delta & 3) && delta >= -(1 << 27) && delta < (1 << 27));
                uint32_t call = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x3ffffffu);
                memcpy(code + instructionAt, &call, sizeof(call));
#endif
            }
            for (auto& reference : regExpCode.tables) {
                size_t instructionAt = codeAt + reference.offset;
                size_t tableAt = regExpTables.get(reference.table.data());
#if CPU(X86_64)
                constexpr size_t sizeOfInstruction = 7;
                int64_t distance = static_cast<int64_t>(tableAt) - static_cast<int64_t>(instructionAt + sizeOfInstruction);
                RELEASE_ASSERT(distance == static_cast<int32_t>(distance));
                int32_t displacement = static_cast<int32_t>(distance);
                memcpy(code + instructionAt + sizeOfInstruction - sizeof(displacement), &displacement, sizeof(displacement));
#else
                int64_t pages = static_cast<int64_t>(tableAt >> 12) - static_cast<int64_t>(instructionAt >> 12);
                RELEASE_ASSERT(pages >= -(1 << 20) && pages < (1 << 20));
                uint32_t adrp = 0x90000000u | (static_cast<uint32_t>(pages) & 3u) << 29 | (static_cast<uint32_t>(pages >> 2) & 0x7ffffu) << 5 | reference.reg;
                uint32_t add = 0x91000000u | static_cast<uint32_t>(tableAt & 0xfff) << 10 | static_cast<uint32_t>(reference.reg) << 5 | reference.reg;
                memcpy(code + instructionAt, &adrp, sizeof(adrp));
                memcpy(code + instructionAt + sizeof(adrp), &add, sizeof(add));
#endif
            }
        }
    }
    for (auto& [at, table] : ownedRegExpTablesPlaced)
        memcpy(code + at, table->span().data(), table->size());
    for (auto& [at, routine] : regExpRoutinesPlaced)
        memcpy(code + at, routine->span().data(), routine->size());
    for (size_t at : stubsAt)
        memcpy(code + at, stubs.bytes.span().data(), stubs.bytes.size());
    for (size_t at : stubsAt) {
        if (argumentThunks.bytes.isEmpty())
            break;
        memcpy(code + at + argumentThunksAt, argumentThunks.bytes.span().data(), argumentThunks.bytes.size());
        for (auto& jump : argumentThunks.jumps)
            retargetStubCall(code, at + argumentThunksAt + jump.offset, at + (jump.thunk ? stubs.thunkOffsets[jump.thunk - 1] : stubs.offsets[static_cast<unsigned>(jump.stub)]), true);
    }
    for (size_t at : stubsAt) {
        if (sharedBodies.bytes.isEmpty())
            break;
        memcpy(code + at + sharedBodiesAt, sharedBodies.bytes.span().data(), sharedBodies.bytes.size());
        for (auto& jump : sharedBodies.jumps) {
            if (jump.offset != OutlinedBodies::noJump)
                retargetStubCall(code, at + sharedBodiesAt + jump.offset, targetOfJumpOfBody(jump, at), true);
        }
    }

    endStep("copying the regular expressions and the stubs"_s);
    Vector<uint32_t> functionInBucket;
    functionInBucket.fill(0, capacity);
    size_t recordAt = 0;
    BitVector isDuplicate(m_functions.size());
    for (size_t index = 0; index < m_functions.size(); ++index) {
        auto& function = m_functions[index];
        auto& info = function.code.info;
        size_t codeAt = placement[index].first;

        ImageFunction record { };
        RELEASE_ASSERT(functionStarts[index] == codeAt);
        static const char* const limitForTesting = byteCast<char>(Options::aotImageLimitToExceedForTesting());
        auto isOver = [&](const char* name, uint64_t value, unsigned numberOfBits) {
            return value >> numberOfBits || (limitForTesting && !strcmp(limitForTesting, name));
        };
        if (isOver("index", index, ImageFunction::numberOfIndexBits) || isOver("frame", functionFrame[index], ImageFunction::numberOfFrameBits) || isOver("callSites", functionCallSites[index], ImageFunction::numberOfCallSitesBits) || isOver("slots", info.numSlots, ImageFunction::numberOfSlotsBits)) {
            m_whyFailed = makeString("the program is too large: it has more functions, kinds of stack frame, call sites or inline cache slots than an executable can describe (function "_s, index, ": frame "_s, functionFrame[index], ", call sites from "_s, functionCallSites[index], ", "_s, info.numSlots, " slots)"_s);
            return { };
        }
        record.index = safeCast<uint32_t>(index);
        record.frame = functionFrame[index];
        record.numberOfParameters = info.convention.numberOfParameters;
        record.takesList = info.convention.signature == Signature::List;
        record.hasInlineFrames = !info.inlineFrames.isEmpty();
        record.callSites = functionCallSites[index];
        record.numSlots = info.numSlots;
        record.hasSiteConstants = !identifierIndices;
        record.startsCold = info.startsCold;
        record.hasQuotes = !info.quotes.isEmpty() || !info.constructSites.isEmpty();

        ImageKey key = function.key;
        key.record = recordAt + 1;
        unsigned mask = capacity - 1;
        unsigned bucket = key.hash() & mask;
        bool duplicate = false;
        while (table[bucket].record) {
            if (table[bucket].sameFunction(key)) {
                duplicate = true;
                break;
            }
            bucket = (bucket + 1) & mask;
        }
        if (duplicate) {
            auto& earlier = m_functions[functionInBucket[bucket]].code.bytes;
            if (earlier.size() != function.code.bytes.size() || memcmp(earlier.span().data(), function.code.bytes.span().data(), earlier.size()))
                table[bucket].kind = std::numeric_limits<uint32_t>::max();
            isDuplicate.quickSet(index);
            continue;
        }
        table[bucket] = key;
        functionInBucket[bucket] = index;

        memcpy(records + recordAt, &record, sizeof(record));
        recordAt += sizeof(record);
        memcpy(records + recordAt, info.sites.span().data(), info.sites.size() * sizeof(Site));
        recordAt += info.sites.size() * sizeof(Site);
        if (!identifierIndices) {
            memcpy(records + recordAt, info.siteConstants.span().data(), info.siteConstants.sizeInBytes());
            recordAt += info.siteConstants.sizeInBytes();
        }
        if (record.hasQuotes) {
            memcpy(records + recordAt, &functionQuotes[index], sizeof(uint32_t));
            recordAt += sizeof(uint32_t);
        }
        memcpy(records + recordAt, info.plans.span().data(), info.plans.sizeInBytes());
        recordAt += info.plans.sizeInBytes();
    }

    endStep("the records of the functions"_s);
    for (auto& entry : entriesInsideFunctions) {
        uint32_t function = ImageFunction::noSuchFunction;
        for (unsigned bucket = entry.function.hash() & (capacity - 1); functionWithKey[bucket] != std::numeric_limits<uint32_t>::max(); bucket = (bucket + 1) & (capacity - 1)) {
            if (m_functions[functionWithKey[bucket]].key.sameFunction(entry.function)) {
                function = functionWithKey[bucket];
                break;
            }
        }
        RELEASE_ASSERT(function != ImageFunction::noSuchFunction);
        size_t at = placement[function].first + entry.offset;
        RELEASE_ASSERT(at && at < ImageKey::isEntryInsideFunction);
        RELEASE_ASSERT(!(at % imageFunctionAlignment));
        static_assert(hasOneBitSet(imageFunctionAlignment));
        static_assert(GetterSetter::getterShortcutIsCode < imageFunctionAlignment);
        ImageKey key { entry.convention.numberOfParameters | (entry.convention.signature == Signature::List ? 1u << 8 : 0u), entry.key.start, entry.key.kind, static_cast<uint32_t>(at) | ImageKey::isEntryInsideFunction };
        unsigned bucket = key.hash() & (capacity - 1);
        while (table[bucket].record)
            bucket = (bucket + 1) & (capacity - 1);
        table[bucket] = key;
    }

    uint32_t numberOfStates = 1;
    for (size_t index = 0; index < m_functions.size(); ++index) {
        auto& function = m_functions[index];
        auto& info = function.code.info;
        if (!isDuplicate.quickGet(index) && (info.numSlots || !info.indexReferences.isEmpty() || !(function.key.kind & ImageKey::typedFunction)))
            info.stateNumber = numberOfStates++;
    }

    endStep("the records of entries inside functions, and the states"_s);
    // What is done to the code of a function writes nothing outside of it.
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        if (isDuplicate.quickGet(index))
            return;
        auto& function = m_functions[index];
        auto& info = function.code.info;
        auto [codeAt, stubsForFunction] = placement[index];
        memcpy(code + codeAt, function.code.bytes.span().data(), function.code.bytes.size());
        for (auto& reference : info.indexReferences)
            IndexReferences::fill(code + codeAt, reference, info.stateNumber);
        for (auto& call : info.stubCalls) {
            if (call.body) {
                uint32_t amongItsLike = numberOfBodyAmongItsLike[call.body - 1];
                retargetStubCall(code, codeAt + call.offset, outlining.bodies[call.body - 1].onlyCaller == Outlining::none ? stubsForFunction + sharedBodiesAt + sharedBodies.offsets[amongItsLike] : placeOfOwnBody[amongItsLike], false);
                continue;
            }
            size_t target = call.thunk ? stubs.thunkOffsets[call.thunk - 1] : stubs.offsets[static_cast<unsigned>(call.stub)];
            if (call.argumentMoves != StubCall::noArgumentMoves)
                target = argumentThunksAt + argumentThunks.offsets[numberOfArgumentMoves(info.argumentMoves[call.argumentMoves])];
            retargetStubCall(code, codeAt + call.offset, stubsForFunction + target, call.isTailCall);
        }
        for (uint32_t number : ownBodiesOf[index]) {
            size_t at = placeOfOwnBody[number];
            uint32_t offset = ownBodies.offsets[number];
            auto& jump = ownBodies.jumps[number];
            memcpy(code + at, ownBodies.bytes.span().data() + offset, ownBodies.sizeOf(number));
            if (jump.offset != OutlinedBodies::noJump)
                retargetStubCall(code, at + jump.offset - offset, targetOfJumpOfBody(jump, stubsForFunction), true);
        }
    });

    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        auto& info = m_functions[index].code.info;
        size_t codeAt = placement[index].first;
        writeOmittedArguments(code, codeAt - sizeAheadOf(index), omittedArgumentsOf[index][0], omittedArgumentsOf[index][1]);
        for (auto& call : info.stubCalls) {
            if (call.function == StubCall::noFunction)
                continue;
            if (auto target = directTargetOf(index, call)) {
                size_t targetAt = placement[*target].first - sizeAheadFor(*target, call);
                size_t from = codeAt + call.offset;
                if (size_t veneer = farCallees[index].find(farCalleeOf(*target, call)); veneer != notFound) {
                    size_t veneerAt = codeAt + WTF::roundUpToMultipleOf<sizeof(uint32_t)>(m_functions[index].code.bytes.size()) + veneer * sizeOfVeneer;
                    writeVeneer(code, veneerAt, targetAt);
                    targetAt = veneerAt;
                }
                RELEASE_ASSERT((from > targetAt ? from - targetAt : targetAt - from) <= stubCallReach + stubCallReachSlack);
                retargetStubCall(code, from, targetAt, call.isTailCall);
                continue;
            }
        }
    });
    endStep("copying and linking the code"_s);
    m_reportableSites = reportableSites();
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        m_functions[index].code = CompiledCode { };
    });
    endStep("the reportable sites, and emptying the functions"_s);
    Typed::destroyBeside(std::exchange(m_functions, { }));
    endStep("clearing the functions"_s);
    return image;
}

namespace {

struct Registry {
    Lock lock;
    Vector<Image*, 2> images;
    std::atomic<bool> hasAny { false };
    const ProgramData* programData { nullptr };
};

Registry& registry()
{
    static NeverDestroyed<Registry> instance;
    return instance;
}

} // anonymous namespace

static bool keepsFirstPagesWithoutAccess()
{
    static bool s_keepsThem;
    if (lowestAccessibleAddress() >= FaultingAccessInImage::reach || s_keepsThem)
        return true;
#if OS(LINUX) && defined(MAP_FIXED_NOREPLACE)
    size_t sizeOfPage = pageSize();
    for (uintptr_t page = 0; page < FaultingAccessInImage::reach; page += sizeOfPage) {
        void* mapped = mmap(std::bit_cast<void*>(page), sizeOfPage, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        bool isElsewhereForZero = !page && mapped != MAP_FAILED && mapped;
        if (isElsewhereForZero)
            munmap(mapped, sizeOfPage);
        if (mapped == MAP_FAILED || isElsewhereForZero) {
            if (!isElsewhereForZero && errno != EPERM && errno != EACCES)
                return false;
            unsigned char isResident;
            if (mincore(std::bit_cast<void*>(page), sizeOfPage, &isResident) != -1 || errno != ENOMEM)
                return false;
            continue;
        }
        if (mapped != std::bit_cast<void*>(page)) {
            munmap(mapped, sizeOfPage);
            return false;
        }
    }
    s_keepsThem = true;
    return true;
#else
    return false;
#endif
}

Image* Image::registerImage(std::span<const uint8_t> data, const void* code)
{
    if (!Options::useAOT() || !Options::useImmutableIntrinsics() || Options::useJIT())
        return nullptr;
    if (data.size() < sizeof(ImageHeader))
        return nullptr;
    auto& header = *reinterpret_cast<const ImageHeader*>(data.data());
    if (header.magic != imageMagic || header.stamp != imageStamp() || header.size > data.size())
        return nullptr;
    if ((header.tableCapacity && !hasOneBitSet(header.tableCapacity))
        || static_cast<uint64_t>(header.tableOffset) + static_cast<uint64_t>(header.tableCapacity) * sizeof(ImageKey) > header.recordsOffset
        || static_cast<uint64_t>(header.recordsOffset) + header.recordsSize > header.environmentsOffset
        || static_cast<uint64_t>(header.environmentsOffset) + static_cast<uint64_t>(header.numberOfEnvironments) * sizeof(ImageEnvironment) > header.codeOffset
        || header.codeOffset + header.codeSize > header.size)
        return nullptr;
    const ProgramData* programData = ProgramData::tryUse(data.subspan(header.size));
    if (!programData)
        return nullptr;
    auto* image = new Image(data, code);
    auto& all = registry();
    Locker locker { all.lock };
    RELEASE_ASSERT(!all.programData);
    all.programData = programData;
    for (Image* other : all.images)
        RELEASE_ASSERT(!header.codeSize || !other->header().codeSize);
    all.images.append(image);
    all.hasAny.store(true, std::memory_order_release);
    if (header.numberOfFaultingAccesses || header.numberOfTrapsAtThrowSites) {
        RELEASE_ASSERT(!header.numberOfFaultingAccesses || keepsFirstPagesWithoutAccess(), lowestAccessibleAddress());
        classifyAddress(nullptr);
        activateSignalHandlersFor(Signal::AccessFault);
    }
    return image;
}

const ProgramData* ProgramData::get()
{
    auto& all = registry();
    return all.hasAny.load(std::memory_order_acquire) ? all.programData : nullptr;
}

Image& Image::of(const ImageFunction& function)
{
    auto& all = registry();
    for (unsigned i = 0; i < all.images.size(); ++i) {
        Image* image = all.images[i];
        if (static_cast<size_t>(reinterpret_cast<const uint8_t*>(&function) - image->m_data.data()) < image->m_data.size())
            return *image;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const Site* FunctionInfo::sitesInImage() const
{
    return Image::withCode()->at<Site>(sites);
}

uint32_t Image::environmentsSize()
{
    Image* image = withCode();
    return image ? image->header().environmentsSize : 0;
}

uint32_t Image::numberOfFunctions()
{
    Image* image = withCode();
    return image ? image->header().numberOfFunctions : 0;
}

size_t Image::totalDataSize()
{
    Image* image = withCode();
    return image ? static_cast<size_t>(image->header().totalDataSizeIn16Bytes) * 16 : 0;
}

ImageEnvironment Image::environmentOf(uint32_t graphModule)
{
    Image* image = withCode();
    if (!image || graphModule >= image->header().numberOfEnvironments)
        return { };
    return reinterpret_cast<const ImageEnvironment*>(image->m_data.data() + image->header().environmentsOffset)[graphModule];
}

bool Image::containsCode(const void* pointer)
{
    auto& all = registry();
    if (!all.hasAny.load(std::memory_order_acquire))
        return false;
    for (unsigned i = 0; i < all.images.size(); ++i) {
        Image* image = all.images[i];
        if (static_cast<const uint8_t*>(pointer) - static_cast<const uint8_t*>(image->m_code) < static_cast<ptrdiff_t>(image->header().codeSize) && pointer >= image->m_code)
            return true;
    }
    return false;
}

bool Image::containsCharactersOf(const StringImpl& string)
{
    auto& all = registry();
    if (!all.hasAny.load(std::memory_order_acquire))
        return false;
    uintptr_t characters = std::bit_cast<uintptr_t>(bytesOf(string).data());
    for (unsigned i = 0; i < all.images.size(); ++i) {
        std::span data = all.images[i]->m_data;
        if (characters - std::bit_cast<uintptr_t>(data.data()) < data.size())
            return true;
    }
    return false;
}

bool Image::usesNumberedIntrinsics()
{
    auto& all = registry();
    if (!all.hasAny.load(std::memory_order_acquire))
        return true;
    for (unsigned i = 0; i < all.images.size(); ++i) {
        if (all.images[i]->header().intrinsicHash)
            return true;
    }
    return false;
}

Image* Image::withCode()
{
    auto& all = registry();
    if (!all.hasAny.load(std::memory_order_acquire))
        return nullptr;
    for (unsigned i = 0; i < all.images.size(); ++i) {
        if (all.images[i]->header().codeSize)
            return all.images[i];
    }
    return nullptr;
}

static const ImageHeader* s_imageHeader;
static uintptr_t s_imageCode;
static const uint32_t* s_functionStarts;
static const uint32_t* s_codeGranules;
static const uint32_t* s_faultingAccesses;

ImageAddressInfo classifyAddress(const void* address)
{
    if (!s_imageHeader) [[unlikely]] {
        Image* image = Image::withCode();
        if (!image)
            return { };
        RELEASE_ASSERT(image->header().functionsEnd < ReturnAddressTrace::isOfConstructed);
        s_imageCode = std::bit_cast<uintptr_t>(image->code());
        s_functionStarts = image->at<uint32_t>(image->header().functionStartsOffset);
        s_codeGranules = image->at<uint32_t>(image->header().codeGranulesOffset);
        s_faultingAccesses = image->at<uint32_t>(image->header().faultingAccessesOffset);
        WTF::storeStoreFence();
        s_imageHeader = &image->header();
    }
    auto& header = *s_imageHeader;
    uintptr_t offset = std::bit_cast<uintptr_t>(address) - s_imageCode;
    if (offset >= header.functionsEnd)
        return { };
    for (unsigned i = 0; i < header.numberOfStubCopies; ++i) {
        uintptr_t inStubs = offset - header.stubCopies[i];
        if (inStubs >= header.stubsSize)
            continue;
        for (uint32_t inAdapter : header.returnsIntoAdapters) {
            if (inAdapter == inStubs)
                return { ImageAddressInfo::Adapter, 0, 0 };
        }
        for (uint32_t behindCall : header.returnsFromFindingTargets) {
            if (behindCall == inStubs)
                return { ImageAddressInfo::Stub, ImageAddressInfo::frameMayBeOfHostFunction, 0 };
        }
        return { ImageAddressInfo::Stub, 0, 0 };
    }
    const uint32_t* starts = s_functionStarts;
    uint32_t index = s_codeGranules[offset >> header.codeGranuleShift];
    while (starts[index + 1] <= offset)
        ++index;
    return { ImageAddressInfo::Function, index, static_cast<uint32_t>(offset - starts[index]) };
}

ImageAddressInfo::Kind kindOfAddress(const void* address)
{
    if (!s_imageHeader) [[unlikely]]
        return classifyAddress(address).kind;
    auto& header = *s_imageHeader;
    uintptr_t offset = std::bit_cast<uintptr_t>(address) - s_imageCode;
    if (offset >= header.functionsEnd)
        return ImageAddressInfo::NotInImage;
    for (unsigned i = 0; i < header.numberOfStubCopies; ++i) {
        uintptr_t inStubs = offset - header.stubCopies[i];
        if (inStubs >= header.stubsSize)
            continue;
        for (uint32_t inAdapter : header.returnsIntoAdapters) {
            if (inAdapter == inStubs)
                return ImageAddressInfo::Adapter;
        }
        return ImageAddressInfo::Stub;
    }
    return ImageAddressInfo::Function;
}

// Takes no lock and allocates nothing: the handler asks.
std::optional<FaultingAccessInImage> faultingAccessAt(const void* instruction)
{
    if (!s_imageHeader)
        return std::nullopt;
    auto& header = *s_imageHeader;
    uintptr_t offset = std::bit_cast<uintptr_t>(instruction) - s_imageCode;
    if (offset >= header.functionsEnd)
        return std::nullopt;
    uint32_t count = header.numberOfFaultingAccesses;
    std::span checkpoints { reinterpret_cast<const std::array<uint32_t, 2>*>(s_faultingAccesses), (count + faultingAccessesBetweenCheckpoints - 1) / faultingAccessesBetweenCheckpoints };
    auto behind = std::ranges::lower_bound(checkpoints, static_cast<uint32_t>(offset), { }, [](auto& checkpoint) {
        return checkpoint[0];
    });
    if (behind == checkpoints.begin())
        return std::nullopt;
    auto& checkpoint = *std::prev(behind);
    std::span words { s_faultingAccesses + 2 * checkpoints.size(), header.numberOfThrowSitesOfFaultingAccesses };
    std::span wideNames { reinterpret_cast<const std::array<uint32_t, 2>*>(words.data() + words.size()), header.numberOfFaultingAccessesWithWideName };
    auto* numbers = reinterpret_cast<const uint8_t*>(wideNames.data() + wideNames.size());
    bool numbersAreNarrow = words.size() <= std::numeric_limits<uint16_t>::max() + 1u;
    const uint8_t* at = numbers + count * (numbersAreNarrow ? sizeof(uint16_t) : sizeof(uint32_t)) + checkpoint[1];
    uint32_t index = static_cast<uint32_t>(std::prev(behind) - checkpoints.begin()) * faultingAccessesBetweenCheckpoints;
    uint32_t last = std::min(count, index + faultingAccessesBetweenCheckpoints);
    uint64_t place = checkpoint[0];
    uint64_t step = 0;
    for (;; ++index) {
        if (index == last)
            return std::nullopt;
        step = readVarint(at);
        place += step >> 1;
        if (place >= offset)
            break;
    }
    if (place != offset)
        return std::nullopt;
    uint32_t number = numbersAreNarrow ? reinterpret_cast<const uint16_t*>(numbers)[index] : reinterpret_cast<const uint32_t*>(numbers)[index];
    FaultingAccessInImage result { { words[number], ThrowSite::none }, !!(step & 1) };
    auto name = std::ranges::lower_bound(wideNames, index, { }, [](auto& entry) {
        return entry[0];
    });
    if (name != wideNames.end() && (*name)[0] == index)
        result.throwSite[1] = (*name)[1];
    return result;
}

#if CPU(ARM64)
static uintptr_t& registerOf(PlatformRegisters& registers, unsigned number)
{
    RELEASE_ASSERT(number == 16 || number == 30);
#if OS(DARWIN)
    return number == 30 ? reinterpret_cast<uintptr_t&>(registers.__lr) : reinterpret_cast<uintptr_t&>(registers.__x[number]);
#elif OS(WINDOWS)
    return reinterpret_cast<uintptr_t&>(registers.X[number]);
#elif OS(FREEBSD)
    return number == 30 ? reinterpret_cast<uintptr_t&>(registers.machineContext.mc_gpregs.gp_lr) : reinterpret_cast<uintptr_t&>(registers.machineContext.mc_gpregs.gp_x[number]);
#else
    return reinterpret_cast<uintptr_t&>(registers.machineContext.regs[number]);
#endif
}
#elif CPU(X86_64)
static uintptr_t& scratchRegisterOf(PlatformRegisters& registers)
{
#if OS(DARWIN)
    return reinterpret_cast<uintptr_t&>(registers.__r11);
#elif OS(WINDOWS)
    return reinterpret_cast<uintptr_t&>(registers.R11);
#elif OS(FREEBSD)
    return reinterpret_cast<uintptr_t&>(registers.machineContext.mc_r11);
#else
    return reinterpret_cast<uintptr_t&>(registers.machineContext.gregs[REG_R11]);
#endif
}
#endif

#if CPU(X86_64) && OS(LINUX)
static bool isTrapAtThrowSite(const void* instruction)
{
    if (!s_imageHeader || !s_imageHeader->numberOfTrapsAtThrowSites)
        return false;
    if (std::bit_cast<uintptr_t>(instruction) - s_imageCode >= s_imageHeader->functionsEnd || *static_cast<const uint8_t*>(instruction) != 0xf4)
        return false;
    ImageAddressInfo place = classifyAddress(instruction);
    if (place.kind != ImageAddressInfo::Function)
        return false;
    const ProgramData* programData = ProgramData::get();
    const ImageFunction* function = programData ? programData->infos()[place.index].function() : nullptr;
    return function && tryCallSiteAt(*function, place.offset + offsetOfReturnAddressInTrap);
}
#endif

void prepareFaultingAccesses()
{
    addSignalHandler(Signal::AccessFault, [](Signal, SigInfo& info, PlatformRegisters& registers) {
        auto instructionPointer = MachineContext::instructionPointer(registers);
        if (!instructionPointer)
            return SignalAction::NotHandled;
#if CPU(X86_64) && OS(LINUX)
        if (isTrapAtThrowSite(instructionPointer->untaggedPtr())) {
            scratchRegisterOf(registers) = std::bit_cast<uintptr_t>(instructionPointer->untaggedPtr()) + offsetOfReturnAddressInTrap;
            MachineContext::setInstructionPointer(registers, std::bit_cast<void*>(s_imageCode + s_imageHeader->stubOffsets[static_cast<unsigned>(Stub::ThrowAtTrap)]));
            return SignalAction::Handled;
        }
#endif
        uintptr_t address = std::bit_cast<uintptr_t>(info.faultingAddress);
        if (address >= FaultingAccessInImage::reach && address - MarkedBlockCage::base() >= FaultingAccessInImage::reach)
            return SignalAction::NotHandled;
        auto access = faultingAccessAt(instructionPointer->untaggedPtr());
        if (!access)
            return SignalAction::NotHandled;
        uintptr_t returnAddress = std::bit_cast<uintptr_t>(instructionPointer->untaggedPtr()) + sizeOfNearCall;
#if CPU(ARM64)
        if (access->hasNoFrame)
            registerOf(registers, 16) = registerOf(registers, 30);
        registerOf(registers, 30) = returnAddress;
#elif CPU(X86_64)
        scratchRegisterOf(registers) = returnAddress;
#endif
        MachineContext::setInstructionPointer(registers, std::bit_cast<void*>(s_imageCode + s_imageHeader->stubOffsets[static_cast<unsigned>(access->hasNoFrame ? Stub::LeafThrowAtFault : Stub::ThrowAtFault)]));
        return SignalAction::Handled;
    });
}

uintptr_t startOfImageCode()
{
    if (!s_imageHeader) [[unlikely]]
        classifyAddress(nullptr);
    return s_imageCode;
}

std::optional<uint32_t> imageCodeOffset(const void* address)
{
    Image* image = Image::withCode();
    if (!image)
        return std::nullopt;
    uintptr_t offset = std::bit_cast<uintptr_t>(address) - std::bit_cast<uintptr_t>(image->code());
    if (offset >= image->header().codeSize)
        return std::nullopt;
    return static_cast<uint32_t>(offset);
}

bool hasCode()
{
    return s_imageHeader || Image::withCode();
}

// Behind the first word. Of sites in the order of the code: how many numbers have no code.
static uint64_t skipInlineFrames(const uint8_t*& at, uint64_t first)
{
    uint64_t numberOfSitesWithoutCode = first & 4 ? readVarint(at) : 0;
    if (!(first & 2))
        return numberOfSitesWithoutCode;
    readVarint(at);
    uint64_t size = readVarint(at);
    at += size;
    return numberOfSitesWithoutCode;
}

ImageInlineFrame inlineFrameOf(const ImageFunction& function, unsigned frame)
{
    RELEASE_ASSERT(function.hasInlineFrames && frame);
    Image& image = Image::of(function);
    const uint8_t* at = image.at<uint8_t>(image.header().callSitesOffset) + function.callSites;
    uint64_t first = readVarint(at);
    RELEASE_ASSERT(first & 2);
    uint64_t numberOfSitesWithoutCode = first & 4 ? readVarint(at) : 0;
    uint64_t count = readVarint(at);
    RELEASE_ASSERT(frame <= count);
    readVarint(at);
    ImageInlineFrame result { };
    for (unsigned i = 0; i < frame; ++i) {
        uint64_t parentAndFlags = readVarint(at);
        result.parent = static_cast<uint32_t>(parentAndFlags >> 2);
        result.flags = parentAndFlags & 3;
        result.callSite = static_cast<uint32_t>(first & 4 ? numberOfSitesWithoutCode - count + i + 1 : readVarint(at));
        result.function = static_cast<uint32_t>(readVarint(at));
    }
    return result;
}

void InlineFramesRead::read(const ImageFunction& function, unsigned last)
{
    RELEASE_ASSERT(function.hasInlineFrames && last);
    Image& image = Image::of(function);
    const uint8_t* at = image.at<uint8_t>(image.header().callSitesOffset) + function.callSites;
    uint64_t first = readVarint(at);
    RELEASE_ASSERT(first & 2);
    uint64_t numberOfSitesWithoutCode = first & 4 ? readVarint(at) : 0;
    uint64_t count = readVarint(at);
    RELEASE_ASSERT(last <= count);
    readVarint(at);
    m_function = &function;
    m_frames.shrink(0);
    m_frames.reserveCapacity(last);
    for (unsigned frame = 1; frame <= last; ++frame) {
        uint64_t parentAndFlags = readVarint(at);
        uint32_t callSite = static_cast<uint32_t>(first & 4 ? numberOfSitesWithoutCode - count + frame : readVarint(at));
        uint32_t inlined = static_cast<uint32_t>(readVarint(at));
        RELEASE_ASSERT(parentAndFlags >> 2 < frame);
        m_frames.unsafeAppendWithoutCapacityCheck(ImageInlineFrame { static_cast<uint32_t>(parentAndFlags >> 2), callSite, inlined, static_cast<uint8_t>(parentAndFlags & 3) });
    }
}

FramesOfSites::FramesOfSites(const ImageFunction& function)
{
    if (!function.callSites)
        return;
    Image& image = Image::of(function);
    const uint8_t* at = image.at<uint8_t>(image.header().callSitesOffset) + function.callSites;
    uint64_t first = readVarint(at);
    RELEASE_ASSERT(first & 4);
    uint64_t withoutCode = readVarint(at);
    if (first & 2) {
        m_inlinedCalls = readVarint(at);
        uint64_t size = readVarint(at);
        m_inlineFrames = at;
        at += size;
    }
    RELEASE_ASSERT(m_inlinedCalls <= withoutCode);
    m_inFunctionItself = withoutCode - m_inlinedCalls;
    m_withCode = first >> 3;
    m_sites = at + SiteCheckpoint::numberFor(m_withCode) * sizeof(SiteCheckpoint);
}

unsigned FramesOfSites::next()
{
    if (m_inFunctionItself) {
        --m_inFunctionItself;
        return 0;
    }
    if (m_inlinedCalls) {
        --m_inlinedCalls;
        unsigned parent = static_cast<unsigned>(readVarint(m_inlineFrames) >> 2);
        readVarint(m_inlineFrames);
        return parent;
    }
    if (!m_withCode)
        return 0;
    --m_withCode;
    if (readVarint(m_sites) & 1)
        m_frame = static_cast<unsigned>(readVarint(m_sites) >> 1);
    return m_frame;
}

std::optional<uint32_t> tryCallSiteAt(const ImageFunction& function, uint32_t offsetOfReturnAddress, bool* constructs)
{
    if (!function.callSites)
        return std::nullopt;
    Image& image = Image::of(function);
    const uint8_t* at = image.at<uint8_t>(image.header().callSitesOffset) + function.callSites;
    uint32_t offset = 0;
    int64_t site = 0;
    uint64_t first = readVarint(at);
    uint64_t number = skipInlineFrames(at, first);
    if (first & 4) {
        unsigned frame = 0;
        uint64_t count = first >> 3;
        if (uint64_t numberOfCheckpoints = SiteCheckpoint::numberFor(count)) {
            const uint8_t* checkpoints = at;
            at += numberOfCheckpoints * sizeof(SiteCheckpoint);
            uint64_t numberAhead = 0;
            for (uint64_t end = numberOfCheckpoints; numberAhead < end;) {
                uint64_t middle = numberAhead + (end - numberAhead) / 2;
                if (SiteCheckpoint::at(checkpoints, middle).offset * codeOffsetUnit < offsetOfReturnAddress)
                    numberAhead = middle + 1;
                else
                    end = middle;
            }
            if (numberAhead) {
                SiteCheckpoint checkpoint = SiteCheckpoint::at(checkpoints, numberAhead - 1);
                offset = checkpoint.offset * codeOffsetUnit;
                frame = checkpoint.positionAndFrame & 0xff;
                at += checkpoint.positionAndFrame >> 8;
                number += numberAhead * SiteCheckpoint::sitesBetween;
                count -= numberAhead * SiteCheckpoint::sitesBetween;
            }
        }
        uint64_t frameAndConstructs = static_cast<uint64_t>(frame) << 1;
        while (count--) {
            uint64_t word = readVarint(at);
            offset += (word >> 1) * codeOffsetUnit;
            if (word & 1)
                frameAndConstructs = readVarint(at);
            ++number;
            if (offset >= offsetOfReturnAddress) {
                if (offset == offsetOfReturnAddress) {
                    if (constructs)
                        *constructs = word & frameAndConstructs & 1;
                    return PackedSite::pack(static_cast<unsigned>(frameAndConstructs >> 1), CallSiteIndex(BytecodeIndex(static_cast<uint32_t>(number))).bits());
                }
                break;
            }
        }
        return std::nullopt;
    }
    for (uint64_t count = first >> 3; count--;) {
        offset += readVarint(at) * codeOffsetUnit;
        uint64_t step = readVarint(at);
        site += static_cast<int64_t>(step >> 1) ^ -static_cast<int64_t>(step & 1);
        if (offset >= offsetOfReturnAddress) {
            if (offset == offsetOfReturnAddress)
                return static_cast<uint32_t>(site);
            break;
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> spreadSite(const ImageFunction& function, uint32_t callSite, unsigned item)
{
    if (!function.callSites)
        return std::nullopt;
    Image& image = Image::of(function);
    const uint8_t* at = image.at<uint8_t>(image.header().callSitesOffset) + function.callSites;
    uint64_t first = readVarint(at);
    if (!(first & 1))
        return std::nullopt;
    skipInlineFrames(at, first);
    if (first & 4)
        at += SiteCheckpoint::numberFor(first >> 3) * sizeof(SiteCheckpoint);
    for (uint64_t count = first >> 3; count--;) {
        uint64_t word = readVarint(at);
        if (!(first & 4) || (word & 1))
            readVarint(at);
    }
    for (uint64_t count = readVarint(at); count--;) {
        uint64_t inlineCallSite = readVarint(at);
        uint64_t itemWord = readVarint(at);
        uint64_t site = readVarint(at);
        if (inlineCallSite == callSite && itemWord == item)
            return static_cast<uint32_t>(site);
    }
    return std::nullopt;
}

bool Image::hasAny()
{
    return registry().hasAny.load(std::memory_order_acquire);
}

const void* Image::stubAddress(Stub stub)
{
    if (!hasAny())
        return nullptr;
    auto& all = registry();
    Locker locker { all.lock };
    for (Image* image : all.images) {
        if (image->header().codeSize)
            return static_cast<const uint8_t*>(image->m_code) + image->header().stubOffsets[static_cast<unsigned>(stub)];
    }
    return nullptr;
}

auto Image::codeForRegExp(const String& pattern, OptionSet<Yarr::Flags> flags) -> std::optional<CodeForRegExp>
{
    if (!hasAny())
        return std::nullopt;
    uint32_t hash = ImageRegExp::hashOf(pattern, flags);
    uint32_t rawFlags = ImageRegExp::significantFlags(flags).toRaw();
    auto& all = registry();
    Locker locker { all.lock };
    for (Image* image : all.images) {
        auto& header = image->header();
        std::span regExps { image->at<ImageRegExp>(header.regExpsOffset), header.numberOfRegExps };
        auto* text = image->at<uint8_t>(0);
        for (auto it = std::ranges::lower_bound(regExps, hash, { }, &ImageRegExp::hash); it != regExps.end() && it->hash == hash; ++it) {
            if (it->flags != rawFlags || it->length != pattern.length())
                continue;
            StringView said = it->is8Bit ? StringView { std::span { reinterpret_cast<const Latin1Character*>(text + it->text), it->length } } : StringView { std::span { reinterpret_cast<const char16_t*>(text + it->text), it->length } };
            if (said.rawCharacters() == StringView(pattern).rawCharacters() || (said.is8Bit() == pattern.is8Bit() ? !it->length || !memcmp(said.rawCharacters(), StringView(pattern).rawCharacters(), static_cast<size_t>(it->length) << !it->is8Bit) : said == pattern))
                return CodeForRegExp { static_cast<const uint8_t*>(image->m_code) + it->codeFor8Bit, static_cast<const uint8_t*>(image->m_code) + it->codeFor16Bit, it->numSubpatterns, it->minimumSize, static_cast<Yarr::SpecificPattern>(it->specificPattern), !!it->keepsMoreOfPattern, !it->hasNoCode, !!it->sourceIsPattern, said };
        }
    }
    return std::nullopt;
}

const ImageFunction* Image::lookup(const ImageKey& key) const
{
    auto& header = this->header();
    std::span<const ImageKey> table { reinterpret_cast<const ImageKey*>(m_data.data() + header.tableOffset), header.tableCapacity };
    if (table.empty() && ProgramData::get())
        table = ProgramData::get()->imageKeys();
    if (table.empty())
        return nullptr;
    unsigned mask = table.size() - 1;
    for (unsigned bucket = key.hash() & mask; table[bucket].record; bucket = (bucket + 1) & mask) {
        if (table[bucket].sameFunction(key))
            return reinterpret_cast<const ImageFunction*>(m_data.data() + header.recordsOffset + table[bucket].record - 1);
    }
    return nullptr;
}

std::optional<ImageView> ImageView::tryCreate(std::span<const uint8_t> data, const void* address)
{
    if (data.size() < sizeof(ImageHeader))
        return std::nullopt;
    auto& header = *reinterpret_cast<const ImageHeader*>(data.data());
    if (header.magic != imageMagic || header.stamp != imageStamp() || header.size > data.size())
        return std::nullopt;
    return ImageView { data, address };
}

std::optional<ImageView::Function> ImageView::find(const ImageKey& key) const
{
    auto& header = this->header();
    auto table = keys();
    if (table.empty())
        return std::nullopt;
    unsigned mask = table.size() - 1;
    for (unsigned bucket = key.hash() & mask; table[bucket].record; bucket = (bucket + 1) & mask) {
        if (!table[bucket].sameFunction(key))
            continue;
        auto& function = *reinterpret_cast<const ImageFunction*>(m_data.data() + header.recordsOffset + table[bucket].record - 1);
        uint64_t start = reinterpret_cast<const uint32_t*>(m_data.data() + header.functionStartsOffset)[function.index];
        auto futureLocation = [&](const void* pointer) { return m_address + (static_cast<const uint8_t*>(pointer) - m_data.data()); };
        return Function { EntryWord::encode(start, function.convention()), function.index,
            reinterpret_cast<const Site*>(futureLocation(function.sites())), reinterpret_cast<const ImageFunction*>(futureLocation(&function)), function.numSlots, !!function.startsCold, !!function.hasSiteConstants, !!function.hasInlineFrames };
    }
    return std::nullopt;
}

std::span<const ImageKey> ImageView::keys() const
{
    auto& header = this->header();
    if (header.tableCapacity)
        return { reinterpret_cast<const ImageKey*>(m_data.data() + header.tableOffset), header.tableCapacity };
    return { reinterpret_cast<const ImageKey*>(m_data.data() + header.size), (m_data.size() - header.size) / sizeof(ImageKey) };
}

std::pair<Image*, const ImageFunction*> Image::find(const ImageKey& key)
{
    auto& all = registry();
    Locker locker { all.lock };
    for (Image* image : all.images) {
        if (auto* function = image->lookup(key))
            return { image, function };
    }
    return { nullptr, nullptr };
}

uint32_t moduleIDFor(SourceProvider& provider)
{
    return provider.aotModuleID();
}

ImageKey imageKeyForTopLevelCode(uint32_t module)
{
    ImageKey key;
    key.module = module;
    key.start = std::numeric_limits<uint32_t>::max();
    key.kind = 0xff << 1;
    return key;
}

std::optional<ImageKey> imageKeyFor(ScriptExecutable* scriptExecutable)
{
    if (scriptExecutable->isFunctionExecutable() || scriptExecutable->isEvalExecutable())
        return std::nullopt;
    SourceProvider* provider = scriptExecutable->source().provider();
    if (!provider)
        return std::nullopt;
    uint32_t module = moduleIDFor(*provider);
    if (!module)
        return std::nullopt;
    return imageKeyForTopLevelCode(module);
}

unsigned codeHash(std::span<const uint8_t> code)
{
    unsigned hash = 2166136261u;
    for (uint8_t byte : code)
        hash = (hash ^ byte) * 16777619u;
    return hash;
}

static String nameForLogging(ScriptExecutable* executable)
{
    if (auto* function = dynamicDowncast<FunctionExecutable>(executable))
        return function->ecmaName().string();
    return "(top level)"_s;
}

Instance& instanceOf(JSScope* scope)
{
    for (JSScope* current = scope; current; current = current->next()) {
        if (current->type() == ModuleEnvironmentType)
            return Instance::ensure(uncheckedDowncast<JSModuleEnvironment>(current)->moduleRecord()->moduleLoader());
    }
    return Instance::ensure(scope->realm());
}

ImageCode findInImage(ScriptExecutable* executable, JSScope* scope)
{
    if (!Options::useAOT())
        return { };
    if (!Image::hasAny())
        return { };

    Image* image = nullptr;
    const ImageFunction* function = nullptr;
    ImageKey key;
    auto executableKey = imageKeyFor(executable);
    if (!executableKey) {
        if (Options::verboseAOTCompilation()) [[unlikely]]
            dataLogLn("AOT: no key for ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL());
        return { };
    }
    key = *executableKey;
    std::tie(image, function) = Image::find(key);
    if (!function) {
        if (Options::verboseAOTCompilation()) [[unlikely]]
            dataLogLn("AOT: not in the image: ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL(), " (module ", key.module, " start ", key.start, " kind ", key.kind, ")");
        return { };
    }

    {
        Instance* instance = &instanceOf(scope);
        uint32_t index = function->index;
        if (instance->isLinked(index) && FunctionRef { instance, index }.executable() != executable)
            return { };
    }

    if (image->header().numberOfProgramIdentifiers) {
        uint32_t index = function->index;
        if (!instanceOf(scope).infos[index].sites)
            return { };
    }

    if (Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("AOT: ", nameForLogging(executable), " (module ", key.module, " start ", key.start, " kind ", key.kind, ") is at ", RawPointer(image->codeFor(*function)), " size ", image->sizeOfCodeOf(*function), " hash ", codeHash({ image->codeFor(*function), image->sizeOfCodeOf(*function) }));
    return { image, function };
}

Ref<JITCode> codeFromImage(ImageCode code, UnlinkedCodeBlock* unlinkedCodeBlock)
{
    auto [image, function] = code;
    return adoptRef(*new JITCode(const_cast<uint8_t*>(image->codeFor(*function)), *function, JITCode::entryBlockFor(unlinkedCodeBlock)));
}

Ref<JITCode> jitCodeForImageFunction(ImageCode code, CodeSpecializationKind kind)
{
    auto [image, function] = code;
    return adoptRef(*new JITCode(const_cast<uint8_t*>(image->codeFor(*function)), *function, isCall(kind) ? JITCode::Way::Call : JITCode::Way::Construct));
}

} // namespace AOT

bool isAOTImagePC(const void* pc)
{
    return AOT::Image::containsCode(pc);
}

bool registerAOTImage(std::span<const uint8_t> image, const void* code)
{
    return !!AOT::Image::registerImage(image, code);
}

#if OS(DARWIN) || OS(LINUX) || OS(FREEBSD)

static void* mapCode(int fileDescriptor, int64_t at, size_t size, const char*& failureReason)
{
    void* code = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fileDescriptor, at);
    if (code == MAP_FAILED && errno == EPERM) {
        code = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fileDescriptor, at);
        if (code != MAP_FAILED && mprotect(code, size, PROT_READ | PROT_EXEC)) {
            munmap(code, size);
            code = MAP_FAILED;
        }
    }
    if (code == MAP_FAILED) {
        failureReason = strerror(errno);
        return nullptr;
    }
#if OS(DARWIN)
    mach_vm_protect(mach_task_self(), reinterpret_cast<mach_vm_address_t>(code), size, true, VM_PROT_READ | VM_PROT_EXECUTE);
#endif
    return code;
}

static void unmapCode(void* code, size_t size)
{
    munmap(code, size);
}

#if OS(LINUX)
static void* copyCodeToAnonymousMemory(std::span<const uint8_t> code, const char*& failureReason)
{
    void* copy = mmap(nullptr, code.size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (copy == MAP_FAILED) {
        failureReason = strerror(errno);
        return nullptr;
    }
    memcpySpan(std::span { static_cast<uint8_t*>(copy), code.size() }, code);
    if (mprotect(copy, code.size(), PROT_READ | PROT_EXEC)) {
        failureReason = strerror(errno);
        munmap(copy, code.size());
        return nullptr;
    }
    __builtin___clear_cache(static_cast<char*>(copy), static_cast<char*>(copy) + code.size());
    return copy;
}
#endif

#elif OS(WINDOWS)

static size_t allocationGranularity()
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwAllocationGranularity;
}

static void* mapCode(void* file, int64_t at, size_t size, const char*& failureReason)
{
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_EXECUTE_READ, 0, 0, nullptr);
    if (!mapping) {
        failureReason = "its file cannot be mapped for execution";
        return nullptr;
    }
    uint64_t start = static_cast<uint64_t>(at) & ~static_cast<uint64_t>(allocationGranularity() - 1);
    size_t skipped = static_cast<uint64_t>(at) - start;
    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_EXECUTE, static_cast<DWORD>(start >> 32), static_cast<DWORD>(start), size + skipped);
    CloseHandle(mapping);
    if (!view) {
        failureReason = "its code cannot be mapped for execution";
        return nullptr;
    }
    return static_cast<uint8_t*>(view) + skipped;
}

static void unmapCode(void* code, size_t)
{
    UnmapViewOfFile(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(code) & ~static_cast<uintptr_t>(allocationGranularity() - 1)));
}

#endif

#if OS(DARWIN) || OS(LINUX) || OS(FREEBSD) || OS(WINDOWS)

static constexpr bool isOnPageBoundaries(uint64_t position, size_t offsetOfCode, size_t sizeOfImage, size_t pageSize)
{
    return !(position % pageSize) && !(offsetOfCode % pageSize) && !(sizeOfImage % pageSize);
}
static_assert(isOnPageBoundaries(3 * AOT::imagePageSize, AOT::imagePageSize, 5 * AOT::imagePageSize, CeilingOnPageSize));

template<typename MapCode, typename UnmapCode>
static AOTFileUse useAOTFile(std::span<const uint8_t> bytes, uint64_t position, const MapCode& mapCode, const UnmapCode& unmapCode)
{
    AOTFileUse result;
    auto neither = [&](const char* why) {
        result.programDataRejectionReason = result.imageRejectionReason = why;
        return result;
    };
    if (!Options::useAOT())
        return neither("useAOT is off");
    auto sizeOfImage = aotImageSize(bytes);
    auto codeRange = aotImageCodeRange(bytes);
    if (!sizeOfImage || !codeRange)
        return neither("not an image");
    auto [offsetOfCode, codeSizeInBytes] = *codeRange;
    if (!isOnPageBoundaries(position, offsetOfCode, *sizeOfImage, WTF::pageSize()))
        return neither("it is not on a page boundary");

    if (*sizeOfImage >= bytes.size())
        return neither("it has no program data");
    if (AOT::Image::hasAny())
        return neither("the process has one already");
    if (lowestAccessibleAddress() < AOT::FaultingAccessInImage::reach) [[unlikely]] {
        auto& header = *reinterpret_cast<const AOT::ImageHeader*>(bytes.data());
        if (header.stamp == AOT::imageStamp() && header.numberOfFaultingAccesses && !AOT::keepsFirstPagesWithoutAccess())
            return neither("it needs the first 16384 bytes of memory to be without access, and on this system something is mapped there or that cannot be made sure of");
    }
    if (!codeSizeInBytes)
        return neither("it holds no code");
    result.programDataSize = bytes.size() - *sizeOfImage;

    void* code = nullptr;
#if OS(LINUX)
    if (Options::useAOTPerfMap()) [[unlikely]]
        code = copyCodeToAnonymousMemory(bytes.subspan(offsetOfCode, codeSizeInBytes), result.imageRejectionReason);
    else
#endif
        code = mapCode(offsetOfCode, codeSizeInBytes, result.imageRejectionReason);
    if (!code)
        return result;
    if (!registerAOTImage(bytes, code)) {
        if (code)
            unmapCode(code, codeSizeInBytes);
        return neither("it was compiled for another build of the engine");
    }
#if OS(LINUX)
    if (Options::useAOTPerfMap()) [[unlikely]]
        AOT::writePerfMap();
#endif
    result.codeSizeInBytes = codeSizeInBytes;
    result.code = code;
    return result;
}

AOTFileUse useAOTFile(std::span<const uint8_t> bytes, AOTFileHandle file, int64_t offsetInFile)
{
    return useAOTFile(bytes, offsetInFile, [&](size_t offsetOfCode, size_t size, const char*& failureReason) {
        return mapCode(file, offsetInFile + offsetOfCode, size, failureReason);
    }, unmapCode);
}

#else

AOTFileUse useAOTFile(std::span<const uint8_t>, AOTFileHandle, int64_t)
{
    constexpr const char* reason = "not supported on this platform";
    return { reason, reason };
}

#endif

#if OS(DARWIN)

AOTFileUse useAOTFileInLoadedSection(std::span<const uint8_t> bytes)
{
    return useAOTFile(bytes, reinterpret_cast<uintptr_t>(bytes.data()), [&](size_t offsetOfCode, size_t, const char*&) -> void* {
        return const_cast<uint8_t*>(bytes.data()) + offsetOfCode;
    }, [](void*, size_t) { });
}

#elif OS(LINUX) || OS(FREEBSD)

AOTFileUse useAOTFileInLoadedSection(std::span<const uint8_t> bytes)
{
    return useAOTFile(bytes, reinterpret_cast<uintptr_t>(bytes.data()), [&](size_t offsetOfCode, size_t size, const char*& failureReason) -> void* {
        void* code = const_cast<uint8_t*>(bytes.data()) + offsetOfCode;
        if (mprotect(code, size, PROT_READ | PROT_EXEC)) {
            failureReason = strerror(errno);
            return nullptr;
        }
        return code;
    }, [](void* code, size_t size) {
        mprotect(code, size, PROT_READ);
    });
}

#elif !OS(WINDOWS)

AOTFileUse useAOTFileInLoadedSection(std::span<const uint8_t>)
{
    constexpr const char* reason = "not supported on this platform";
    return { reason, reason };
}

#endif

#if OS(WINDOWS)

AOTFileUse useAOTFileInLoadedSection(std::span<const uint8_t> bytes)
{
    return useAOTFile(bytes, reinterpret_cast<uintptr_t>(bytes.data()), [&](size_t offsetOfCode, size_t size, const char*& failureReason) -> void* {
        void* code = const_cast<uint8_t*>(bytes.data()) + offsetOfCode;
        DWORD previousProtection;
        if (!VirtualProtect(code, size, PAGE_EXECUTE_READ, &previousProtection)) {
            failureReason = "its code cannot be made executable";
            return nullptr;
        }
        FlushInstructionCache(GetCurrentProcess(), code, size);
        return code;
    }, [](void* code, size_t size) {
        DWORD previousProtection;
        VirtualProtect(code, size, PAGE_READONLY, &previousProtection);
    });
}

#endif

Vector<uint8_t> buildAOTFile(VM& vm, std::span<const SourceCode> sources, bool isModule, std::span<const uint8_t> typedProgram, unsigned numberOfModulesWithoutEntry)
{
    vm.useImmutableIntrinsics = true;

    EncoderStringTable strings;
    BytecodeLinkEncoder::Result linked;
    {
        DeferGC deferGC(vm);
        BytecodeLinkEncoder::Hints hints;
        hints.compileAheadOfTime = true;
        BytecodeLinkEncoder encoder(vm, &strings, WTF::move(hints));
        Vector<uint32_t, 1> modules;
        unsigned numberOfModules = 0;
        for (auto& source : sources) {
            ParserError error;
            UnlinkedCodeBlock* codeBlock = isModule
                ? static_cast<UnlinkedCodeBlock*>(recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, source, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, std::numeric_limits<unsigned>::max(), OptimizeBytecode::Yes))
                : static_cast<UnlinkedCodeBlock*>(recursivelyGenerateUnlinkedCodeBlockForProgram(vm, source, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, std::numeric_limits<unsigned>::max(), OptimizeBytecode::Yes));
            if (error.isValid() || !codeBlock)
                return { };
            encoder.addModule(isModule ? sourceCodeKeyForSerializedModule(vm, source) : sourceCodeKeyForSerializedProgram(vm, source), codeBlock, source, { });
            if (numberOfModules++ >= numberOfModulesWithoutEntry)
                modules.append(numberOfModules - 1);
        }
        if (typedProgram.empty()) {
            dataLogLn("AOT: the typed program is empty");
            return { };
        }
        if (encoder.setTypedProgram(typedProgram, modules.span()))
            linked = encoder.finish();
        for (auto& error : encoder.takeBuildErrors()) {
            if (error.path.isEmpty())
                dataLogLn("AOT: ", error.message);
            else
                dataLogLn("AOT: ", error.message, " (", error.path, ":", error.position.line, ":", error.position.column, ")");
        }
    }
    auto sizeOfImage = aotImageSize(linked.aotImage.span());
    if (!linked.payload || !sizeOfImage)
        return { };
    MonotonicTime start = MonotonicTime::now();
    AOT::ProgramData::RetainedPositions positions { linked.reportableSites, [](uint32_t, LineColumn, UTF8CString&, LineColumn&) { return false; } };
    Vector<uint8_t> data = AOT::ProgramData::build(vm, strings.serialize().span(), linked.payload->span(), linked.moduleEntryOffsets.span(), linked.aotImage.span(), positions, linked.reportableSites.span(), linked.variablesExportedByModules.span());
    if (data.isEmpty())
        return { };
    AOT::ProgramData::release(linked.reportableSites);
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: the program data took ", static_cast<unsigned>((MonotonicTime::now() - start).milliseconds()), " ms");
    start = MonotonicTime::now();
    Vector<uint8_t> file;
    file.reserveInitialCapacity(*sizeOfImage + data.size());
    file.append(linked.aotImage.span().first(*sizeOfImage));
    file.appendVector(data);
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: copying the image into the file took ", static_cast<unsigned>((MonotonicTime::now() - start).milliseconds()), " ms");
    return file;
}

std::optional<size_t> aotImageSize(std::span<const uint8_t> image)
{
    if (image.size() < sizeof(AOT::ImageHeader))
        return std::nullopt;
    auto& header = *reinterpret_cast<const AOT::ImageHeader*>(image.data());
    if (header.magic != AOT::imageMagic || header.size > image.size())
        return std::nullopt;
    return static_cast<size_t>(header.size);
}

std::optional<unsigned> aotImageRegExpCount(std::span<const uint8_t> image)
{
    if (!aotImageSize(image))
        return std::nullopt;
    return reinterpret_cast<const AOT::ImageHeader*>(image.data())->numberOfRegExps;
}

std::optional<std::pair<size_t, size_t>> aotImageCodeRange(std::span<const uint8_t> image)
{
    if (image.size() < sizeof(AOT::ImageHeader))
        return std::nullopt;
    auto& header = *reinterpret_cast<const AOT::ImageHeader*>(image.data());
    if (header.magic != AOT::imageMagic || header.size > image.size() || header.codeOffset > header.size)
        return std::nullopt;
    return std::pair { static_cast<size_t>(header.codeOffset), static_cast<size_t>(header.size - header.codeOffset) };
}

size_t aotImageAlignment() { return AOT::imagePageSize; }

} // namespace JSC

#elif USE(BUN_JSC_ADDITIONS)


namespace JSC {

bool isAOTImagePC(const void*) { return false; }
bool registerAOTImage(std::span<const uint8_t>, const void*) { return false; }
AOTFileUse useAOTFile(std::span<const uint8_t>, int, int64_t) { return { "not supported on this platform", "not supported on this platform" }; }
AOTFileUse useAOTFileInLoadedSection(std::span<const uint8_t>) { return { "not supported on this platform", "not supported on this platform" }; }
Vector<uint8_t> buildAOTFile(VM&, std::span<const SourceCode>, bool, std::span<const uint8_t>, unsigned) { return { }; }
std::optional<size_t> aotImageSize(std::span<const uint8_t>) { return std::nullopt; }
std::optional<unsigned> aotImageRegExpCount(std::span<const uint8_t>) { return std::nullopt; }
std::optional<std::pair<size_t, size_t>> aotImageCodeRange(std::span<const uint8_t>) { return std::nullopt; }
size_t aotImageAlignment() { return 1; }

} // namespace JSC

#endif // ENABLE(AOT)
