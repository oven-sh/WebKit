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
#include "WasmCodeInImage.h"
#include "YarrClassTablePacker.h"
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
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#if OS(LINUX)
#include <link.h>
#endif
#elif OS(WINDOWS)
#include <windows.h>
#endif

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageBuilder);
WTF_MAKE_TZONE_ALLOCATED_IMPL(Image);

#if OS(LINUX)
static int addProgramBuildID(struct dl_phdr_info* info, size_t, void* context)
{
    auto& identity = *static_cast<uint64_t*>(context);
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        auto& segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_NOTE)
            continue;
        const uint8_t* at = reinterpret_cast<const uint8_t*>(info->dlpi_addr + segment.p_vaddr);
        const uint8_t* end = at + segment.p_memsz;
        while (static_cast<size_t>(end - at) >= sizeof(ElfW(Nhdr))) {
            auto& note = *reinterpret_cast<const ElfW(Nhdr)*>(at);
            size_t nameSize = WTF::roundUpToMultipleOf<4>(static_cast<size_t>(note.n_namesz));
            size_t descriptionSize = WTF::roundUpToMultipleOf<4>(static_cast<size_t>(note.n_descsz));
            at += sizeof(ElfW(Nhdr));
            if (static_cast<size_t>(end - at) < nameSize + descriptionSize)
                break;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 && !memcmp(at, "GNU", 4)) {
                for (size_t byte = 0; byte < note.n_descsz; ++byte)
                    identity = (identity ^ at[nameSize + byte]) * 0x100000001b3ULL;
            }
            at += nameSize + descriptionSize;
        }
    }
    return 1;
}
#endif

static uint64_t currentBinaryIdentity()
{
    static const uint64_t result = [] {
        uint64_t identity = 0;
#if OS(LINUX)
        dl_iterate_phdr(addProgramBuildID, &identity);
#elif OS(DARWIN)
        auto* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
        auto* command = reinterpret_cast<const uint8_t*>(header + 1);
        for (uint32_t i = 0; header && i < header->ncmds; ++i) {
            auto& loadCommand = *reinterpret_cast<const load_command*>(command);
            if (loadCommand.cmd == LC_UUID) {
                for (uint8_t byte : reinterpret_cast<const uuid_command*>(command)->uuid)
                    identity = (identity ^ byte) * 0x100000001b3ULL;
            }
            command += loadCommand.cmdsize;
        }
#endif
        return identity;
    }();
    return result;
}

