/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImage.h"

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
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SHA1.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageBuilder);
WTF_MAKE_TZONE_ALLOCATED_IMPL(Image);

uint64_t imageStamp()
{
    // The code has the layout of the engine's objects in it, and the order of the runtime table. Not proof against every change
    // (an image belongs in the executable that has the engine in it), but it catches the ones that come from mixing builds.
    uint64_t stamp = computeJSCBytecodeCacheVersion();
    auto mix = [&](uint64_t value) {
        stamp = (stamp ^ value) * 0x100000001b3ULL;
        stamp ^= stamp >> 29;
    };
    mix(numberOfEntries);
    mix(Instance::offsetOfData());
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
    mix(Options::useImmutableIntrinsics()); // What code takes for granted.
#if CPU(ARM64)
    mix(1 | MacroAssemblerARM64::supportsDoubleToInt32ConversionUsingJavaScriptSemantics() << 8);
#elif CPU(X86_64)
    mix(2);
#endif
    return stamp;
}

// The order of ImageHeader::selectorsInOrderOffset.
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

// ---- What error messages quote, of a program that goes without its text

static constexpr unsigned longestQuoteKeptWhole = 64;
static constexpr unsigned longestCalleeKept = 96;
static constexpr unsigned lengthOfTheEndsOfALongQuote = 40;

