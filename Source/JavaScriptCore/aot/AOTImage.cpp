/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImage.h"

#include "AOTTypeTable.h"

#if ENABLE(FTL_JIT)

#include "AOTCompiler.h"
#include "CachedTypes.h"
#include "CodeBlock.h"
#include "CodeCache.h"
#include "ExecutableAllocator.h"
#include "StaticHeap.h"
#include "FunctionExecutable.h"
#include "JSCBytecodeCacheVersion.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSModuleEnvironment.h"
#include "JSModuleRecord.h"
#include "Options.h"
#include "ParseInt.h"
#include "SourceProvider.h"
#include <wtf/FilePrintStream.h>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SHA1.h>
#include <wtf/TZoneMallocInlines.h>

#if OS(DARWIN) || OS(LINUX)
#include <pthread.h>
#include <sys/mman.h>
#endif

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageBuilder);
WTF_MAKE_TZONE_ALLOCATED_IMPL(Image);

uint64_t imageStamp()
{
    // Compiled code depends on the layout of the engine's objects and on the order of the runtime table. The stamp does not detect
    // every change (an image belongs in the executable that contains its engine), but it catches images and engines from different
    // builds.
    uint64_t stamp = computeJSCBytecodeCacheVersion();
    auto mix = [&](uint64_t value) {
        stamp = (stamp ^ value) * 0x100000001b3ULL;
        stamp ^= stamp >> 29;
    };
    mix(numberOfEntries);
    mix(Instance::offsetOfStates());
    mix(Instance::leastStateWithData);
    mix(numberOfStubs);
    mix(numOpcodeIDs);
    mix(sizeof(VM));
    mix(sizeof(JSGlobalObject));
    mix(sizeof(CodeBlock));
    mix(sizeof(Data));
    mix(VM::offsetOfSoftStackLimit());
    mix(VM::offsetOfHeapBarrierThreshold());
    mix(CodeBlock::offsetOfJITData());
    mix(JSGlobalObject::offsetOfGlobalLexicalBindingEpoch());
#if CPU(ARM64)
    mix(1 | MacroAssemblerARM64::supportsDoubleToInt32ConversionUsingJavaScriptSemantics() << 8);
#elif CPU(X86_64)
    mix(2);
#endif
    return stamp;
}

// The sort order of ImageHeader::selectorsInOrderOffset.
static int compareSelectors(bool is8BitA, std::span<const uint8_t> a, bool is8BitB, std::span<const uint8_t> b)
{
    if (is8BitA != is8BitB)
        return is8BitA ? -1 : 1;
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    return memcmp(a.data(), b.data(), a.size());
}

static std::span<const uint8_t> bytesOf(const StringImpl& string) { return string.is8Bit() ? asBytes(string.span8()) : asBytes(string.span16()); }
static int compareSelectors(const StringImpl& a, const StringImpl& b) { return compareSelectors(a.is8Bit(), bytesOf(a), b.is8Bit(), bytesOf(b)); }

// ---- Building

// ---- The source text that error messages quote, for a program that ships without its source

static constexpr unsigned maxUntruncatedQuoteLength = 64;
static constexpr unsigned maxCalleeQuoteLength = 96;
static constexpr unsigned truncatedQuoteEndLength = 40;

// In "f.g(a, (b))": the index of the "(" that matches the final ")". Found the same way as functionCallBase() in
// ExceptionHelpers.cpp.
static size_t indexOfArgumentListStart(StringView text)
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
        // The same computation as appendSourceToErrorMessage() in ErrorInstance.cpp.
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
            else if (size_t open = indexOfArgumentListStart(said); open != notFound && open < maxCalleeQuoteLength) {
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

static Compress s_compress;
static Decompress s_decompress;

void setCodec(Compress compress, Decompress decompress)
{
    s_compress = compress;
    s_decompress = decompress;
}

// See ImageHeader::sizeOfBlockOfTextOfQuotes. Returns nullopt if there is no compressor or compression fails.
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

// Reads the bytes that compressInBlocks() was given, one at a time, from any offset. A block size of zero means the bytes are not
// compressed.
class BlockReader {
public:
    BlockReader(const uint8_t* bytes, size_t sizeOfBlock, uint64_t start)
        : m_bytes(bytes)
        , m_sizeOfBlock(sizeOfBlock)
        , m_at(start)
    {
    }

    bool failed() const { return m_failed; }

    // (Returns only zeros after a failure, which terminates a varint and a list.)
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

String Image::textOfQuote(uint64_t start, size_t length) const
{
    BlockReader reader(this->at<uint8_t>(header().textOfQuotesOffset), header().sizeOfBlockOfTextOfQuotes, start);
    Vector<uint8_t> bytes(length, [&](size_t) { return reader.next(); });
    if (reader.failed())
        return { };
    return String::fromUTF8(bytes.span());
}

// The format: the number of quotes, then for each quote, in order: the delta of its bytecode offset from the previous quote, the
// signed delta of its start in the text of quotes from the previous quote's start, and its length combined with its Quote::Kind.
std::optional<std::pair<String, bool>> Image::quoteAt(const ImageFunction& function, unsigned bytecodeOffset) const
{
    if (!function.quotes)
        return std::nullopt;
    BlockReader reader(this->at<uint8_t>(header().quotesOffset), header().sizeOfBlockOfQuotes, function.quotes);
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
        String text = textOfQuote(start, static_cast<size_t>(lengthAndKind >> 2));
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
            info.startsOfConstructions.append(start);
            break;
        }
        default:
            break;
        }
    }
}

Vector<ReportableSitesOfFunction> ImageBuilder::reportableSites()
{
    Vector<ReportableSitesOfFunction> all;
    all.reserveInitialCapacity(m_functions.size());
    for (auto& function : m_functions) {
        auto& info = function.code.info;
        ReportableSitesOfFunction result;
        result.offsets = WTF::move(info.callSites);
        result.numbersOfIdentifiers = WTF::move(info.numbersOfIdentifiers);
        result.numbersOfConstants = WTF::move(info.numbersOfConstants);
        for (unsigned i = 0; i < info.constructSites.size(); ++i) {
            if (info.startsOfConstructions[i].first || info.startsOfConstructions[i].second)
                result.constructions.append({ info.constructSites[i], info.startsOfConstructions[i].first, info.startsOfConstructions[i].second });
        }
        all.append(WTF::move(result));
    }
    return all;
}