uint64_t imageStamp()
{
    uint64_t stamp = computeJSCBytecodeCacheVersion();
    auto mix = [&](uint64_t value) {
        stamp = (stamp ^ value) * 0x100000001b3ULL;
        stamp ^= stamp >> 29;
    };
    mix(numberOfEntries);
    mix(Instance::offsetOfStates());
    mix(Instance::linkedWithoutDataState());
    mix(FunctionInfo::offsetOfBitsAndStateNumber() << 8 | FunctionInfo::stateNumberShift);
    mix(Instance::offsetOfOwnData());
    mix(BiasedDataPointer::bias);
    mix(Instance::offsetOfTypedStatics());
    mix(Instance::offsetOfTypedCards());
    mix(static_cast<uint64_t>(adapterSaveAreaSize) << 8 | static_cast<uint64_t>(-offsetOfInstanceInAdapter));
#if CPU(ARM64) || CPU(X86_64)
    mix(static_cast<uint64_t>(cellCageBaseGPR) << 16 | Instance::offsetOfCellCageBase());
#endif
    mix(MarkedBlockCage::shift);
    mix(Instance::bytesReservedForEnvironments);
    mix(Instance::bytesReservedForStructureDescriptors);
    mix(static_cast<uint64_t>(Instance::offsetOfNumberTag()) << 16 | Instance::offsetOfNotCellMask());
    mix(Instance::offsetOfNotUndefinedOrNullMask());
    mix(Instance::granuleBelowInstance);
    mix(Instance::typedCardShift);
    mix(Typed::Data::stringLengthBits);
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
    mix(1 | MacroAssembler::buildTargetFeatures() << 8);
#elif CPU(X86_64)
    mix(2 | MacroAssembler::buildTargetFeatures() << 8);
#endif
    mix(Instance::offsetOfCachedIntegerDoubles());
    mix(sizeof(Instance::NegativeOffsetFields));
    mix(Typed::builtinsHash);
    mix(Typed::Format::version);
    mix(currentBinaryIdentity());
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

static std::optional<Vector<uint8_t>> compressInBlocks(std::span<const uint8_t> bytes, size_t blockSize)
{
    if (!s_compress || !s_decompress || bytes.size() < 4 * KB)
        return std::nullopt;
    size_t numberOfBlocks = (bytes.size() + blockSize - 1) / blockSize;
    Vector<uint32_t> words;
    words.append(safeCast<uint32_t>(numberOfBlocks));
    words.append(safeCast<uint32_t>(bytes.size() - (numberOfBlocks - 1) * blockSize));
    Vector<uint8_t> blocks;
    Vector<uint8_t> packed(2 * blockSize);
    for (size_t block = 0; block < numberOfBlocks; ++block) {
        auto source = bytes.subspan(block * blockSize, std::min(blockSize, bytes.size() - block * blockSize));
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
    BlockReader(const uint8_t* bytes, size_t blockSize, uint64_t start)
        : m_bytes(bytes)
        , m_blockSize(blockSize)
        , m_at(start)
    {
    }

    bool failed() const { return m_failed; }

    uint8_t next()
    {
        if (!m_blockSize)
            return m_bytes[m_at++];
        size_t block = m_at / m_blockSize;
        if (block != m_block && !unpack(block))
            return 0;
        size_t inBlock = m_at++ % m_blockSize;
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
        m_unpacked.resize(block + 1 == numberOfBlocks ? words[1] : m_blockSize);
        if (!s_decompress(blocks + starts[block], starts[block + 1] - starts[block], m_unpacked.mutableSpan().data(), m_unpacked.size()))
            return false;
        m_failed = false;
        m_block = block;
        return true;
    }

    const uint8_t* m_bytes;
    size_t m_blockSize;
    uint64_t m_at;
    size_t m_block { std::numeric_limits<size_t>::max() };
    Vector<uint8_t> m_unpacked;
    bool m_failed { false };
};

String Image::quoteString(uint64_t start, size_t length) const
{
    BlockReader reader(this->at<uint8_t>(header().quoteStringsOffset), header().quoteStringBlockSize, start);
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
        String text = quoteString(start, static_cast<size_t>(lengthAndKind >> 2));
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
        result.parameterCheckSites = WTF::move(info.parameterCheckSites);
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

static String escapeForMapLine(StringView text)
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

void ImageBuilder::Functions::swap(Functions& other)
{
    std::swap(m_first, other.m_first);
    std::swap(m_size, other.m_size);
    std::swap(m_capacity, other.m_capacity);
}

void ImageBuilder::Functions::clear()
{
    std::destroy_n(m_first, m_size);
    fastFree(m_first);
    m_first = nullptr;
    m_size = 0;
    m_capacity = 0;
}

void ImageBuilder::Functions::reserveCapacity(size_t capacity)
{
    if (capacity <= m_capacity)
        return;
    RELEASE_ASSERT(capacity <= std::numeric_limits<size_t>::max() / sizeof(Function));
    auto* first = static_cast<Function*>(fastMalloc(capacity * sizeof(Function)));
    std::uninitialized_move_n(m_first, m_size, first);
    std::destroy_n(m_first, m_size);
    fastFree(m_first);
    m_first = first;
    m_capacity = capacity;
}

void ImageBuilder::Functions::append(Function&& function)
{
    if (m_size == m_capacity)
        reserveCapacity(std::max<size_t>(16, m_capacity + m_capacity / 4 + 1));
    new (NotNull, m_first + m_size) Function(WTF::move(function));
    ++m_size;
}

void ImageBuilder::Functions::growOnPool(size_t newSize)
{
    RELEASE_ASSERT(newSize >= m_size);
    reserveCapacity(newSize);
    constexpr size_t functionsInChunk = 1024;
    size_t number = newSize - m_size;
    Typed::forEachIndexInParallel(static_cast<unsigned>((number + functionsInChunk - 1) / functionsInChunk), [&](unsigned chunk) {
        size_t start = static_cast<size_t>(chunk) * functionsInChunk;
        std::uninitialized_value_construct_n(m_first + m_size + start, std::min(functionsInChunk, number - start));
    });
    m_size = newSize;
}

size_t ImageBuilder::growFunctions(size_t number)
{
    Locker locker { m_lock };
    size_t first = m_functions.size();
    m_functions.growOnPool(first + number);
    return first;
}

void ImageBuilder::set(size_t place, ImageKey key, uint64_t rank, CompiledCode&& code, String&& nameForMap)
{
    clearStubCalls(code);
    m_functions[place] = Function { key, rank, WTF::move(code), WTF::move(nameForMap) };
}

struct SourcePiece {
    uint32_t offset;
    uint32_t length;
};

template<typename CharacterType>
struct SourcePiecePool {
    static constexpr unsigned window = 64;
    static constexpr uint64_t base = 0x9e3779b97f4a7c15ull;

    static uint64_t hashAt(std::span<const CharacterType> all, size_t at)
    {
        uint64_t hash = 0;
        for (CharacterType unit : all.subspan(at, window))
            hash = hash * base + unit;
        return hash;
    }

    void lay(Vector<SourcePiece>& pieces, CharacterType unit)
    {
        if (!pieces.isEmpty() && pieces.last().offset + pieces.last().length == units.size())
            pieces.last().length++;
        else
            pieces.append({ safeCast<uint32_t>(units.size()), 1 });
        units.append(unit);
        if (units.size() >= window)
            places.add(hashAt(units.span(), units.size() - window) >> 2, safeCast<uint32_t>(units.size() - window));
    }

    Vector<SourcePiece> cut(std::span<const CharacterType> text)
    {
        uint64_t firstWeight = 1;
        for (unsigned i = 1; i < window; ++i)
            firstWeight *= base;
        Vector<SourcePiece> pieces;
        std::optional<uint64_t> hash;
        for (size_t i = 0; i < text.size();) {
            if (i + window > text.size()) {
                lay(pieces, text[i++]);
                continue;
            }
            if (!hash)
                hash = hashAt(text, i);
            if (auto found = places.find(*hash >> 2); found != places.end() && equalSpans(text.subspan(i, window), units.span().subspan(found->value, window))) {
                size_t at = found->value;
                size_t length = window;
                while (i + length < text.size() && at + length < units.size() && text[i + length] == units[at + length])
                    ++length;
                if (!pieces.isEmpty() && pieces.last().offset + pieces.last().length == at)
                    pieces.last().length += length;
                else
                    pieces.append({ safeCast<uint32_t>(at), safeCast<uint32_t>(length) });
                i += length;
                hash = std::nullopt;
                continue;
            }
            lay(pieces, text[i]);
            if (i + window < text.size())
                hash = (*hash - text[i] * firstWeight) * base + text[i + window];
            else
                hash = std::nullopt;
            ++i;
        }
        return pieces;
    }

    Vector<CharacterType> units;
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> places;
};

static constexpr size_t regExpCompilerStackSize = 448 * KB;

static size_t remainingStackOnThisThread()
{
    return static_cast<char*>(currentStackPointer()) - static_cast<char*>(Thread::currentSingleton().stack().end());
}

static bool hasStackForRegExpCompiler()
{
    return remainingStackOnThisThread() >= regExpCompilerStackSize + 16 * KB;
}

String ImageBuilder::flagsAndPattern(OptionSet<Yarr::Flags> flags, const String& pattern)
{
    return makeString(flags.toRaw(), '/', pattern);
}

void ImageBuilder::PrecompiledRegExps::compile(VM& vm, const RegExpSourcesAndFlags& sourcesAndFlags)
{
    if (!hasStackForRegExpCompiler())
        return;
    for (auto& [source, allFlags] : sourcesAndFlags) {
        auto flags = ImageRegExp::significantFlags(allFlags);
        String key = flagsAndPattern(flags, source);
        {
            Locker locker { m_lock };
            if (m_isClaimed)
                return;
            if (!m_outcomes.add(key, nullptr).isNewEntry)
                continue;
        }
        auto outcome = makeUnique<RegExpOutcome>();
        outcome->code = compileRegExp(vm, RegExpRequest { source.isolatedCopy(), flags, false }, outcome->fallsBackToInterpreter);
        Locker locker { m_lock };
        if (m_isClaimed)
            return;
        m_outcomes.set(key, WTF::move(outcome));
    }
}

void ImageBuilder::takePrecompiledRegExps(PrecompiledRegExps& precompiledRegExps)
{
    Locker locker { precompiledRegExps.m_lock };
    precompiledRegExps.m_isClaimed = true;
    m_precompiledRegExps = std::exchange(precompiledRegExps.m_outcomes, { });
}

void ImageBuilder::requestRegExp(VM& vm, const String& pattern, OptionSet<Yarr::Flags> flags, bool isFoundByEvaluator)
{
    flags = ImageRegExp::significantFlags(flags);
    String key = flagsAndPattern(flags, pattern);
    RegExpRequest request { pattern.isolatedCopy(), flags, isFoundByEvaluator };
    Locker locker { m_lock };
    if (!m_requestedRegExps.add(WTF::move(key)).isNewEntry)
        return;
    m_regExpVM = &vm;
    m_regExpRequests.append(WTF::move(request));
    m_numberOfRegExpsWaiting.fetch_add(1, std::memory_order_relaxed);
    m_regExpIsRequested.notifyOne();
}

NEVER_INLINE auto ImageBuilder::compileRegExp(VM& vm, const RegExpRequest& request, bool& fallsBackToInterpreter) -> std::optional<RegExpCode>
{
    size_t left = remainingStackOnThisThread();
    RELEASE_ASSERT(left >= regExpCompilerStackSize);
    StackCheck stackCheck(Thread::currentSingleton().stack(), left - regExpCompilerStackSize + StackBounds::DefaultReservedZone);
    auto& pattern = request.pattern;
    auto flags = request.flags;
    RegExpCode result { pattern, flags, { } };
    result.isFoundByEvaluator = request.isFoundByEvaluator;
    auto notCompiled = [&](ASCIILiteral reason) -> std::optional<RegExpCode> {
        fallsBackToInterpreter = true;
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
        result.retainsPatternDetails = !yarrPattern.m_captureGroupNames.isEmpty() || !yarrPattern.m_namedGroupToParenIndices.isEmpty();
    }
    for (auto& code : result.code)
        Yarr::tidyBodyForImage(code);
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
    std::unique_ptr<RegExpOutcome> precompiledRegExps;
    {
        Locker locker { m_lock };
        if (m_nextRegExpRequest == m_regExpRequests.size())
            return false;
        request = WTF::move(m_regExpRequests[m_nextRegExpRequest++]);
        m_numberOfRegExpsWaiting.fetch_sub(1, std::memory_order_relaxed);
        vm = m_regExpVM;
        precompiledRegExps = m_precompiledRegExps.take(flagsAndPattern(request.flags, request.pattern));
    }
    MonotonicTime start = MonotonicTime::now();
    bool fallsBackToInterpreter = precompiledRegExps && precompiledRegExps->fallsBackToInterpreter;
    auto result = precompiledRegExps ? WTF::move(precompiledRegExps->code) : compileRegExp(*vm, request, fallsBackToInterpreter);
    if (result)
        result->isFoundByEvaluator = request.isFoundByEvaluator;
    request = { };
    Locker locker { m_lock };
    if (result)
        m_regExps.append(WTF::move(*result));
    m_numberOfRegExpsNotCompiled += fallsBackToInterpreter;
    if (Options::verboseAOTCompilation()) [[unlikely]] {
        m_timeInRegExpCompiler += MonotonicTime::now() - start;
        m_regExpCompilerThreads.add(&Thread::currentSingleton());
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

void ImageBuilder::compileRemainingRegExps()
{
    size_t unchangedCount = m_numberOfRegExpsWaiting.load();
    bool hasStack = hasStackForRegExpCompiler();
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: regular expressions left for finish(): ", unchangedCount, ", compiled on ", hasStack ? "this thread" : "a thread made for them");
    if (!unchangedCount)
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
    compileRemainingRegExps();
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
        dataLogLn("AOT: Yarr's compiler took ", static_cast<unsigned>(m_timeInRegExpCompiler.milliseconds()), " ms in all, on ", m_regExpCompilerThreads.size(), " threads");
    }
    MonotonicTime stepStart = MonotonicTime::now();
    Typed::PhaseTimes phaseTimes("image"_s);
    auto endStep = [&](ASCIILiteral name) {
        if (!Typed::shouldPrintTimings()) [[likely]]
            return;
        MonotonicTime now = MonotonicTime::now();
        dataLogLn("AOT: - ", name, " took ", static_cast<unsigned>((now - stepStart).milliseconds()), " ms");
        stepStart = now;
        phaseTimes.didFinish(name);
    };
    auto reorderFunctions = [&](const Vector<uint32_t>& order) {
        Functions taken;
        taken.growOnPool(order.size());
        Typed::forEachIndexInParallel(order.size(), [&](unsigned index) {
            taken[index] = WTF::move(m_functions[order[index]]);
        });
        Typed::destroyOnHelperThread(std::exchange(m_functions, WTF::move(taken)));
    };
    auto sortedKeysAndIndices = [&] {
        constexpr size_t numberOfRuns = 16;
        size_t count = m_functions.size();
        Vector<std::array<uint32_t, 4>> sorted(count);
        Vector<std::array<uint32_t, 4>> merged(count);
        auto runOf = [&](Vector<std::array<uint32_t, 4>>& list, size_t run, size_t runs) {
            size_t start = count * run / runs;
            return list.mutableSpan().subspan(start, count * (run + 1) / runs - start);
        };
        Typed::forEachIndexInParallel(numberOfRuns, [&](unsigned run) {
            size_t index = count * run / numberOfRuns;
            auto entries = runOf(sorted, run, numberOfRuns);
            for (auto& entry : entries) {
                auto& key = m_functions[index].key;
                entry = { key.module, key.start, key.kind, static_cast<uint32_t>(index++) };
            }
            std::ranges::sort(entries);
        });
        for (size_t runs = numberOfRuns; runs > 1; runs /= 2) {
            Typed::forEachIndexInParallel(runs / 2, [&](unsigned pair) {
                std::ranges::merge(runOf(sorted, 2 * pair, runs), runOf(sorted, 2 * pair + 1, runs), runOf(merged, pair, runs / 2).begin());
            });
            std::swap(sorted, merged);
        }
        return sorted;
    };
    {
        struct SortKey {
            uint64_t rank;
            uint32_t module;
            uint32_t start;
            uint32_t kind;
            uint32_t index;
        };
        Vector<SortKey> places(m_functions.size());
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            auto& function = m_functions[index];
            places[index] = { function.rank, function.key.module, function.key.start, function.key.kind, index };
        });
        auto isAhead = [](const SortKey& a, const SortKey& b) {
            return std::tie(a.rank, a.module, a.start, a.kind) < std::tie(b.rank, b.module, b.start, b.kind);
        };
        bool isSorted = true;
        for (size_t index = 1; index < places.size() && isSorted; ++index)
            isSorted = isAhead(places[index - 1], places[index]);
        if (!isSorted) {
            std::ranges::sort(places, [](const SortKey& a, const SortKey& b) {
                return std::tie(a.rank, a.module, a.start, a.kind, a.index) < std::tie(b.rank, b.module, b.start, b.kind, b.index);
            });
            reorderFunctions(WTF::map(places, [](const SortKey& place) {
                return place.index;
            }));
        }
    }

    endStep("the functions in order"_s);
    unsigned capacity = 16;
    while (capacity * 3 < m_functions.size() * 4)
        capacity *= 2;

    const IdentifierIndices* identifierIndices = m_programIdentifierIndices;
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        auto& info = m_functions[index].code.info;
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
    });
    endStep("the plans at the sites of the functions"_s);
    struct EntryInsideFunction {
        ImageKey key;
        Convention convention;
        ImageKey function;
        size_t offset;
        size_t precedingWordSize { 0 };
        bool startHasCompatibleResultsMask { false };
    };
    Vector<EntryInsideFunction> entriesInsideFunctions;
    if (m_numberOfProgramIdentifiers) {
#if CPU(ARM64)
        constexpr size_t sizeOfJump = 4;
#else
        constexpr size_t sizeOfJump = 5;
#endif
        constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
        Vector<std::array<uint32_t, 4>> keysAndIndices = sortedKeysAndIndices();
        auto hasKey = [](const std::array<uint32_t, 4>& entry, const ImageKey& key) {
            return entry[0] == key.module && entry[1] == key.start && entry[2] == key.kind;
        };
        BitVector isDuplicated(m_functions.size());
        for (size_t i = 1; i < keysAndIndices.size(); ++i) {
            if (std::ranges::equal(std::span { keysAndIndices[i] }.first(3), std::span { keysAndIndices[i - 1] }.first(3))) {
                isDuplicated.set(keysAndIndices[i - 1][3]);
                isDuplicated.set(keysAndIndices[i][3]);
            }
        }
        Vector<uint32_t> typedFunctionIndices;
        for (auto& entry : keysAndIndices | std::views::reverse) {
            if (entry[0] || entry[2] != ImageKey::typedFunction)
                continue;
            if (typedFunctionIndices.isEmpty())
                typedFunctionIndices.fill(none, entry[1] + 1);
            typedFunctionIndices[entry[1]] = entry[3];
        }
        auto indexOf = [&](const ImageKey& key) {
            if (!key.module && key.kind == ImageKey::typedFunction)
                return key.start < typedFunctionIndices.size() ? typedFunctionIndices[key.start] : none;
            auto found = std::ranges::lower_bound(keysAndIndices, std::array<uint32_t, 4> { key.module, key.start, key.kind, 0 });
            return found != keysAndIndices.end() && hasKey(*found, key) ? (*found)[3] : none;
        };
        Vector<uint32_t> jumpsTo(FillWith { }, m_functions.size(), none);
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            auto& function = m_functions[index];
            auto& info = function.code.info;
            if (function.key.kind != ImageKey::typedFunction || isDuplicated.get(index) || !info.hasNoFrame || info.stubCalls.size() != 1 || info.numSlots || info.typedPositions.size() != 1 || info.valueEntry != StubCall::noFunction || info.signatureAdapter != StubCall::noFunction || info.adapterResult != StubCall::noFunction
                || !info.callSites.isEmpty() || !info.inlineFrames.isEmpty() || !info.indexReferences.isEmpty() || !info.faultingAccesses.isEmpty() || !info.spreadSites.isEmpty() || !info.plans.isEmpty())
                return;
            auto& jump = info.stubCalls[0];
            if (!jump.isTailCall || jump.thunk || jump.function == StubCall::noFunction || jump.offset + sizeOfJump > function.code.bytes.size())
                return;
            auto afterJump = function.code.bytes.span().subspan(jump.offset + sizeOfJump);
#if CPU(X86_64)
            while (!afterJump.empty() && afterJump.front() == 0xcc)
                afterJump = afterJump.subspan(1);
            std::array<uint8_t, imageFunctionAlignment> padding;
            if (afterJump.size() >= padding.size())
                return;
            X86Assembler::fillNops(padding.data(), afterJump.size());
            if (!equalSpans(afterJump, std::span<const uint8_t> { padding }.first(afterJump.size())))
                return;
#else
            if (!afterJump.empty())
                return;
#endif
            uint32_t callee = indexOf(info.knownCallees[jump.function]);
            if (callee != none && callee != index && !isDuplicated.get(callee))
                jumpsTo[index] = callee;
        });
        auto isOneJump = [&](uint32_t index) {
            return jumpsTo[index] != none && !m_functions[index].code.info.stubCalls[0].offset;
        };
        auto resolveJumps = [&](uint32_t index) {
            for (unsigned steps = 0; steps < 16 && isOneJump(index); ++steps)
                index = jumpsTo[index];
            return index;
        };
        Vector<uint32_t> uses(FillWith { }, m_functions.size(), 0u);
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            auto& info = m_functions[index].code.info;
            for (unsigned k = 0; k < info.knownCallees.size(); ++k) {
                auto& key = info.knownCallees[k];
                uint32_t callee = indexOf(key);
                if (callee == none)
                    continue;
                if (uint32_t last = resolveJumps(callee); last != callee && !isOneJump(last)) {
                    key = m_functions[last].key;
                    callee = last;
                }
                if (k != info.valueEntry)
                    WTF::atomicExchangeAdd(&uses[callee], 1u, std::memory_order_relaxed);
            }
        });
        BitVector isRemoved(m_functions.size());
        size_t numberOfStarts = 0;
        uint32_t ahead = none;
        struct AppendedEntry {
            uint32_t index;
            uint32_t function;
            uint32_t entry;
        };
        Vector<AppendedEntry> appendedEntries;
        Vector<std::pair<uint32_t, uint32_t>> hostsAndFirstAppendedEntries;
        for (uint32_t index = 0; index < m_functions.size(); ++index) {
            uint32_t function = jumpsTo[index] == none || uses[index] ? none : resolveJumps(jumpsTo[index]);
            if (function == none || jumpsTo[function] != none || (!isOneJump(index) && (ahead == none || isDuplicated.get(ahead)))) {
                ahead = index;
                continue;
            }
            auto& entry = m_functions[index];
            isRemoved.set(index);
            uint64_t results = entry.code.info.compatibleResultsMask;
            size_t sizeOfResults = entry.code.info.compatibleResultsMaskSize;
            if (isOneJump(index)) {
                ++numberOfStarts;
                auto& startInfo = m_functions[function].code.info;
                bool hasCompatibleResultsMask = results && (!startInfo.compatibleResultsMask || startInfo.compatibleResultsMask == results);
                if (hasCompatibleResultsMask) {
                    startInfo.compatibleResultsMask = results;
                    startInfo.compatibleResultsMaskSize = sizeOfResults;
                }
                entriesInsideFunctions.append({ entry.key, entry.code.info.convention, m_functions[function].key, 0, 0, hasCompatibleResultsMask });
                continue;
            }
            if (hostsAndFirstAppendedEntries.isEmpty() || hostsAndFirstAppendedEntries.last().first != ahead)
                hostsAndFirstAppendedEntries.append({ ahead, safeCast<uint32_t>(appendedEntries.size()) });
            appendedEntries.append({ index, function, safeCast<uint32_t>(entriesInsideFunctions.size()) });
            entriesInsideFunctions.append({ entry.key, entry.code.info.convention, m_functions[ahead].key, 0, 0 });
        }
        Typed::forEachIndexInParallel(hostsAndFirstAppendedEntries.size(), [&](unsigned number) {
            auto& host = m_functions[hostsAndFirstAppendedEntries[number].first];
            size_t appendedEntriesEnd = number + 1 < hostsAndFirstAppendedEntries.size() ? hostsAndFirstAppendedEntries[number + 1].second : appendedEntries.size();
            for (auto& guest : appendedEntries.span().subspan(hostsAndFirstAppendedEntries[number].second, appendedEntriesEnd - hostsAndFirstAppendedEntries[number].second)) {
                auto& entry = m_functions[guest.index];
                uint32_t function = guest.function;
                uint64_t results = entry.code.info.compatibleResultsMask;
                size_t sizeOfResults = entry.code.info.compatibleResultsMaskSize;
                size_t offset = WTF::roundUpToMultipleOf<imageFunctionAlignment>(host.code.bytes.size());
                if (!host.code.instructions.isEmpty() && host.code.bytes.size() < offset)
                    host.code.instructions.append(static_cast<uint8_t>((offset - host.code.bytes.size()) | 0x80));
                static_assert(imageFunctionAlignment <= 16);
                host.code.appendedEntriesStart = std::min(host.code.appendedEntriesStart, safeCast<uint32_t>(offset));
                while (host.code.bytes.size() < offset)
                    host.code.bytes.append(0);
                size_t precedingWordSize = 0;
                if (results) {
                    precedingWordSize = sizeOfResults;
                    for (size_t word = 0; word < sizeOfResults / sizeof(uint32_t) && !host.code.instructions.isEmpty(); ++word)
                        host.code.instructions.append(static_cast<uint8_t>(sizeof(uint32_t) | 0x80));
                    host.code.dataInCode.append({ safeCast<uint32_t>(offset), safeCast<uint32_t>(sizeOfResults / sizeof(uint32_t)), false });
                    host.code.bytes.append(std::span { reinterpret_cast<const uint8_t*>(&results), sizeOfResults });
                }
                host.code.instructions.appendVector(entry.code.instructions);
                host.code.hasCodeAnnotations &= entry.code.hasCodeAnnotations;
                for (DataInCode data : entry.code.dataInCode) {
                    data.offset += offset + precedingWordSize;
                    host.code.dataInCode.append(data);
                }
                host.code.bytes.appendVector(entry.code.bytes);
                StubCall jump = entry.code.info.stubCalls[0];
                jump.offset += offset + precedingWordSize;
                size_t callee = host.code.info.knownCallees.findIf([&](const ImageKey& key) {
                    return key.sameFunction(m_functions[function].key);
                });
                if (callee == notFound) {
                    callee = host.code.info.knownCallees.size();
                    host.code.info.knownCallees.append(m_functions[function].key);
                }
                jump.function = safeCast<uint32_t>(callee);
                host.code.info.stubCalls.append(jump);
                entriesInsideFunctions[guest.entry].offset = offset;
                entriesInsideFunctions[guest.entry].precedingWordSize = precedingWordSize;
            }
        });
        if (!entriesInsideFunctions.isEmpty()) {
            Vector<uint32_t> kept;
            kept.reserveInitialCapacity(m_functions.size());
            for (uint32_t index = 0; index < m_functions.size(); ++index) {
                if (!isRemoved.get(index))
                    kept.append(index);
            }
            reorderFunctions(kept);
        }
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", entriesInsideFunctions.size(), " functions that only jump to another have no rows: ", numberOfStarts, " are the start of that one, ", entriesInsideFunctions.size() - numberOfStarts, " are the tail of the one ahead of them");
    }

    endStep("entries inside functions"_s);
    struct FoldedFunction {
        ImageKey key;
        ImageKey canonicalFunction;
        size_t size;
        String nameForMap;
    };
    Vector<FoldedFunction> foldedFunctions;
    if (m_numberOfProgramIdentifiers) {
        constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
        Vector<std::array<uint32_t, 4>> keysAndIndices = sortedKeysAndIndices();
        auto hasKey = [](const std::array<uint32_t, 4>& entry, const ImageKey& key) {
            return entry[0] == key.module && entry[1] == key.start && entry[2] == key.kind;
        };
        BitVector isDuplicated(m_functions.size());
        for (size_t i = 1; i < keysAndIndices.size(); ++i) {
            if (std::ranges::equal(std::span { keysAndIndices[i] }.first(3), std::span { keysAndIndices[i - 1] }.first(3))) {
                isDuplicated.set(keysAndIndices[i - 1][3]);
                isDuplicated.set(keysAndIndices[i][3]);
            }
        }
        Vector<uint32_t> typedFunctionIndices;
        for (auto& entry : keysAndIndices | std::views::reverse) {
            if (entry[0] || entry[2] != ImageKey::typedFunction)
                continue;
            if (typedFunctionIndices.isEmpty())
                typedFunctionIndices.fill(none, entry[1] + 1);
            typedFunctionIndices[entry[1]] = entry[3];
        }
        auto indexOf = [&](const ImageKey& key) {
            if (!key.module && key.kind == ImageKey::typedFunction)
                return key.start < typedFunctionIndices.size() ? typedFunctionIndices[key.start] : none;
            auto found = std::ranges::lower_bound(keysAndIndices, std::array<uint32_t, 4> { key.module, key.start, key.kind, 0 });
            return found != keysAndIndices.end() && hasKey(*found, key) ? (*found)[3] : none;
        };
        Vector<uint32_t> candidates;
        Vector<Vector<uint64_t, 8>> factsOf(m_functions.size());
        Vector<uint8_t> isCandidate(FillWith { }, m_functions.size(), static_cast<uint8_t>(0));
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            auto& function = m_functions[index];
            auto& info = function.code.info;
            if (function.key.kind != ImageKey::typedFunction || isDuplicated.get(index) || (!info.hasNoFrame && !info.isGenericEntrypoint) || info.numSlots || !function.code.continuations.isEmpty() || !info.callSites.isEmpty() || info.valueEntry != StubCall::noFunction || info.signatureAdapter != StubCall::noFunction || info.adapterResult != StubCall::noFunction || info.compatibleResultsMask
                || !info.typedCatchEntrypointsForCallSites.isEmpty() || !info.inlineFrames.isEmpty() || !info.indexReferences.isEmpty() || !info.faultingAccesses.isEmpty() || !info.spreadSites.isEmpty() || !info.plans.isEmpty())
                return;
            bool hasNoStubCallsToIt = std::ranges::all_of(info.stubCalls, [&](const StubCall& call) {
                return call.callSite == StubCall::noCallSite || (info.isGenericEntrypoint && call.function != StubCall::noFunction);
            });
            if (!hasNoStubCallsToIt)
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
        Typed::sortOnPool(candidates.mutableSpan(), [&](uint32_t first, uint32_t second) {
            int order = compareWithoutCallees(first, second);
            return order ? order < 0 : first < second;
        });
        Vector<uint8_t> duplicatesEarlierCandidate(candidates.size());
        Typed::forEachIndexInParallel(candidates.size(), [&](unsigned i) {
            duplicatesEarlierCandidate[i] = i && !compareWithoutCallees(candidates[i - 1], candidates[i]);
        });
        for (size_t i = 1; i < candidates.size(); ++i) {
            if (duplicatesEarlierCandidate[i])
                classOf[candidates[i]] = classOf[candidates[i - 1]];
        }
        Vector<Vector<uint32_t, 2>> candidateCallees(m_functions.size());
        Typed::forEachIndexInParallel(candidates.size(), [&](unsigned candidate) {
            uint32_t index = candidates[candidate];
            auto& info = m_functions[index].code.info;
            for (auto& call : info.stubCalls) {
                if (call.function != StubCall::noFunction)
                    candidateCallees[index].append(indexOf(info.knownCallees[call.function]));
            }
        });
        auto compare = [&](uint32_t first, uint32_t second) {
            if (classOf[first] != classOf[second])
                return classOf[first] < classOf[second] ? -1 : 1;
            for (size_t i = 0; i < candidateCallees[first].size(); ++i) {
                uint32_t a = candidateCallees[first][i];
                uint32_t b = candidateCallees[second][i];
                a = a == none ? none : classOf[a];
                b = b == none ? none : classOf[b];
                if (a != b)
                    return a < b ? -1 : 1;
            }
            return 0;
        };
        Vector<uint32_t> next;
        auto refine = [&](std::span<uint32_t> some) {
            Typed::sortOnPool(some, [&](uint32_t first, uint32_t second) {
                int order = compare(first, second);
                return order ? order < 0 : first < second;
            });
            next.resize(some.size());
            Typed::forEachIndexInParallel(some.size(), [&](unsigned i) {
                duplicatesEarlierCandidate[i] = i && !compare(some[i - 1], some[i]);
            });
            bool changed = false;
            for (size_t i = 0; i < some.size(); ++i) {
                next[i] = duplicatesEarlierCandidate[i] ? next[i - 1] : some[i];
                changed |= next[i] != classOf[some[i]];
            }
            for (size_t i = 0; i < some.size(); ++i)
                classOf[some[i]] = next[i];
            return changed;
        };
        enum : uint8_t { NotSeen, InProgress, ReachesNoCycle, ReachesCycle };
        Vector<uint8_t> state(FillWith { }, m_functions.size(), static_cast<uint8_t>(NotSeen));
        Vector<uint32_t> height(FillWith { }, m_functions.size(), 0u);
        Vector<std::pair<uint32_t, uint32_t>> inProgress;
        for (uint32_t root : candidates) {
            if (state[root] != NotSeen)
                continue;
            state[root] = InProgress;
            inProgress.append({ root, 0 });
            while (!inProgress.isEmpty()) {
                auto [index, nextCallee] = inProgress.last();
                auto& callees = candidateCallees[index];
                if (nextCallee < callees.size()) {
                    ++inProgress.last().second;
                    uint32_t callee = callees[nextCallee];
                    if (callee != none && isCandidate[callee] && state[callee] == NotSeen) {
                        state[callee] = InProgress;
                        inProgress.append({ callee, 0 });
                    }
                    continue;
                }
                bool reachesCycle = false;
                for (uint32_t callee : callees) {
                    if (callee == none || !isCandidate[callee])
                        continue;
                    reachesCycle |= state[callee] != ReachesNoCycle;
                    height[index] = std::max(height[index], height[callee] + 1);
                }
                state[index] = reachesCycle ? ReachesCycle : ReachesNoCycle;
                inProgress.removeLast();
            }
        }
        Typed::sortOnPool(candidates.mutableSpan(), [&](uint32_t first, uint32_t second) {
            if ((state[first] == ReachesCycle) != (state[second] == ReachesCycle))
                return state[second] == ReachesCycle;
            return height[first] != height[second] ? height[first] < height[second] : first < second;
        });
        size_t acyclicCandidateCount = std::ranges::partition_point(candidates, [&](uint32_t candidate) {
            return state[candidate] == ReachesNoCycle;
        }) - candidates.begin();
        for (size_t begin = 0; begin < acyclicCandidateCount;) {
            size_t end = begin + 1;
            while (end < acyclicCandidateCount && height[candidates[end]] == height[candidates[begin]])
                ++end;
            refine(candidates.mutableSpan().subspan(begin, end - begin));
            begin = end;
        }
        for (bool changed = acyclicCandidateCount < candidates.size(); changed;)
            changed = refine(candidates.mutableSpan().subspan(acyclicCandidateCount));
        auto canonicalKeyFor = [&](ImageKey& key) {
            if (uint32_t index = indexOf(key); index != none && classOf[index] != index)
                key = m_functions[classOf[index]].key;
        };
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            for (auto& key : m_functions[index].code.info.knownCallees)
                canonicalKeyFor(key);
        });
        for (auto& entry : entriesInsideFunctions)
            canonicalKeyFor(entry.function);
        size_t bytes = 0;
        Vector<ImageKey> keyOf = WTF::map(m_functions, [](const Function& function) {
            return function.key;
        });
        Vector<uint32_t> kept;
        kept.reserveInitialCapacity(m_functions.size());
        for (uint32_t index = 0; index < m_functions.size(); ++index) {
            auto& function = m_functions[index];
            if (classOf[index] == index) {
                kept.append(index);
                continue;
            }
            ImageKey canonicalFunction = keyOf[classOf[index]];
            bytes += function.code.bytes.size();
            entriesInsideFunctions.append({ function.key, function.code.info.convention, canonicalFunction, 0 });
            foldedFunctions.append({ function.key, canonicalFunction, function.code.bytes.size(), WTF::move(function.nameForMap) });
        }
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", foldedFunctions.size(), " functions have the code of another: ", bytes, " bytes");
        if (kept.size() < m_functions.size())
            reorderFunctions(kept);
        Typed::destroyOnHelperThread(std::tuple { WTF::move(factsOf), WTF::move(candidateCallees) });
    }

    endStep("folding functions"_s);
    Vector<ImageKey> functionKeys(m_functions.size());
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        functionKeys[index] = m_functions[index].key;
    });
    Vector<uint32_t> functionWithKey;
    functionWithKey.fill(std::numeric_limits<uint32_t>::max(), capacity);
    for (size_t index = 0; index < m_functions.size(); ++index) {
        unsigned bucket = functionKeys[index].hash() & (capacity - 1);
        while (functionWithKey[bucket] != std::numeric_limits<uint32_t>::max() && !functionKeys[functionWithKey[bucket]].sameFunction(functionKeys[index]))
            bucket = (bucket + 1) & (capacity - 1);
        if (functionWithKey[bucket] == std::numeric_limits<uint32_t>::max())
            functionWithKey[bucket] = index;
    }
    endStep("the functions by key"_s);
    Vector<Vector<uint32_t>> calleesOf(m_functions.size());
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        calleesOf[index] = WTF::map(m_functions[index].code.info.knownCallees, [&](const ImageKey& key) {
            for (unsigned bucket = key.hash() & (capacity - 1); functionWithKey[bucket] != std::numeric_limits<uint32_t>::max(); bucket = (bucket + 1) & (capacity - 1)) {
                if (functionKeys[functionWithKey[bucket]].sameFunction(key))
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

    auto verifyUncheckedStackUsage = [&]() -> bool {
        constexpr uint32_t notSeen = std::numeric_limits<uint32_t>::max();
        constexpr uint32_t inProgress = notSeen - 1;
        Vector<uint32_t> stackUsage(FillWith { }, m_functions.size(), notSeen);
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
            m_failureReason = makeString("internal error: @"_s, key.module, ':', key.start, ':', key.kind, " is reached from a function that does not check for stack overflow, and "_s, why);
        };
        auto frameOf = [&](uint32_t function) {
            auto& info = m_functions[function].code.info;
            return info.stackUsedAtCalls + (info.hasNoFrame ? 0 : info.frameSizeInBytes + static_cast<uint32_t>(sizeof(CallerFrameAndPC)));
        };
        auto stackUsageAtEntry = [&](uint32_t function, bool isJump) {
            return m_functions[function].code.info.hasNoFrame && !isJump ? static_cast<uint32_t>(sizeof(void*)) : 0;
        };
        for (uint32_t root = 0; root < m_functions.size(); ++root) {
            if (!m_functions[root].code.info.isProvenToFitOnStack || stackUsage[root] != notSeen)
                continue;
            stackUsage[root] = inProgress;
            visits.append({ root, 0, 0, frameOf(root), false });
            while (!visits.isEmpty()) {
                Visit visit = visits.last();
                auto& code = m_functions[visit.function].code;
                if (visit.nextCall < code.info.stubCalls.size()) {
                    ++visits.last().nextCall;
                    auto& call = code.info.stubCalls[visit.nextCall];
                    if (call.offset >= code.appendedEntriesStart)
                        continue;
                    auto target = directTargetOf(visit.function, call);
                    if (!target || !m_functions[*target].code.info.isEnteredWithoutStackCheck)
                        continue;
                    uint32_t atEntry = stackUsageAtEntry(static_cast<uint32_t>(*target), call.isTailCall);
                    uint32_t taken = stackUsage[*target];
                    if (taken == inProgress) {
                        uint32_t stackUsagePerCycle = atEntry;
                        size_t head = visits.size();
                        while (visits[--head].function != *target)
                            stackUsagePerCycle += visits[head].own;
                        stackUsagePerCycle += frameOf(static_cast<uint32_t>(*target));
                        if (!stackUsagePerCycle) {
                            for (size_t i = head + 1; i < visits.size(); ++i)
                                visits[i].isOnRingWithoutStack = true;
                            continue;
                        }
                        refuse(static_cast<uint32_t>(*target), "neither does any function on a way from it back to it"_s);
                        return false;
                    }
                    if (taken == notSeen) {
                        stackUsage[*target] = inProgress;
                        visits.append({ static_cast<uint32_t>(*target), 0, 0, frameOf(static_cast<uint32_t>(*target)) + atEntry, false });
                        continue;
                    }
                    visits.last().below = std::max(visits.last().below, taken + atEntry);
                    continue;
                }
                uint32_t taken = visit.below + visit.own;
                if (taken > maxStackUsedWithoutCheck) {
                    refuse(visit.function, "the frames from it on take more than is set aside for them"_s);
                    return false;
                }
                stackUsage[visit.function] = visit.isOnRingWithoutStack ? notSeen : visit.below + frameOf(visit.function);
                visits.removeLast();
                if (!visits.isEmpty())
                    visits.last().below = std::max(visits.last().below, taken);
            }
        }
        return true;
    };
    endStep("the stack that is taken without asking"_s);

    if (Options::verboseAOTCompilation()) [[unlikely]] {
        unsigned inlinePath = 0;
        unsigned byStub = 0;
        for (auto& function : m_functions) {
            inlinePath += function.code.info.numberOfInlineEmptyArrayLiterals;
            byStub += function.code.info.numberOfStubEmptyArrayLiterals;
        }
        dataLogLn("AOT: empty array literals that are pushed to and born contiguous: ", inlinePath, " are made in place, ", byStub, " by the stub");
    }

    Vector<std::array<uint8_t, 2>> omittedArgumentsOf(FillWith { }, m_functions.size(), std::array<uint8_t, 2> { });
    auto collectOmittedArguments = [&] {
        for (size_t index = 0; index < m_functions.size(); ++index) {
            for (auto& call : m_functions[index].code.info.stubCalls) {
                if (call.firstOmittedGPR == call.endOfOmittedGPRs || call.entersPreamble())
                    continue;
                auto& [first, end] = omittedArgumentsOf[*directTargetOf(index, call)];
                first = end ? std::min(first, call.firstOmittedGPR) : call.firstOmittedGPR;
                end = std::max(end, call.endOfOmittedGPRs);
            }
        }
    };
    constexpr uint32_t isNotInTypedData = std::numeric_limits<uint32_t>::max();
    UncheckedKeyHashMap<String, uint32_t> offsetsInTypedData;
    auto textOf = [](const String& pattern) {
        return pattern.isNull() ? emptyString() : pattern;
    };
    auto sortRegExpsAndLocatePatternSources = [&] {
        std::ranges::sort(m_regExps, [](const RegExpCode& a, const RegExpCode& b) {
            uint32_t hashA = ImageRegExp::hashOf(a.pattern, a.flags);
            uint32_t hashB = ImageRegExp::hashOf(b.pattern, b.flags);
            if (hashA != hashB)
                return hashA < hashB;
            if (a.flags != b.flags)
                return a.flags.toRaw() < b.flags.toRaw();
            return codePointCompareLessThan(a.pattern, b.pattern);
        });
        auto* typedData = m_typedData.isEmpty() ? nullptr : reinterpret_cast<const Typed::Data*>(m_typedData.span().data());
        BitVector patternLengths;
        for (auto& regExp : m_regExps) {
            if (!typedData || !regExp.pattern.containsOnlyASCII() || !regExp.pattern.is8Bit())
                continue;
            offsetsInTypedData.add(textOf(regExp.pattern), isNotInTypedData);
            patternLengths.set(regExp.pattern.length());
        }
        for (uint32_t index = 0; !offsetsInTypedData.isEmpty() && index < typedData->numberOfStrings; ++index) {
            if (typedData->hasOnlyDecodedForm(index) || !patternLengths.get(typedData->stringSize(index)))
                continue;
            auto text = typedData->string(index);
            auto found = offsetsInTypedData.find<StringViewHashTranslator>(StringView { byteCast<Latin1Character>(text) });
            if (found != offsetsInTypedData.end() && found->value == isNotInTypedData)
                found->value = safeCast<uint32_t>(text.data() - m_typedData.span().data());
        }
    };
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> typedEntriesInsideFunctions;
    for (uint32_t i = 0; i < entriesInsideFunctions.size(); ++i) {
        if (entriesInsideFunctions[i].key.kind == ImageKey::typedFunction)
            typedEntriesInsideFunctions.add(entriesInsideFunctions[i].key.start, i);
    }
    Vector<Vector<uint8_t>> preambles(m_functions.size());
    size_t sizeBeforeEntryWithThis = 0;
    auto generateFunctionPreambles = [&] {
        auto functionsWithPreambles = Typed::collectInParallel<uint32_t>(m_functions.size(), [&](unsigned index, Vector<uint32_t>& collected) {
            auto& info = m_functions[index].code.info;
            if (omittedArgumentsOf[index][1])
                info.compatibleResultsMask = 0;
            if (info.signatureAdapter != StubCall::noFunction || info.adapterResult != StubCall::noFunction || info.compatibleResultsMask)
                collected.append(index);
        });
        for (uint32_t index : functionsWithPreambles) {
            auto& info = m_functions[index].code.info;
            RELEASE_ASSERT(!omittedArgumentsOf[index][1] && info.valueEntry == StubCall::noFunction);
            if (info.compatibleResultsMask)
                preambles[index].append(std::span { reinterpret_cast<const uint8_t*>(&info.compatibleResultsMask), info.compatibleResultsMaskSize });
            else if (info.adapterResult != StubCall::noFunction)
                preambles[index] = generateResultAdapterPreamble(info.adapterResult, info.compatibleResultsMaskSize, info.adapterArgumentCount, sizeBeforeEntryWithThis);
            else
                preambles[index] = generateSignatureAdapterPreamble(info.signatureAdapter, sizeBeforeEntryWithThis);
        }
    };
    auto preambleSize = [&](size_t index) {
        return sizeOfOmittedArguments(omittedArgumentsOf[index][0], omittedArgumentsOf[index][1]) + (m_functions[index].code.info.valueEntry == StubCall::noFunction ? 0 : 2 * sizeof(uint32_t)) + preambles[index].size();
    };
    auto preambleSizeForCall = [&](size_t target, const StubCall& call) -> size_t {
        if (call.entersPreamble())
            return preambles[target].size() - (call.entersPreambleWithThis() ? sizeBeforeEntryWithThis : 0);
        return call.firstOmittedGPR == call.endOfOmittedGPRs ? 0 : sizeOfOmittedArguments(call.firstOmittedGPR, omittedArgumentsOf[target][1]);
    };
    constexpr unsigned farCalleeEntryShift = 27;
    RELEASE_ASSERT(!(m_functions.size() >> farCalleeEntryShift));
    auto farCalleeOf = [&](size_t target, const StubCall& call) {
        return static_cast<uint32_t>(target) | (call.entersPreambleWithThis() ? 30u : call.entersPreamble() ? 31u : call.firstOmittedGPR == call.endOfOmittedGPRs ? 0 : call.firstOmittedGPR + 1u) << farCalleeEntryShift;
    };
    endStep("inlining"_s);
    Outlining outlining;
    auto indexOfFunctionWithKey = [&](const ImageKey& key) -> uint32_t {
        for (unsigned bucket = key.hash() & (capacity - 1); functionWithKey[bucket] != std::numeric_limits<uint32_t>::max(); bucket = (bucket + 1) & (capacity - 1)) {
            if (functionKeys[functionWithKey[bucket]].sameFunction(key))
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
    auto firstAppendedEntryIn = [&](unsigned index) {
        uint32_t first = std::numeric_limits<uint32_t>::max();
        for (uint32_t offset : entriesInsideOf[index])
            first = std::min(first, offset);
        return first;
    };
    Vector<uint8_t> quotes { 0 };
    Vector<uint8_t> quotedSourceBytes;
    Vector<uint32_t> functionQuotes;
    auto gatherQuotes = [&] {
        functionQuotes.fill(0, m_functions.size());
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
                    uint32_t result = safeCast<uint32_t>(quotedSourceBytes.size());
                    quotedSourceBytes.append(kept->span());
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
    };
    Vector<ImageFrame> frames;
    Vector<uint16_t> functionFrame;
    auto gatherFrames = [&] {
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
    };
    Vector<ImageRegExp> imageRegExps;
    Vector<uint8_t> regExpSourceBytes;
    BitVector regExpSourceIsInTypedData;
    auto offsetInTypedData = [&](const String& pattern) -> std::optional<uint32_t> {
        auto found = offsetsInTypedData.find(textOf(pattern));
        if (found == offsetsInTypedData.end() || found->value == isNotInTypedData)
            return std::nullopt;
        return found->value;
    };
    auto buildRegExpRecordsAndSources = [&] {
        for (size_t number = 0; number < m_regExps.size(); ++number) {
            auto& regExp = m_regExps[number];
            ImageRegExp record { };
            record.hash = ImageRegExp::hashOf(regExp.pattern, regExp.flags);
            record.length = regExp.pattern.length();
            record.is8Bit = regExp.pattern.is8Bit();
            record.flags = regExp.flags.toRaw();
            record.numSubpatterns = regExp.numSubpatterns;
            record.minimumSize = regExp.minimumSize;
            record.specificPattern = static_cast<uint8_t>(regExp.specificPattern);
            record.retainsPatternDetails = regExp.retainsPatternDetails;
            record.hasNoCode = regExp.hasNoCode;
            record.sourceIsPattern = regExp.sourceIsPattern;
            imageRegExps.append(record);
        }
        SourcePiecePool<Latin1Character> piecePool8Bit;
        SourcePiecePool<char16_t> piecePool16Bit;
        Vector<std::pair<size_t, Vector<SourcePiece>>> sourcesInPieces;
        for (bool isStoredAsSource : { false, true }) {
            for (size_t number = 0; number < m_regExps.size(); ++number) {
                auto& regExp = m_regExps[number];
                if ((regExp.pattern.length() > ImageRegExp::minLengthStoredAsImageString) != isStoredAsSource)
                    continue;
                if (auto offset = regExp.pattern.containsOnlyASCII() && regExp.pattern.is8Bit() ? offsetInTypedData(regExp.pattern) : std::nullopt) {
                    imageRegExps[number].text = *offset;
                    regExpSourceIsInTypedData.set(number);
                    continue;
                }
                if (isStoredAsSource && !regExp.hasNoCode && !regExp.retainsPatternDetails) {
                    sourcesInPieces.append({ number, regExp.pattern.is8Bit() ? piecePool8Bit.cut(regExp.pattern.span8()) : piecePool16Bit.cut(regExp.pattern.span16()) });
                    continue;
                }
                if (!isStoredAsSource) {
                    imageRegExps[number].text = safeCast<uint32_t>(regExpSourceBytes.size());
                    imageRegExps[number].sourceIsDigest = true;
                    regExpSourceBytes.append(std::span<const uint8_t> { Typed::HashedString::digestOfNarrowestEncoding(regExp.pattern) });
                    continue;
                }
                if (!regExp.pattern.is8Bit())
                    regExpSourceBytes.insertFill(regExpSourceBytes.size(), 0, regExpSourceBytes.size() % 2);
                imageRegExps[number].text = safeCast<uint32_t>(regExpSourceBytes.size());
                regExpSourceBytes.append(regExp.pattern.is8Bit() ? asBytes(regExp.pattern.span8()) : asBytes(regExp.pattern.span16()));
            }
        }
        regExpSourceBytes.insertFill(regExpSourceBytes.size(), 0, regExpSourceBytes.size() % 2);
        uint32_t startOf16BitPieces = safeCast<uint32_t>(regExpSourceBytes.size());
        regExpSourceBytes.append(asBytes(piecePool16Bit.units.span()));
        uint32_t startOf8BitPieces = safeCast<uint32_t>(regExpSourceBytes.size());
        regExpSourceBytes.append(asBytes(piecePool8Bit.units.span()));
        regExpSourceBytes.insertFill(regExpSourceBytes.size(), 0, (4 - regExpSourceBytes.size() % 4) % 4);
        size_t numberOfPieces = 0;
        size_t sourcesInPiecesSize = 0;
        for (auto& [number, pieces] : sourcesInPieces) {
            bool is8Bit = m_regExps[number].pattern.is8Bit();
            for (auto& piece : pieces)
                piece.offset = is8Bit ? startOf8BitPieces + piece.offset : startOf16BitPieces + piece.offset * 2;
            sourcesInPiecesSize += m_regExps[number].pattern.length() * (is8Bit ? 1 : 2);
            if (pieces.size() == 1) {
                imageRegExps[number].text = pieces[0].offset;
                continue;
            }
            numberOfPieces += pieces.size();
            imageRegExps[number].text = safeCast<uint32_t>(regExpSourceBytes.size());
            imageRegExps[number].sourceIsInPieces = true;
            uint32_t count = pieces.size();
            regExpSourceBytes.append(asBytes(singleElementSpan(count)));
            regExpSourceBytes.append(asBytes(pieces.span()));
        }
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: the texts of ", sourcesInPieces.size(), " long regular expressions, ", sourcesInPiecesSize, " bytes: ", numberOfPieces, " pieces of ", piecePool8Bit.units.size() + piecePool16Bit.units.size() * 2, " bytes");
    };
    Vector<std::array<String, 2>> regExpBodyKeys;
    auto makeRegExpBodyKeys = [&] {
        regExpBodyKeys.grow(m_regExps.size());
        for (size_t number = 0; number < m_regExps.size(); ++number) {
            for (unsigned i = 0; i < 2; ++i) {
                auto& body = m_regExps[number].code[i];
                if (body.bytes.isEmpty())
                    continue;
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
                regExpBodyKeys[number][i] = String { byteCast<Latin1Character>(content.span()) };
                regExpBodyKeys[number][i].impl()->hash();
            }
        }
    };
    {
        Vector<std::pair<uint32_t, uint32_t>> entries;
        for (auto& entry : entriesInsideFunctions) {
            if (uint32_t index = indexOfFunctionWithKey(entry.function); index != ImageFunction::noSuchFunction)
                entries.append({ index, safeCast<uint32_t>(entry.offset) });
        }
        bool stackUsageIsValid = true;
        Ref independentSteps = Thread::create("AOT image"_s, [&] {
            stackUsageIsValid = verifyUncheckedStackUsage();
            collectOmittedArguments();
            findEntriesInside();
            sortRegExpsAndLocatePatternSources();
            makeRegExpBodyKeys();
            buildRegExpRecordsAndSources();
            gatherQuotes();
            gatherFrames();
        });
        outlining = chooseWindows(WTF::map(m_functions, [](const Function& function) {
            return &function.code;
        }), WTF::move(entries));
        independentSteps->waitForCompletion();
        if (!stackUsageIsValid)
            return { };
        generateFunctionPreambles();
        endStep("choosing what is outlined"_s);
    }
    static const char* const outlinesForTesting = byteCast<char>(Options::aotOutlineForTesting());
    if (outlinesForTesting && outlining.bodies.isEmpty()) {
        const bool outlinesCalls = !!strchr(outlinesForTesting, 'k');
        struct Found {
            OutlinedSite place;
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
            static const char* const faultForTesting = byteCast<char>(Options::aotOutlinerFaultForTesting());
            const uint32_t firstAppendedEntry = isARM64() && !(faultForTesting && !strcmp(faultForTesting, "guest")) ? firstAppendedEntryIn(index) : std::numeric_limits<uint32_t>::max();
            BitVector isTarget(code.bytes.size() + 1);
            BitVector isNotOutlinable(code.bytes.size() + 8);
            forEachInstructionInCode(code, [&](const InstructionInCode& instruction) {
                using Kind = InstructionInCode::Kind;
                if (instruction.kind == Kind::Jump || instruction.kind == Kind::ConditionalJump || instruction.kind == Kind::TableAddress || instruction.kind == Kind::TableEntry)
                    isTarget.set(instruction.target);
            });
            for (auto& entry : info.typedCatchEntrypointsForCallSites)
                isTarget.set(entry.second);
            for (uint32_t offset : entriesInsideOf[index])
                isTarget.set(offset);
            for (auto& reference : info.indexReferences) {
                if (isARM64()) {
                    isNotOutlinable.set(reference.offset);
                    isNotOutlinable.set(reference.offset + sizeof(uint32_t));
                } else
                    isNotOutlinable.set(reference.offset - 1);
            }
            for (auto& access : info.faultingAccesses)
                isNotOutlinable.set(access.codeOffset);
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
                bool isNotOutlinableHere = instruction.offset >= firstAppendedEntry;
                for (uint32_t i = 0; i < instruction.length; ++i)
                    isNotOutlinableHere |= isNotOutlinable.get(instruction.offset + i);
                if (isTarget.get(instruction.offset))
                    flush(nullptr);
                if ((instruction.kind == Kind::Plain || (instruction.kind == Kind::Pinned && faultForTesting && !strcmp(faultForTesting, "pinned"))) && !isNotOutlinableHere) {
                    run.append({ instruction.offset, instruction.length });
                    return;
                }
                const StubCall* call = instruction.kind == Kind::StubCall && outlinesCalls && !isNotOutlinableHere ? &info.stubCalls[instruction.target] : nullptr;
                if (call && (call->isTailCall || call->body || (isARM64() && call->function != StubCall::noFunction)))
                    call = nullptr;
                flush(call);
            });
            flush(nullptr);
        });
        outlining.places.grow(m_functions.size());
        std::map<Vector<uint64_t>, uint32_t, WordsAreInOrder> bodyForWords;
        for (uint32_t index = 0; index < m_functions.size(); ++index) {
            for (auto& found : foundIn[index]) {
                auto [entry, isNew] = bodyForWords.try_emplace(WTF::move(found.words), static_cast<uint32_t>(outlining.bodies.size()));
                if (isNew)
                    outlining.bodies.append({ index, found.place.offset, found.place.length, 0, index, found.endsInCall });
                auto& body = outlining.bodies[entry->second];
                ++body.numberOfPlaces;
                if (body.singleCaller != index)
                    body.singleCaller = Outlining::none;
                found.place.body = entry->second;
                outlining.places[index].append(found.place);
            }
        }
    }
    OutlinedBodies sharedBodies;
    OutlinedBodies ownBodies;
    Vector<uint32_t> bodyIndexWithinKind;
    Vector<Vector<uint32_t, 0>> ownBodiesOf(m_functions.size());
    Vector<ImageKey> bodyCallees;
    Vector<ArgumentMoves> bodyArgumentMoves;
    Vector<Vector<std::pair<uint32_t, uint32_t>, 0>> paddingBeforeEntries(m_functions.size());
    if (!outlining.bodies.isEmpty()) {
        RELEASE_ASSERT(outlining.places.size() == m_functions.size());
        findEntriesInside();
        for (auto& body : outlining.bodies) {
            auto& from = m_functions[body.function].code;
            RELEASE_ASSERT(body.length >= sizeOfNearCall && body.offset + body.length <= from.bytes.size());
            auto& like = body.singleCaller == Outlining::none ? sharedBodies : ownBodies;
            bodyIndexWithinKind.append(like.offsets.size());
            if (body.singleCaller != Outlining::none)
                ownBodiesOf[body.singleCaller].append(like.offsets.size());
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
                    bodyCallees.append(from.info.knownCallees[jump.function]);
                    jump.function = bodyCallees.size() - 1;
                }
                if (jump.argumentMoves != StubCall::noArgumentMoves) {
                    bodyArgumentMoves.append(from.info.argumentMoves[jump.argumentMoves]);
                    jump.argumentMoves = bodyArgumentMoves.size() - 1;
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
        auto placeWith = [](const Vector<OutlinedSite>& places, uint32_t firstByte, uint32_t lastByte) -> const OutlinedSite* {
            auto* found = std::upper_bound(places.begin(), places.end(), firstByte, [](uint32_t byte, const OutlinedSite& place) {
                return byte < place.offset + place.length;
            });
            return found != places.end() && found->offset <= lastByte ? found : nullptr;
        };
        struct OriginalCode {
            Vector<uint8_t> bytes;
            Vector<uint8_t> comparableAt;
            Vector<uint32_t> bodyCalls;
        };
        constexpr uint8_t isRelinkedOnARM64 = 0xff;
        auto isUnmoved = [&](const Vector<uint8_t>& oldBytes, const Vector<uint8_t>& comparableAt, const Vector<uint32_t>& bodyCalls, const Vector<uint8_t>& newBytes, const Vector<OutlinedSite>& places, uint32_t oldOffset, uint32_t newOffset) {
            if (auto* place = placeWith(places, oldOffset, oldOffset))
                return place->offset == oldOffset && newOffset == bodyCalls[place - places.begin()];
            uint8_t comparable = oldOffset < comparableAt.size() ? comparableAt[oldOffset] : 0;
            if (comparable == isRelinkedOnARM64) {
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
        Vector<OriginalCode> originalHosts(m_functions.size());
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            auto& places = outlining.places[index];
            if (places.isEmpty())
                return;
            auto& code = m_functions[index].code;
            auto& info = code.info;
            auto& shifts = shiftsOf[index];
            RELEASE_ASSERT(info.typedFunction != FunctionReportableSites::notTyped);
            RELEASE_ASSERT(!isARM64() || (!info.hasNoFrame && places.last().offset + places.last().length <= firstAppendedEntryIn(index)));
            Vector<uint8_t> bytes;
            bytes.reserveInitialCapacity(code.bytes.size());
            Vector<StubCall> calls;
            Vector<uint32_t> bodyCalls;
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
                    paddingBeforeEntries[index].append({ entry, filling });
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
                RELEASE_ASSERT(body.singleCaller == Outlining::none || body.singleCaller == index);
                copyUpTo(place.offset);
                StubCall call { };
                call.offset = safeCast<uint32_t>(bytes.size());
                call.isTailCall = false;
                call.body = place.body + 1;
                calls.append(call);
                bodyCalls.append(call.offset);
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
            size_t nextPlace = 0;
            forEachInstructionInCode(code, [&](const InstructionInCode& instruction) {
                using Kind = InstructionInCode::Kind;
                while (nextPlace < places.size() && places[nextPlace].offset + places[nextPlace].length <= instruction.offset)
                    ++nextPlace;
                if (auto* place = nextPlace < places.size() && places[nextPlace].offset <= instruction.offset + instruction.length - 1 ? &places[nextPlace] : nullptr) {
                    bool endsOutlinedRange = instruction.offset + instruction.length == place->offset + place->length;
                    bool endsInCall = endsOutlinedRange && outlining.bodies[place->body].endsInCall;
                    RELEASE_ASSERT(instruction.offset >= place->offset && instruction.offset + instruction.length <= place->offset + place->length);
                    RELEASE_ASSERT(instruction.kind == (endsInCall ? Kind::StubCall : Kind::Plain));
                    numberOfHeads += instruction.offset == place->offset;
                }
                bool isRelinked = instruction.kind == Kind::Jump || instruction.kind == Kind::ConditionalJump || instruction.kind == Kind::TableAddress;
                comparableAt[instruction.offset] = instruction.kind == Kind::TableEntry ? 0 : !isRelinked ? instruction.length : isARM64() ? isRelinkedOnARM64 : instruction.length - (instruction.length == 2 ? 1 : sizeof(int32_t));
                if (instruction.kind == Kind::TableEntry && !isInTable)
                    startOfTable = instruction.offset;
                isInTable = instruction.kind == Kind::TableEntry;
                if (instruction.kind != Kind::Jump && instruction.kind != Kind::ConditionalJump && instruction.kind != Kind::TableAddress && instruction.kind != Kind::TableEntry)
                    return;
                RELEASE_ASSERT(isAtHeadOrOutside(instruction.target));
                if (instruction.kind == Kind::TableEntry) {
                    int32_t entry;
                    memcpy(&entry, code.bytes.span().data() + instruction.offset, sizeof(entry));
                    RELEASE_ASSERT(instruction.target == startOfTable + entry);
                }
                if ((breaks("jump") && instruction.kind == Kind::ConditionalJump) || (breaks("table") && (instruction.kind == Kind::TableEntry || instruction.kind == Kind::TableAddress)))
                    return;
                setTarget(bytes.mutableSpan(), instruction, map(instruction.offset), map(instruction.target), map(startOfTable));
            });
            RELEASE_ASSERT(numberOfHeads == places.size());
            auto stands = [&](uint32_t oldOffset, uint32_t newOffset) {
                return isUnmoved(code.bytes, comparableAt, bodyCalls, bytes, places, oldOffset, newOffset);
            };
            auto referenceIsUnmoved = [&](uint32_t oldOffset, uint32_t newOffset) {
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
                RELEASE_ASSERT(referenceIsUnmoved(was, reference.offset));
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
                originalHosts[index] = { WTF::move(code.bytes), WTF::move(comparableAt), WTF::move(bodyCalls) };
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
            auto& host = originalHosts[index];
            RELEASE_ASSERT(isUnmoved(host.bytes, host.comparableAt, host.bodyCalls, m_functions[index].code.bytes, outlining.places[index], was, safeCast<uint32_t>(entry.offset)));
        }
    }
    endStep("outlining"_s);
    Vector<size_t> functionEnds(m_functions.size());
    Vector<uint8_t> isReturnedInto(m_functions.size());
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        auto& code = m_functions[index].code;
        functionEnds[index] = code.bytes.size();
        isReturnedInto[index] = !code.bytes.isEmpty() && !std::ranges::all_of(code.info.stubCalls, [](auto& call) { return call.isTailCall; });
    });
    for (size_t index = 1; index < functionEnds.size(); ++index)
        functionEnds[index] += WTF::roundUpToMultipleOf<imageFunctionAlignment>(functionEnds[index - 1]);
    auto walkIsShort = [&](unsigned shift) {
        size_t startsPassed = 0;
        size_t lookups = 0;
        size_t granuleStart = 0;
        for (size_t index = 0; index < functionEnds.size(); ++index) {
            if (!isReturnedInto[index])
                continue;
            size_t startOfGranule = (functionEnds[index] - 1) >> shift << shift;
            while (functionEnds[granuleStart] <= startOfGranule)
                ++granuleStart;
            startsPassed += index - granuleStart;
            ++lookups;
        }
        return 2 * startsPassed <= 5 * lookups;
    };
    unsigned codeGranuleShift = maxCodeGranuleShift;
    while (codeGranuleShift > minCodeGranuleShift && !walkIsShort(codeGranuleShift))
        --codeGranuleShift;
    endStep("the size of a granule of code"_s);
    BitVector calledThunks;
    BitVector calledStubs;
    auto recordThunkForCall = [](const StubCall& call, BitVector& stubsCalled, BitVector& thunksCalled) {
        stubsCalled.set(static_cast<unsigned>(call.stub));
        if (call.thunk)
            thunksCalled.set(call.thunk - 1);
    };
    auto recordThunkForMoves = [](const ArgumentMoves& moves, BitVector& stubsCalled, BitVector& thunksCalled) {
        stubsCalled.set(static_cast<unsigned>(moves[0]));
        if (moves[1] == StubCall::noImmediate)
            return;
        if (auto thunk = thunkFor(static_cast<Stub>(moves[0]), static_cast<uint32_t>(moves[1])))
            thunksCalled.set(*thunk);
    };
    {
        constexpr size_t functionsAtATime = 1024;
        Lock calledStubsLock;
        Typed::forEachIndexInParallel((m_functions.size() + functionsAtATime - 1) / functionsAtATime, [&](unsigned chunk) {
            BitVector stubsCalled;
            BitVector thunksCalled;
            for (auto& function : m_functions.span().subspan(chunk * functionsAtATime, std::min(functionsAtATime, m_functions.size() - chunk * functionsAtATime))) {
                for (auto& call : function.code.info.stubCalls)
                    recordThunkForCall(call, stubsCalled, thunksCalled);
                for (auto& moves : function.code.info.argumentMoves)
                    recordThunkForMoves(moves, stubsCalled, thunksCalled);
            }
            Locker locker { calledStubsLock };
            calledStubs.merge(stubsCalled);
            calledThunks.merge(thunksCalled);
        });
    }
    for (auto& jump : sharedBodies.jumps)
        recordThunkForCall(jump, calledStubs, calledThunks);
    for (auto& jump : ownBodies.jumps)
        recordThunkForCall(jump, calledStubs, calledThunks);
    for (auto& moves : bodyArgumentMoves)
        recordThunkForMoves(moves, calledStubs, calledThunks);
    std::unique_ptr<StubBlob> imageStubs;
    Ref stubGeneratorThread = Thread::create("AOT image"_s, [&] {
        imageStubs = createStubBlob(codeGranuleShift, !m_typedData.isEmpty() && reinterpret_cast<const Typed::Data*>(m_typedData.span().data())->functionLengthsAreBytes, calledThunks, calledStubs, !m_webAssemblyCode.isEmpty());
    }, ThreadType::Unknown, Thread::defaultQOS, Thread::defaultSchedulingPolicy, StackAllocationSpecification::RequestSize(4 * MB));
    Vector<const ArgumentMoves*> argumentMoves;
    auto argumentMovesAreInOrder = [](const ArgumentMoves* a, const ArgumentMoves* b) {
        return std::ranges::lexicographical_compare(*a, *b);
    };
    auto removeDuplicates = [](Vector<const ArgumentMoves*>& list) {
        list.shrink(std::unique(list.begin(), list.end(), [](const ArgumentMoves* a, const ArgumentMoves* b) {
            return *a == *b;
        }) - list.begin());
    };
    auto sortAndRemoveDuplicates = [&](Vector<const ArgumentMoves*>& list) {
        std::ranges::sort(list, argumentMovesAreInOrder);
        removeDuplicates(list);
    };
    constexpr size_t functionsInChunk = 1024;
    constexpr unsigned shardBits = 6;
    constexpr unsigned numberOfShards = 1 << shardBits;
    Vector<std::array<Vector<const ArgumentMoves*>, numberOfShards>> chunkArgumentMoves((m_functions.size() + functionsInChunk - 1) / functionsInChunk);
    Typed::forEachIndexInParallel(chunkArgumentMoves.size(), [&](unsigned chunk) {
        Vector<const ArgumentMoves*> own;
        for (auto& function : m_functions.span().subspan(chunk * functionsInChunk, std::min(functionsInChunk, m_functions.size() - chunk * functionsInChunk))) {
            for (auto& call : function.code.info.stubCalls) {
                if (call.argumentMoves != StubCall::noArgumentMoves)
                    own.append(&function.code.info.argumentMoves[call.argumentMoves]);
            }
        }
        sortAndRemoveDuplicates(own);
        for (auto* moves : own) {
            uint64_t hash = moves->size();
            for (uint64_t word : *moves)
                hash = (hash ^ word) * 0x9e3779b97f4a7c15ULL;
            chunkArgumentMoves[chunk][hash >> (64 - shardBits)].append(moves);
        }
    });
    std::array<Vector<const ArgumentMoves*>, numberOfShards> shardArgumentMoves;
    Typed::forEachIndexInParallel(numberOfShards, [&](unsigned shard) {
        for (auto& ofChunk : chunkArgumentMoves)
            shardArgumentMoves[shard].appendVector(ofChunk[shard]);
        sortAndRemoveDuplicates(shardArgumentMoves[shard]);
    });
    for (auto& ofShard : shardArgumentMoves)
        argumentMoves.appendVector(ofShard);
    for (auto& moves : bodyArgumentMoves)
        argumentMoves.append(&moves);
    Typed::sortOnPool(argumentMoves.mutableSpan(), argumentMovesAreInOrder);
    removeDuplicates(argumentMoves);
    auto numberOfArgumentMoves = [&](const ArgumentMoves& moves) {
        return static_cast<size_t>(std::ranges::lower_bound(argumentMoves, &moves, argumentMovesAreInOrder) - argumentMoves.begin());
    };
    ArgumentThunks argumentThunks = generateArgumentThunks(argumentMoves);
    Vector<size_t> sizeOfOwnBodiesOf(FillWith { }, m_functions.size(), static_cast<size_t>(0));
    for (size_t index = 0; index < m_functions.size(); ++index) {
        for (uint32_t number : ownBodiesOf[index])
            sizeOfOwnBodiesOf[index] += ownBodies.sizeOf(number);
    }
    stubGeneratorThread->waitForCompletion();
    const StubBlob& stubs = *imageStubs;
    auto offsetOfThunk = [&](unsigned thunk) {
        RELEASE_ASSERT(!thunkIsGeneratedOnlyIfCalled(thunk) || calledThunks.get(thunk));
        return stubs.thunkOffsets[thunk];
    };
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
    bool stubsServeBothSides = false;
    Vector<std::array<size_t, 2>> codeAndPreambleSizes(m_functions.size());
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        codeAndPreambleSizes[index] = { m_functions[index].code.bytes.size(), preambleSize(index) };
    });
    {
        size_t sizeOfCode = sizeOfStubs + imageStubsAlignment + (static_cast<size_t>(1) << codeGranuleShift);
        for (size_t index = 0; index < m_functions.size(); ++index)
            sizeOfCode += WTF::roundUpToMultipleOf<imageFunctionAlignment>(codeAndPreambleSizes[index][0] + sizeOfOwnBodiesOf[index] + codeAndPreambleSizes[index][1]) + imageFunctionAlignment;
        stubsServeBothSides = sizeOfCode > reach;
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
        size_t end = stubsServeBothSides ? static_cast<size_t>(1) << codeGranuleShift : 0;
        size_t lastStubs = std::numeric_limits<size_t>::max();
        bool isBeforeStubs = stubsServeBothSides;
        size_t startOfCodeBeforeStubs = end;
        for (auto [functionSize, sizeBeforeFunction] : codeAndPreambleSizes) {
            end = WTF::roundUpToMultipleOf<imageFunctionAlignment>(end + sizeBeforeFunction);
            size_t endOfFunction = end + functionSize + sizeOfOwnBodiesOf[at.size()];
            if (!isBeforeStubs && stubsServeBothSides && endOfFunction - lastStubs > reach) {
                isBeforeStubs = true;
                startOfCodeBeforeStubs = end;
            }
            if (isBeforeStubs ? endOfFunction + imageStubsAlignment + sizeOfStubs - startOfCodeBeforeStubs > reach : lastStubs == std::numeric_limits<size_t>::max() || endOfFunction - lastStubs > reach) {
                isBeforeStubs = false;
                end = lastStubs = WTF::roundUpToMultipleOf<imageStubsAlignment>(end);
                end = WTF::roundUpToMultipleOf<imageFunctionAlignment>(end + sizeOfStubs + sizeBeforeFunction);
            }
            at.append(end);
            end += functionSize + sizeOfOwnBodiesOf[at.size() - 1];
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
            m_failureReason = makeString("the program has too much machine code: what is laid beside every copy of the runtime's stubs must stay under "_s, stubCallReachSlack, " bytes: the stubs are "_s, stubs.bytes.size(), " bytes, the thunks that set the arguments of calls "_s, argumentThunks.bytes.size(), ", the code that several functions share "_s, sharedBodies.bytes.size(), ", and calls between functions that are too far apart need "_s, sizeOfVeneers.load());
            return { };
        }
    }
    endStep("where the copies of the stubs go"_s);
    BitVector fallsIntoNext(m_functions.size());
    BitVector hasSharedKey(m_functions.size());
    Vector<uint32_t> firstWithKey(m_functions.size());
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        firstWithKey[index] = indexOfFunctionWithKey(functionKeys[index]);
    });
    for (size_t index = 0; index < m_functions.size(); ++index) {
        if (uint32_t first = firstWithKey[index]; first != index) {
            hasSharedKey.quickSet(first);
            hasSharedKey.quickSet(index);
        }
    }
    constexpr uint32_t noJumpToNext = std::numeric_limits<uint32_t>::max();
    Vector<uint32_t> jumpToNext(FillWith { }, m_functions.size(), noJumpToNext);
    Vector<size_t> sizesBeforeCut(FillWith { }, m_functions.size(), static_cast<size_t>(0));
    Vector<uint8_t> jumpIsReturnedTo(FillWith { }, m_functions.size(), static_cast<uint8_t>(0));
    Typed::forEachIndexInParallel(m_functions.isEmpty() ? 0 : m_functions.size() - 1, [&](unsigned index) {
        auto& function = m_functions[index].code;
        auto& calls = function.info.stubCalls;
        size_t size = function.bytes.size();
        sizesBeforeCut[index] = size;
        size_t last = calls.findIf([&](const StubCall& call) {
            return call.isTailCall && !call.thunk && !call.body && call.function != StubCall::noFunction && call.argumentMoves == StubCall::noArgumentMoves && call.firstOmittedGPR == call.endOfOmittedGPRs
                && call.offset + sizeOfJump <= size && size - call.offset - sizeOfJump < imageFunctionAlignment + isX86() && *directTargetOf(index, call) == index + 1;
        });
        bool mayBeCut = last != notFound && !hasSharedKey.quickGet(index) && farCallees[index].isEmpty() && !sizeOfOwnBodiesOf[index] && !preambleSize(index + 1);
#if CPU(X86_64)
        if (mayBeCut) {
            auto afterJump = function.bytes.span().subspan(calls[last].offset + sizeOfJump);
            while (!afterJump.empty() && afterJump.front() == 0xcc)
                afterJump = afterJump.subspan(1);
            std::array<uint8_t, imageFunctionAlignment> padding;
            X86Assembler::fillNops(padding.data(), afterJump.size());
            mayBeCut = equalSpans(afterJump, std::span<const uint8_t> { padding }.first(afterJump.size()));
        }
#endif
        if (!mayBeCut)
            return;
        jumpToNext[index] = safeCast<uint32_t>(last);
        size_t offset = calls[last].offset;
        jumpIsReturnedTo[index] = !(offset % imageFunctionAlignment) && calls.containsIf([&](const StubCall& call) {
            return !call.isTailCall && call.offset + sizeOfNearCall == offset;
        });
    });
    size_t numberOfJumpsReturnedTo = 0;
    for (size_t index = 0, sizeOfRun = 0; index + 1 < m_functions.size(); ++index) {
        constexpr size_t maxRunSize = 1 << 20;
        sizeOfRun += sizesBeforeCut[index];
        if (jumpToNext[index] == noJumpToNext || sizeOfRun > maxRunSize) {
            sizeOfRun = 0;
            continue;
        }
        if (jumpIsReturnedTo[index]) {
            ++numberOfJumpsReturnedTo;
            if (!isX86()) {
                sizeOfRun = 0;
                continue;
            }
        }
        fallsIntoNext.quickSet(index);
    }
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        if (!fallsIntoNext.quickGet(index))
            return;
        auto& function = m_functions[index].code;
        auto& calls = function.info.stubCalls;
        size_t last = jumpToNext[index];
        size_t offset = calls[last].offset;
        size_t filling = jumpIsReturnedTo[index] ? imageFunctionAlignment : WTF::roundUpToMultipleOf<imageFunctionAlignment>(offset) - offset;
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
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", fallsIntoNext.bitCount(), " functions fall into the function that is placed next; ", hasSharedKey.bitCount(), " share their key with another and are left as they are; ", numberOfJumpsReturnedTo, " such jumps stand where a call returns to, with no filling behind");
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        auto& function = m_functions[index].code;
        for (auto& call : function.info.stubCalls)
            RELEASE_ASSERT_WITH_MESSAGE(call.isTailCall || call.stub == Stub::PollAtPrologue || call.offset + sizeOfNearCall < function.bytes.size(), "AOT: a function ends in a call: its return address would be taken for the next function's");
    });
    endStep("functions that fall into the next"_s);
    Vector<uint8_t> callSites { 0 };
    Vector<uint32_t> functionCallSites;
    Ref callSitesGatherer = Thread::create("AOT image"_s, [&] {
        Vector<Vector<uint8_t>> callSitesOf(m_functions.size());
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
            auto& function = m_functions[index];
            auto& callSites = callSitesOf[index];
            Vector<std::pair<uint32_t, uint32_t>> all;
            BitVector constructs;
            for (uint32_t number : function.code.info.constructingSites)
                constructs.set(number);
            for (auto& call : function.code.info.stubCalls) {
                if (call.callSite != StubCall::noCallSite && !call.isTailCall) {
                    all.append({ call.offset + static_cast<uint32_t>(sizeOfNearCall), call.callSite });
                    if ((call.stub == Stub::ConstructClosure || call.stub == Stub::ConstructClosureList) && function.code.info.typedFunction != FunctionReportableSites::notTyped)
                        constructs.set(CallSiteIndex(PackedSite::bits(call.callSite)).bytecodeIndex().offset());
                }
            }
            for (auto& access : function.code.info.faultingAccesses) {
                if (access.callSite == StubCall::noCallSite)
                    continue;
                all.append({ access.codeOffset + static_cast<uint32_t>(access.isLoopPoll ? sizeOfLoopPoll : access.throwSite.isEmpty() ? offsetOfReturnAddressInTrap : sizeOfNearCall), access.callSite });
            }
            if (all.isEmpty())
                return;
            std::ranges::sort(all);
            auto& spreadSites = function.code.info.spreadSites;
            auto& inlineFrames = function.code.info.inlineFrames;
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
            unsigned writtenCount = 0;
            for (auto& [offset, site] : all) {
                if (isInCodeOrder) {
                    RELEASE_ASSERT(CallSiteIndex(PackedSite::bits(site)).bytecodeIndex().offset() == ++number && !PackedSite::isTailCallSite(site));
                    if (writtenCount && !(writtenCount % SiteCheckpoint::sitesBetween)) {
                        RELEASE_ASSERT(!(sitesInCodeOrder.size() >> 24));
                        SiteCheckpoint checkpoint { static_cast<uint32_t>(previousOffset / codeOffsetUnit), static_cast<uint32_t>(sitesInCodeOrder.size() << 8 | previousFrame) };
                        callSites.append(std::span { reinterpret_cast<const uint8_t*>(&checkpoint), sizeof(checkpoint) });
                    }
                    ++writtenCount;
                    unsigned frame = PackedSite::inlineFrame(site);
                    bool differsFromPrevious = frame != previousFrame || constructs.get(number);
                    appendVarint(sitesInCodeOrder, static_cast<uint64_t>((offset - previousOffset) / codeOffsetUnit) << 1 | differsFromPrevious);
                    if (differsFromPrevious)
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
        UncheckedKeyHashMap<uint64_t, Vector<std::pair<uint32_t, uint32_t>, 1>> emittedStreams;
        size_t deduplicatedStreamsSize = 0;
        for (auto& ofFunction : callSitesOf) {
            if (ofFunction.isEmpty()) {
                functionCallSites.append(0);
                continue;
            }
            uint64_t hash = 1469598103934665603ull;
            for (uint8_t byte : ofFunction)
                hash = (hash ^ byte) * 1099511628211ull;
            auto& withHash = emittedStreams.add(hash | 1, Vector<std::pair<uint32_t, uint32_t>, 1> { }).iterator->value;
            size_t same = withHash.findIf([&](auto& laid) {
                return laid.second == ofFunction.size() && equalSpans(callSites.span().subspan(laid.first, laid.second), ofFunction.span());
            });
            if (same != notFound) {
                functionCallSites.append(withHash[same].first);
                deduplicatedStreamsSize += ofFunction.size();
                continue;
            }
            withHash.append({ safeCast<uint32_t>(callSites.size()), safeCast<uint32_t>(ofFunction.size()) });
            functionCallSites.append(safeCast<uint32_t>(callSites.size()));
            callSites.appendVector(ofFunction);
        }
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: the call sites: ", callSites.size(), " bytes; ", deduplicatedStreamsSize, " bytes of streams equal to one laid already");
        callSitesOf.clear();
    });
    auto waitForCallSitesIfFinishFails = makeScopeExit([&] {
        callSitesGatherer->waitForCompletion();
    });
    size_t recordsSize = 0;
    size_t codeSize = stubsServeBothSides ? static_cast<size_t>(1) << codeGranuleShift : 0;
    bool isBeforeStubs = stubsServeBothSides;
    size_t firstFunctionBeforeStubs = 0;
    size_t startOfCodeBeforeStubs = codeSize;
    auto placeStubs = [&](size_t sizeBeforeNext) {
        codeSize = WTF::roundUpToMultipleOf<imageStubsAlignment>(codeSize);
        stubsAt.append(codeSize);
        if (isBeforeStubs) {
            for (size_t index = firstFunctionBeforeStubs; index < placement.size(); ++index)
                placement[index].second = codeSize;
        }
        isBeforeStubs = false;
        static_assert(!((1 << minCodeGranuleShift) % imageFunctionAlignment));
        codeSize = WTF::roundUpToMultipleOf(static_cast<size_t>(1) << codeGranuleShift, codeSize + sizeOfStubs + sizeBeforeNext);
    };
    Vector<size_t> recordSizes(m_functions.size());
    Vector<size_t> preambleSizes(m_functions.size());
    Vector<size_t> sizesWithVeneers(m_functions.size());
    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        auto& function = m_functions[index];
        recordSizes[index] = sizeof(ImageFunction) + function.code.info.sites.size() * (sizeof(Site) + (identifierIndices ? 0 : sizeof(uint32_t))) + function.code.info.plans.sizeInBytes() + (function.code.info.quotes.isEmpty() && function.code.info.constructSites.isEmpty() ? 0 : sizeof(uint32_t));
        RELEASE_ASSERT(function.code.info.sites.size() == function.code.info.numSlots);
        preambleSizes[index] = preambleSize(index);
        sizesWithVeneers[index] = sizeWithVeneers(index);
    });
    for (size_t functionIndex = 0; functionIndex < m_functions.size(); ++functionIndex) {
        recordsSize += recordSizes[functionIndex];
        codeSize = WTF::roundUpToMultipleOf<imageFunctionAlignment>(codeSize + preambleSizes[functionIndex]);
        bool isFallenInto = functionIndex && fallsIntoNext.quickGet(functionIndex - 1);
        RELEASE_ASSERT(!isFallenInto || codeSize == placement.last().first + m_functions[functionIndex - 1].code.bytes.size());
        size_t sizeOfRun = sizesWithVeneers[functionIndex];
        for (size_t last = functionIndex; !isFallenInto && fallsIntoNext.quickGet(last); ++last)
            sizeOfRun += sizesWithVeneers[last + 1];
        if (!isBeforeStubs && stubsServeBothSides && !isFallenInto && codeSize + sizeOfRun - stubsAt.last() > reach) {
            isBeforeStubs = true;
            firstFunctionBeforeStubs = placement.size();
            startOfCodeBeforeStubs = codeSize;
        }
        if (isBeforeStubs ? !isFallenInto && codeSize + sizeOfRun + imageStubsAlignment + sizeOfStubs - startOfCodeBeforeStubs > reach : stubsAt.isEmpty() || (!isFallenInto && codeSize + sizeOfRun - stubsAt.last() > reach))
            placeStubs(preambleSizes[functionIndex]);
        placement.append({ codeSize, stubsAt.isEmpty() ? 0 : stubsAt.last() });
        if (Options::logAOTTypeInference()) [[unlikely]]
            dataLogLn("PLACED @", m_functions[placement.size() - 1].key.module, ":", m_functions[placement.size() - 1].key.start, ":", m_functions[placement.size() - 1].key.kind, " ", codeSize);
        codeSize += sizesWithVeneers[functionIndex];
    }
    if (isBeforeStubs && firstFunctionBeforeStubs < placement.size())
        placeStubs(0);
    Vector<size_t> ownBodyOffsets(FillWith { }, ownBodies.offsets.size(), static_cast<size_t>(0));
    auto bodyJumpTarget = [&](const StubCall& jump, size_t stubCopyIndex) -> size_t {
        if (jump.function != StubCall::noFunction) {
            uint32_t callee = indexOfFunctionWithKey(bodyCallees[jump.function]);
            RELEASE_ASSERT(callee != ImageFunction::noSuchFunction);
            return placement[callee].first - preambleSizeForCall(callee, jump);
        }
        if (jump.argumentMoves != StubCall::noArgumentMoves)
            return stubCopyIndex + argumentThunksAt + argumentThunks.offsets[numberOfArgumentMoves(bodyArgumentMoves[jump.argumentMoves])];
        return stubCopyIndex + (jump.thunk ? offsetOfThunk(jump.thunk - 1) : stubs.offsets[static_cast<unsigned>(jump.stub)]);
    };
    for (size_t index = 0; index < m_functions.size(); ++index) {
        size_t at = placement[index].first + sizesWithVeneers[index] - sizeOfOwnBodiesOf[index];
        for (uint32_t number : ownBodiesOf[index]) {
            ownBodyOffsets[number] = at;
            at += ownBodies.sizeOf(number);
        }
    }
    size_t functionsEnd = codeSize;
    static_assert(maxStubCopiesPerImage * stubCallReach > std::numeric_limits<uint32_t>::max());
    if (stubsAt.size() > maxStubCopiesPerImage) {
        m_failureReason = makeString("the program has too much machine code: "_s, codeSize, " bytes need "_s, stubsAt.size(), " copies of the runtime's stubs, and the limit is "_s, maxStubCopiesPerImage);
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
            if (function.code.rareCodeStart != std::numeric_limits<uint32_t>::max())
                out->println("H\t", index, "\t", function.code.rareCodeStart);
        }
        for (auto& folded : foldedFunctions) {
            for (unsigned bucket = folded.canonicalFunction.hash() & (capacity - 1); functionWithKey[bucket] != std::numeric_limits<uint32_t>::max(); bucket = (bucket + 1) & (capacity - 1)) {
                if (!m_functions[functionWithKey[bucket]].key.sameFunction(folded.canonicalFunction))
                    continue;
                out->println("A\t", functionStarts[functionWithKey[bucket]], "\t", folded.size, "\t", folded.key.module, "\t", folded.key.start, "\t", folded.key.kind, "\t", folded.nameForMap);
                break;
            }
        }
        for (size_t at : stubsAt)
            out->println("C\t", at);
        for (size_t i = 0; i < stubs.names.size(); ++i) {
            if (i >= numberOfStubs && thunkIsGeneratedOnlyIfCalled(i - numberOfStubs) && !calledThunks.get(i - numberOfStubs))
                continue;
            if (i < numberOfStubs && isGeneratedOnlyIfCalled(static_cast<Stub>(i)) && !calledStubs.get(i))
                continue;
            out->println("T\t", stubs.names[i].second, "\t", stubs.names[i].first);
        }
        for (size_t i = 0; i < argumentMoves.size(); ++i) {
            auto& name = stubs.names[argumentMoves[i]->at(1) == StubCall::noImmediate ? static_cast<size_t>(argumentMoves[i]->at(0)) : numberOfStubs + *thunkFor(static_cast<Stub>(argumentMoves[i]->at(0)), static_cast<uint32_t>(argumentMoves[i]->at(1)))].first;
            out->println("T\t", argumentThunksAt + argumentThunks.offsets[i], "\t", name, " with arguments");
        }
        if (!outlining.bodies.isEmpty())
            out->println("Y\tS, O, J and G hold the offsets and the bytes of the functions as they were ahead of the outlining");
        for (uint32_t number = 0; number < outlining.bodies.size(); ++number) {
            uint32_t indexWithinKind = bodyIndexWithinKind[number];
            if (outlining.bodies[number].singleCaller == Outlining::none)
                out->println("T\t", sharedBodiesAt + sharedBodies.offsets[indexWithinKind], "\toutlined ", number);
            else
                out->println("U\t", ownBodyOffsets[indexWithinKind], "\t", ownBodies.sizeOf(indexWithinKind), "\t", outlining.bodies[number].singleCaller, "\toutlined ", number);
        }
        for (size_t index = 0; index < outlining.places.size(); ++index) {
            if (outlining.places[index].isEmpty())
                continue;
            out->print("P\t", index);
            for (auto& place : outlining.places[index])
                out->print("\t", place.offset, "\t", place.length, "\t", place.body);
            out->println();
            if (paddingBeforeEntries[index].isEmpty())
                continue;
            out->print("E\t", index);
            for (auto [entry, filling] : paddingBeforeEntries[index])
                out->print("\t", entry, "\t", filling);
            out->println();
        }
        dumpRegistersClobberedByStubs(*out);
        out->println("R\t", functionsEnd);
        unsigned numberOfLayoutsWithMessageData = 0;
        unsigned numberOfTypesDescribed = 0;
        if (!m_typedData.isEmpty()) {
            auto& typedData = *reinterpret_cast<const Typed::Data*>(m_typedData.span().data());
            Vector<uint32_t> typeOffsets(std::span { typedData.at<uint32_t>(typedData.typesOffset), typedData.numberOfTypes });
            std::ranges::sort(typeOffsets);
            for (size_t i = 0; i < typeOffsets.size(); ++i)
                numberOfTypesDescribed += typeOffsets[i] && (!i || typeOffsets[i] != typeOffsets[i - 1]);
            for (uint32_t offset : std::span { typedData.at<uint32_t>(typedData.layoutsOffset), typedData.numberOfLayouts })
                numberOfLayoutsWithMessageData += offset && typedData.at<Typed::LayoutData>(offset)->hasMessageData;
        }
        out->println("Z\t", m_typedData.size(), "\t", numberOfLayoutsWithMessageData, "\t", numberOfTypesDescribed);
        if (identifierIndices) {
            out->println("D\t", sizeof(Data), "\t", sizeof(Slot));
            Vector<std::pair<uint32_t, const StringImpl*>> identifiers;
            for (auto& entry : *identifierIndices)
                identifiers.append({ entry.value, entry.key });
            std::ranges::sort(identifiers);
            for (auto& [number, identifier] : identifiers)
                out->println("I\t", number, "\t", escapeForMapLine(identifier));
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
    Vector<uint32_t> faultingAccesses;
    Vector<uint32_t> faultingAccessThrowSites;
    Vector<uint32_t> wideFaultingAccessNames;
    uint32_t numberOfTrapsAtThrowSites = 0;
    uint32_t numberOfFaultingAccesses = 0;
    uint32_t numberOfFaultingAccessThrowSites = 0;
    Ref granuleAndFaultingAccessThread = Thread::create("AOT image"_s, [&] {
        {
            uint32_t function = 0;
            for (size_t start = 0; start < functionsEnd; start += 1 << codeGranuleShift) {
                while (functionStarts[function + 1] <= start)
                    ++function;
                codeGranules.append(function);
            }
        }
        for (unsigned index = 0; index < m_functions.size(); ++index) {
            auto& info = m_functions[index].code.info;
            RELEASE_ASSERT(FaultingAccessInImage::hasNoFrameBit || !info.hasNoFrame || info.faultingAccesses.isEmpty());
            RELEASE_ASSERT(!info.hasNoFrame || !info.calleeSaveRegisters.registerCount());
            for (auto& access : info.faultingAccesses) {
                if (access.isLoopPoll)
                    continue;
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
                    wideFaultingAccessNames.append(faultingAccesses.size());
                    wideFaultingAccessNames.append(access.throwSite[1]);
                }
                faultingAccesses.append(offset | (info.hasNoFrame ? FaultingAccessInImage::hasNoFrameBit : 0));
                faultingAccessThrowSites.append(word);
            }
        }
        numberOfFaultingAccesses = faultingAccesses.size();
        {
            Vector<uint32_t> checkpoints;
            Vector<uint8_t> steps;
            uint32_t lastOffset = 0;
            for (uint32_t i = 0; i < numberOfFaultingAccesses; ++i) {
                if (!(i % faultingAccessesBetweenCheckpoints))
                    checkpoints.appendList({ lastOffset, safeCast<uint32_t>(steps.size()) });
                uint32_t place = faultingAccesses[i] & ~FaultingAccessInImage::hasNoFrameBit;
                appendVarint(steps, static_cast<uint64_t>(place - lastOffset) << 1 | !!(faultingAccesses[i] & FaultingAccessInImage::hasNoFrameBit));
                lastOffset = place;
            }
            Vector<uint32_t> distinct;
            UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numberOfWord;
            Vector<uint32_t> numbers;
            for (uint32_t word : faultingAccessThrowSites) {
                auto added = numberOfWord.add(word, distinct.size());
                if (added.isNewEntry)
                    distinct.append(word);
                numbers.append(added.iterator->value);
            }
            numberOfFaultingAccessThrowSites = distinct.size();
            Vector<uint8_t> bytes;
            bytes.append(asBytes(checkpoints.span()));
            bytes.append(asBytes(distinct.span()));
            bytes.append(asBytes(wideFaultingAccessNames.span()));
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
    });
    endStep("granules and faulting accesses"_s);
    if (Options::verboseAOTCompilation()) [[unlikely]] {
        size_t hashCollisionCount = 0;
        for (size_t i = 0; i < m_regExps.size(); ++i) {
            auto hashAt = [&](size_t index) {
                return ImageRegExp::hashOf(m_regExps[index].pattern, m_regExps[index].flags);
            };
            hashCollisionCount += (i && hashAt(i - 1) == hashAt(i)) || (i + 1 < m_regExps.size() && hashAt(i + 1) == hashAt(i));
        }
        dataLogLn("AOT: regular expressions that have the hash of another: ", hashCollisionCount);
    }
    UncheckedKeyHashMap<const uint8_t*, size_t> regExpTables;
    Vector<std::array<size_t, 2>> regExpPlacement;
    UncheckedKeyHashMap<String, size_t> ownedRegExpTablesAt;
    Vector<std::pair<size_t, const Vector<uint8_t>*>> ownedRegExpTablesPlaced;
    struct RegExpBitPlaneTable {
        size_t at;
        uint8_t usedBits;
    };
    Vector<RegExpBitPlaneTable> regExpBitPlaneTables;
    Vector<std::pair<size_t, const Vector<uint8_t>*>> regExpBitPlanesPlaced;
    Vector<std::pair<size_t, Vector<uint8_t>>> regExpClassTablesPlaced;
    Vector<std::array<Vector<size_t>, 2>> ownedRegExpTableTargets;
    UncheckedKeyHashMap<String, size_t> regExpRoutinesAt;
    Vector<std::pair<size_t, const Vector<uint8_t>*>> regExpRoutinesPlaced;
    Vector<std::array<Vector<size_t>, 2>> regExpRoutineTargets;
    {
        size_t before = codeSize;
        for (auto& regExp : m_regExps) {
            for (auto& code : regExp.code) {
                for (auto& reference : code.tables) {
                    regExpTables.ensure(reference.table.data(), [&] {
                        codeSize = WTF::roundUpToMultipleOf<imageStubsAlignment>(codeSize);
                        size_t at = codeSize;
                        codeSize += reference.table.size();
                        if (map) [[unlikely]]
                            map->println("XT\t", at, "\t", reference.table.size());
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
        std::pair<unsigned, size_t> firstBodyAfterPackedClassTables { 0, 0 };
        auto packClassTablesForNextBodies = [&] {
            constexpr size_t plainAddressReach = isX86_64() ? std::numeric_limits<size_t>::max() : MB - 64 * KB;
            Yarr::ClassTablePacker packer;
            Vector<std::pair<String, size_t>> packedTables;
            UncheckedKeyHashSet<String> seenTables;
            size_t bytesUpToEndOfBody = 0;
            auto [i, number] = firstBodyAfterPackedClassTables;
            bool isFirstBody = true;
            for (; i < 2; ++i, number = 0) {
                for (; number < m_regExps.size(); ++number) {
                    auto& body = m_regExps[number].code[i];
                    size_t bodyBytes = body.bytes.size() + imageStubsAlignment;
                    BitVector countedTables;
                    for (auto& reference : body.ownedTableReferences) {
                        if (countedTables.set(reference.table))
                            continue;
                        auto& table = body.ownedTables[reference.table];
                        String content { byteCast<Latin1Character>(table.span()) };
                        if (seenTables.contains(content) || (reference.isFar && ownedRegExpTablesAt.contains(content)))
                            continue;
                        bool isPacked = reference.kind == Yarr::YarrCodeForImage::OwnedTableKind::ClassTable || reference.kind == Yarr::YarrCodeForImage::OwnedTableKind::ClassTableForRoutine;
                        bodyBytes += (isPacked ? table.size() / 2 : table.size()) + sizeof(uint64_t);
                    }
                    for (auto& routine : body.routines)
                        bodyBytes += routine.size() + imageStubsAlignment;
                    if (bytesUpToEndOfBody + bodyBytes > plainAddressReach && !isFirstBody)
                        break;
                    bytesUpToEndOfBody += bodyBytes;
                    isFirstBody = false;
                    for (auto& reference : body.ownedTableReferences) {
                        using Kind = Yarr::YarrCodeForImage::OwnedTableKind;
                        auto& table = body.ownedTables[reference.table];
                        String content { byteCast<Latin1Character>(table.span()) };
                        if (reference.isFar && ownedRegExpTablesAt.contains(content))
                            continue;
                        if (!seenTables.add(content).isNewEntry)
                            continue;
                        if (reference.kind != Kind::ClassTable && reference.kind != Kind::ClassTableForRoutine)
                            continue;
                        size_t packed = packer.add(table.span(), reference.kind == Kind::ClassTable ? Yarr::ClassTablePacker::Layout::IndexThenBlocks : Yarr::ClassTablePacker::Layout::ForRoutine);
                        if (packed != Yarr::ClassTablePacker::notFound)
                            packedTables.append({ content, packed });
                    }
                }
                if (number < m_regExps.size())
                    break;
            }
            firstBodyAfterPackedClassTables = { i, number };
            if (packedTables.isEmpty())
                return;
            packer.pack();
            codeSize = WTF::roundUpToMultipleOf<8>(codeSize);
            for (auto& [content, packed] : packedTables)
                ownedRegExpTablesAt.set(content, codeSize + packer.offsetOf(packed));
            if (map) [[unlikely]]
                map->println("XT\t", codeSize, "\t", packer.bytes().size());
            regExpClassTablesPlaced.append({ codeSize, packer.bytes() });
            codeSize += packer.bytes().size();
        };
        for (unsigned i = 0; i < 2; ++i) {
            for (size_t number = 0; number < m_regExps.size(); ++number) {
                auto& regExp = m_regExps[number];
                auto& at = regExpPlacement[number];
                auto& targets = ownedRegExpTableTargets[number];
                auto& routineTargets = regExpRoutineTargets[number];
                if (firstBodyAfterPackedClassTables == std::pair { i, number })
                    packClassTablesForNextBodies();
                if (!regExp.code[i].bytes.isEmpty()) {
                    auto& body = regExp.code[i];
                    auto placed = regExpBodiesPlaced.add(regExpBodyKeys[number][i], std::pair<size_t, unsigned> { number, i });
                    if (!placed.isNewEntry) {
                        auto [index, width] = placed.iterator->value;
                        at[i] = regExpPlacement[index][width];
                        targets[i] = ownedRegExpTableTargets[index][width];
                        routineTargets[i] = regExpRoutineTargets[index][width];
                        if (map) [[unlikely]]
                            map->println("XD\t", at[i], "\t", body.bytes.size(), "\t", i ? 16 : 8, "\t", regExp.flags.toRaw(), "\t", escapeForMapLine(regExp.pattern));
                        continue;
                    }
                }
                constexpr size_t regExpBodyAlignment = isX86_64() ? 1 : 4;
                codeSize = WTF::roundUpToMultipleOf<regExpBodyAlignment>(codeSize);
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
                    if (reference.kind == Yarr::YarrCodeForImage::OwnedTableKind::BitPlane) {
                        uint8_t bit = 0;
                        for (uint8_t entry : table)
                            bit |= entry;
                        RELEASE_ASSERT(hasOneBitSet(bit));
                        RegExpBitPlaneTable* shared = nullptr;
                        for (size_t k = regExpBitPlaneTables.size(); k-- && !shared;) {
                            auto& candidate = regExpBitPlaneTables[k];
#if !CPU(X86_64)
                            if (!reference.isFar && instructionAt - candidate.at >= (1 << 20) - 16)
                                break;
#endif
                            if (!(candidate.usedBits & bit))
                                shared = &candidate;
                        }
                        if (shared) {
                            shared->usedBits |= bit;
                            ownedRegExpTablesAt.set(content, shared->at);
                            regExpBitPlanesPlaced.append({ shared->at, &table });
                            targets[i].append(shared->at);
                            continue;
                        }
                        codeSize = WTF::roundUpToMultipleOf<8>(codeSize);
                        regExpBitPlaneTables.append({ codeSize, bit });
                        ownedRegExpTablesAt.set(content, codeSize);
                        regExpBitPlanesPlaced.append({ codeSize, &table });
                        if (map) [[unlikely]]
                            map->println("XT\t", codeSize, "\t", table.size());
                        targets[i].append(codeSize);
                        codeSize += table.size();
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
                    map->println("X\t", at[i], "\t", regExp.code[i].bytes.size(), "\t", i ? 16 : 8, "\t", regExp.flags.toRaw(), "\t", escapeForMapLine(regExp.pattern));
            }
        }
        for (size_t number = 0; number < m_regExps.size(); ++number) {
            imageRegExps[number].codeFor8Bit = safeCast<uint32_t>(regExpPlacement[number][0]);
            imageRegExps[number].codeFor16Bit = safeCast<uint32_t>(regExpPlacement[number][1]);
        }
    }
    size_t webAssemblyCodeAt = 0;
    if (!m_webAssemblyCode.isEmpty()) {
        codeSize = WTF::roundUpToMultipleOf<16>(codeSize);
        webAssemblyCodeAt = codeSize;
        codeSize += m_webAssemblyCode.size();
    }
    granuleAndFaultingAccessThread->waitForCompletion();
    endStep("placing the regular expressions"_s);
    RELEASE_ASSERT(recordsSize < std::numeric_limits<uint32_t>::max());

    ImageHeader header { };
    header.magic = imageMagic;
    header.stamp = imageStamp();
    header.tableOffset = sizeof(ImageHeader);
    endStep("the quotes"_s);
    constexpr size_t compressionBlockSize = 64 * KB;
    uint32_t quoteStringBlockSize = 0;
    if (auto packed = compressInBlocks(quotedSourceBytes.span(), compressionBlockSize)) {
        quotedSourceBytes = WTF::move(*packed);
        quoteStringBlockSize = compressionBlockSize;
    }
    uint32_t quoteBlockSize = 0;
    if (auto packed = compressInBlocks(quotes.span(), compressionBlockSize)) {
        quotes = WTF::move(*packed);
        quoteBlockSize = compressionBlockSize;
    }

    endStep("compressing the quotes"_s);
    waitForCallSitesIfFinishFails.release();
    callSitesGatherer->waitForCompletion();
    endStep("the call sites of each function"_s);
    endStep("the call sites together"_s);
    bool keysAreOmitted = !!m_numberOfProgramIdentifiers;
    header.tableCapacity = keysAreOmitted ? 0 : capacity;
    header.recordsOffset = header.tableOffset + header.tableCapacity * sizeof(ImageKey);
    header.recordsSize = recordsSize;
    size_t totalDataSize = 0;
    auto dataSizes = Typed::collectInParallel<size_t>(m_functions.size(), [&](unsigned index, Vector<size_t>& sums) {
        if (sums.isEmpty())
            sums.append(0);
        sums[0] += roundUpToMultipleOf<16>(Data::allocationSize(m_functions[index].code.info.numSlots, true));
    });
    for (size_t size : dataSizes)
        totalDataSize += size;
    header.totalDataSizeIn16Bytes = safeCast<uint32_t>(totalDataSize / 16);
    RELEASE_ASSERT_WITH_MESSAGE(!m_typedData.size() || sizeof(Instance::NegativeOffsetFields) + m_environmentsSize <= Instance::bytesReservedForEnvironments - Instance::bytesReservedForLoopPollPage, "AOT: the program has more environments than there is room for below the instance");
    RELEASE_ASSERT_WITH_MESSAGE(!m_typedData.size() || reinterpret_cast<const Typed::Data*>(m_typedData.span().data())->numberOfStructureDescriptors <= Instance::maxStructureDescriptors, "AOT: the program has more rows than there is room for below the instance");
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
    header.quoteStringsOffset = place(quotedSourceBytes.size());
    header.quoteStringBlockSize = quoteStringBlockSize;
    header.quoteBlockSize = quoteBlockSize;
    header.numberOfProgramIdentifiers = m_numberOfProgramIdentifiers;
    header.numberOfProgramConstants = m_numberOfProgramConstants;
    header.regExpsOffset = place(imageRegExps.sizeInBytes());
    header.numberOfRegExps = imageRegExps.size();
    header.regExpSourceOffset = place(regExpSourceBytes.size());
    header.functionStartsOffset = place(functionStarts.sizeInBytes());
    header.codeGranulesOffset = place(codeGranules.sizeInBytes());
    header.codeGranuleShift = codeGranuleShift;
    header.callSitesOffset = place(callSites.size());
    header.faultingAccessesOffset = place(faultingAccesses.sizeInBytes());
    header.numberOfFaultingAccesses = numberOfFaultingAccesses;
    header.numberOfFaultingAccessesWithWideName = wideFaultingAccessNames.size() / 2;
    header.numberOfFaultingAccessThrowSites = numberOfFaultingAccessThrowSites;
    header.numberOfTrapsAtThrowSites = numberOfTrapsAtThrowSites;
    header.framesOffset = place(frames.sizeInBytes());
    header.typedDataOffset = place(m_typedData.size());
    header.typedDataSize = safeCast<uint32_t>(m_typedData.size());
    header.webAssemblyDirectoryOffset = m_webAssemblyDirectory.isEmpty() ? 0 : place(m_webAssemblyDirectory.size());
    header.webAssemblyCodeOffset = safeCast<uint32_t>(webAssemblyCodeAt);
    for (size_t index = 0; index < imageRegExps.size(); ++index)
        imageRegExps[index].text = safeCast<uint32_t>(static_cast<uint64_t>(imageRegExps[index].text) + (regExpSourceIsInTypedData.get(index) ? header.typedDataOffset : header.regExpSourceOffset));
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
    RELEASE_ASSERT(stubs.returnsFromFindingTargets.size() == maxReturnsIntoHostFunctionFrames);
    for (unsigned i = 0; i < maxReturnsIntoHostFunctionFrames; ++i)
        header.returnsFromFindingTargets[i] = stubs.returnsFromFindingTargets[i];
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
    memcpy(base + header.quoteStringsOffset, quotedSourceBytes.span().data(), quotedSourceBytes.size());
    memcpy(base + header.regExpsOffset, imageRegExps.span().data(), imageRegExps.sizeInBytes());
    memcpy(base + header.regExpSourceOffset, regExpSourceBytes.span().data(), regExpSourceBytes.size());
    memcpy(base + header.functionStartsOffset, functionStarts.span().data(), functionStarts.sizeInBytes());
    memcpy(base + header.codeGranulesOffset, codeGranules.span().data(), codeGranules.sizeInBytes());
    memcpy(base + header.callSitesOffset, callSites.span().data(), callSites.size());
    memcpy(base + header.faultingAccessesOffset, faultingAccesses.span().data(), faultingAccesses.sizeInBytes());
    memcpy(base + header.framesOffset, frames.span().data(), frames.sizeInBytes());
    memcpy(base + header.typedDataOffset, m_typedData.span().data(), m_typedData.size());
    if (!m_webAssemblyDirectory.isEmpty()) {
        memcpy(base + header.webAssemblyDirectoryOffset, m_webAssemblyDirectory.span().data(), m_webAssemblyDirectory.size());
        memcpy(code + webAssemblyCodeAt, m_webAssemblyCode.span().data(), m_webAssemblyCode.size());
    }
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
                constexpr size_t instructionSize = 7;
                int64_t distance = static_cast<int64_t>(tableAt) - static_cast<int64_t>(instructionAt + instructionSize);
                RELEASE_ASSERT(distance == static_cast<int32_t>(distance));
                int32_t displacement = static_cast<int32_t>(distance);
                memcpy(code + instructionAt + instructionSize - sizeof(displacement), &displacement, sizeof(displacement));
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
                RELEASE_ASSERT(regExpCode.routineCalls[k].offset + sizeOfCall <= regExpCode.bytes.size() && code[instructionAt] == (regExpCode.routineCalls[k].isTailCall ? 0xe9 : 0xe8));
                int64_t distance = static_cast<int64_t>(routineAt) - static_cast<int64_t>(instructionAt + sizeOfCall);
                RELEASE_ASSERT(distance == static_cast<int32_t>(distance));
                int32_t displacement = static_cast<int32_t>(distance);
                memcpy(code + instructionAt + sizeOfCall - sizeof(displacement), &displacement, sizeof(displacement));
#else
                RELEASE_ASSERT(regExpCode.routineCalls[k].offset + sizeof(uint32_t) <= regExpCode.bytes.size());
                int64_t delta = static_cast<int64_t>(routineAt) - static_cast<int64_t>(instructionAt);
                RELEASE_ASSERT(!(delta & 3) && delta >= -(1 << 27) && delta < (1 << 27));
                uint32_t call = (regExpCode.routineCalls[k].isTailCall ? 0x14000000u : 0x94000000u) | (static_cast<uint32_t>(delta >> 2) & 0x3ffffffu);
                memcpy(code + instructionAt, &call, sizeof(call));
#endif
            }
            for (auto& reference : regExpCode.tables) {
                size_t instructionAt = codeAt + reference.offset;
                size_t tableAt = regExpTables.get(reference.table.data());
#if CPU(X86_64)
                constexpr size_t instructionSize = 7;
                int64_t distance = static_cast<int64_t>(tableAt) - static_cast<int64_t>(instructionAt + instructionSize);
                RELEASE_ASSERT(distance == static_cast<int32_t>(distance));
                int32_t displacement = static_cast<int32_t>(distance);
                memcpy(code + instructionAt + instructionSize - sizeof(displacement), &displacement, sizeof(displacement));
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
    for (auto& [at, bytes] : regExpClassTablesPlaced)
        memcpy(code + at, bytes.span().data(), bytes.size());
    for (auto& table : regExpBitPlaneTables)
        memset(code + table.at, 0, Yarr::CharacterClass::latin1TableSize);
    for (auto& [at, table] : regExpBitPlanesPlaced) {
        RELEASE_ASSERT(table->size() == Yarr::CharacterClass::latin1TableSize);
        for (size_t k = 0; k < table->size(); ++k)
            code[at + k] |= table->at(k);
    }
    for (auto& [at, routine] : regExpRoutinesPlaced)
        memcpy(code + at, routine->span().data(), routine->size());
    for (size_t at : stubsAt)
        memcpy(code + at, stubs.bytes.span().data(), stubs.bytes.size());
    for (size_t at : stubsAt) {
        if (argumentThunks.bytes.isEmpty())
            break;
        memcpy(code + at + argumentThunksAt, argumentThunks.bytes.span().data(), argumentThunks.bytes.size());
        for (auto& jump : argumentThunks.jumps) {
            RELEASE_ASSERT(!isGeneratedOnlyIfCalled(jump.stub) || calledStubs.get(static_cast<unsigned>(jump.stub)));
            retargetStubCall(code, at + argumentThunksAt + jump.offset, at + (jump.thunk ? offsetOfThunk(jump.thunk - 1) : stubs.offsets[static_cast<unsigned>(jump.stub)]), true);
        }
    }
    for (size_t at : stubsAt) {
        if (sharedBodies.bytes.isEmpty())
            break;
        memcpy(code + at + sharedBodiesAt, sharedBodies.bytes.span().data(), sharedBodies.bytes.size());
        for (auto& jump : sharedBodies.jumps) {
            if (jump.offset != OutlinedBodies::noJump)
                retargetStubCall(code, at + sharedBodiesAt + jump.offset, bodyJumpTarget(jump, at), true);
        }
    }

    endStep("copying the regular expressions and the stubs"_s);
    BitVector isDuplicate(m_functions.size());
    uint32_t numberOfStates = 1;
    for (size_t index = 0; index < m_functions.size(); ++index) {
        auto& function = m_functions[index];
        auto& info = function.code.info;
        if (hasSharedKey.quickGet(index) && indexOfFunctionWithKey(function.key) != index) {
            isDuplicate.quickSet(index);
            continue;
        }
        if (info.numSlots || !info.indexReferences.isEmpty() || !(function.key.kind & ImageKey::typedFunction))
            info.stateNumber = numberOfStates++;
    }
    Ref recordWriterThread = Thread::create("AOT image"_s, [&] {
        Vector<uint32_t> functionInBucket;
        functionInBucket.fill(0, capacity);
        size_t recordAt = 0;
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
                m_failureReason = makeString("the program is too large: it has more functions, kinds of stack frame, call sites or inline cache slots than an executable can describe (function "_s, index, ": frame "_s, functionFrame[index], ", call sites from "_s, functionCallSites[index], ", "_s, info.numSlots, " slots)"_s);
                return;
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
            record.hasValueEntry = info.valueEntry != StubCall::noFunction;
            record.hasCompatibleResultsMask = !!info.compatibleResultsMask;
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
                RELEASE_ASSERT(isDuplicate.quickGet(index));
                continue;
            }
            RELEASE_ASSERT(!isDuplicate.quickGet(index));
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
        for (auto& entry : entriesInsideFunctions) {
            uint32_t function = ImageFunction::noSuchFunction;
            for (unsigned bucket = entry.function.hash() & (capacity - 1); functionWithKey[bucket] != std::numeric_limits<uint32_t>::max(); bucket = (bucket + 1) & (capacity - 1)) {
                if (functionKeys[functionWithKey[bucket]].sameFunction(entry.function)) {
                    function = functionWithKey[bucket];
                    break;
                }
            }
            RELEASE_ASSERT(function != ImageFunction::noSuchFunction);
            size_t at = placement[function].first + entry.offset + entry.precedingWordSize;
            bool hasCompatibleResultsMask = entry.precedingWordSize || (entry.startHasCompatibleResultsMask && m_functions[function].code.info.compatibleResultsMask);
            RELEASE_ASSERT(at && at < ImageKey::isEntryInsideFunction);
            RELEASE_ASSERT(!(at % imageFunctionAlignment));
            static_assert(hasOneBitSet(imageFunctionAlignment));
            static_assert(GetterSetter::getterShortcutIsCode < imageFunctionAlignment);
            ImageKey key { entry.convention.numberOfParameters | (entry.convention.signature == Signature::List ? 1u << 8 : 0u) | (hasCompatibleResultsMask ? 1u << 9 : 0u), entry.key.start, entry.key.kind, static_cast<uint32_t>(at) | ImageKey::isEntryInsideFunction };
            unsigned bucket = key.hash() & (capacity - 1);
            while (table[bucket].record)
                bucket = (bucket + 1) & (capacity - 1);
            table[bucket] = key;
        }
    });
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
                uint32_t indexWithinKind = bodyIndexWithinKind[call.body - 1];
                retargetStubCall(code, codeAt + call.offset, outlining.bodies[call.body - 1].singleCaller == Outlining::none ? stubsForFunction + sharedBodiesAt + sharedBodies.offsets[indexWithinKind] : ownBodyOffsets[indexWithinKind], false);
                continue;
            }
            RELEASE_ASSERT(!isGeneratedOnlyIfCalled(call.stub) || calledStubs.get(static_cast<unsigned>(call.stub)));
            size_t target = call.thunk ? offsetOfThunk(call.thunk - 1) : stubs.offsets[static_cast<unsigned>(call.stub)];
            if (call.argumentMoves != StubCall::noArgumentMoves)
                target = argumentThunksAt + argumentThunks.offsets[numberOfArgumentMoves(info.argumentMoves[call.argumentMoves])];
            retargetStubCall(code, codeAt + call.offset, stubsForFunction + target, call.isTailCall);
        }
        for (uint32_t number : ownBodiesOf[index]) {
            size_t at = ownBodyOffsets[number];
            uint32_t offset = ownBodies.offsets[number];
            auto& jump = ownBodies.jumps[number];
            memcpy(code + at, ownBodies.bytes.span().data() + offset, ownBodies.sizeOf(number));
            if (jump.offset != OutlinedBodies::noJump)
                retargetStubCall(code, at + jump.offset - offset, bodyJumpTarget(jump, stubsForFunction), true);
        }
    });

    Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        auto& info = m_functions[index].code.info;
        size_t codeAt = placement[index].first;
        writeOmittedArguments(code, codeAt - preambleSize(index), omittedArgumentsOf[index][0], omittedArgumentsOf[index][1]);
        memcpy(code + codeAt - preambles[index].size(), preambles[index].span().data(), preambles[index].size());
        if (info.valueEntry != StubCall::noFunction) {
            RELEASE_ASSERT(!omittedArgumentsOf[index][1]);
            uint32_t target = calleesOf[index][info.valueEntry];
            size_t offset = 0;
            if (target == ImageFunction::noSuchFunction) {
                auto& entry = entriesInsideFunctions[typedEntriesInsideFunctions.get(info.knownCallees[info.valueEntry].start)];
                RELEASE_ASSERT(entry.key.sameFunction(info.knownCallees[info.valueEntry]));
                target = indexOfFunctionWithKey(entry.function);
                offset = entry.offset;
            }
            RELEASE_ASSERT(target != ImageFunction::noSuchFunction);
            std::array<uint32_t, 2> words { static_cast<uint32_t>(safeCast<int32_t>(static_cast<int64_t>(placement[target].first + offset) - static_cast<int64_t>(codeAt))), info.valueEntrySignatureIndex };
            memcpy(code + codeAt - sizeof(words), words.data(), sizeof(words));
        }
        for (auto& call : info.stubCalls) {
            if (call.function == StubCall::noFunction)
                continue;
            if (auto target = directTargetOf(index, call)) {
                size_t targetAt = placement[*target].first - preambleSizeForCall(*target, call);
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
    recordWriterThread->waitForCompletion();
    if (!m_failureReason.isNull())
        return { };
    endStep("the records of the functions"_s);
    endStep("the records of entries inside functions, and the states"_s);
    m_reportableSites = reportableSites();
    endStep("the reportable sites, and emptying the functions"_s);
    Typed::destroyOnHelperThread(std::tuple { std::exchange(m_functions, { }), WTF::move(calleesOf), WTF::move(farCallees), WTF::move(outlining) });
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

static bool reservesInaccessibleFirstPages()
{
    static bool s_reservesThem;
    if (lowestAccessibleAddress() >= FaultingAccessInImage::reach || s_reservesThem)
        return true;
#if OS(LINUX) && defined(MAP_FIXED_NOREPLACE)
    size_t pageBytes = pageSize();
    for (uintptr_t page = 0; page < FaultingAccessInImage::reach; page += pageBytes) {
        void* mapped = mmap(std::bit_cast<void*>(page), pageBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        bool zeroPageWasRelocated = !page && mapped != MAP_FAILED && mapped;
        if (zeroPageWasRelocated)
            munmap(mapped, pageBytes);
        if (mapped == MAP_FAILED || zeroPageWasRelocated) {
            if (!zeroPageWasRelocated && errno != EPERM && errno != EACCES)
                return false;
            unsigned char isResident;
            if (mincore(std::bit_cast<void*>(page), pageBytes, &isResident) != -1 || errno != ENOMEM)
                return false;
            continue;
        }
        if (mapped != std::bit_cast<void*>(page)) {
            munmap(mapped, pageBytes);
            return false;
        }
    }
    s_reservesThem = true;
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
#if ENABLE(WEBASSEMBLY_OMGJIT)
    if (header.webAssemblyDirectoryOffset)
        Wasm::CodeInImage::setRange(static_cast<const uint8_t*>(image->m_code) + header.webAssemblyCodeOffset, header.codeSize - header.webAssemblyCodeOffset);
#endif
#if CPU(X86_64) && OS(LINUX)
    bool mayTrapAtPrologues = header.codeSize;
#else
    bool mayTrapAtPrologues = false;
#endif
    if (header.numberOfFaultingAccesses || header.numberOfTrapsAtThrowSites || mayTrapAtPrologues) {
        RELEASE_ASSERT(!header.numberOfFaultingAccesses || reservesInaccessibleFirstPages(), lowestAccessibleAddress());
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
        return Options::useAOT();
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

std::pair<const uint8_t*, const uint8_t*> Image::webAssembly()
{
    Image* image = withCode();
    if (!image || !image->header().webAssemblyDirectoryOffset)
        return { nullptr, nullptr };
    return { image->at<uint8_t>(image->header().webAssemblyDirectoryOffset), static_cast<const uint8_t*>(image->m_code) + image->header().webAssemblyCodeOffset };
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
        RELEASE_ASSERT(image->header().functionsEnd < ReturnAddressTrace::isConstructedRoot);
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
        for (uint32_t afterCall : header.returnsFromFindingTargets) {
            if (afterCall == inStubs)
                return { ImageAddressInfo::Stub, ImageAddressInfo::frameMayBeHostFunction, 0 };
        }
        return { ImageAddressInfo::Stub, 0, 0 };
    }
    const uint32_t* starts = s_functionStarts;
    uint32_t index = s_codeGranules[offset >> header.codeGranuleShift];
    while (starts[index + 1] <= offset)
        ++index;
    return { ImageAddressInfo::Function, index, static_cast<uint32_t>(offset - starts[index]) };
}

ImageAddressInfo::Kind addressKind(const void* address)
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
    std::span words { s_faultingAccesses + 2 * checkpoints.size(), header.numberOfFaultingAccessThrowSites };
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
    RELEASE_ASSERT(number == 16 || number == 30 || number == static_cast<unsigned>(cellCageBaseGPR));
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

static uintptr_t cellCageBaseIn(PlatformRegisters& registers)
{
#if CPU(ARM64)
    return registerOf(registers, static_cast<unsigned>(cellCageBaseGPR));
#elif CPU(X86_64)
    static_assert(cellCageBaseGPR == X86Registers::r14);
#if OS(DARWIN)
    return registers.__r14;
#elif OS(WINDOWS)
    return registers.R14;
#elif OS(FREEBSD)
    return registers.machineContext.mc_r14;
#else
    return registers.machineContext.gregs[REG_R14];
#endif
#else
    UNUSED_PARAM(registers);
    return MarkedBlockCage::base();
#endif
}

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

static bool isTrapAtPrologue(void* instruction)
{
    if (!s_imageHeader || std::bit_cast<uintptr_t>(instruction) - s_imageCode >= s_imageHeader->functionsEnd || *static_cast<const uint8_t*>(instruction) != trapInstruction)
        return false;
    ImageAddressInfo place = classifyAddress(instruction);
    return place.kind == ImageAddressInfo::Function && afterStackCheckBranchingTo(static_cast<uint8_t*>(instruction) - place.offset, static_cast<uint8_t*>(instruction));
}

static bool isLoopPoll(const void* instruction, const void* faultingAddress, PlatformRegisters& registers, bool hasNoFrame)
{
    static_assert(instanceGPR == X86Registers::r13 && CCallHelpers::s_scratchRegister == X86Registers::r11);
    if (!s_imageHeader || std::bit_cast<uintptr_t>(instruction) - s_imageCode >= s_imageHeader->functionsEnd)
        return false;
    int32_t displacement = static_cast<int32_t>(Instance::offsetOfLoopPollPage() + (hasNoFrame ? offsetOfLeafPollInLoopPollPage : 0));
    if (std::bit_cast<uintptr_t>(faultingAddress) != static_cast<uintptr_t>(registers.machineContext.gregs[REG_R13]) + displacement)
        return false;
    std::array<uint8_t, sizeOfLoopPoll> poll { 0x45, 0x8b, 0x9d, static_cast<uint8_t>(displacement), static_cast<uint8_t>(displacement >> 8), static_cast<uint8_t>(displacement >> 16), static_cast<uint8_t>(displacement >> 24) };
    return !memcmp(instruction, poll.data(), poll.size());
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
        if (isTrapAtPrologue(instructionPointer->untaggedPtr())) {
            void*& stackPointer = MachineContext::stackPointerImpl(registers);
            auto* returnAddress = static_cast<uintptr_t*>(stackPointer) - 1;
            *returnAddress = std::bit_cast<uintptr_t>(instructionPointer->untaggedPtr()) + sizeOfCallAtPrologue;
            stackPointer = returnAddress;
            MachineContext::setInstructionPointer(registers, std::bit_cast<void*>(s_imageCode + s_imageHeader->stubOffsets[static_cast<unsigned>(Stub::PollAtPrologue)]));
            return SignalAction::Handled;
        }
        if (isLoopPoll(instructionPointer->untaggedPtr(), info.faultingAddress, registers, true)) {
            void*& stackPointer = MachineContext::stackPointerImpl(registers);
            void*& framePointer = MachineContext::framePointerImpl(registers);
            auto* frame = static_cast<uintptr_t*>(stackPointer) - 1;
            *frame = std::bit_cast<uintptr_t>(framePointer);
            framePointer = frame;
            frame[-1] = std::bit_cast<uintptr_t>(instructionPointer->untaggedPtr()) + sizeOfLoopPoll;
            stackPointer = frame - 1;
            MachineContext::setInstructionPointer(registers, std::bit_cast<void*>(s_imageCode + s_imageHeader->stubOffsets[static_cast<unsigned>(Stub::LeafPollAtLoop)]));
            return SignalAction::Handled;
        }
        if (isLoopPoll(instructionPointer->untaggedPtr(), info.faultingAddress, registers, false)) {
            void*& stackPointer = MachineContext::stackPointerImpl(registers);
            auto* returnAddress = static_cast<uintptr_t*>(stackPointer) - 1;
            *returnAddress = std::bit_cast<uintptr_t>(instructionPointer->untaggedPtr()) + sizeOfLoopPoll;
            stackPointer = returnAddress;
            MachineContext::setInstructionPointer(registers, std::bit_cast<void*>(s_imageCode + s_imageHeader->stubOffsets[static_cast<unsigned>(Stub::PollAtLoop)]));
            return SignalAction::Handled;
        }
#endif
        uintptr_t address = std::bit_cast<uintptr_t>(info.faultingAddress);
        if (address >= FaultingAccessInImage::reach && address - cellCageBaseIn(registers) >= FaultingAccessInImage::reach)
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

uintptr_t imageCodeStart()
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

void InlineFrameReader::read(const ImageFunction& function, unsigned last)
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

SiteFrames::SiteFrames(const ImageFunction& function)
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
    m_sitesInFunction = withoutCode - m_inlinedCalls;
    m_withCode = first >> 3;
    m_sites = at + SiteCheckpoint::numberFor(m_withCode) * sizeof(SiteCheckpoint);
}

unsigned SiteFrames::next()
{
    if (m_sitesInFunction) {
        --m_sitesInFunction;
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

void SiteFrames::skip(uint64_t numberOfSites)
{
    uint64_t sitesInFunction = std::min(numberOfSites, m_sitesInFunction);
    m_sitesInFunction -= sitesInFunction;
    numberOfSites -= sitesInFunction;
    for (; numberOfSites && m_inlinedCalls; --numberOfSites)
        next();
    uint64_t numberOfCheckpoints = SiteCheckpoint::numberFor(m_withCode);
    if (uint64_t precedingCount = std::min(numberOfSites / SiteCheckpoint::sitesBetween, numberOfCheckpoints)) {
        SiteCheckpoint checkpoint = SiteCheckpoint::at(m_sites - numberOfCheckpoints * sizeof(SiteCheckpoint), precedingCount - 1);
        m_sites += checkpoint.positionAndFrame >> 8;
        m_frame = checkpoint.positionAndFrame & 0xff;
        m_withCode -= precedingCount * SiteCheckpoint::sitesBetween;
        numberOfSites -= precedingCount * SiteCheckpoint::sitesBetween;
    }
    for (numberOfSites = std::min(numberOfSites, m_withCode); numberOfSites; --numberOfSites)
        next();
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
            uint64_t precedingCount = 0;
            for (uint64_t end = numberOfCheckpoints; precedingCount < end;) {
                uint64_t middle = precedingCount + (end - precedingCount) / 2;
                if (SiteCheckpoint::at(checkpoints, middle).offset * codeOffsetUnit < offsetOfReturnAddress)
                    precedingCount = middle + 1;
                else
                    end = middle;
            }
            if (precedingCount) {
                SiteCheckpoint checkpoint = SiteCheckpoint::at(checkpoints, precedingCount - 1);
                offset = checkpoint.offset * codeOffsetUnit;
                frame = checkpoint.positionAndFrame & 0xff;
                at += checkpoint.positionAndFrame >> 8;
                number += precedingCount * SiteCheckpoint::sitesBetween;
                count -= precedingCount * SiteCheckpoint::sitesBetween;
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

template<typename Functor>
static void forEachSourcePiece(const uint8_t* image, const ImageHeader& header, const ImageRegExp& regExp, const Functor& functor)
{
    auto* list = reinterpret_cast<const uint32_t*>(image + regExp.text);
    const uint8_t* texts = image + header.regExpSourceOffset;
    for (auto& piece : std::span { reinterpret_cast<const SourcePiece*>(list + 1), *list }) {
        if (!functor(regExp.is8Bit ? StringView { std::span { reinterpret_cast<const Latin1Character*>(texts + piece.offset), piece.length } } : StringView { std::span { reinterpret_cast<const char16_t*>(texts + piece.offset), piece.length } }))
            return;
    }
}

auto Image::codeForRegExp(const String& pattern, OptionSet<Yarr::Flags> flags, bool wasFoundBefore) -> std::optional<CodeForRegExp>
{
    if (!hasAny())
        return std::nullopt;
    uint32_t hash = ImageRegExp::hashOf(pattern, flags);
    uint32_t rawFlags = ImageRegExp::significantFlags(flags).toRaw();
    std::optional<Typed::HashedString::Digest> digest;
    auto& all = registry();
    Locker locker { all.lock };
    bool isTheOneFoundBefore = false;
    if (wasFoundBefore) {
        unsigned numberOfCandidates = 0;
        for (Image* image : all.images) {
            std::span regExps { image->at<ImageRegExp>(image->header().regExpsOffset), image->header().numberOfRegExps };
            for (auto it = std::ranges::lower_bound(regExps, hash, { }, &ImageRegExp::hash); it != regExps.end() && it->hash == hash; ++it)
                numberOfCandidates += it->flags == rawFlags && it->length == pattern.length();
        }
        isTheOneFoundBefore = numberOfCandidates == 1;
    }
    for (Image* image : all.images) {
        auto& header = image->header();
        std::span regExps { image->at<ImageRegExp>(header.regExpsOffset), header.numberOfRegExps };
        auto* text = image->at<uint8_t>(0);
        for (auto it = std::ranges::lower_bound(regExps, hash, { }, &ImageRegExp::hash); it != regExps.end() && it->hash == hash; ++it) {
            if (it->flags != rawFlags || it->length != pattern.length())
                continue;
            if (it->sourceIsDigest) {
                if (!digest && !isTheOneFoundBefore)
                    digest = Typed::HashedString::digestOfNarrowestEncoding(pattern);
                if (isTheOneFoundBefore || equalSpans(std::span<const uint8_t> { *digest }, std::span { text + it->text, digest->size() }))
                    return CodeForRegExp { static_cast<const uint8_t*>(image->m_code) + it->codeFor8Bit, static_cast<const uint8_t*>(image->m_code) + it->codeFor16Bit, it->numSubpatterns, it->minimumSize, static_cast<Yarr::SpecificPattern>(it->specificPattern), !!it->retainsPatternDetails, !it->hasNoCode, !!it->sourceIsPattern, { }, nullptr, !isTheOneFoundBefore };
                continue;
            }
            if (it->sourceIsInPieces) {
                unsigned position = 0;
                bool isSame = true;
                forEachSourcePiece(text, header, *it, [&](StringView piece) {
                    if (isTheOneFoundBefore)
                        return false;
                    isSame = StringView(pattern).substring(position, piece.length()) == piece;
                    position += piece.length();
                    return isSame;
                });
                if (isSame)
                    return CodeForRegExp { static_cast<const uint8_t*>(image->m_code) + it->codeFor8Bit, static_cast<const uint8_t*>(image->m_code) + it->codeFor16Bit, it->numSubpatterns, it->minimumSize, static_cast<Yarr::SpecificPattern>(it->specificPattern), !!it->retainsPatternDetails, !it->hasNoCode, !!it->sourceIsPattern, { }, std::to_address(it) };
                continue;
            }
            StringView said = it->is8Bit ? StringView { std::span { reinterpret_cast<const Latin1Character*>(text + it->text), it->length } } : StringView { std::span { reinterpret_cast<const char16_t*>(text + it->text), it->length } };
            if (isTheOneFoundBefore || said.rawCharacters() == StringView(pattern).rawCharacters() || (said.is8Bit() == pattern.is8Bit() ? !it->length || !memcmp(said.rawCharacters(), StringView(pattern).rawCharacters(), static_cast<size_t>(it->length) << !it->is8Bit) : said == pattern))
                return CodeForRegExp { static_cast<const uint8_t*>(image->m_code) + it->codeFor8Bit, static_cast<const uint8_t*>(image->m_code) + it->codeFor16Bit, it->numSubpatterns, it->minimumSize, static_cast<Yarr::SpecificPattern>(it->specificPattern), !!it->retainsPatternDetails, !it->hasNoCode, !!it->sourceIsPattern, said, nullptr };
        }
    }
    return std::nullopt;
}

const ImageRegExp* Image::findRegExpWithSourceInPieces(std::span<const StringView> leaves, unsigned length, OptionSet<Yarr::Flags> flags)
{
    if (!hasAny())
        return nullptr;
    uint32_t rawFlags = ImageRegExp::significantFlags(flags).toRaw();
    auto& all = registry();
    Locker locker { all.lock };
    for (Image* image : all.images) {
        auto& header = image->header();
        for (auto& regExp : std::span { image->at<ImageRegExp>(header.regExpsOffset), header.numberOfRegExps }) {
            if (!regExp.sourceIsInPieces || regExp.length != length || regExp.flags != rawFlags)
                continue;
            size_t leaf = 0;
            unsigned positionInLeaf = 0;
            bool isSame = true;
            forEachSourcePiece(image->at<uint8_t>(0), header, regExp, [&](StringView piece) {
                while (!piece.isEmpty()) {
                    while (leaf < leaves.size() && positionInLeaf == leaves[leaf].length()) {
                        ++leaf;
                        positionInLeaf = 0;
                    }
                    if (leaf == leaves.size()) {
                        isSame = false;
                        return false;
                    }
                    unsigned count = std::min(piece.length(), leaves[leaf].length() - positionInLeaf);
                    if (leaves[leaf].substring(positionInLeaf, count) != piece.left(count)) {
                        isSame = false;
                        return false;
                    }
                    positionInLeaf += count;
                    piece = piece.substring(count);
                }
                return true;
            });
            if (isSame)
                return &regExp;
        }
    }
    return nullptr;
}

Image& Image::ofRegExp(const ImageRegExp& regExp)
{
    auto& all = registry();
    Locker locker { all.lock };
    for (Image* image : all.images) {
        if (std::bit_cast<uintptr_t>(&regExp) - std::bit_cast<uintptr_t>(image->m_data.data()) < image->m_data.size())
            return *image;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

auto Image::codeForRegExpRecord(const ImageRegExp& regExp) -> CodeForRegExp
{
    Image& image = ofRegExp(regExp);
    return CodeForRegExp { static_cast<const uint8_t*>(image.m_code) + regExp.codeFor8Bit, static_cast<const uint8_t*>(image.m_code) + regExp.codeFor16Bit, regExp.numSubpatterns, regExp.minimumSize, static_cast<Yarr::SpecificPattern>(regExp.specificPattern), !!regExp.retainsPatternDetails, !regExp.hasNoCode, !!regExp.sourceIsPattern, { }, &regExp };
}

String Image::regExpSource(const ImageRegExp& regExp)
{
    RELEASE_ASSERT(regExp.sourceIsInPieces);
    Image& image = ofRegExp(regExp);
    size_t position = 0;
    if (regExp.is8Bit) {
        std::span<Latin1Character> units;
        String text = StringImpl::createUninitialized(regExp.length, units);
        forEachSourcePiece(image.at<uint8_t>(0), image.header(), regExp, [&](StringView piece) {
            memcpySpan(units.subspan(position), piece.span8());
            position += piece.length();
            return true;
        });
        RELEASE_ASSERT(position == regExp.length);
        return text;
    }
    std::span<char16_t> units;
    String text = StringImpl::createUninitialized(regExp.length, units);
    forEachSourcePiece(image.at<uint8_t>(0), image.header(), regExp, [&](StringView piece) {
        memcpySpan(units.subspan(position), piece.span16());
        position += piece.length();
        return true;
    });
    RELEASE_ASSERT(position == regExp.length);
    return text;
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
            reinterpret_cast<const Site*>(futureLocation(function.sites())), reinterpret_cast<const ImageFunction*>(futureLocation(&function)), function.numSlots, !!function.startsCold, !!function.hasSiteConstants, !!function.hasInlineFrames, !!function.hasValueEntry, !!function.hasCompatibleResultsMask };
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
    return adoptRef(*new JITCode(const_cast<uint8_t*>(image->codeFor(*function)), *function, JITCode::entryKindFor(unlinkedCodeBlock)));
}

Ref<JITCode> jitCodeForImageFunction(ImageCode code, CodeSpecializationKind kind)
{
    auto [image, function] = code;
    return adoptRef(*new JITCode(const_cast<uint8_t*>(image->codeFor(*function)), *function, isCall(kind) ? JITCode::EntryKind::Call : JITCode::EntryKind::Construct));
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
        if (header.stamp == AOT::imageStamp() && header.numberOfFaultingAccesses && !AOT::reservesInaccessibleFirstPages())
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