// In "f.g(a, (b))": where the "(" is that goes with the ")" at the end. As functionCallBase() finds it (ExceptionHelpers.cpp).
static size_t whereArgumentsStart(StringView text)
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
        // As appendSourceToErrorMessage() goes about it (ErrorInstance.cpp).
        auto entry = codeBlock->expressionInfoForBytecodeIndex(BytecodeIndex(offset));
        int divot = entry.divot + sourceOffset;
        int start = divot - entry.startOffset;
        int stop = divot + entry.endOffset;
        if (stop <= 0 || start < 0 || start > length || stop > length)
            continue;
        Quote quote { offset, static_cast<uint32_t>(start), Quote::Exact, { } };
        if (start < stop) {
            StringView said = text.substring(start, stop - start);
            if (said.length() <= longestQuoteKeptWhole)
                quote.text = said.utf8();
            else if (size_t open = whereArgumentsStart(said); open != notFound && open < longestCalleeKept) {
                quote.kind = Quote::Call;
                quote.text = said.left(open + 1).utf8();
            } else {
                quote.start = std::numeric_limits<uint32_t>::max();
                quote.text = makeString(said.left(lengthOfTheEndsOfALongQuote), "..."_s, said.right(lengthOfTheEndsOfALongQuote)).utf8();
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

// How many there are. Then for each, in order: how much further on in the bytecode it is than the one before; how much further on in
// the text of quotes it starts than the one before did, which may be less than nothing; and how long it is, with its Quote::Kind.
std::optional<std::pair<String, bool>> Image::quoteAt(const ImageFunction& function, unsigned bytecodeOffset) const
{
    if (!function.quotes)
        return std::nullopt;
    const uint8_t* at = this->at<uint8_t>(header().quotesOffset) + function.quotes;
    uint64_t offset = 0;
    int64_t start = 0;
    for (uint64_t count = readVarint(at); count--;) {
        offset += readVarint(at);
        uint64_t step = readVarint(at);
        start += static_cast<int64_t>(step >> 1) ^ -static_cast<int64_t>(step & 1);
        uint64_t lengthAndKind = readVarint(at);
        if (offset < bytecodeOffset)
            continue;
        if (offset > bytecodeOffset)
            break;
        String text = String::fromUTF8(std::span { this->at<char8_t>(header().textOfQuotesOffset) + start, static_cast<size_t>(lengthAndKind >> 2) });
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

// After what quoteAt() goes by. How many there are. Then for each, in order, how much further on in the bytecode it is than the one before.
bool Image::constructsAt(const ImageFunction& function, unsigned bytecodeOffset) const
{
    if (!function.quotes)
        return false;
    const uint8_t* at = this->at<uint8_t>(header().quotesOffset) + function.quotes;
    for (uint64_t count = readVarint(at); count--;) {
        readVarint(at);
        readVarint(at);
        readVarint(at);
    }
    uint64_t offset = 0;
    for (uint64_t count = readVarint(at); count--;) {
        offset += readVarint(at);
        if (offset >= bytecodeOffset)
            return offset == bytecodeOffset;
    }
    return false;
}

void ImageBuilder::add(ImageKey key, uint64_t rank, CompiledCode&& code)
{
    // Where they pointed depends on where the code was when it was compiled, which is nothing to do with the code.
    for (auto& call : code.info.stubCalls)
        memset(code.bytes.mutableSpan().data() + call.offset, 0, sizeof(uint32_t));
    Locker locker { m_lock };
    m_functions.append(Function { key, rank, WTF::move(code) });
}

bool ImageBuilder::addRegExp(VM& vm, const String& pattern, OptionSet<Yarr::Flags> flags)
{
    Locker locker { m_lock };
    flags = ImageRegExp::flagsThatMatter(flags);
    auto asked = m_regExpsAsked.add(makeString(flags.toRaw(), '/', pattern), false);
    if (!asked.isNewEntry)
        return asked.iterator->value;
    RegExpCode result { pattern, flags, { } };
    for (auto charSize : { Yarr::CharSize::Char8, Yarr::CharSize::Char16 }) {
        // As RegExp::compile() goes about it.
        Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
        Yarr::YarrPattern yarrPattern(pattern, flags, error);
        if (Yarr::hasError(error) || yarrPattern.containsUnsignedLengthPattern() || (yarrPattern.m_containsLookbehinds && !Options::useRegExpLookbehindJIT()))
            return false;
        // (Then it is looked for the way any string is, and no code is run.)
        if (!yarrPattern.m_atom.isNull())
            return false;
        auto code = Yarr::jitCompileForImage(yarrPattern, pattern, charSize, &vm, Yarr::ExecutionMode::IncludeSubpatterns);
        if (!code)
            return false;
        result.code[charSize == Yarr::CharSize::Char16] = WTF::move(*code);
    }
    m_regExps.append(WTF::move(result));
    m_regExpsAsked.set(makeString(flags.toRaw(), '/', pattern), true);
    return true;
}

Vector<uint8_t> ImageBuilder::finish()
{
    Locker locker { m_lock };
    std::ranges::sort(m_functions, [](const Function& a, const Function& b) {
        return a.rank < b.rank;
    });

    if (Options::aotReportStats()) [[unlikely]] {
        // TEMPORARY-FOLDING-STATS: how much of the code is the same as some other function's, but for where it is and what it calls
        // by way of an address (which has to be the same thing).
        struct Group {
            uint64_t count { 0 };
            uint64_t size { 0 };
        };
        UncheckedKeyHashMap<String, Group> groups;
        uint64_t total = 0;
        for (auto& function : m_functions) {
            Vector<uint8_t> bytes = function.code.bytes;
            auto& info = function.code.info;
            for (auto& call : info.stubCalls) {
                uint32_t what = static_cast<uint32_t>(call.stub) << 8 | call.thunk << 16 | call.isTailCall;
                memcpy(bytes.mutableSpan().data() + call.offset, &what, sizeof(what));
                if (call.function != StubCall::noFunction) {
                    const ImageKey& key = info.knownCallees[call.function];
                    uint32_t words[3] = { key.module, key.start, key.kind };
                    bytes.append(std::span { reinterpret_cast<const uint8_t*>(words), sizeof(words) });
                }
            }
            total += function.code.bytes.size();
            SHA1 sha1;
            sha1.addBytes(bytes.span());
            SHA1::Digest digest;
            sha1.computeHash(digest);
            auto& group = groups.add(String { std::span { reinterpret_cast<const Latin1Character*>(digest.data()), digest.size() } }, Group { }).iterator->value;
            group.count++;
            group.size = function.code.bytes.size();
        }
        uint64_t duplicates = 0, saved = 0, bySize[6] = { }, savedBySize[6] = { };
        Vector<Group> largest;
        for (auto& entry : groups) {
            auto& group = entry.value;
            if (group.count < 2)
                continue;
            duplicates += group.count - 1;
            // (Each still needs a header, and a jump.)
            uint64_t each = group.size > 32 ? group.size - 32 : 0;
            saved += (group.count - 1) * each;
            unsigned bucket = group.size <= 128 ? 0 : group.size <= 256 ? 1 : group.size <= 512 ? 2 : group.size <= 1024 ? 3 : group.size <= 4096 ? 4 : 5;
            bySize[bucket] += group.count - 1;
            savedBySize[bucket] += (group.count - 1) * each;
            largest.append(group);
        }
        dataLogLn("FOLDING ", m_functions.size(), " functions, ", total, " bytes; ", groups.size(), " distinct; ", duplicates, " are copies, ", saved, " bytes to be had");
        static constexpr ASCIILiteral names[] = { "<=128"_s, "<=256"_s, "<=512"_s, "<=1024"_s, "<=4096"_s, ">4096"_s };
        for (unsigned i = 0; i < 6; ++i)
            dataLogLn("FOLDING   size ", names[i], ": ", bySize[i], " copies, ", savedBySize[i], " bytes");
        std::ranges::sort(largest, [](auto& a, auto& b) { return a.count * a.size > b.count * b.size; });
        for (unsigned i = 0; i < std::min<size_t>(largest.size(), 8); ++i)
            dataLogLn("FOLDING   a group of ", largest[i].count, " of ", largest[i].size, " bytes");
    }

    unsigned capacity = 16;
    while (capacity * 3 < m_functions.size() * 4)
        capacity *= 2;

    // The shapes that objects are made with and the names that properties are read by, numbered for the whole program in the order
    // they turn up in.
    // Or, if the identifiers of the program are numbered, by those numbers: then a site that says what it reads has said this too.
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
    };
    Vector<Shape> shapes(1);
    UncheckedKeyHashMap<String, uint32_t> numberOfShape;
    BitVector selectorIsRead;
    for (auto& function : m_functions) {
        auto& info = function.code.info;
        RELEASE_ASSERT(info.siteConstants.size() == info.numSlots);
        for (unsigned slot = 0; slot < info.numSlots; ++slot) {
            uint32_t& constant = info.siteConstants[slot];
            if (!constant)
                continue;
            // What makes objects has no identifier to say.
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
            Vector<uint32_t, 16> words { known.inlineCapacity };
            words.appendVector(shape.names);
            String key { std::span { reinterpret_cast<const Latin1Character*>(words.span().data()), words.size() * sizeof(uint32_t) } };
            // (A Structure has sixteen bits to say which in. The rest are made the way they would be if nobody had noticed.)
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

    // The dispatch table. The longest rows first, each as near the start as it fits.
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
                int32_t location = i < shape.inlineCapacity || !shape.inlineCapacity
                    ? static_cast<int32_t>(JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + i)
                    : -static_cast<int32_t>(i - shape.inlineCapacity) - 2;
                rows[shape.names[i]].append({ number, location });
            }
        }
        Vector<uint32_t> order;
        for (uint32_t selector = 1; selector < selectors.size(); ++selector) {
            if (!rows[selector].isEmpty())
                order.append(selector);
        }
        // (Of those with one entry, in the order of the shapes: then finding each a place goes on from where the last one was found.)
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
        // Any row can be asked about any shape.
        size_t furthest = 0;
        for (uint32_t row : rowOfSelector)
            furthest = std::max<size_t>(furthest, row);
        growWithZeros(furthest + shapes.size());
    }
    Vector<ImageShape> imageShapes;
    size_t numberOfPropertiesOfShapes = 0;
    for (auto& shape : shapes) {
        imageShapes.append({ safeCast<uint16_t>(shape.names.size()), safeCast<uint16_t>(shape.inlineCapacity) });
        numberOfPropertiesOfShapes += shape.names.size();
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
    if (Options::aotReportStats()) [[unlikely]]
        dataLogLn("AOT: ", shapes.size() - 1, " shapes with ", numberOfPropertiesOfShapes, " properties, ", selectorsInOrder.size(), " selectors of which ", selectorIsRead.bitCount(), " are read by, ", dispatch.size(), " entries in the dispatch table");

    // Which function a call is to: the first that has the key.
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
        // (Whoever compiled the program has seen to it that there is code for what is called like that.)
        RELEASE_ASSERT_NOT_REACHED();
        return std::nullopt;
    };

    // Where everything goes. The stubs come first, and again whenever the last copy is about to be out of reach.
    const StubBlob& stubs = stubBlob();
    Vector<size_t> stubsAt;
    Vector<std::pair<size_t, size_t>> placement; // Of each function: where it is, and where the stubs it calls are.
    // After each function's code, a veneer for each function that it calls directly and that is out of reach. Which those are
    // depends on where everything goes: so that is worked out without any first, and they are few enough not to change the answer.
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
            if (lastStubs == std::numeric_limits<size_t>::max() || end + function.code.bytes.size() - lastStubs > reachOfStubCall) {
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
                if (((from > to ? from - to : to - from) <= reachOfStubCall && !Options::aotForceVeneers()) || farCallees[index].contains(static_cast<uint32_t>(*target)))
                    continue;
                farCallees[index].append(static_cast<uint32_t>(*target));
                sizeOfVeneers += sizeOfVeneer + sizeof(uint32_t) + imageStubsAlignment;
            }
        }
        RELEASE_ASSERT(sizeOfVeneers + stubs.bytes.size() + imageStubsAlignment < roomToSpareInReachOfStubCall);
        if (Options::aotReportStats()) [[unlikely]]
            dataLogLn("AOT: at most ", sizeOfVeneers, " bytes of veneers for calls out of reach");
    }
    size_t recordsSize = 0;
    size_t codeSize = 0;
    for (size_t indexOfFunction = 0; indexOfFunction < m_functions.size(); ++indexOfFunction) {
        auto& function = m_functions[indexOfFunction];
        recordsSize += sizeof(ImageFunction) + function.code.info.catchEntrypoints.size() * sizeof(ImageCatchEntrypoint) + function.code.info.sites.size() * (sizeof(Site) + (numbersOfIdentifiers ? 0 : sizeof(uint32_t))) + function.code.info.knownCallees.size() * sizeof(uint32_t) + function.code.info.plans.sizeInBytes();
        RELEASE_ASSERT(function.code.info.sites.size() == function.code.info.numSlots);
        codeSize = WTF::roundUpToMultipleOf<imageFunctionAlignment>(codeSize);
        if (usesStubs && (stubsAt.isEmpty() || codeSize + sizeWithVeneers(indexOfFunction) - stubsAt.last() > reachOfStubCall)) {
            codeSize = WTF::roundUpToMultipleOf<imageStubsAlignment>(codeSize);
            stubsAt.append(codeSize);
            codeSize = WTF::roundUpToMultipleOf<imageFunctionAlignment>(codeSize + stubs.bytes.size());
        }
        placement.append({ codeSize, stubsAt.isEmpty() ? 0 : stubsAt.last() });
        if (Options::aotLogsFacts()) [[unlikely]]
            dataLogLn("PLACED @", m_functions[placement.size() - 1].key.module, ":", m_functions[placement.size() - 1].key.start, ":", m_functions[placement.size() - 1].key.kind, " ", codeSize);
        codeSize += sizeWithVeneers(indexOfFunction);
    }
    size_t endOfFunctions = codeSize;
    RELEASE_ASSERT(stubsAt.size() <= mostCopiesOfStubsInImage);

    // Which function an address is in.
    Vector<uint32_t> startsOfFunctions;
    for (auto& [at, stubsForIt] : placement)
        startsOfFunctions.append(safeCast<uint32_t>(at));
    startsOfFunctions.append(std::numeric_limits<uint32_t>::max());
    Vector<uint32_t> granulesOfCode;
    {
        uint32_t function = 0;
        for (size_t start = 0; start < endOfFunctions; start += 1 << shiftOfGranuleOfCode) {
            while (startsOfFunctions[function + 1] <= start)
                ++function;
            granulesOfCode.append(function);
        }
    }
    // And where in its bytecode a function is that is going to be returned to at an address: see callSiteAt().
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
        appendVarint(callSites, all.size() << 1 | !sitesOfSpreads.isEmpty());
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

    // The code of regular expressions comes last, after one copy of each table that any number of them use.
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
        if (Options::aotReportStats()) [[unlikely]] {
            size_t sizes[2] = { };
            for (auto& regExp : m_regExps) {
                for (unsigned i = 0; i < 2; ++i)
                    sizes[i] += regExp.code[i].bytes.size();
            }
            dataLogLn("AOT: ", m_regExps.size(), " regular expressions of ", m_regExpsAsked.size(), " have code: ", sizes[0], " bytes for 8 bit strings, ", sizes[1], " for 16 bit ones, ", sizeOfTables, " of tables in common, ", textOfRegExps.size() + imageRegExps.sizeInBytes(), " to find them by");
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
            // Expressions are made of expressions: what is said in the middle of something else that is kept is there already.
            // (Where that is can only be told from where it was in the source if a character is a byte.)
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
        if (Options::aotReportStats()) [[unlikely]]
            dataLogLn("AOT: ", numberOfQuotes, " places that an error message may quote: ", quotes.size(), " bytes, and ", textOfQuotes.size(), " of text in ", whereItIs.size(), " pieces");
    }

    // Code that goes by the tables of a static heap is no use without one, and that says which function nearly every executable is.
    // So it is for whoever makes it to keep the keys of the rest (StaticHeap::keysOfImage()): the table comes after the image.
    bool keysAreLeftOut = !!m_numberOfIdentifiersOfProgram;
    header.tableCapacity = keysAreLeftOut ? 0 : capacity;
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
    header.selectorsOffset = place(imageSelectors.sizeInBytes());
    header.numberOfSelectors = selectors.size();
    header.rowsOfSelectorsOffset = place(rowOfSelector.sizeInBytes());
    header.textOfSelectorsOffset = place(textOfSelectors.size());
    header.selectorsInOrderOffset = place(selectorsInOrder.sizeInBytes());
    header.numberOfSelectorsInOrder = selectorsInOrder.size();
    header.dispatchOffset = place(dispatch.sizeInBytes());
    header.quotesOffset = place(quotes.size());
    header.textOfQuotesOffset = place(textOfQuotes.size());
    header.numberOfIdentifiersOfProgram = m_numberOfIdentifiersOfProgram;
    header.numberOfConstantsOfProgram = m_numberOfConstantsOfProgram;
    header.regExpsOffset = place(imageRegExps.sizeInBytes());
    header.numberOfRegExps = imageRegExps.size();
    header.textOfRegExpsOffset = place(textOfRegExps.size());
    header.startsOfFunctionsOffset = place(startsOfFunctions.sizeInBytes());
    header.granulesOfCodeOffset = place(granulesOfCode.sizeInBytes());
    header.callSitesOffset = place(callSites.size());
    header.endOfFunctions = safeCast<uint32_t>(endOfFunctions);
    header.sizeOfStubs = safeCast<uint32_t>(stubs.bytes.size());
    header.numberOfCopiesOfStubs = stubsAt.size();
    for (unsigned i = 0; i < stubsAt.size(); ++i)
        header.copiesOfStubs[i] = safeCast<uint32_t>(stubsAt[i]);
    RELEASE_ASSERT(stubs.returnsIntoAdapters.size() == numberOfAdapters);
    for (unsigned i = 0; i < numberOfAdapters; ++i)
        header.returnsIntoAdapters[i] = stubs.returnsIntoAdapters[i];
    header.hashOfIntrinsics = Options::useImmutableIntrinsics() && ImmutableIntrinsics::shared() ? ImmutableIntrinsics::shared()->hash() : 0;
    header.dispatchSize = safeCast<uint32_t>(dispatch.size());
    header.codeOffset = WTF::roundUpToMultipleOf<imagePageSize>(endOfTables);
    header.codeSize = codeSize;
    header.size = WTF::roundUpToMultipleOf<imagePageSize>(header.codeOffset + codeSize);
    header.numberOfFunctions = m_functions.size();
    for (unsigned i = 0; i < numberOfStubs; ++i)
        header.stubOffsets[i] = stubs.offsets[i];
    if (Options::aotReportStats()) [[unlikely]] {
        // TEMPORARY-IMAGE-STATS
        size_t heads = 0, calleeSaves = 0, catchEntrypoints = 0, sites = 0, siteConstants = 0, knownCallees = 0, plans = 0, code = 0, sitesWithConstant = 0;
        for (auto& function : m_functions) {
            auto& info = function.code.info;
            heads += sizeof(ImageFunction);
            catchEntrypoints += info.catchEntrypoints.size() * sizeof(ImageCatchEntrypoint);
            sites += info.sites.size() * sizeof(Site);
            siteConstants += numbersOfIdentifiers ? 0 : info.sites.size() * sizeof(uint32_t);
            for (uint32_t constant : info.siteConstants)
                sitesWithConstant += !!constant;
            knownCallees += info.knownCallees.size() * sizeof(uint32_t);
            plans += info.plans.sizeInBytes();
            code += function.code.bytes.size();
        }
        dataLogLn("IMAGE: table of keys ", header.tableCapacity * sizeof(ImageKey), " (", m_functions.size(), " of ", capacity, " used); records ", recordsSize, ": heads ", heads, ", callee saves ", calleeSaves, ", catch entrypoints ", catchEntrypoints,
            ", sites ", sites, ", site constants ", siteConstants, " (", sitesWithConstant, " are not zero), known callees ", knownCallees, ", plans ", plans);
        dataLogLn("IMAGE: shapes ", imageShapes.sizeInBytes(),  ", selectors ", imageSelectors.sizeInBytes() + rowOfSelector.sizeInBytes() + selectorsInOrder.sizeInBytes(), ", their text ", textOfSelectors.size(),
            ", dispatch ", dispatch.sizeInBytes(), ", quotes and construct sites ", quotes.size(), ", text of quotes ", textOfQuotes.size(), "; code ", codeSize, ", of which the functions' own ", code, " and ", stubsAt.size(), " copies of ", stubs.bytes.size(), " bytes of stubs");
    }

    Vector<uint8_t> image;
    image.fill(0, header.size + (keysAreLeftOut ? capacity * sizeof(ImageKey) : 0));
    uint8_t* base = image.mutableSpan().data();
    memcpy(base, &header, sizeof(header));
    auto* table = reinterpret_cast<ImageKey*>(base + (keysAreLeftOut ? header.size : header.tableOffset));
    uint8_t* records = base + header.recordsOffset;
    uint8_t* code = base + header.codeOffset;

    memcpy(base + header.environmentsOffset, m_environments.span().data(), m_environments.size() * sizeof(ImageEnvironment));
    memcpy(base + header.shapesOffset, imageShapes.span().data(), imageShapes.sizeInBytes());
    memcpy(base + header.selectorsOffset, imageSelectors.span().data(), imageSelectors.sizeInBytes());
    memcpy(base + header.rowsOfSelectorsOffset, rowOfSelector.span().data(), rowOfSelector.sizeInBytes());
    memcpy(base + header.textOfSelectorsOffset, textOfSelectors.span().data(), textOfSelectors.size());
    memcpy(base + header.selectorsInOrderOffset, selectorsInOrder.span().data(), selectorsInOrder.sizeInBytes());
    memcpy(base + header.dispatchOffset, dispatch.span().data(), dispatch.sizeInBytes());
    memcpy(base + header.quotesOffset, quotes.span().data(), quotes.size());
    memcpy(base + header.textOfQuotesOffset, textOfQuotes.span().data(), textOfQuotes.size());
    memcpy(base + header.regExpsOffset, imageRegExps.span().data(), imageRegExps.sizeInBytes());
    memcpy(base + header.textOfRegExpsOffset, textOfRegExps.span().data(), textOfRegExps.size());
    memcpy(base + header.startsOfFunctionsOffset, startsOfFunctions.span().data(), startsOfFunctions.sizeInBytes());
    memcpy(base + header.granulesOfCodeOffset, granulesOfCode.span().data(), granulesOfCode.sizeInBytes());
    memcpy(base + header.callSitesOffset, callSites.span().data(), callSites.size());
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
            // The code starts on a page boundary wherever it is, so which page of it something is on is as good as an address.
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
        record.codeOffset = codeAt;
        record.codeSize = function.code.bytes.size();
        RELEASE_ASSERT(!(info.frameSizeInBytes % stackAlignmentBytes()));
        record.index = safeCast<uint32_t>(index);
        record.numberOfParameters = info.convention.numberOfParameters;
        record.takesList = info.convention.signature == Signature::List;
        record.callSites = callSitesOfFunction[index];
        record.frameSizeInUnits = safeCast<uint16_t>(info.frameSizeInBytes / stackAlignmentBytes());
        record.numSlots = info.numSlots;
        uint64_t calleeSaveRegisters = 0;
        for (unsigned i = 0; i < info.calleeSaveRegisters.registerCount(); ++i) {
            const RegisterAtOffset& entry = info.calleeSaveRegisters.at(i);
            RELEASE_ASSERT(entry.reg().index() < 64 && entry.offset() == info.calleeSaveRegisters.at(0).offset() + static_cast<ptrdiff_t>(i * sizeof(CPURegister)) && (!i || entry.reg().index() > info.calleeSaveRegisters.at(i - 1).reg().index()));
            calleeSaveRegisters |= 1ULL << entry.reg().index();
        }
        if (info.calleeSaveRegisters.registerCount()) {
            ptrdiff_t offset = info.calleeSaveRegisters.at(0).offset();
            RELEASE_ASSERT(offset < 0 && !(offset % static_cast<ptrdiff_t>(sizeof(CPURegister))));
            record.whereCalleeSavesStart = safeCast<uint16_t>(-offset / static_cast<ptrdiff_t>(sizeof(CPURegister)));
        }
        record.calleeSaveRegisters = ImageFunction::packRegisters(calleeSaveRegisters);
        record.numberOfCatchEntrypoints = info.catchEntrypoints.size();
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
            // The same function again (the same text evaluated twice) is fine. Two functions that cannot be told apart are not:
            // neither is in the image then, since either could be asked for.
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

    // With every function in its place: the calls from one to another.
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
    // Its functions know what their numbers are, and what an address is in is looked up in one place.
    for (Image* other : all.images)
        RELEASE_ASSERT(!header.codeSize || !other->header().codeSize);
    all.images.append(image);
    all.hasAny.store(true, std::memory_order_release);
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
    // (Nothing is ever taken out, and this may be asked while the thread that has the lock is stopped.)
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
    // (Nothing is ever taken out, and this may be asked while the thread that has the lock is stopped.)
    for (unsigned i = 0; i < all.images.size(); ++i) {
        if (all.images[i]->header().codeSize)
            return all.images[i];
    }
    return nullptr;
}