// This list follows the one that quoteAt() reads. The format: the number of entries, then for each entry, in order, the delta of
// its bytecode offset from the previous entry.
bool Image::constructsAt(const ImageFunction& function, unsigned bytecodeOffset) const
{
    if (!function.quotes)
        return false;
    BlockReader reader(this->at<uint8_t>(header().quotesOffset), header().sizeOfBlockOfQuotes, function.quotes);
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

void noteThatLinkTimeConstantIsUsed(unsigned which)
{
    s_linkTimeConstantsUsed[which / 64].fetch_or(1ull << which % 64, std::memory_order_relaxed);
}

void ImageBuilder::add(ImageKey key, uint64_t rank, CompiledCode&& code)
{
    // Clear the branch targets, which depend on the address the code was compiled at, so that the output is deterministic.
    for (auto& call : code.info.stubCalls)
        memset(code.bytes.mutableSpan().data() + call.offset, 0, sizeof(uint32_t));
    Locker locker { m_lock };
    m_functions.append(Function { key, rank, WTF::move(code) });
}

bool ImageBuilder::addRegExp(VM& vm, const String& pattern, OptionSet<Yarr::Flags> flags)
{
    Locker locker { m_lock };
    flags = ImageRegExp::significantFlags(flags);
    auto asked = m_regExpsAsked.add(makeString(flags.toRaw(), '/', pattern), false);
    if (!asked.isNewEntry)
        return asked.iterator->value;
    RegExpCode result { pattern, flags, { } };
    // Without a JIT, a pattern that has no code in the image is matched by the Yarr interpreter, which is about ten times slower.
    auto notCompiled = [&](ASCIILiteral reason) {
        ++m_numberOfRegExpsNotCompiled;
        if (Options::verboseAOTCompilation()) [[unlikely]]
            dataLogLn("AOT: regular expression not compiled (", reason, "): /", pattern, "/ flags ", flags.toRaw());
        return false;
    };
    for (auto charSize : { Yarr::CharSize::Char8, Yarr::CharSize::Char16 }) {
        // The same steps as RegExp::compile().
        Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
        Yarr::YarrPattern yarrPattern(pattern, flags, error);
        if (Yarr::hasError(error) || yarrPattern.containsUnsignedLengthPattern() || (yarrPattern.m_containsLookbehinds && !Options::useRegExpLookbehindJIT()))
            return notCompiled("the JIT does not support it either"_s);
        // (Such a pattern is matched with a plain string search, without running compiled code.)
        if (!yarrPattern.m_atom.isNull())
            return false;
        auto code = Yarr::jitCompileForImage(yarrPattern, pattern, charSize, &vm, Yarr::ExecutionMode::IncludeSubpatterns);
        if (!code)
            return notCompiled(charSize == Yarr::CharSize::Char8 ? "no code for 8-bit subjects"_s : "no code for 16-bit subjects"_s);
        result.code[charSize == Yarr::CharSize::Char16] = WTF::move(*code);
    }
    m_regExps.append(WTF::move(result));
    m_regExpsAsked.set(makeString(flags.toRaw(), '/', pattern), true);
    return true;
}

Vector<uint8_t> ImageBuilder::finish()
{
    Locker locker { m_lock };
    if (Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("AOT: compiled ", m_regExps.size(), " regular expressions, and left ", m_numberOfRegExpsNotCompiled, " to the interpreter");
    std::ranges::sort(m_functions, [](const Function& a, const Function& b) {
        return a.rank < b.rank;
    });


    unsigned capacity = 16;
    while (capacity * 3 < m_functions.size() * 4)
        capacity *= 2;

    // The shapes that objects are created with and the names that properties are read by (selectors) are numbered for the whole
    // program, in order of first use.
    // If the program's identifiers are numbered, selectors use those numbers instead. A site's identifier is then also its
    // selector.
    const NumbersOfIdentifiers* numbersOfIdentifiers = m_numbersOfIdentifiersOfProgram;
    Vector<UniquedStringImpl*> selectors { nullptr };
    if (numbersOfIdentifiers)
        selectors.fill(nullptr, m_numberOfIdentifiersOfProgram);
    UncheckedKeyHashMap<UniquedStringImpl*, uint32_t> numberOfSelector;
    auto selectorFor = [&](UniquedStringImpl* name) {
        if (numbersOfIdentifiers) {
            uint32_t number = numbersOfIdentifiers->get(name);
            RELEASE_ASSERT(number);
            selectors[number] = name;
            return number;
        }
        return numberOfSelector.ensure(name, [&] {
            selectors.append(name);
            return static_cast<uint32_t>(selectors.size() - 1);
        }).iterator->value;
    };
    struct Shape {
        unsigned inlineCapacity { 0 };
        Vector<uint32_t, 8> names;
        Vector<uint16_t, 8> slots;
        uint16_t layoutID { 0 };
        uint16_t reserved { 0 };
        uint16_t inlineSlots { 0 };
    };
    // The layouts from the type table come first and keep their own numbers, whether or not any object is created with them.
    Vector<Shape> shapes(1 + (TypeTable::shared() ? TypeTable::shared()->numberOfLayouts() : 0));
    UncheckedKeyHashMap<String, uint32_t> numberOfShape;
    BitVector selectorIsRead;
    for (auto& function : m_functions) {
        auto& info = function.code.info;
        RELEASE_ASSERT(info.siteConstants.size() == info.numSlots);
        for (unsigned slot = 0; slot < info.numSlots; ++slot) {
            uint32_t& constant = info.siteConstants[slot];
            if (!constant)
                continue;
            // A site that creates objects has no identifier, so the field is free to hold the constant.
            auto keepInSite = makeScopeExit([&] {
                if (!numbersOfIdentifiers)
                    return;
                RELEASE_ASSERT(!info.sites[slot].identifierAndExtra);
                info.sites[slot].identifierAndExtra = constant;
            });
            if (constant & CompiledFunctionInfo::siteConstantIsPlan) {
                constant &= ~CompiledFunctionInfo::siteConstantIsPlan;
                continue;
            }
            if (!(constant & CompiledFunctionInfo::siteConstantIsShape)) {
                constant = selectorFor(info.selectors[constant - 1]);
                selectorIsRead.set(constant);
                keepInSite.release();
                RELEASE_ASSERT(!numbersOfIdentifiers || (info.sites[slot].identifierAndExtra & ((1u << Site::identifierBits) - 1)) == constant);
                continue;
            }
            const KnownShape& known = info.shapes[(constant & ~CompiledFunctionInfo::siteConstantIsShape) - 1];
            Shape shape;
            shape.inlineCapacity = known.inlineCapacity;
            for (UniquedStringImpl* name : known.names)
                shape.names.append(selectorFor(name));
            if (known.number) {
                shape.slots = known.slots;
                shape.layoutID = known.layoutID;
                shape.reserved = known.reserved;
                shape.inlineSlots = known.inlineSlots;
                constant = known.number;
                if (shapes[constant].names.isEmpty())
                    shapes[constant] = WTF::move(shape);
                else
                    RELEASE_ASSERT(shapes[constant].names == shape.names && shapes[constant].slots == shape.slots && shapes[constant].inlineCapacity == shape.inlineCapacity);
                continue;
            }
            Vector<uint32_t, 16> words { known.inlineCapacity };
            words.appendVector(shape.names);
            if (known.layoutID) {
                shape.slots = known.slots;
                shape.layoutID = known.layoutID;
                shape.reserved = known.reserved;
                shape.inlineSlots = known.inlineSlots;
                words.append(0xffff0000u | known.layoutID);
                for (uint16_t slot : known.slots)
                    words.append(slot);
            }
            String key { std::span { reinterpret_cast<const Latin1Character*>(words.span().data()), words.size() * sizeof(uint32_t) } };
            // (A Structure has 16 bits for the shape number. Objects with any further shape are created without one.)
            if (auto it = numberOfShape.find(key); it != numberOfShape.end())
                constant = it->value;
            else if (shapes.size() > std::numeric_limits<uint16_t>::max())
                constant = 0;
            else {
                shapes.append(WTF::move(shape));
                constant = shapes.size() - 1;
                numberOfShape.add(key, constant);
            }
        }
    }
    RELEASE_ASSERT(selectors.size() < (1u << (32 - ImageDispatchEntry::locationBits)));

    // Build the dispatch table by row displacement: longest rows first, each at the lowest offset where it fits.
    Vector<uint32_t> rowOfSelector;
    rowOfSelector.fill(0, selectors.size());
    Vector<uint32_t> dispatch;
    {
        Vector<Vector<std::pair<uint32_t, int32_t>, 0>> rows(selectors.size());
        for (uint32_t number = 1; number < shapes.size(); ++number) {
            auto& shape = shapes[number];
            for (unsigned i = 0; i < shape.names.size(); ++i) {
                if (!selectorIsRead.get(shape.names[i]))
                    continue;
                unsigned at = shape.slots.isEmpty() ? i : shape.slots[i];
                unsigned inObject = shape.layoutID ? shape.inlineSlots : shape.inlineCapacity;
                int32_t location = at < inObject || !inObject
                    ? static_cast<int32_t>(JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + at)
                    : -static_cast<int32_t>(at - inObject) - 2;
                rows[shape.names[i]].append({ number, location });
            }
        }
        Vector<uint32_t> order;
        for (uint32_t selector = 1; selector < selectors.size(); ++selector) {
            if (!rows[selector].isEmpty())
                order.append(selector);
        }
        // (Rows with one entry are sorted by shape, so that the search for a free offset can continue from the previous one.)
        std::ranges::sort(order, [&](uint32_t a, uint32_t b) {
            if (rows[a].size() != rows[b].size())
                return rows[a].size() > rows[b].size();
            return rows[a][0].first < rows[b][0].first;
        });
        size_t firstFree = 0;
        size_t lastForOne = 0;
        auto isFree = [&](size_t index) { return index >= dispatch.size() || !dispatch[index]; };
        auto growWithZeros = [&](size_t size) {
            if (size > dispatch.size())
                dispatch.insertFill(dispatch.size(), 0, size - dispatch.size());
        };
        for (uint32_t selector : order) {
            auto& row = rows[selector];
            uint32_t first = row[0].first;
            while (!isFree(firstFree))
                ++firstFree;
            size_t at = std::max<size_t>(row.size() == 1 ? std::max(firstFree, lastForOne) : firstFree, first);
            for (;; ++at) {
                if (std::ranges::all_of(row, [&](auto& entry) { return isFree(at - first + entry.first); }))
                    break;
            }
            if (row.size() == 1)
                lastForOne = at;
            size_t start = at - first;
            growWithZeros(start + row.last().first + 1);
            for (auto& [shape, location] : row)
                dispatch[start + shape] = ImageDispatchEntry::encode(selector, location);
            rowOfSelector[selector] = safeCast<uint32_t>(start);
        }
        // Any row can be indexed by any shape, so the table has to extend that far.
        size_t furthest = 0;
        for (uint32_t row : rowOfSelector)
            furthest = std::max<size_t>(furthest, row);
        growWithZeros(furthest + shapes.size());
    }
    Vector<ImageShape> imageShapes;
    Vector<uint16_t> slotsOfShapes;
    size_t numberOfPropertiesOfShapes = 0;
    for (auto& shape : shapes) {
        imageShapes.append({ safeCast<uint16_t>(shape.names.size()), safeCast<uint16_t>(shape.inlineCapacity), shape.slots.isEmpty() ? 0 : safeCast<uint32_t>(slotsOfShapes.size() + 1), shape.layoutID, shape.reserved, shape.inlineSlots, 0 });
        slotsOfShapes.appendVector(shape.slots);
        if (shape.layoutID && !shape.slots.isEmpty() && Options::useAOTTypedFields() && TypeTable::hasTypedFields() && TypeTable::shared()->isUsable(shape.layoutID) && TypeTable::shared()->usesFieldIDs(shape.layoutID)) {
            imageShapes.last().hasIds = 1;
            for (uint32_t name : shape.names)
                slotsOfShapes.append(TypeTable::shared()->idOfField(shape.layoutID, selectors[name]));
        }
        numberOfPropertiesOfShapes += shape.names.size();
    }
    Vector<uint32_t> slotRanges;
    Vector<TypedLayoutTable::FieldType> slotTypes;
    Vector<uint32_t> fieldRanges;
    Vector<TypedLayoutTable::Field> fieldRecords;
    Vector<TypedLayoutTable::FieldType> fieldTypes;
    Vector<uint16_t> fieldLayoutIDs;
    Vector<uint32_t> fieldsOfSlot[Structure::numberOfSlotsWithFieldIDs]; // Indexed by field ID, from one: an index into `named`.
    Vector<uint16_t> layoutIDsOfSlot[Structure::numberOfSlotsWithFieldIDs]; // Likewise: the typed layout that the field belongs to.
    Vector<uint8_t> inlineSlotCounts;
    if (Options::useAOTTypedFields() && TypeTable::hasTypedFields()) {
        RELEASE_ASSERT_WITH_MESSAGE(numbersOfIdentifiers, "Structs go by the numbers of the program's identifiers");
        for (uint32_t number = 0; number <= TypeTable::shared()->numberOfTypedLayouts(); ++number) {
            auto layout = TypeTable::shared()->typedLayout(number);
            RELEASE_ASSERT(slotTypes.size() < (1u << 24) && fieldRecords.size() < (1u << 20));
            slotRanges.append(static_cast<uint32_t>(slotTypes.size()) << 8 | layout.capacity);
            size_t start = slotTypes.size();
            for (unsigned slot = 0; slot < layout.capacity; ++slot)
                slotTypes.append({ 0, 0, 0, 0 });
            size_t firstFieldOfLayout = fieldRecords.size();
            for (auto& name : layout.fields) {
                // (With field IDs, the type is looked up by name, and the slot has no type.)
                if (!layout.usesFieldIDs)
                    slotTypes[start + name.slot] = { name.fieldType.packedKinds(), name.fieldType.first, name.fieldType.last, 0 };
                if (name.id) {
                    // (In the order the IDs were assigned. A field whose name the program never uses has no record.)
                    RELEASE_ASSERT(fieldsOfSlot[name.slot].size() + 1 == name.id);
                    fieldsOfSlot[name.slot].append(safeCast<uint32_t>(firstFieldOfLayout));
                    layoutIDsOfSlot[name.slot].append(safeCast<uint16_t>(number));
                }
            }
            // Sorted by hash: see TypedLayoutTable::named().
            Vector<const TypeTable::LayoutField*, 16> inOrder;
            for (auto& name : layout.fields) {
                // (The program cannot add a property by a name that it never uses.)
                if (numbersOfIdentifiers->contains(name.name))
                    inOrder.append(&name);
            }
            std::ranges::stable_sort(inOrder, [](auto* a, auto* b) { return a->name->existingHash() < b->name->existingHash(); });
            for (auto* name : inOrder) {
                if (name->id)
                    fieldsOfSlot[name->slot][name->id - 1] = safeCast<uint32_t>(fieldRecords.size());
                fieldRecords.append({ numbersOfIdentifiers->get(name->name), safeCast<uint8_t>(name->slot), name->mayBeAbsent, name->id });
                fieldTypes.append({ name->fieldType.packedKinds(), name->fieldType.first, name->fieldType.last, 0 });
                fieldLayoutIDs.append(safeCast<uint16_t>(number));
            }
            RELEASE_ASSERT(fieldRecords.size() - firstFieldOfLayout < (1u << 12));
            fieldRanges.append(static_cast<uint32_t>(firstFieldOfLayout) << 12 | (fieldRecords.size() - firstFieldOfLayout));
            RELEASE_ASSERT(layout.inlineSlots < TypedLayoutTable::usesFieldIDsBit);
            inlineSlotCounts.append(static_cast<uint8_t>(layout.inlineSlots | (layout.usesFieldIDs ? TypedLayoutTable::usesFieldIDsBit : 0)));
        }
    } else if (Options::useAOTTypedFields() && TypeTable::shared()) {
        for (uint32_t number = 0; number < shapes.size() && number <= TypeTable::shared()->numberOfLayouts(); ++number) {
            auto typesBySlot = shapes[number].names.isEmpty() ? Vector<TypeTable::FieldType, 8> { } : TypeTable::shared()->fieldTypesBySlot(number);
            RELEASE_ASSERT(typesBySlot.size() < 256 && slotTypes.size() < (1u << 24));
            slotRanges.append(static_cast<uint32_t>(slotTypes.size()) << 8 | typesBySlot.size());
            for (auto& fieldType : typesBySlot)
                slotTypes.append({ safeCast<uint16_t>(fieldType.kinds), Options::useAOTFunctionSplitting() ? fieldType.first : uint16_t(0), Options::useAOTFunctionSplitting() ? fieldType.last : uint16_t(0), 0 });
        }
    }
    Vector<ImageSelector> imageSelectors;
    Vector<uint8_t> textOfSelectors;
    for (uint32_t selector = 0; selector < selectors.size() && !numbersOfIdentifiers; ++selector) {
        UniquedStringImpl* name = selectors[selector];
        if (!name) {
            imageSelectors.append({ 0, 0, 1 });
            continue;
        }
        if (!name->is8Bit() && textOfSelectors.size() % 2)
            textOfSelectors.append(0);
        imageSelectors.append({ safeCast<uint32_t>(textOfSelectors.size()), name->length(), name->is8Bit() });
        textOfSelectors.append(name->is8Bit() ? asBytes(name->span8()) : asBytes(name->span16()));
    }
    Vector<uint32_t> selectorsInOrder;
    for (uint32_t selector = 1; selector < selectors.size(); ++selector) {
        if (selectors[selector])
            selectorsInOrder.append(selector);
    }
    std::ranges::sort(selectorsInOrder, [&](uint32_t a, uint32_t b) {
        return compareSelectors(*selectors[a], *selectors[b]) < 0;
    });

    // Maps a key to the function that a call refers to: the first function with that key.
    Vector<uint32_t> functionWithKey;
    functionWithKey.fill(std::numeric_limits<uint32_t>::max(), capacity);
    for (size_t index = 0; index < m_functions.size(); ++index) {
        unsigned bucket = m_functions[index].key.hash() & (capacity - 1);
        while (functionWithKey[bucket] != std::numeric_limits<uint32_t>::max() && !m_functions[functionWithKey[bucket]].key.sameFunction(m_functions[index].key))
            bucket = (bucket + 1) & (capacity - 1);
        if (functionWithKey[bucket] == std::numeric_limits<uint32_t>::max())
            functionWithKey[bucket] = index;
    }
    auto directTargetOf = [&](const CompiledFunctionInfo& info, const StubCall& call) -> std::optional<size_t> {
        if (call.function == StubCall::noFunction)
            return std::nullopt;
        const ImageKey& key = info.knownCallees[call.function];
        for (unsigned bucket = key.hash() & (capacity - 1); functionWithKey[bucket] != std::numeric_limits<uint32_t>::max(); bucket = (bucket + 1) & (capacity - 1)) {
            size_t target = functionWithKey[bucket];
            if (m_functions[target].key.sameFunction(key))
                return target;
        }
        // (The compiler guarantees that every function that is called directly has code.)
        dataLogLn("AOT: there is no code for @", key.module, ":", key.start, ":", key.kind, ", which something calls without asking");
        RELEASE_ASSERT_NOT_REACHED();
        return std::nullopt;
    };

    // A function that is only called directly, and whose every call has been inlined, needs no code. Its metadata is kept, because
    // stack traces still name it.
    if (Options::useAOTInlining()) {
        BitVector isCalled(m_functions.size());
        Vector<uint32_t> worklist;
        auto call = [&](size_t index) {
            if (!isCalled.set(index))
                worklist.append(static_cast<uint32_t>(index));
        };
        auto indexOf = [&](const ImageKey& key) -> std::optional<size_t> {
            for (unsigned bucket = key.hash() & (capacity - 1); functionWithKey[bucket] != std::numeric_limits<uint32_t>::max(); bucket = (bucket + 1) & (capacity - 1)) {
                if (m_functions[functionWithKey[bucket]].key.sameFunction(key))
                    return functionWithKey[bucket];
            }
            return std::nullopt;
        };
        // The same goes for a function expression, even one that escapes, if no remaining code creates it. For example, a closure
        // passed to a function that was inlined, where the closure was inlined too.
        BitVector isOnlyCreatedAtItsExpression(m_functions.size());
        for (auto& function : m_functions) {
            for (auto& key : function.code.info.functionExpressionsInCode) {
                if (auto index = indexOf(key))
                    isOnlyCreatedAtItsExpression.set(*index);
            }
        }
        for (size_t index = 0; index < m_functions.size(); ++index) {
            if (!m_functions[index].code.info.isOnlyCalledDirectly && !isOnlyCreatedAtItsExpression.get(index))
                call(index);
        }
        while (!worklist.isEmpty()) {
            auto& info = m_functions[worklist.takeLast()].code.info;
            for (auto& stubCall : info.stubCalls) {
                if (auto target = directTargetOf(info, stubCall))
                    call(*target);
            }
            for (auto& key : info.functionsCreated) {
                // (Creating a function that is only called directly does not make it reachable.)
                if (auto index = indexOf(key); index && !m_functions[*index].code.info.isOnlyCalledDirectly)
                    call(*index);
            }
        }
        size_t functions = 0;
        size_t bytes = 0;
        for (size_t index = 0; index < m_functions.size(); ++index) {
            if (isCalled.get(index))
                continue;
            auto& code = m_functions[index].code;
            ++functions;
            bytes += code.bytes.size();
            static constexpr uint32_t breakpoint = 0xd4200000;
            code.bytes.resize(sizeof(breakpoint));
            memcpy(code.bytes.mutableSpan().data(), &breakpoint, sizeof(breakpoint));
            code.info.stubCalls.clear();
            code.info.indexReferences.clear();
            code.info.sitesOfSpreads.clear();
            code.info.inlineFrames.clear();
            code.info.catchEntrypoints.clear();
        }
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", functions, " functions that nothing calls any more had ", bytes, " bytes of code");
    }

    // Lay out the code. The stubs come first, and are repeated whenever the previous copy is about to go out of branch range.
    const StubBlob& stubs = stubBlob();
    size_t reach = reachOfStubCall;
    if (unsigned copies = Options::numberOfAOTStubCopiesForTesting()) [[unlikely]] {
        size_t sizeOfFunctions = 0;
        for (auto& function : m_functions)
            sizeOfFunctions += WTF::roundUpToMultipleOf<imageFunctionAlignment>(function.code.bytes.size());
        reach = stubs.bytes.size() + imageStubsAlignment + sizeOfFunctions / copies;
    }
    Vector<size_t> stubsAt;
    Vector<std::pair<size_t, size_t>> placement; // For each function: its offset, and the offset of the copy of the stubs that it calls.
    // After each function's code there is a veneer for every direct callee that is out of branch range. Which callees those are
    // depends on the layout, so the layout is first computed without veneers. There are few enough that adding them does not change
    // the answer.
    Vector<Vector<uint32_t, 0>> farCallees(m_functions.size());
    auto sizeWithVeneers = [&](size_t index) {
        size_t size = m_functions[index].code.bytes.size();
        return farCallees[index].isEmpty() ? size : WTF::roundUpToMultipleOf<sizeof(uint32_t)>(size) + farCallees[index].size() * sizeOfVeneer;
    };
    if (usesStubs) {
        Vector<size_t> at;
        size_t end = 0;
        size_t lastStubs = std::numeric_limits<size_t>::max();
        for (auto& function : m_functions) {
            end = WTF::roundUpToMultipleOf<imageFunctionAlignment>(end);
            if (lastStubs == std::numeric_limits<size_t>::max() || end + function.code.bytes.size() - lastStubs > reach) {
                end = lastStubs = WTF::roundUpToMultipleOf<imageStubsAlignment>(end);
                end = WTF::roundUpToMultipleOf<imageFunctionAlignment>(end + stubs.bytes.size());
            }
            at.append(end);
            end += function.code.bytes.size();
        }
        size_t sizeOfVeneers = 0;
        for (size_t index = 0; index < m_functions.size(); ++index) {
            auto& info = m_functions[index].code.info;
            for (auto& call : info.stubCalls) {
                auto target = directTargetOf(info, call);
                if (!target)
                    continue;
                size_t from = at[index] + call.offset;
                size_t to = at[*target];
                if (((from > to ? from - to : to - from) <= reach && !Options::forceAOTVeneers()) || farCallees[index].contains(static_cast<uint32_t>(*target)))
                    continue;
                farCallees[index].append(static_cast<uint32_t>(*target));
                sizeOfVeneers += sizeOfVeneer + sizeof(uint32_t) + imageStubsAlignment;
            }
        }
        RELEASE_ASSERT(sizeOfVeneers + stubs.bytes.size() + imageStubsAlignment < roomToSpareInReachOfStubCall);
    }
    size_t recordsSize = 0;
    size_t codeSize = 0;
    for (size_t indexOfFunction = 0; indexOfFunction < m_functions.size(); ++indexOfFunction) {
        auto& function = m_functions[indexOfFunction];
        recordsSize += sizeof(ImageFunction) + function.code.info.catchEntrypoints.size() * sizeof(ImageCatchEntrypoint) + function.code.info.sites.size() * (sizeof(Site) + (numbersOfIdentifiers ? 0 : sizeof(uint32_t))) + function.code.info.knownCallees.size() * sizeof(uint32_t) + function.code.info.plans.sizeInBytes();
        RELEASE_ASSERT(function.code.info.sites.size() == function.code.info.numSlots);
        codeSize = WTF::roundUpToMultipleOf<imageFunctionAlignment>(codeSize);
        if (usesStubs && (stubsAt.isEmpty() || codeSize + sizeWithVeneers(indexOfFunction) - stubsAt.last() > reach)) {
            codeSize = WTF::roundUpToMultipleOf<imageStubsAlignment>(codeSize);
            stubsAt.append(codeSize);
            codeSize = WTF::roundUpToMultipleOf<imageFunctionAlignment>(codeSize + stubs.bytes.size());
        }
        placement.append({ codeSize, stubsAt.isEmpty() ? 0 : stubsAt.last() });
        if (Options::logAOTTypeInference()) [[unlikely]]
            dataLogLn("PLACED @", m_functions[placement.size() - 1].key.module, ":", m_functions[placement.size() - 1].key.start, ":", m_functions[placement.size() - 1].key.kind, " ", codeSize);
        codeSize += sizeWithVeneers(indexOfFunction);
    }
    size_t endOfFunctions = codeSize;
    if (stubsAt.size() > mostCopiesOfStubsInImage) {
        dataLogLn("AOT: the program has too much machine code for an image: ", codeSize, " bytes need ", stubsAt.size(), " copies of the stubs, and the limit is ", mostCopiesOfStubsInImage);
        return { };
    }

    // The table that maps an address to its function.
    Vector<uint32_t> startsOfFunctions;
    for (auto& [at, stubsForIt] : placement)
        startsOfFunctions.append(safeCast<uint32_t>(at));
    startsOfFunctions.append(std::numeric_limits<uint32_t>::max());
    if (Options::aotMapFilePath()) [[unlikely]] {
        // One line per function: its index, the offset and size of its code, and its key.
        auto out = FilePrintStream::open(Options::aotMapFilePath(), "w");
        RELEASE_ASSERT(out);
        for (unsigned index = 0; index < m_functions.size(); ++index) {
            auto& function = m_functions[index];
            out->println("F\t", index, "\t", startsOfFunctions[index], "\t", function.code.bytes.size(), "\t", function.key.module, "\t", function.key.start, "\t", function.key.kind);
        }
        // For attributing the time spent in a stub to the property that was accessed.
        // D: the offset of the first inline cache slot in a function's Data, and the size of a slot.
        // I: the number of an identifier, and its text.
        // S: the index of a function, the index of one of its slots, and the number of the identifier in the slot's Site. (The Site of
        // an allocation holds a shape number instead.)
        if (numbersOfIdentifiers) {
            out->println("D\t", sizeof(Data), "\t", sizeof(Slot));
            for (auto& entry : *numbersOfIdentifiers)
                out->println("I\t", entry.value, "\t", static_cast<const StringImpl*>(entry.key));
            for (unsigned index = 0; index < m_functions.size(); ++index) {
                auto& sites = m_functions[index].code.info.sites;
                for (unsigned slot = 0; slot < sites.size(); ++slot) {
                    if (uint32_t identifier = sites[slot].identifierAndExtra & ((1u << Site::identifierBits) - 1))
                        out->println("S\t", index, "\t", slot, "\t", identifier);
                }
            }
        }
    }
    Vector<uint32_t> granulesOfCode;
    {
        uint32_t function = 0;
        for (size_t start = 0; start < endOfFunctions; start += 1 << shiftOfGranuleOfCode) {
            while (startsOfFunctions[function + 1] <= start)
                ++function;
            granulesOfCode.append(function);
        }
    }
    // The table that maps a return address to a bytecode index: see callSiteAt().
    Vector<uint8_t> callSites { 0 };
    Vector<uint32_t> callSitesOfFunction;
    for (auto& function : m_functions) {
        Vector<std::pair<uint32_t, uint32_t>> all;
        for (auto& call : function.code.info.stubCalls) {
            if (call.callSite != StubCall::noCallSite && !call.isTailCall)
                all.append({ call.offset + static_cast<uint32_t>(sizeof(uint32_t)), call.callSite });
        }
        if (all.isEmpty()) {
            callSitesOfFunction.append(0);
            continue;
        }
        std::ranges::sort(all);
        callSitesOfFunction.append(safeCast<uint32_t>(callSites.size()));
        auto& sitesOfSpreads = function.code.info.sitesOfSpreads;
        auto& inlineFrames = function.code.info.inlineFrames;
        appendVarint(callSites, all.size() << 2 | !inlineFrames.isEmpty() << 1 | !sitesOfSpreads.isEmpty());
        if (!inlineFrames.isEmpty()) {
            // (Inline frame zero is the function itself.)
            appendVarint(callSites, inlineFrames.size() - 1);
            for (unsigned frame = 1; frame < inlineFrames.size(); ++frame) {
                appendVarint(callSites, inlineFrames[frame].parent << 1 | inlineFrames[frame].isTailCall);
                appendVarint(callSites, inlineFrames[frame].callSite);
                StubCall call { };
                call.function = inlineFrames[frame].knownCallee;
                appendVarint(callSites, *directTargetOf(function.code.info, call));
            }
        }
        uint32_t previousOffset = 0;
        int64_t previousSite = 0;
        for (auto& [offset, site] : all) {
            appendVarint(callSites, (offset - previousOffset) / sizeof(uint32_t));
            int64_t step = static_cast<int64_t>(site) - previousSite;
            appendVarint(callSites, static_cast<uint64_t>(step << 1) ^ static_cast<uint64_t>(step >> 63));
            previousOffset = offset;
            previousSite = site;
        }
        if (!sitesOfSpreads.isEmpty()) {
            appendVarint(callSites, sitesOfSpreads.size());
            for (auto& entry : sitesOfSpreads) {
                appendVarint(callSites, entry.callSite);
                appendVarint(callSites, entry.item);
                appendVarint(callSites, entry.site);
            }
        }
    }

    // The code of regular expressions comes last, after a single copy of each table that they share.
    std::ranges::sort(m_regExps, [](const RegExpCode& a, const RegExpCode& b) {
        return ImageRegExp::hashOf(a.pattern, a.flags) < ImageRegExp::hashOf(b.pattern, b.flags);
    });
    UncheckedKeyHashMap<const uint8_t*, size_t> tablesOfRegExps;
    Vector<std::array<size_t, 2>> placementOfRegExps;
    Vector<ImageRegExp> imageRegExps;
    Vector<uint8_t> textOfRegExps;
    {
        size_t before = codeSize;
        for (auto& regExp : m_regExps) {
            for (auto& code : regExp.code) {
                for (auto& reference : code.tables) {
                    tablesOfRegExps.ensure(reference.table.data(), [&] {
                        codeSize = WTF::roundUpToMultipleOf<imageStubsAlignment>(codeSize);
                        size_t at = codeSize;
                        codeSize += reference.table.size();
                        return at;
                    });
                }
            }
        }
        size_t sizeOfTables = codeSize - before;
        for (auto& regExp : m_regExps) {
            std::array<size_t, 2> at;
            for (unsigned i = 0; i < 2; ++i) {
                codeSize = WTF::roundUpToMultipleOf<imageStubsAlignment>(codeSize);
                at[i] = codeSize;
                codeSize += regExp.code[i].bytes.size();
            }
            placementOfRegExps.append(at);
            ImageRegExp record { };
            record.hash = ImageRegExp::hashOf(regExp.pattern, regExp.flags);
            record.text = safeCast<uint32_t>(textOfRegExps.size());
            record.length = regExp.pattern.length();
            record.is8Bit = regExp.pattern.is8Bit();
            record.flags = regExp.flags.toRaw();
            record.codeFor8Bit = safeCast<uint32_t>(at[0]);
            record.codeFor16Bit = safeCast<uint32_t>(at[1]);
            imageRegExps.append(record);
            if (regExp.pattern.is8Bit())
                textOfRegExps.append(asBytes(regExp.pattern.span8()));
            else {
                textOfRegExps.grow(WTF::roundUpToMultipleOf<2>(textOfRegExps.size()));
                imageRegExps.last().text = safeCast<uint32_t>(textOfRegExps.size());
                textOfRegExps.append(asBytes(regExp.pattern.span16()));
            }
        }
    }
    RELEASE_ASSERT(recordsSize < std::numeric_limits<uint32_t>::max());

    ImageHeader header { };
    header.magic = imageMagic;
    header.stamp = imageStamp();
    header.tableOffset = sizeof(ImageHeader);
    // See Image::quoteAt().
    Vector<uint8_t> quotes { 0 };
    Vector<uint8_t> textOfQuotes;
    Vector<uint32_t> quotesOfFunction;
    quotesOfFunction.fill(0, m_functions.size());
    {
        UncheckedKeyHashMap<CString, uint32_t> whereItIs;
        size_t numberOfQuotes = 0;
        for (size_t index = 0; index < m_functions.size(); ++index) {
            auto& all = m_functions[index].code.info.quotes;
            auto& constructSites = m_functions[index].code.info.constructSites;
            if (all.isEmpty() && constructSites.isEmpty())
                continue;
            numberOfQuotes += all.size();
            // Expressions nest, so a quote that lies inside another quote that is kept reuses its text.
            // (The offset within the outer quote can only be derived from source offsets if every character is one byte.)
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
            // Remove type tags from the text (the lexer's encoding: a byte with the value 1 followed by six characters), which must
            // not appear in messages. The nesting of quotes was computed above, using offsets in the original text.
            if (Options::useTypeTags()) {
                auto withoutTypeTags = [](std::span<const char> text) {
                    Vector<char, 64> result;
                    for (size_t at = 0; at < text.size(); ++at) {
                        if (text[at] == 1)
                            at += 6;
                        else
                            result.append(text[at]);
                    }
                    return CString(result.span());
                };
                for (auto& place : within)
                    place.offset = withoutTypeTags(all[place.quote].text.span().first(place.offset)).length();
                for (auto& quote : all)
                    quote.text = withoutTypeTags(quote.text.span());
            }
            quotesOfFunction[index] = safeCast<uint32_t>(quotes.size());
            appendVarint(quotes, all.size());
            uint32_t previousOffset = 0;
            int64_t previousStart = 0;
            for (unsigned i = 0; i < all.size(); ++i) {
                auto& quote = all[i];
                const CString* kept = &all[within[i].quote].text;
                uint32_t start = within[i].offset + whereItIs.ensure(*kept, [&] {
                    uint32_t result = safeCast<uint32_t>(textOfQuotes.size());
                    textOfQuotes.append(kept->span());
                    return result;
                }).iterator->value;
                appendVarint(quotes, quote.bytecodeOffset - previousOffset);
                int64_t step = static_cast<int64_t>(start) - previousStart;
                appendVarint(quotes, static_cast<uint64_t>(step << 1) ^ static_cast<uint64_t>(step >> 63));
                appendVarint(quotes, static_cast<uint64_t>(quote.text.length()) << 2 | quote.kind);
                previousOffset = quote.bytecodeOffset;
                previousStart = start;
            }
            // See Image::constructsAt().
            appendVarint(quotes, constructSites.size());
            uint32_t previous = 0;
            for (uint32_t offset : constructSites) {
                appendVarint(quotes, offset - previous);
                previous = offset;
            }
        }
    }
    // Both are only read to produce an error message, so they are compressed.
    size_t compressionBlockSize = std::max(64u, Options::aotQuoteCompressionBlockSize());
    uint32_t sizeOfBlockOfTextOfQuotes = 0;
    if (auto packed = compressInBlocks(textOfQuotes.span(), compressionBlockSize)) {
        textOfQuotes = WTF::move(*packed);
        sizeOfBlockOfTextOfQuotes = compressionBlockSize;
    }
    uint32_t sizeOfBlockOfQuotes = 0;
    if (auto packed = compressInBlocks(quotes.span(), compressionBlockSize)) {
        quotes = WTF::move(*packed);
        sizeOfBlockOfQuotes = compressionBlockSize;
    }

    // Code that uses the tables of a static heap cannot run without one, and the static heap already records the function of nearly
    // every executable. So the static heap keeps the keys of the remaining functions (StaticHeap::keysOfImage()), and the key table
    // is placed after the image.
    bool keysAreOmitted = !!m_numberOfIdentifiersOfProgram;
    header.tableCapacity = keysAreOmitted ? 0 : capacity;
    header.recordsOffset = header.tableOffset + header.tableCapacity * sizeof(ImageKey);
    header.recordsSize = recordsSize;
    header.environmentsSize = m_environmentsSize;
    header.environmentsOffset = WTF::roundUpToMultipleOf<8>(static_cast<size_t>(header.recordsOffset) + recordsSize);
    header.numberOfEnvironments = m_environments.size();
    size_t endOfTables = static_cast<size_t>(header.environmentsOffset) + m_environments.size() * sizeof(ImageEnvironment);
    auto place = [&](size_t size) {
        endOfTables = WTF::roundUpToMultipleOf<8>(endOfTables);
        uint32_t result = safeCast<uint32_t>(endOfTables);
        endOfTables += size;
        return result;
    };
    header.shapesOffset = place(imageShapes.sizeInBytes());
    header.numberOfShapes = imageShapes.size();
    header.slotsOfShapesOffset = place(slotsOfShapes.sizeInBytes());
    header.slotRangesOffset = place(slotRanges.sizeInBytes());
    header.numberOfSlotRanges = slotRanges.size();
    header.slotTypesOffset = place(slotTypes.sizeInBytes());
    header.fieldRangesOffset = fieldRanges.isEmpty() ? 0 : place(fieldRanges.sizeInBytes());
    header.fieldRecordsOffset = place(fieldRecords.sizeInBytes());
    header.fieldTypesOffset = place(fieldTypes.sizeInBytes());
    header.fieldLayoutIDsOffset = place(fieldLayoutIDs.sizeInBytes());
    Vector<uint32_t> startOfFields;
    Vector<uint32_t> fields;
    header.largestFieldID = 0;
    for (auto& ofSlot : fieldsOfSlot) {
        // (Field IDs start at one.)
        startOfFields.append(safeCast<uint32_t>(fields.size()) - 1);
        fields.appendVector(ofSlot);
        header.largestFieldID = std::max(header.largestFieldID, safeCast<uint32_t>(ofSlot.size()));
    }
    Vector<uint16_t> layoutIDsByFieldID;
    for (auto& ofSlot : layoutIDsOfSlot)
        layoutIDsByFieldID.appendVector(ofSlot);
    header.layoutIDsByFieldIDOffset = place(layoutIDsByFieldID.sizeInBytes());
    header.startOfFieldsOffset = place(startOfFields.sizeInBytes());
    header.fieldsOffset = place(fields.sizeInBytes());
    header.inlineSlotCountsOffset = place(inlineSlotCounts.sizeInBytes());
    header.auditsTypes = Options::auditAOTTypedFields();
    header.selectorsOffset = place(imageSelectors.sizeInBytes());
    header.numberOfSelectors = selectors.size();
    header.rowsOfSelectorsOffset = place(rowOfSelector.sizeInBytes());
    header.textOfSelectorsOffset = place(textOfSelectors.size());
    header.selectorsInOrderOffset = place(selectorsInOrder.sizeInBytes());
    header.numberOfSelectorsInOrder = selectorsInOrder.size();
    header.dispatchOffset = place(dispatch.sizeInBytes());
    header.quotesOffset = place(quotes.size());
    header.textOfQuotesOffset = place(textOfQuotes.size());
    header.sizeOfBlockOfTextOfQuotes = sizeOfBlockOfTextOfQuotes;
    header.sizeOfBlockOfQuotes = sizeOfBlockOfQuotes;
    header.numberOfIdentifiersOfProgram = m_numberOfIdentifiersOfProgram;
    header.numberOfConstantsOfProgram = m_numberOfConstantsOfProgram;
    header.regExpsOffset = place(imageRegExps.sizeInBytes());
    header.numberOfRegExps = imageRegExps.size();
    header.textOfRegExpsOffset = place(textOfRegExps.size());
    Vector<uint32_t> numbersOfFunctions;
    for (auto& function : m_functions)
        numbersOfFunctions.append(function.code.info.numberOfFunction);
    // (Only validation reads them.)
    if (!Options::validateAOTInferredTypes())
        numbersOfFunctions.clear();
    header.numbersOfFunctionsOffset = place(numbersOfFunctions.sizeInBytes());
    header.startsOfFunctionsOffset = place(startsOfFunctions.sizeInBytes());
    header.granulesOfCodeOffset = place(granulesOfCode.sizeInBytes());
    header.callSitesOffset = place(callSites.size());
    // The distinct frame layouts, deduplicated.
    Vector<ImageFrame> frames;
    Vector<uint16_t> frameOfFunction;
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
                RELEASE_ASSERT(offset < 0 && !(offset % static_cast<ptrdiff_t>(sizeof(CPURegister))));
                frame.whereCalleeSavesStart = safeCast<uint16_t>(-offset / static_cast<ptrdiff_t>(sizeof(CPURegister)));
            }
            frame.calleeSaveRegisters = ImageFunction::packRegisters(calleeSaveRegisters);
            frameOfFunction.append(numbers.ensure(frame.bits(), [&] {
                RELEASE_ASSERT(frames.size() < 1u << 15);
                frames.append(frame);
                return static_cast<uint16_t>(frames.size() - 1);
            }).iterator->value);
        }
    }
    header.framesOffset = place(frames.sizeInBytes());
    header.endOfFunctions = safeCast<uint32_t>(endOfFunctions);
    header.sizeOfStubs = safeCast<uint32_t>(stubs.bytes.size());
    for (unsigned i = 0; i < 4; ++i)
        header.linkTimeConstantsUsed[i] = s_linkTimeConstantsUsed[i].load();
    header.numberOfCopiesOfStubs = stubsAt.size();
    header.identifiesModulesByText = m_identifiesModulesByText;
    for (unsigned i = 0; i < stubsAt.size(); ++i)
        header.copiesOfStubs[i] = safeCast<uint32_t>(stubsAt[i]);
    RELEASE_ASSERT(stubs.returnsIntoAdapters.size() == numberOfAdapters);
    for (unsigned i = 0; i < numberOfAdapters; ++i)
        header.returnsIntoAdapters[i] = stubs.returnsIntoAdapters[i];
    header.hashOfIntrinsics = ImmutableIntrinsics::shared() ? ImmutableIntrinsics::shared()->hash() : 0;
    header.dispatchSize = safeCast<uint32_t>(dispatch.size());
    header.codeOffset = WTF::roundUpToMultipleOf<imagePageSize>(endOfTables);
    header.codeSize = codeSize;
    header.size = WTF::roundUpToMultipleOf<imagePageSize>(header.codeOffset + codeSize);
    header.numberOfFunctions = m_functions.size();
    for (unsigned i = 0; i < numberOfStubs; ++i)
        header.stubOffsets[i] = stubs.offsets[i];

    Vector<uint8_t> image;
    image.fill(0, header.size + (keysAreOmitted ? capacity * sizeof(ImageKey) : 0));
    uint8_t* base = image.mutableSpan().data();
    memcpy(base, &header, sizeof(header));
    auto* table = reinterpret_cast<ImageKey*>(base + (keysAreOmitted ? header.size : header.tableOffset));
    uint8_t* records = base + header.recordsOffset;
    uint8_t* code = base + header.codeOffset;

    memcpy(base + header.environmentsOffset, m_environments.span().data(), m_environments.size() * sizeof(ImageEnvironment));
    memcpy(base + header.shapesOffset, imageShapes.span().data(), imageShapes.sizeInBytes());
    memcpy(base + header.slotsOfShapesOffset, slotsOfShapes.span().data(), slotsOfShapes.sizeInBytes());
    memcpy(base + header.slotRangesOffset, slotRanges.span().data(), slotRanges.sizeInBytes());
    memcpy(base + header.slotTypesOffset, slotTypes.span().data(), slotTypes.sizeInBytes());
    if (header.fieldRangesOffset) {
        memcpy(base + header.fieldRangesOffset, fieldRanges.span().data(), fieldRanges.sizeInBytes());
        memcpy(base + header.fieldRecordsOffset, fieldRecords.span().data(), fieldRecords.sizeInBytes());
        memcpy(base + header.fieldTypesOffset, fieldTypes.span().data(), fieldTypes.sizeInBytes());
        memcpy(base + header.fieldLayoutIDsOffset, fieldLayoutIDs.span().data(), fieldLayoutIDs.sizeInBytes());
        memcpy(base + header.startOfFieldsOffset, startOfFields.span().data(), startOfFields.sizeInBytes());
        memcpy(base + header.fieldsOffset, fields.span().data(), fields.sizeInBytes());
        memcpy(base + header.layoutIDsByFieldIDOffset, layoutIDsByFieldID.span().data(), layoutIDsByFieldID.sizeInBytes());
        memcpy(base + header.inlineSlotCountsOffset, inlineSlotCounts.span().data(), inlineSlotCounts.sizeInBytes());
    }
    memcpy(base + header.selectorsOffset, imageSelectors.span().data(), imageSelectors.sizeInBytes());
    memcpy(base + header.rowsOfSelectorsOffset, rowOfSelector.span().data(), rowOfSelector.sizeInBytes());
    memcpy(base + header.textOfSelectorsOffset, textOfSelectors.span().data(), textOfSelectors.size());
    memcpy(base + header.selectorsInOrderOffset, selectorsInOrder.span().data(), selectorsInOrder.sizeInBytes());
    memcpy(base + header.dispatchOffset, dispatch.span().data(), dispatch.sizeInBytes());
    memcpy(base + header.quotesOffset, quotes.span().data(), quotes.size());
    memcpy(base + header.textOfQuotesOffset, textOfQuotes.span().data(), textOfQuotes.size());
    memcpy(base + header.regExpsOffset, imageRegExps.span().data(), imageRegExps.sizeInBytes());
    memcpy(base + header.textOfRegExpsOffset, textOfRegExps.span().data(), textOfRegExps.size());
    memcpy(base + header.numbersOfFunctionsOffset, numbersOfFunctions.span().data(), numbersOfFunctions.sizeInBytes());
    memcpy(base + header.startsOfFunctionsOffset, startsOfFunctions.span().data(), startsOfFunctions.sizeInBytes());
    memcpy(base + header.granulesOfCodeOffset, granulesOfCode.span().data(), granulesOfCode.sizeInBytes());
    memcpy(base + header.callSitesOffset, callSites.span().data(), callSites.size());
    memcpy(base + header.framesOffset, frames.span().data(), frames.sizeInBytes());
    for (auto& [table, at] : tablesOfRegExps) {
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
            size_t codeAt = placementOfRegExps[index][i];
            memcpy(code + codeAt, regExpCode.bytes.span().data(), regExpCode.bytes.size());
            // The code is page-aligned wherever it is loaded, so page-relative addressing (adrp) works without relocation.
            for (auto& reference : regExpCode.tables) {
                size_t instructionAt = codeAt + reference.offset;
                size_t tableAt = tablesOfRegExps.get(reference.table.data());
                int64_t pages = static_cast<int64_t>(tableAt >> 12) - static_cast<int64_t>(instructionAt >> 12);
                RELEASE_ASSERT(pages >= -(1 << 20) && pages < (1 << 20));
                uint32_t adrp = 0x90000000u | (static_cast<uint32_t>(pages) & 3u) << 29 | (static_cast<uint32_t>(pages >> 2) & 0x7ffffu) << 5 | reference.reg;
                uint32_t add = 0x91000000u | static_cast<uint32_t>(tableAt & 0xfff) << 10 | static_cast<uint32_t>(reference.reg) << 5 | reference.reg;
                memcpy(code + instructionAt, &adrp, sizeof(adrp));
                memcpy(code + instructionAt + sizeof(adrp), &add, sizeof(add));
            }
        }
    }
    for (size_t at : stubsAt)
        memcpy(code + at, stubs.bytes.span().data(), stubs.bytes.size());

    Vector<uint32_t> functionInBucket;
    functionInBucket.fill(0, capacity);
    size_t recordAt = 0;
    for (size_t index = 0; index < m_functions.size(); ++index) {
        auto& function = m_functions[index];
        auto& info = function.code.info;
        auto [codeAt, stubsForThis] = placement[index];

        ImageFunction record { };
        RELEASE_ASSERT(startsOfFunctions[index] == codeAt);
        record.index = safeCast<uint32_t>(index);
        record.frame = frameOfFunction[index];
        record.numberOfParameters = info.convention.numberOfParameters;
        record.takesList = info.convention.signature == Signature::List;
        record.hasInlineFrames = !info.inlineFrames.isEmpty();
        record.callSites = callSitesOfFunction[index];
        record.numSlots = info.numSlots;
        record.numberOfCatchEntrypoints = safeCast<uint16_t>(info.catchEntrypoints.size());
        RELEASE_ASSERT(info.knownCallees.size() < 1u << 17);
        record.numberOfKnownCallees = info.knownCallees.size();
        record.hasSiteConstants = !numbersOfIdentifiers;
        record.usesStaticImports = info.usesStaticImports;
        record.startsCold = info.startsCold;
        record.quotes = quotesOfFunction[index];

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
            // The same function twice (the same text evaluated twice) is fine. Two different functions with the same key are not:
            // both are left out of the image, because a lookup could mean either.
            auto& earlier = m_functions[functionInBucket[bucket]].code.bytes;
            if (earlier.size() != function.code.bytes.size() || memcmp(earlier.span().data(), function.code.bytes.span().data(), earlier.size()))
                table[bucket].kind = std::numeric_limits<uint32_t>::max();
            continue;
        }
        table[bucket] = key;
        functionInBucket[bucket] = index;

        memcpy(records + recordAt, &record, sizeof(record));
        recordAt += sizeof(record);
        memcpy(records + recordAt, info.sites.span().data(), info.sites.size() * sizeof(Site));
        recordAt += info.sites.size() * sizeof(Site);
        if (!numbersOfIdentifiers) {
            memcpy(records + recordAt, info.siteConstants.span().data(), info.siteConstants.sizeInBytes());
            recordAt += info.siteConstants.sizeInBytes();
        }
        for (auto& callee : info.knownCallees) {
            uint32_t indexOfCallee = ImageFunction::noSuchFunction;
            for (unsigned bucket = callee.hash() & (capacity - 1); functionWithKey[bucket] != std::numeric_limits<uint32_t>::max(); bucket = (bucket + 1) & (capacity - 1)) {
                if (m_functions[functionWithKey[bucket]].key.sameFunction(callee)) {
                    indexOfCallee = functionWithKey[bucket];
                    break;
                }
            }
            memcpy(records + recordAt, &indexOfCallee, sizeof(indexOfCallee));
            recordAt += sizeof(indexOfCallee);
        }
        for (auto& [bytecodeOffset, codeOffset] : info.catchEntrypoints) {
            ImageCatchEntrypoint entrypoint { bytecodeOffset, codeOffset };
            memcpy(records + recordAt, &entrypoint, sizeof(entrypoint));
            recordAt += sizeof(entrypoint);
        }
        memcpy(records + recordAt, info.plans.span().data(), info.plans.sizeInBytes());
        recordAt += info.plans.sizeInBytes();

        memcpy(code + codeAt, function.code.bytes.span().data(), function.code.bytes.size());
        for (auto& reference : info.indexReferences)
            IndexReferences::fill(code + codeAt, reference, safeCast<uint32_t>(index));
        for (auto& call : info.stubCalls)
            retargetStubCall(code, codeAt + call.offset, stubsForThis + (call.thunk ? stubs.thunkOffsets[call.thunk - 1] : stubs.offsets[static_cast<unsigned>(call.stub)]), call.isTailCall);
    }

    // Now that every function has an address, link the direct calls between functions.
    for (size_t index = 0; index < m_functions.size(); ++index) {
        auto& info = m_functions[index].code.info;
        size_t codeAt = placement[index].first;
        for (auto& call : info.stubCalls) {
            if (call.function == StubCall::noFunction)
                continue;
            if (auto target = directTargetOf(info, call)) {
                size_t targetAt = placement[*target].first;
                size_t from = codeAt + call.offset;
                if (size_t veneer = farCallees[index].find(static_cast<uint32_t>(*target)); veneer != notFound) {
                    size_t veneerAt = codeAt + WTF::roundUpToMultipleOf<sizeof(uint32_t)>(m_functions[index].code.bytes.size()) + veneer * sizeOfVeneer;
                    writeVeneer(code, veneerAt, targetAt);
                    targetAt = veneerAt;
                }
                RELEASE_ASSERT((from > targetAt ? from - targetAt : targetAt - from) <= reachOfStubCall + roomToSpareInReachOfStubCall);
                retargetStubCall(code, from, targetAt, call.isTailCall);
                continue;
            }
        }
    }
    m_reportableSites = reportableSites();
    m_functions.clear();
    return image;
}

// ---- Running

namespace {

struct Registry {
    Lock lock;
    Vector<Image*, 2> images;
    std::atomic<bool> hasAny { false };
};

Registry& registry()
{
    static NeverDestroyed<Registry> instance;
    return instance;
}

} // anonymous namespace

Image* Image::registerImage(std::span<const uint8_t> data, const void* code)
{
    // Its code was compiled on the assumption that the intrinsics cannot change.
    if (!Options::useImmutableIntrinsics())
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
    auto* image = new Image(data, code);
    auto& all = registry();
    Locker locker { all.lock };
    // Compiled functions embed their own index, and address lookup uses a single table, so only one image may contain code.
    for (Image* other : all.images)
        RELEASE_ASSERT(!header.codeSize || !other->header().codeSize);
    all.images.append(image);
    all.hasAny.store(true, std::memory_order_release);
    if (header.numberOfSlotRanges)
        TypedLayoutTable::setSlotTypes({ image->at<uint32_t>(header.slotRangesOffset), header.numberOfSlotRanges }, image->at<TypedLayoutTable::FieldType>(header.slotTypesOffset));
    if (header.fieldRangesOffset)
        TypedLayoutTable::setFields(image->at<uint32_t>(header.fieldRangesOffset), image->at<TypedLayoutTable::Field>(header.fieldRecordsOffset), image->at<TypedLayoutTable::FieldType>(header.fieldTypesOffset), image->at<uint16_t>(header.fieldLayoutIDsOffset),
            image->at<uint8_t>(header.inlineSlotCountsOffset), image->at<uint32_t>(header.startOfFieldsOffset), image->at<uint32_t>(header.fieldsOffset), image->at<uint16_t>(header.layoutIDsByFieldIDOffset), Instance::convertToTypedLayout, header.auditsTypes);
    return image;
}