// What whatIsAt() goes by, where it takes no finding. There is one image with code in it, for good.
static const ImageHeader* s_headerOfImageWithCode;
static uintptr_t s_codeOfImageWithCode;
static const uint32_t* s_startsOfFunctions;
static const uint32_t* s_granulesOfCode;

WhatIsAt whatIsAt(const void* address)
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
                return { WhatIsAt::Adapter, 0, 0 };
        }
        return { WhatIsAt::Stub, 0, 0 };
    }
    const uint32_t* starts = s_startsOfFunctions;
    uint32_t index = s_granulesOfCode[offset >> shiftOfGranuleOfCode];
    while (starts[index + 1] <= offset)
        ++index;
    return { WhatIsAt::Function, index, static_cast<uint32_t>(offset - starts[index]) };
}

bool hasCode()
{
    return s_headerOfImageWithCode || Image::withCode();
}

std::optional<uint32_t> tryCallSiteAt(const ImageFunction& function, uint32_t offsetOfReturnAddress)
{
    if (!function.callSites)
        return std::nullopt;
    Image& image = Image::of(function);
    const uint8_t* at = image.at<uint8_t>(image.header().callSitesOffset) + function.callSites;
    uint32_t offset = 0;
    int64_t site = 0;
    for (uint64_t count = readVarint(at) >> 1; count--;) {
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
    for (uint64_t count = first >> 1; count--;) {
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
    RELEASE_ASSERT(result); // Nobody was to ask about what is called from there.
    return *result;
}

Image* Image::registerImageFromFile(const char* path)
{
    auto contents = FileSystem::readEntireFile(String::fromUTF8(path));
    if (!contents || contents->size() < sizeof(ImageHeader))
        return nullptr;
    // Both for good.
    auto* data = new Vector<uint8_t>(WTF::move(*contents));
    auto& header = *reinterpret_cast<const ImageHeader*>(data->span().data());
    if (header.magic != imageMagic || header.codeOffset + header.codeSize > data->size())
        return nullptr;
    void* code = nullptr;
    if (header.codeSize) {
        // (On a page boundary, as it is when it is mapped: some of it goes by that.)
        RefPtr<ExecutableMemoryHandle> handle = ExecutableAllocator::singleton().allocate(header.codeSize + imagePageSize, JITCompilationCanFail);
        if (!handle)
            return nullptr;
        code = reinterpret_cast<void*>(WTF::roundUpToMultipleOf<imagePageSize>(reinterpret_cast<uintptr_t>(handle->start().untaggedPtr())));
        performJITMemcpy<jitMemcpyRepatch>(code, data->span().data() + header.codeOffset, header.codeSize);
        MacroAssembler::cacheFlush(code, header.codeSize);
        auto* forGood = handle.leakRef();
        UNUSED_VARIABLE(forGood);
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

static std::atomic<uint64_t> s_regExpsFromImage;
static std::atomic<uint64_t> s_regExpsNotInImage;

auto Image::codeForRegExp(const String& pattern, OptionSet<Yarr::Flags> flags) -> std::optional<CodeForRegExp>
{
    if (!hasAny() || !Options::aotCompileRegExps())
        return std::nullopt;
    uint32_t hash = ImageRegExp::hashOf(pattern, flags);
    uint32_t rawFlags = ImageRegExp::flagsThatMatter(flags).toRaw();
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
                s_regExpsFromImage++;
                return CodeForRegExp { static_cast<const uint8_t*>(image->m_code) + it->codeFor8Bit, static_cast<const uint8_t*>(image->m_code) + it->codeFor16Bit };
            }
        }
    }
    s_regExpsNotInImage++;
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
        size_t start = header.codeOffset + function.codeOffset;
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
    // Without an embedder to number the modules, a module is known by its text.
    if (!Options::aotImagePath())
        return 0;
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
    // All the code there is for it is what it says it has.
    if (scriptExecutable->isShortForm())
        return std::nullopt;
    SourceProvider* provider = scriptExecutable->source().provider();
    // The source of a constructor that nobody wrote is one of the engine's own.
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

static std::atomic<unsigned> s_installedFromImage;

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
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char* path = Options::aotImagePath()) {
            if (!Image::registerImageFromFile(path))
                dataLogLn("AOT: ", path, " is not an image for this engine");
        }
        if (Options::aotReportStats()) {
            atexit([] {
                dataLogLn("AOT: ", s_installedFromImage.load(), " functions ran from an image");
                dataLogLn("AOT: ", s_regExpsFromImage.load(), " regular expressions got their code from an image, ", s_regExpsNotInImage.load(), " did not");
            });
        }
    });
    if (!Image::hasAny())
        return { };

    Image* image = nullptr;
    const ImageFunction* function = nullptr;
    ImageKey key;
    if (auto* ofFunction = dynamicDowncast<FunctionExecutable>(executable); ofFunction && ofFunction->aotEntryFor(kind) && !(kind == CodeSpecializationKind::CodeForConstruct && ofFunction->constructsByCalling())) {
        // An executable of the static heap's says which function it is.
        function = StaticHeap::imageFunctionOfFunction(ofFunction->aotIndexFor(kind));
        image = &Image::of(*function);
    } else {
        auto keyOfExecutable = imageKeyFor(executable, kind);
        if (!keyOfExecutable) {
            if (Options::aotVerbose()) [[unlikely]]
                dataLogLn("AOT: no key for ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL());
            return { };
        }
        key = *keyOfExecutable;
        std::tie(image, function) = Image::find(key);
        if (!function) {
            if (Options::aotVerbose()) [[unlikely]]
                dataLogLn("AOT: not in the image: ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL(), " (module ", key.module, " start ", key.start, " kind ", key.kind, ")");
            return { };
        }
    }

    // The code finds what it has of the realm by its own number, so that is for one function of the realm. The same text evaluated
    // a second time is another function: it has constants of its own, for one thing.
    if (Instance* instance = scope->realm()->aotInstance()) {
        uint32_t index = function->index;
        if (instance->data[index] && FunctionRef { instance, index }.executable() != executable)
            return { };
        if (const FunctionInfo& info = instance->infos[index]; info.executable() && info.executable() != executable)
            return { };
    }

    // The code goes by a table that only StaticHeap makes, for the realm that the program is run in.
    if (image->header().numberOfIdentifiersOfProgram) {
        uint32_t index = function->index;
        if (!Instance::ensure(scope->realm()).infos[index].sites)
            return { };
    }

    if (function->usesStaticImports) {
        if (!moduleIsLinkedAsCompiled(scope)) {
            if (Options::aotVerbose()) [[unlikely]]
                dataLogLn("AOT: the module of ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL(), " is not linked the way it was compiled for");
            return { };
        }
    }

    if (Options::aotVerbose()) [[unlikely]]
        dataLogLn("AOT: ", nameForLogging(executable), " (module ", key.module, " start ", key.start, " kind ", key.kind, ") is at ", RawPointer(image->codeFor(*function)), " size ", function->codeSize, " hash ", hashOfCode({ image->codeFor(*function), function->codeSize }));
    return { image, function };
}

Ref<JITCode> codeFromImage(ImageCode code, UnlinkedCodeBlock* unlinkedCodeBlock)
{
    auto [image, function] = code;
    s_installedFromImage++;
    return adoptRef(*new JITCode(const_cast<uint8_t*>(image->codeFor(*function)), *function, JITCode::wayInto(unlinkedCodeBlock)));
}

bool canDoWithoutUnlinkedCode(JSGlobalObject* globalObject, ImageCode code)
{
    return !!FunctionRef { &Instance::ensure(globalObject), code.function->index }.facts();
}

Ref<JITCode> codeOfFunctionFromImage(ImageCode code, CodeSpecializationKind kind)
{
    auto [image, function] = code;
    s_installedFromImage++;
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

std::optional<size_t> aotImageSize(std::span<const uint8_t> image)
{
    if (image.size() < sizeof(AOT::ImageHeader))
        return std::nullopt;
    auto& header = *reinterpret_cast<const AOT::ImageHeader*>(image.data());
    if (header.magic != AOT::imageMagic || header.size > image.size())
        return std::nullopt;
    return static_cast<size_t>(header.size);
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