static Image* imageWithEnvironments()
{
    auto& all = registry();
    if (!all.hasAny.load(std::memory_order_acquire))
        return nullptr;
    for (unsigned i = 0; i < all.images.size(); ++i) {
        if (all.images[i]->header().environmentsSize)
            return all.images[i];
    }
    return nullptr;
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

Image* Image::withShapes()
{
    auto& all = registry();
    if (!all.hasAny.load(std::memory_order_acquire))
        return nullptr;
    for (unsigned i = 0; i < all.images.size(); ++i) {
        if (all.images[i]->header().numberOfShapes > 1)
            return all.images[i];
    }
    return nullptr;
}

uint32_t Image::selectorNamed(const StringImpl& name) const
{
    const uint32_t* inOrder = at<uint32_t>(header().selectorsInOrderOffset);
    const ImageSelector* all = at<ImageSelector>(header().selectorsOffset);
    const uint8_t* text = at<uint8_t>(header().textOfSelectorsOffset);
    UniquedStringImpl* const* identifiers = header().numberOfIdentifiersOfProgram ? StaticHeap::identifiersOfProgram() : nullptr;
    if (header().numberOfIdentifiersOfProgram && !identifiers)
        return 0;
    size_t low = 0;
    size_t high = header().numberOfSelectorsInOrder;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order;
        if (identifiers)
            order = compareSelectors(*identifiers[inOrder[middle]], name);
        else {
            const ImageSelector& entry = all[inOrder[middle]];
            order = compareSelectors(entry.is8Bit, { text + entry.text, static_cast<size_t>(entry.length) * (entry.is8Bit ? 1 : 2) }, name.is8Bit(), bytesOf(name));
        }
        if (!order)
            return inOrder[middle];
        if (order < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return 0;
}

uint32_t Image::environmentsSize()
{
    Image* image = imageWithEnvironments();
    return image ? image->header().environmentsSize : 0;
}

uint32_t Image::numberOfFunctionsOfImageWithEnvironments()
{
    Image* image = imageWithEnvironments();
    return image ? image->header().numberOfFunctions : 0;
}

ImageEnvironment Image::environmentOf(uint32_t moduleOfGraph)
{
    Image* image = imageWithEnvironments();
    if (!image || moduleOfGraph >= image->header().numberOfEnvironments)
        return { };
    return reinterpret_cast<const ImageEnvironment*>(image->m_data.data() + image->header().environmentsOffset)[moduleOfGraph];
}

bool Image::containsCode(const void* pointer)
{
    auto& all = registry();
    if (!all.hasAny.load(std::memory_order_acquire))
        return false;
    // (Images are never removed. This runs without the lock, because the thread that holds the lock may be suspended.)
    for (unsigned i = 0; i < all.images.size(); ++i) {
        Image* image = all.images[i];
        if (static_cast<const uint8_t*>(pointer) - static_cast<const uint8_t*>(image->m_code) < static_cast<ptrdiff_t>(image->header().codeSize) && pointer >= image->m_code)
            return true;
    }
    return false;
}

Image* Image::withCode()
{
    auto& all = registry();
    if (!all.hasAny.load(std::memory_order_acquire))
        return nullptr;
    // (Images are never removed. This runs without the lock, because the thread that holds the lock may be suspended.)
    for (unsigned i = 0; i < all.images.size(); ++i) {
        if (all.images[i]->header().codeSize)
            return all.images[i];
    }
    return nullptr;
}

// Cached for classifyAddress(), so that it needs no lookup. There is at most one image with code, and it is never unloaded.
static const ImageHeader* s_headerOfImageWithCode;
static uintptr_t s_codeOfImageWithCode;
static const uint32_t* s_startsOfFunctions;
static const uint32_t* s_granulesOfCode;

ImageAddressInfo classifyAddress(const void* address)
{
    if (!s_headerOfImageWithCode) [[unlikely]] {
        Image* image = Image::withCode();
        if (!image)
            return { };
        s_codeOfImageWithCode = std::bit_cast<uintptr_t>(image->code());
        s_startsOfFunctions = image->at<uint32_t>(image->header().startsOfFunctionsOffset);
        s_granulesOfCode = image->at<uint32_t>(image->header().granulesOfCodeOffset);
        WTF::storeStoreFence();
        s_headerOfImageWithCode = &image->header();
    }
    auto& header = *s_headerOfImageWithCode;
    uintptr_t offset = std::bit_cast<uintptr_t>(address) - s_codeOfImageWithCode;
    if (offset >= header.endOfFunctions)
        return { };
    for (unsigned i = 0; i < header.numberOfCopiesOfStubs; ++i) {
        uintptr_t inStubs = offset - header.copiesOfStubs[i];
        if (inStubs >= header.sizeOfStubs)
            continue;
        for (uint32_t inAdapter : header.returnsIntoAdapters) {
            if (inAdapter == inStubs)
                return { ImageAddressInfo::Adapter, 0, 0 };
        }
        return { ImageAddressInfo::Stub, 0, 0 };
    }
    const uint32_t* starts = s_startsOfFunctions;
    uint32_t index = s_granulesOfCode[offset >> shiftOfGranuleOfCode];
    while (starts[index + 1] <= offset)
        ++index;
    return { ImageAddressInfo::Function, index, static_cast<uint32_t>(offset - starts[index]) };
}

bool hasCode()
{
    return s_headerOfImageWithCode || Image::withCode();
}

static void skipInlineFrames(const uint8_t*& at, uint64_t first)
{
    if (!(first & 2))
        return;
    for (uint64_t count = readVarint(at) * 3; count--;)
        readVarint(at);
}

ImageInlineFrame inlineFrameOf(const ImageFunction& function, unsigned frame)
{
    RELEASE_ASSERT(function.hasInlineFrames && frame);
    Image& image = Image::of(function);
    const uint8_t* at = image.at<uint8_t>(image.header().callSitesOffset) + function.callSites;
    uint64_t first = readVarint(at);
    RELEASE_ASSERT(first & 2);
    uint64_t count = readVarint(at);
    RELEASE_ASSERT(frame <= count);
    ImageInlineFrame result { };
    for (unsigned i = 0; i < frame; ++i) {
        uint64_t parentAndIsTailCall = readVarint(at);
        result.parent = static_cast<uint32_t>(parentAndIsTailCall >> 1);
        result.isTailCall = parentAndIsTailCall & 1;
        result.callSite = static_cast<uint32_t>(readVarint(at));
        result.function = static_cast<uint32_t>(readVarint(at));
    }
    return result;
}

std::optional<uint32_t> tryCallSiteAt(const ImageFunction& function, uint32_t offsetOfReturnAddress)
{
    if (!function.callSites)
        return std::nullopt;
    Image& image = Image::of(function);
    const uint8_t* at = image.at<uint8_t>(image.header().callSitesOffset) + function.callSites;
    uint32_t offset = 0;
    int64_t site = 0;
    uint64_t first = readVarint(at);
    skipInlineFrames(at, first);
    for (uint64_t count = first >> 2; count--;) {
        offset += readVarint(at) * sizeof(uint32_t);
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

std::optional<uint32_t> siteOfSpread(const ImageFunction& function, uint32_t callSite, unsigned item)
{
    if (!function.callSites)
        return std::nullopt;
    Image& image = Image::of(function);
    const uint8_t* at = image.at<uint8_t>(image.header().callSitesOffset) + function.callSites;
    uint64_t first = readVarint(at);
    if (!(first & 1))
        return std::nullopt;
    skipInlineFrames(at, first);
    for (uint64_t count = first >> 2; count--;) {
        readVarint(at);
        readVarint(at);
    }
    for (uint64_t count = readVarint(at); count--;) {
        uint64_t itsCallSite = readVarint(at);
        uint64_t itsItem = readVarint(at);
        uint64_t site = readVarint(at);
        if (itsCallSite == callSite && itsItem == item)
            return static_cast<uint32_t>(site);
    }
    return std::nullopt;
}

uint32_t callSiteAt(const ImageFunction& function, uint32_t offsetOfReturnAddress)
{
    auto result = tryCallSiteAt(function, offsetOfReturnAddress);
    RELEASE_ASSERT(result); // The call at that address was not expected to need a call site.
    return *result;
}

Image* Image::registerImageFromFile(const char* path)
{
    auto contents = FileSystem::readEntireFile(String::fromUTF8(path));
    if (!contents)
        return nullptr;
    return registerImageCopyingCode(WTF::move(*contents));
}

// Returns an executable copy of the code, aligned to imagePageSize and never freed, or null. It does not come from ExecutableAllocator, so
// that an image can be used with the JIT off, as it is when its code is mapped from an executable file.
static void* copyToExecutableMemory(std::span<const uint8_t> code)
{
#if OS(DARWIN) || OS(LINUX)
    size_t size = WTF::roundUpToMultipleOf(WTF::pageSize(), code.size() + imagePageSize);
#if OS(DARWIN) && CPU(ARM64)
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#else
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (mapping == MAP_FAILED)
        return nullptr;
    void* result = reinterpret_cast<void*>(WTF::roundUpToMultipleOf<imagePageSize>(reinterpret_cast<uintptr_t>(mapping)));
#if OS(DARWIN) && CPU(ARM64)
    pthread_jit_write_protect_np(false);
    memcpy(result, code.data(), code.size());
    pthread_jit_write_protect_np(true);
#else
    memcpy(result, code.data(), code.size());
    if (mprotect(mapping, size, PROT_READ | PROT_EXEC)) {
        munmap(mapping, size);
        return nullptr;
    }
#endif
    MacroAssembler::cacheFlush(result, code.size());
    return result;
#else
    UNUSED_PARAM(code);
    return nullptr;
#endif
}

Image* Image::registerImageCopyingCode(Vector<uint8_t>&& contents)
{
    if (contents.size() < sizeof(ImageHeader))
        return nullptr;
    // The image and its code are never freed.
    auto* data = new Vector<uint8_t>(WTF::move(contents));
    auto& header = *reinterpret_cast<const ImageHeader*>(data->span().data());
    if (header.magic != imageMagic || header.codeOffset + header.codeSize > data->size())
        return nullptr;
    void* code = nullptr;
    if (header.codeSize) {
        code = copyToExecutableMemory(data->span().subspan(header.codeOffset, header.codeSize));
        if (!code)
            return nullptr;
    }
    return registerImage(data->span(), code);
}

bool Image::hasAny()
{
    return registry().hasAny.load(std::memory_order_acquire);
}

const void* Image::addressOfStub(Stub stub)
{
    if (!usesStubs || !hasAny())
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
        auto* text = image->at<uint8_t>(header.textOfRegExpsOffset);
        for (auto it = std::ranges::lower_bound(regExps, hash, { }, &ImageRegExp::hash); it != regExps.end() && it->hash == hash; ++it) {
            if (it->flags != rawFlags || it->length != pattern.length())
                continue;
            StringView said = it->is8Bit ? StringView { std::span { reinterpret_cast<const Latin1Character*>(text + it->text), it->length } } : StringView { std::span { reinterpret_cast<const char16_t*>(text + it->text), it->length } };
            if (said == pattern) {
                return CodeForRegExp { static_cast<const uint8_t*>(image->m_code) + it->codeFor8Bit, static_cast<const uint8_t*>(image->m_code) + it->codeFor16Bit };
            }
        }
    }
    return std::nullopt;
}

const ImageFunction* Image::lookup(const ImageKey& key) const
{
    auto& header = this->header();
    std::span<const ImageKey> table { reinterpret_cast<const ImageKey*>(m_data.data() + header.tableOffset), header.tableCapacity };
    if (table.empty())
        table = StaticHeap::keysOfImage();
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
    if (header.magic != imageMagic || header.stamp != imageStamp() || header.size > data.size() || !header.environmentsSize)
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
        size_t start = header.codeOffset + reinterpret_cast<const uint32_t*>(m_data.data() + header.startsOfFunctionsOffset)[function.index];
        auto whereItIsGoingToBe = [&](const void* pointer) { return m_address + (static_cast<const uint8_t*>(pointer) - m_data.data()); };
        return Function { EntryWord::encode(m_address + start, function.convention()), function.index,
            reinterpret_cast<const Site*>(whereItIsGoingToBe(function.sites())), reinterpret_cast<const ImageFunction*>(whereItIsGoingToBe(&function)), function.numSlots, !!function.startsCold, !!function.hasSiteConstants };
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

uint32_t ImageView::indexOfFunctionWith(const ImageKey& key) const
{
    auto& header = this->header();
    auto& function = *reinterpret_cast<const ImageFunction*>(m_data.data() + header.recordsOffset + key.record - 1);
    return function.index;
}

void* ImageView::addressOfStub(Stub stub) const
{
    return const_cast<uint8_t*>(m_address) + header().codeOffset + header().stubOffsets[static_cast<unsigned>(stub)];
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
    if (uint32_t id = provider.aotModuleID())
        return id;
    Image* image = Image::withCode();
    if (!image || !image->header().identifiesModulesByText)
        return 0;
    return moduleIDFromText(provider);
}

uint32_t moduleIDFromText(SourceProvider& provider)
{
    uint32_t id = provider.hash() | 1;
    provider.setAOTModuleID(id);
    return id;
}

ImageKey imageKeyForTopLevelCode(uint32_t module)
{
    ImageKey key;
    key.module = module;
    key.start = std::numeric_limits<uint32_t>::max();
    key.kind = 0xff << 1;
    return key;
}

std::optional<ImageKey> imageKeyFor(ScriptExecutable* scriptExecutable, CodeSpecializationKind kind)
{
    // An executable in the short form refers to its code directly, and has no key.
    if (scriptExecutable->isShortForm())
        return std::nullopt;
    SourceProvider* provider = scriptExecutable->source().provider();
    // The source of a default class constructor belongs to the engine, so use the provider of the code that contains the class.
    if (auto* function = dynamicDowncast<FunctionExecutable>(scriptExecutable); function && function->unlinkedExecutable()->isBuiltinDefaultClassConstructor() && function->topLevelExecutable())
        provider = function->topLevelExecutable()->source().provider();
    if (!provider)
        return std::nullopt;
    uint32_t module = moduleIDFor(*provider);
    if (!module)
        return std::nullopt;
    auto* executable = dynamicDowncast<FunctionExecutable>(scriptExecutable);
    if (!executable) {
        if (scriptExecutable->isEvalExecutable())
            return std::nullopt;
        return imageKeyForTopLevelCode(module);
    }
    auto functionKey = orderFunctionKey(*executable->unlinkedExecutable(), executable->source());
    if (!functionKey)
        return std::nullopt;
    ImageKey key;
    key.module = module;
    key.start = functionKey->start;
    key.kind = static_cast<uint32_t>(functionKey->kind) << 1 | (kind == CodeSpecializationKind::CodeForConstruct);
    return key;
}

unsigned hashOfCode(std::span<const uint8_t> code)
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

bool moduleIsLinkedAsCompiled(JSScope* scope)
{
    while (scope && scope->type() != ModuleEnvironmentType)
        scope = scope->next();
    auto* record = scope ? dynamicDowncast<JSModuleRecord>(uncheckedDowncast<JSModuleEnvironment>(scope)->moduleRecord()) : nullptr;
    return record && record->isLinkedAsInImage(scope->globalObject());
}

ImageCode findInImage(ScriptExecutable* executable, CodeSpecializationKind kind, UnlinkedCodeBlock* unlinkedCodeBlock, JSScope* scope)
{
    if (!Options::useAOT())
        return { };
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char* path = Options::aotImagePath()) {
            if (!Image::registerImageFromFile(path))
                dataLogLn("AOT: ", path, " is not an image for this engine");
        }
    });
    if (!Image::hasAny())
        return { };

    Image* image = nullptr;
    const ImageFunction* function = nullptr;
    ImageKey key;
    if (auto* ofFunction = dynamicDowncast<FunctionExecutable>(executable); ofFunction && ofFunction->aotEntryFor(kind) && !(kind == CodeSpecializationKind::CodeForConstruct && ofFunction->constructsByCalling())) {
        // An executable in the static heap records the index of its function.
        function = StaticHeap::imageFunctionOfFunction(ofFunction->aotIndexFor(kind));
        image = &Image::of(*function);
    } else {
        auto keyOfExecutable = imageKeyFor(executable, kind);
        if (!keyOfExecutable) {
            if (Options::verboseAOTCompilation()) [[unlikely]]
                dataLogLn("AOT: no key for ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL());
            return { };
        }
        key = *keyOfExecutable;
        std::tie(image, function) = Image::find(key);
        if (!function) {
            if (Options::verboseAOTCompilation()) [[unlikely]]
                dataLogLn("AOT: not in the image: ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL(), " (module ", key.module, " start ", key.start, " kind ", key.kind, ")");
            return { };
        }
    }

    // Compiled code finds its per-realm data by its function index, so each index belongs to one function per realm. The same text
    // evaluated a second time is a different function, with its own constants, and cannot share the index.
    if (Instance* instance = scope->realm()->aotInstance()) {
        uint32_t index = function->index;
        if (instance->isLinked(index) && FunctionRef { instance, index }.executable() != executable)
            return { };
        if (const FunctionInfo& info = instance->infos[index]; info.executable() && info.executable() != executable)
            return { };
    }

    // The code uses a table that only StaticHeap creates, and only for the realm that the program runs in.
    if (image->header().numberOfIdentifiersOfProgram) {
        uint32_t index = function->index;
        if (!Instance::ensure(scope->realm()).infos[index].sites)
            return { };
    }

    if (function->usesStaticImports) {
        if (!moduleIsLinkedAsCompiled(scope)) {
            if (Options::verboseAOTCompilation()) [[unlikely]]
                dataLogLn("AOT: the module of ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL(), " is not linked the way it was compiled for");
            return { };
        }
    }

    if (Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("AOT: ", nameForLogging(executable), " (module ", key.module, " start ", key.start, " kind ", key.kind, ") is at ", RawPointer(image->codeFor(*function)), " size ", image->sizeOfCodeOf(*function), " hash ", hashOfCode({ image->codeFor(*function), image->sizeOfCodeOf(*function) }));
    return { image, function };
}

Ref<JITCode> codeFromImage(ImageCode code, UnlinkedCodeBlock* unlinkedCodeBlock)
{
    auto [image, function] = code;
    return adoptRef(*new JITCode(const_cast<uint8_t*>(image->codeFor(*function)), *function, JITCode::entryBlockFor(unlinkedCodeBlock)));
}

bool canRunWithoutUnlinkedCode(JSGlobalObject* globalObject, ImageCode code)
{
    return !!FunctionRef { &Instance::ensure(globalObject), code.function->index }.metadata();
}

Ref<JITCode> codeOfFunctionFromImage(ImageCode code, CodeSpecializationKind kind)
{
    auto [image, function] = code;
    return adoptRef(*new JITCode(const_cast<uint8_t*>(image->codeFor(*function)), *function, isCall(kind) ? JITCode::Way::Call : JITCode::Way::Construct));
}

} // namespace AOT

bool isPCOfAOTImage(const void* pc)
{
    return AOT::Image::containsCode(pc);
}

bool registerAOTImage(std::span<const uint8_t> image, const void* code)
{
    return !!AOT::Image::registerImage(image, code);
}

static void compileAllIn(VM&, UnlinkedCodeBlock&, AOTCompileAllResult&);

static void compileAllOf(VM& vm, UnlinkedFunctionExecutable& executable, AOTCompileAllResult& result)
{
    auto [forCall, forConstruct] = executable.codeBlocksDecodingCached(vm);
    for (UnlinkedFunctionCodeBlock* codeBlock : { forCall, forConstruct }) {
        if (!codeBlock)
            continue;
        result.functions++;
        result.bytecodeBytes += codeBlock->instructionsSize();
        AOT::CompiledCode code;
        if (AOT::compileForImage(vm, codeBlock, code)) {
            result.compiled++;
            result.codeBytes += code.bytes.size();
        }
        compileAllIn(vm, *codeBlock, result);
    }
}

static void compileAllIn(VM& vm, UnlinkedCodeBlock& codeBlock, AOTCompileAllResult& result)
{
    for (unsigned i = 0; i < codeBlock.numberOfFunctionDecls(); ++i)
        compileAllOf(vm, *codeBlock.functionDecl(i), result);
    for (unsigned i = 0; i < codeBlock.numberOfFunctionExprs(); ++i)
        compileAllOf(vm, *codeBlock.functionExpr(i), result);
}

std::optional<AOTCompileAllResult> aotCompileAllFunctions(VM& vm, const SourceCode& source, bool isModule)
{
    DeferGC deferGC(vm);
    vm.keepUnlinkedCode();
    ParserError error;
    UnlinkedCodeBlock* codeBlock = isModule
        ? static_cast<UnlinkedCodeBlock*>(recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, source, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, std::numeric_limits<unsigned>::max(), OptimizeBytecode::Yes))
        : static_cast<UnlinkedCodeBlock*>(recursivelyGenerateUnlinkedCodeBlockForProgram(vm, source, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, std::numeric_limits<unsigned>::max(), OptimizeBytecode::Yes));
    std::optional<AOTCompileAllResult> result;
    if (!error.isValid() && codeBlock) {
        result = AOTCompileAllResult { };
        result->functions++;
        result->bytecodeBytes += codeBlock->instructionsSize();
        AOT::CompiledCode code;
        if (AOT::compileForImage(vm, codeBlock, code)) {
            result->compiled++;
            result->codeBytes += code.bytes.size();
        }
        compileAllIn(vm, *codeBlock, *result);
    }
    vm.stopKeepingUnlinkedCode();
    return result;
}

namespace {

class SourceCompiler {
public:
    SourceCompiler(VM& vm, uint32_t module)
        : m_vm(vm)
        , m_module(module)
    {
        m_builder.setIdentifiesModulesByText();
    }

    void add(AOT::ImageKey key, UnlinkedCodeBlock* codeBlock)
    {
        // An inner function of a function with both call and construct code blocks is reached twice.
        if (!m_keys.add({ key.start, key.kind }).isNewEntry)
            return;
        AOT::CompiledCode code;
        if (AOT::compileForImage(m_vm, codeBlock, code))
            m_builder.add(key, m_rank++, WTF::move(code));
    }

    void addFunctionsIn(UnlinkedCodeBlock& codeBlock, const SourceCode& source)
    {
        for (unsigned i = 0; i < codeBlock.numberOfFunctionDecls(); ++i)
            addFunction(*codeBlock.functionDecl(i), source);
        for (unsigned i = 0; i < codeBlock.numberOfFunctionExprs(); ++i)
            addFunction(*codeBlock.functionExpr(i), source);
    }

    Vector<uint8_t> finish() { return m_builder.numberOfFunctions() ? m_builder.finish() : Vector<uint8_t> { }; }

private:
    void addFunction(UnlinkedFunctionExecutable& executable, const SourceCode& parentSource)
    {
        SourceCode source = executable.linkedSourceCode(parentSource);
        auto functionKey = orderFunctionKey(executable, executable.isBuiltinDefaultClassConstructor() ? parentSource : source);
        auto [forCall, forConstruct] = executable.codeBlocksDecodingCached(m_vm);
        for (bool isConstruct : { false, true }) {
            UnlinkedFunctionCodeBlock* codeBlock = isConstruct ? forConstruct : forCall;
            if (!codeBlock)
                continue;
            if (functionKey)
                add({ m_module, functionKey->start, static_cast<uint32_t>(functionKey->kind) << 1 | isConstruct, 0 }, codeBlock);
            addFunctionsIn(*codeBlock, source);
        }
    }

    VM& m_vm;
    uint32_t m_module;
    uint64_t m_rank { 0 };
    AOT::ImageBuilder m_builder;
    using KeyTraits = PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    UncheckedKeyHashSet<std::pair<uint32_t, uint32_t>, DefaultHash<std::pair<uint32_t, uint32_t>>, KeyTraits> m_keys;
};

} // anonymous namespace

bool aotCompileAndRegisterImage(VM& vm, const SourceCode& source, bool isModule)
{
    // The runtime supports one image with code per process.
    if (!AOT::usesStubs || AOT::Image::withCode())
        return false;
    Vector<uint8_t> image;
    {
        DeferGC deferGC(vm);
        vm.keepUnlinkedCode();
        ParserError error;
        UnlinkedCodeBlock* codeBlock = isModule
            ? static_cast<UnlinkedCodeBlock*>(recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, source, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None))
            : static_cast<UnlinkedCodeBlock*>(recursivelyGenerateUnlinkedCodeBlockForProgram(vm, source, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None));
        if (!error.isValid() && codeBlock) {
            uint32_t module = AOT::moduleIDFromText(*source.provider());
            SourceCompiler compiler(vm, module);
            compiler.add(AOT::imageKeyForTopLevelCode(module), codeBlock);
            compiler.addFunctionsIn(*codeBlock, source);
            image = compiler.finish();
        }
        vm.stopKeepingUnlinkedCode();
    }
    return !image.isEmpty() && AOT::Image::registerImageCopyingCode(WTF::move(image));
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

std::optional<unsigned> aotImageNumberOfRegExps(std::span<const uint8_t> image)
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

} // namespace JSC

#endif // ENABLE(FTL_JIT)
