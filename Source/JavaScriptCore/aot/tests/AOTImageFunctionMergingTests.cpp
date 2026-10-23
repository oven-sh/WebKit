/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTImageBuildSteps.h"
#include "AOTImageTestSupport.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsExceptions.h"
#include "AOTVector.h"
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC::AOT::ImageTests {

static constexpr uint32_t sizeOfPlainWord = 4;
static constexpr uint32_t sizeOfDataPrefix = 4;
static constexpr uint32_t asyncFunction = 3;
static constexpr uint32_t sloppyFunction = 4;
static constexpr uint32_t entrypointCallee = 5;
static constexpr uint32_t libraryFunction = 6;

static constexpr ASCIILiteral entrypointProgramText =
    "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
    "entry\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"
    "    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n"
    "    v4 = NewClosure function=f2\n    v5 = Call v1, -, v4\n"
    "    v6 = NewClosure function=f3\n    v7 = Call v1, -, v6\n"
    "    v8 = NewClosure function=f4\n    v9 = Call v1, -, v8\n"
    "    v10 = NewClosure function=f5\n    v11 = Call v1, -, v10\n"
    "    Return\n"
    "}\n"
    "function f1 name=\"first\" parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    Return v0\n}\n"
    "function f2 name=\"second\" parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    Return v0\n}\n"
    "function f3 name=\"async\" flags=[IsAsync] parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    v1 = Await v0\n    Return v1\n}\n"
    "function f4 name=\"sloppy\" flags=[IsSloppy] parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    Return v0\n}\n"
    "function f5 name=\"callee\" parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    Return v0\n}\n"_s;

static uint32_t appendRelocatedSite(CodeSketch& sketch, RelocationKind kind, CodeSymbol symbol)
{
    uint32_t offset = sketch.pinnedBytes(MacroAssembler::relocationSize(kind));
    sketch.code().body.relocations.append(Relocation { offset, kind, symbol.space, symbol.entry, symbol.index });
    return offset;
}

static void addDataPrefix(CodeSketch& sketch, uint32_t word)
{
    AOT_TEST_CHECK(!sketch.size());
    sketch.pinned(word);
    sketch.code().body.entries[0].offset = sketch.size();
    AOT_TEST_CHECK(sketch.size() == sizeOfDataPrefix);
}

static void addSecondaryEntry(CodeSketch& sketch, unsigned size = 4)
{
    AOT_TEST_CHECK(!sketch.size());
    sketch.pinnedBytes(size);
    CodeBody& body = sketch.code().body;
    body.entries[0].offset = sketch.size();
    body.entries.append(CodeEntry { 0, 1 });
}

template<typename Edit>
static CompiledCode framedAfter(uint32_t function, const Edit& edit)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    edit(sketch);
    sketch.plain(0x1000 + function);
    sketch.end();
    return sketch.take();
}

static CompiledCode framed(uint32_t function)
{
    return framedAfter(function, [](CodeSketch&) { });
}

static CompiledCode framedCaller(uint32_t function, uint32_t callee)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.plain(0x1000 + function);
    sketch.callFunction(callee);
    sketch.end();
    return sketch.take();
}

static CompiledCode oneJump(uint32_t function, uint32_t target)
{
    CodeSketch sketch(function);
    sketch.jumpToFunction(target);
    return sketch.take();
}

static CompiledCode movesAndJump(uint32_t function, uint32_t target, uint32_t word = 5)
{
    CodeSketch sketch(function);
    sketch.plain(word);
    sketch.jumpToFunction(target);
    return sketch.take();
}

static CompiledCode prefixAndJump(uint32_t function, uint32_t target, uint32_t word)
{
    CodeSketch sketch(function);
    addDataPrefix(sketch, word);
    sketch.jumpToFunction(target);
    sketch.code().entry.isGenericEntrypoint = true;
    return sketch.take();
}

static CompiledCode duplicateBody(uint32_t function, uint32_t word = 7, StubKey stub = catchException(), unsigned count = 1)
{
    CodeSketch sketch(function);
    sketch.plain(word, count);
    sketch.jumpToStub(stub);
    return sketch.take();
}

static CompiledCode withParameters(CompiledCode code, uint8_t numberOfParameters)
{
    code.entry.numberOfParameters = numberOfParameters;
    return code;
}

static CompiledCode withStackProof(CompiledCode code)
{
    code.frame.isProvenToFitOnStack = true;
    return code;
}

static CompiledCode withoutStackCheck(CompiledCode code)
{
    code.frame.isEnteredWithoutStackCheck = true;
    return code;
}

static EntryAlias aliasAt(uint32_t function, uint32_t body)
{
    return { .function = FunctionID(function), .body = FunctionID(body), .entry = { } };
}

static EntryAlias appendedAliasAt(uint32_t function, uint32_t body, uint32_t start)
{
    return { .function = FunctionID(function), .body = FunctionID(body), .entry = { }, .placement = AliasPlacement::AfterBody, .start = start };
}

struct Threaded {
    explicit Threaded(Vector<CompiledCode>&& code)
        : functions(createLinkedFunctions(WTF::move(code), context))
    {
        removeJumpOnlyFunctions(functions, aliases, testTemplates(), context);
    }

    BuildContext context { wireOptions() };
    LinkedFunctions functions;
    Vector<EntryAlias> aliases;
};

struct Folded {
    Folded(Vector<CompiledCode>&& code, Vector<EntryAlias>&& entryAliases = { })
        : functions(createLinkedFunctions(WTF::move(code), build.context()))
        , aliases(WTF::move(entryAliases))
        , folded(foldIdenticalFunctions(functions, aliases, build.program(), build.context()))
    {
    }

    TestBuild build { 12 };
    LinkedFunctions functions;
    Vector<EntryAlias> aliases;
    Vector<FoldedFunction> folded;
};

static Vector<uint32_t> idsOf(const LinkedFunctions& functions)
{
    Vector<uint32_t> ids;
    ids.reserveInitialCapacity(functions.size());
    for (const CompiledCode& code : functions)
        ids.append(code.function.index());
    return ids;
}

static bool hasIds(const LinkedFunctions& functions, std::initializer_list<uint32_t> expected)
{
    return idsOf(functions) == Vector<uint32_t>(expected);
}

template<typename List>
static const auto& findByFunction(const List& list, uint32_t function)
{
    auto hasFunction = [&](const auto& element) {
        return element.function == FunctionID(function);
    };
    AOT_TEST_CHECK(std::ranges::count_if(list, hasFunction) == 1);
    return *std::ranges::find_if(list, hasFunction);
}

static uint64_t statisticOf(BuildContext& context, StatisticID id)
{
    return context.statistics().values()[static_cast<unsigned>(id)];
}

static bool isPlacedAt(const EntryAlias& alias, uint32_t body, AliasPlacement placement, uint32_t start = 0, uint32_t prefixSize = 0)
{
    return alias.body == FunctionID(body) && alias.placement == placement && alias.start == start && alias.prefixSize == prefixSize;
}

static bool isAtPrimaryEntryOf(const EntryAlias& alias, uint32_t body)
{
    return isPlacedAt(alias, body, AliasPlacement::AtPrimaryEntry) && !alias.innerEntryOffset();
}

static bool entriesAreEqual(const EntryDescription& first, const EntryDescription& second)
{
    return first.numberOfParameters == second.numberOfParameters && first.isGenericEntrypoint == second.isGenericEntrypoint;
}

static bool isJumpToFunction(const CodeBody& body, const Relocation& relocation, uint32_t offset, uint32_t target)
{
    return relocation.offset == offset && relocation.kind == RelocationKind::Jump && relocation.space == SymbolSpace::Function
        && relocation.entry == primaryEntry && body.callees[relocation.symbol] == FunctionID(target);
}

static bool startsWithWord(std::span<const uint8_t> bytes, uint32_t word)
{
    return bytes.size() >= sizeof(word) && std::ranges::equal(bytes.first(sizeof(word)), asByteSpan(word));
}

static uint32_t alignedEnd(const CompiledCode& host, uint32_t prefixSize = 0)
{
    AOT_TEST_CHECK(!host.body.primaryEntryOffset());
    return static_cast<uint32_t>(WTF::roundUpToMultipleOf<imageFunctionAlignment>(host.body.bytes.size() + prefixSize)) - prefixSize;
}

static void testCodeIndex()
{
    Vector<CompiledCode> code = bodies(framed(3), framed(7));
    CodeIndex index(code.span());
    AOT_TEST_CHECK(index.indexOf(FunctionID(3u)) == 0u);
    AOT_TEST_CHECK(index.indexOf(FunctionID(7u)) == 1u);
    AOT_TEST_CHECK(!index.indexOf(FunctionID(5u)));
    AOT_TEST_CHECK(!index.indexOf(FunctionID(100u)));
}

static void testAliasIndex()
{
    Vector<EntryAlias> aliases { aliasAt(9, 5), appendedAliasAt(4, 5, 8) };
    AliasIndex index(aliases.span());
    AOT_TEST_CHECK(index.aliasOf(FunctionID(9u)) == OptionalIndex { 0 });
    AOT_TEST_CHECK(index.aliasOf(FunctionID(4u)) == OptionalIndex { 1 });
    AOT_TEST_CHECK(!index.aliasOf(FunctionID(5u)));
    AOT_TEST_CHECK(!index.aliasOf(FunctionID(100u)));
    AOT_TEST_CHECK(!AliasIndex(std::span<const EntryAlias> { }).aliasOf(FunctionID(0u)));
}

static void checkTrimmedBody(const Threaded& threaded, size_t size)
{
    AOT_TEST_CHECK(threaded.functions.size() == 1);
    const CompiledCode& code = threaded.functions[0];
    AOT_TEST_CHECK(code.body.bytes.size() == size);
    AOT_TEST_CHECK(code.body.paddingStart == size);
    AOT_TEST_CHECK(verifyCodeUnits(code) == CodeUnitsError::None);
}

static CompiledCode paddedBody()
{
    CodeSketch sketch(1);
    sketch.withFrame();
    sketch.plain(0x1001);
    sketch.end();
    sketch.padding(12);
    return sketch.take();
}

static void testPaddingGoes()
{
    CompiledCode code = paddedBody();
    size_t paddingStart = code.body.paddingStart;
    AOT_TEST_CHECK(paddingStart < code.body.bytes.size());
    Threaded threaded(bodies(WTF::move(code)));
    checkTrimmedBody(threaded, paddingStart);
}

static void testOneUnitAfterReturningCall()
{
    CodeSketch sketch(1);
    sketch.withFrame();
    sketch.plain(0x1001);
    uint32_t call = sketch.callStub(writeBarrier());
    sketch.padding(12);
    Threaded threaded(bodies(sketch.take()));
    checkTrimmedBody(threaded, call + sizeOfNearCall + codeOffsetUnit);
}

static void testFaultAtEndGrowsBody()
{
    CodeSketch sketch(1);
    sketch.withFrame();
    sketch.plain(0x1001);
    uint32_t access = sketch.pinned(1);
    sketch.code().sites.faultingAccesses.append(FaultingAccess { access, { }, { 8 } });
    size_t sizeBefore = sketch.size();
    size_t sizeAfter = access + sizeOfNearCall + codeOffsetUnit;
    AOT_TEST_CHECK(sizeAfter > sizeBefore);
    Threaded threaded(bodies(sketch.takeUnchecked()));
    checkTrimmedBody(threaded, sizeAfter);

    const CodeBody& body = threaded.functions[0].body;
    CodeUnit last = body.units.last();
    AOT_TEST_CHECK(last.kind == CodeUnit::Kind::Pinned);
    AOT_TEST_CHECK(last.offset == sizeBefore);
    AOT_TEST_CHECK(last.length == sizeAfter - sizeBefore);

    const Vector<uint8_t>& pattern = testTemplates().breakpoint;
    for (size_t offset = sizeBefore; offset < sizeAfter; ++offset)
        AOT_TEST_CHECK(body.bytes[offset] == pattern[(offset - sizeBefore) % pattern.size()]);
}

static CompiledCode trimmedBody()
{
    Threaded threaded(bodies(paddedBody()));
    return WTF::move(threaded.functions[0]);
}

static void checkBodyUnchanged(CompiledCode code)
{
    Vector<uint8_t> bytes = code.body.bytes;
    Threaded threaded(bodies(WTF::move(code)));
    checkTrimmedBody(threaded, bytes.size());
    AOT_TEST_CHECK(threaded.functions[0].body.bytes == bytes);
}

static void testChainOfThree()
{
    Threaded threaded(bodies(withParameters(oneJump(1, 2), 1), withParameters(oneJump(2, 3), 2), withParameters(oneJump(3, 4), 3), framed(4)));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 4 }));
    AOT_TEST_CHECK(threaded.aliases.size() == 3);
    for (uint32_t function = 1; function <= 3; ++function) {
        const EntryAlias& alias = findByFunction(threaded.aliases, function);
        AOT_TEST_CHECK(isAtPrimaryEntryOf(alias, 4));
        AOT_TEST_CHECK(alias.entry.numberOfParameters == function);
    }
    AOT_TEST_CHECK(statisticOf(threaded.context, StatisticID::JumpOnlyFunctionsRemoved) == 3);
}

static void testChainOfForty()
{
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= 40; ++function)
        code.append(oneJump(function, function + 1));
    code.append(framed(41));
    Threaded threaded(WTF::move(code));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 41 }));
    AOT_TEST_CHECK(threaded.aliases.size() == 40);
    for (uint32_t function = 1; function <= 40; ++function)
        AOT_TEST_CHECK(isAtPrimaryEntryOf(findByFunction(threaded.aliases, function), 41));
}

static void testCallerCalleesUnchanged()
{
    Threaded threaded(bodies(framedCaller(1, 2), oneJump(2, 3), framed(3)));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 1, 3 }));
    AOT_TEST_CHECK(isAtPrimaryEntryOf(findByFunction(threaded.aliases, 2), 3));
    const Vector<FunctionID>& callees = findByFunction(threaded.functions, 1).body.callees;
    AOT_TEST_CHECK(callees.size() == 1);
    AOT_TEST_CHECK(callees[0] == FunctionID(2u));
}

using CreateJumpBody = CompiledCode (*)(uint32_t function, uint32_t target, uint32_t word);

static void checkHeadIsPrepended(CreateJumpBody create, uint32_t prefixSize)
{
    CompiledCode target = framed(3);
    Vector<uint8_t> targetBytes = target.body.bytes;
    Vector<uint8_t> hostBytes = framed(1).body.bytes;
    CompiledCode removed = withParameters(create(2, 3, 0x0105), 2);
    EntryDescription removedEntry = removed.entry;
    uint32_t headSize = removed.body.relocations[0].offset;
    AOT_TEST_CHECK(headSize == sizeOfPlainWord);
    Threaded threaded(bodies(framed(1), WTF::move(removed), WTF::move(target)));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 1, 3 }));
    AOT_TEST_CHECK(findByFunction(threaded.functions, 1).body.bytes == hostBytes);

    const EntryAlias& alias = findByFunction(threaded.aliases, 2);
    AOT_TEST_CHECK(isPlacedAt(alias, 3, AliasPlacement::BeforeBody, 0, prefixSize));
    AOT_TEST_CHECK(alias.innerEntryOffset() == prefixSize);
    AOT_TEST_CHECK(entriesAreEqual(alias.entry, removedEntry));

    const CompiledCode& code = findByFunction(threaded.functions, 3);
    AOT_TEST_CHECK(entriesAreEqual(code.entry, { }));
    AOT_TEST_CHECK(code.body.primaryEntryOffset() == headSize);
    AOT_TEST_CHECK(startsWithWord(code.body.bytes.span(), 0x0105));
    AOT_TEST_CHECK(std::ranges::equal(code.body.bytes.subspan(headSize), targetBytes));
    AOT_TEST_CHECK(code.body.relocations.isEmpty());
    AOT_TEST_CHECK(code.body.paddingStart == code.body.bytes.size());
    AOT_TEST_CHECK(code.body.units.first().kind == CodeUnit::Kind::Pinned && code.body.units.first().length == headSize);
    AOT_TEST_CHECK(code.frame.frameRanges.size() == 1);
    AOT_TEST_CHECK(code.frame.frameRanges[0].begin == headSize && code.frame.frameRanges[0].end == code.body.bytes.size());
    AOT_TEST_CHECK(verifyCodeUnits(code) == CodeUnitsError::None);
}

static void testPrependedHeadIsPadded()
{
    CodeSketch sketch(1);
    sketch.pinnedBytes(3);
    uint32_t headSize = sketch.size();
    sketch.jumpToFunction(2);
    Vector<uint8_t> head(sketch.code().body.bytes.span().first(headSize));
    Vector<uint8_t> targetBytes = framed(2).body.bytes;
    Threaded threaded(bodies(sketch.take(), framed(2)));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 2 }));
    AOT_TEST_CHECK(isPlacedAt(findByFunction(threaded.aliases, 1), 2, AliasPlacement::BeforeBody));

    const CompiledCode& code = findByFunction(threaded.functions, 2);
    uint32_t entry = static_cast<uint32_t>(WTF::roundUpToMultipleOf<imageFunctionAlignment>(headSize));
    Vector<uint8_t> nops(FillWith { }, entry - headSize, static_cast<uint8_t>(0));
    fillWithNops(nops.mutableSpan());
    AOT_TEST_CHECK(code.body.primaryEntryOffset() == entry);
    AOT_TEST_CHECK(std::ranges::equal(code.body.bytes.span().first(headSize), head));
    AOT_TEST_CHECK(std::ranges::equal(code.body.bytes.subspan(headSize, entry - headSize), nops));
    AOT_TEST_CHECK(std::ranges::equal(code.body.bytes.subspan(entry), targetBytes));
    AOT_TEST_CHECK(verifyCodeUnits(code) == CodeUnitsError::None);
}

// A word, a jump to the part behind, the jump to the body. Behind it: the jump to the fallback.
static CompiledCode entrance(uint32_t function, uint32_t body, uint32_t fallback)
{
    CodeSketch sketch(function);
    sketch.pinned(0x0105);
    sketch.jumpTo(sketch.size() + 2 * static_cast<uint32_t>(sizeOfJump));
    sketch.jumpToFunction(body);
    sketch.jumpToFunction(fallback);
    return sketch.take();
}

// Both entries are aligned. Where the word and the jump in front of the jump to the body have an odd size, a prefix without meaning on the first instruction makes it even.
static constexpr uint32_t sizeOfPrefixOfEntrance = (sizeOfPlainWord + sizeOfJump) % imageFunctionAlignment;

static void testEntranceEndsInJumpToNextBody()
{
    constexpr uint32_t sizeBehindJump = static_cast<uint32_t>(sizeOfJump);
    constexpr uint32_t firstInstruction = sizeBehindJump + sizeOfPrefixOfEntrance;
    static_assert(sizeOfPrefixOfEntrance == (isX86() ? 1 : 0));
    Vector<uint8_t> bytesOfBody = framed(2).body.bytes;
    Threaded threaded(bodies(entrance(1, 2, 3), framed(2), framed(3)));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 1, 2, 3 }));
    AOT_TEST_CHECK(threaded.aliases.isEmpty());
    AOT_TEST_CHECK(findByFunction(threaded.functions, 2).body.bytes == bytesOfBody);

    const CodeBody& body = findByFunction(threaded.functions, 1).body;
    AOT_TEST_CHECK(body.primaryEntryOffset() == sizeBehindJump);
    AOT_TEST_CHECK(!sizeOfPrefixOfEntrance || body.bytes[sizeBehindJump] == 0x3e);
    AOT_TEST_CHECK(startsWithWord(body.bytes.subspan(firstInstruction), 0x0105));
    AOT_TEST_CHECK(!((body.bytes.size() - sizeOfJump - body.primaryEntryOffset()) % imageFunctionAlignment));
    AOT_TEST_CHECK(body.relocations.size() == 2);
    AOT_TEST_CHECK(isJumpToFunction(body, body.relocations[0], 0, 3));
    AOT_TEST_CHECK(isJumpToFunction(body, body.relocations[1], static_cast<uint32_t>(body.bytes.size() - sizeOfJump), 2));
    AOT_TEST_CHECK(body.paddingStart == body.bytes.size());

    AOT_TEST_CHECK(body.localSites.size() == 1);
    const LinkedSite& site = body.localSites[0];
    AOT_TEST_CHECK(site.offset == firstInstruction + sizeOfPlainWord && !site.target);
    Vector<uint8_t> expected(body.bytes.subspan(site.offset, sizeOfJump));
    AOT_TEST_CHECK(MacroAssembler::setRelocationDistance(site.kind, expected.mutableSpan(), -static_cast<int64_t>(site.offset)));
    AOT_TEST_CHECK(std::ranges::equal(body.bytes.subspan(site.offset, sizeOfJump), expected));
}

#if CPU(X86_64)
static void testEntranceOfEvenSizeHasNoPrefix()
{
    constexpr uint32_t sizeBehindJump = static_cast<uint32_t>(sizeOfJump);
    CodeSketch sketch(1);
    sketch.pinned(0x0105);
    sketch.pinnedBytes(1);
    sketch.jumpTo(sketch.size() + 2 * static_cast<uint32_t>(sizeOfJump));
    sketch.jumpToFunction(2);
    sketch.jumpToFunction(3);
    size_t size = sketch.size();
    Threaded threaded(bodies(sketch.take(), framed(2), framed(3)));
    const CodeBody& body = findByFunction(threaded.functions, 1).body;
    AOT_TEST_CHECK(body.primaryEntryOffset() == sizeBehindJump);
    AOT_TEST_CHECK(body.bytes.size() == size);
    AOT_TEST_CHECK(startsWithWord(body.bytes.subspan(sizeBehindJump), 0x0105));
}
#endif

// The head of an entrypoint that only leads to the entrance lies in front of the entrance's entry and moves with it.
static void testHeadMovesWithEntrance()
{
    constexpr uint32_t sizeBehindJump = static_cast<uint32_t>(sizeOfJump);
    Threaded threaded(bodies(movesAndJump(1, 2, 0x0207), entrance(2, 3, 4), framed(3), framed(4)));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 2, 3, 4 }));
    AOT_TEST_CHECK(isPlacedAt(findByFunction(threaded.aliases, 1), 2, AliasPlacement::BeforeBody, sizeBehindJump, 0));

    const CodeBody& body = findByFunction(threaded.functions, 2).body;
    AOT_TEST_CHECK(body.primaryEntryOffset() == sizeBehindJump + sizeOfPlainWord);
    AOT_TEST_CHECK(isJumpToFunction(body, body.relocations[0], 0, 4));
    AOT_TEST_CHECK(startsWithWord(body.bytes.subspan(sizeBehindJump), 0x0207));
    AOT_TEST_CHECK(startsWithWord(body.bytes.subspan(body.primaryEntryOffset()), 0x0105));
    AOT_TEST_CHECK(isJumpToFunction(body, body.relocations[1], static_cast<uint32_t>(body.bytes.size() - sizeOfJump), 3));
}

static void testEntranceFarFromItsBodyStays()
{
    Vector<uint8_t> bytes = entrance(1, 3, 2).body.bytes;
    Threaded threaded(bodies(entrance(1, 3, 2), framed(2), framed(3)));
    const CodeBody& body = findByFunction(threaded.functions, 1).body;
    AOT_TEST_CHECK(!body.primaryEntryOffset());
    AOT_TEST_CHECK(body.bytes == bytes);
}

static void checkSecondHeadIsAppended(CreateJumpBody create, uint32_t prefixSize)
{
    CompiledCode host = framed(1);
    CompiledCode guest = create(3, 4, 6);
    EntryDescription guestEntry = guest.entry;
    uint32_t start = alignedEnd(host, prefixSize);
    uint32_t jumpOffset = start + guest.body.relocations[0].offset;
    Vector<uint8_t> guestBytes = guest.body.bytes;
    Threaded threaded(bodies(WTF::move(host), create(2, 4, 5), WTF::move(guest), framed(4)));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 1, 4 }));
    AOT_TEST_CHECK(isPlacedAt(findByFunction(threaded.aliases, 2), 4, AliasPlacement::BeforeBody, 0, prefixSize));
    AOT_TEST_CHECK(startsWithWord(findByFunction(threaded.functions, 4).body.bytes.span(), 5));

    const EntryAlias& alias = findByFunction(threaded.aliases, 3);
    AOT_TEST_CHECK(isPlacedAt(alias, 1, AliasPlacement::AfterBody, start, prefixSize));
    AOT_TEST_CHECK(alias.innerEntryOffset() == start + prefixSize);
    AOT_TEST_CHECK(!(*alias.innerEntryOffset() % imageFunctionAlignment));
    AOT_TEST_CHECK(entriesAreEqual(alias.entry, guestEntry));

    const CompiledCode& code = findByFunction(threaded.functions, 1);
    AOT_TEST_CHECK(startsWithWord(code.body.bytes.subspan(start), 6));
    AOT_TEST_CHECK(std::ranges::equal(code.body.bytes.subspan(start), guestBytes));
    AOT_TEST_CHECK(code.body.relocations.size() == 1);
    AOT_TEST_CHECK(isJumpToFunction(code.body, code.body.relocations[0], jumpOffset, 4));
    AOT_TEST_CHECK(code.body.paddingStart == code.body.bytes.size());
    AOT_TEST_CHECK(verifyCodeUnits(code) == CodeUnitsError::None);
}

static void testTwoGuests()
{
    Threaded threaded(bodies(framed(1), movesAndJump(2, 5), movesAndJump(3, 5, 6), movesAndJump(4, 5, 7), framed(5)));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 1, 5 }));
    AOT_TEST_CHECK(isPlacedAt(findByFunction(threaded.aliases, 2), 5, AliasPlacement::BeforeBody));
    const EntryAlias& first = findByFunction(threaded.aliases, 3);
    const EntryAlias& second = findByFunction(threaded.aliases, 4);
    AOT_TEST_CHECK(first.body == FunctionID(1u) && first.placement == AliasPlacement::AfterBody);
    AOT_TEST_CHECK(second.body == FunctionID(1u) && second.placement == AliasPlacement::AfterBody);
    AOT_TEST_CHECK(first.start < second.start);
    AOT_TEST_CHECK(!(second.start % imageFunctionAlignment));

    const CompiledCode& host = findByFunction(threaded.functions, 1);
    AOT_TEST_CHECK(startsWithWord(host.body.bytes.subspan(first.start), 6));
    AOT_TEST_CHECK(startsWithWord(host.body.bytes.subspan(second.start), 7));
    AOT_TEST_CHECK(host.body.relocations.size() == 2);
    AOT_TEST_CHECK(std::ranges::count(host.body.callees, FunctionID(5u)) == 1);
    AOT_TEST_CHECK(verifyCodeUnits(host) == CodeUnitsError::None);
}

static void testAppendedJumpThroughChain()
{
    Threaded threaded(bodies(framed(1), movesAndJump(5, 3, 6), movesAndJump(2, 3), oneJump(3, 4), framed(4)));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 1, 4 }));
    AOT_TEST_CHECK(isPlacedAt(findByFunction(threaded.aliases, 5), 4, AliasPlacement::BeforeBody));
    AOT_TEST_CHECK(startsWithWord(findByFunction(threaded.functions, 4).body.bytes.span(), 6));
    const EntryAlias& alias = findByFunction(threaded.aliases, 2);
    AOT_TEST_CHECK(alias.body == FunctionID(1u) && alias.placement == AliasPlacement::AfterBody);
    AOT_TEST_CHECK(isAtPrimaryEntryOf(findByFunction(threaded.aliases, 3), 4));
    const CodeBody& host = findByFunction(threaded.functions, 1).body;
    AOT_TEST_CHECK(isJumpToFunction(host, host.relocations.last(), alias.start + sizeOfPlainWord, 4));
}

template<typename Edit>
static void checkTargetTakesNoHead(const Edit& edit)
{
    CompiledCode target = framedAfter(4, edit);
    Vector<uint8_t> targetBytes = target.body.bytes;
    uint32_t targetEntry = target.body.primaryEntryOffset();
    AOT_TEST_CHECK(targetEntry);
    Threaded threaded(bodies(framed(1), movesAndJump(2, 4), oneJump(3, 4), WTF::move(target), framed(5)));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 1, 4, 5 }));
    const EntryAlias& alias = findByFunction(threaded.aliases, 2);
    AOT_TEST_CHECK(alias.body == FunctionID(1u) && alias.placement == AliasPlacement::AfterBody);
    AOT_TEST_CHECK(isAtPrimaryEntryOf(findByFunction(threaded.aliases, 3), 4));
    AOT_TEST_CHECK(findByFunction(threaded.functions, 4).body.bytes == targetBytes);
    AOT_TEST_CHECK(findByFunction(threaded.functions, 4).body.primaryEntryOffset() == targetEntry);
}

#if CPU(X86_64)
static void testAppendedToHostWithOddEntry()
{
    CompiledCode host = framedAfter(1, [](CodeSketch& sketch) {
        addSecondaryEntry(sketch, 5);
    });
    uint32_t hostEntry = host.body.primaryEntryOffset();
    size_t hostSize = host.body.bytes.size();
    AOT_TEST_CHECK(hostEntry % imageFunctionAlignment);
    CompiledCode guest = movesAndJump(2, 3);
    Vector<uint8_t> guestBytes = guest.body.bytes;
    Threaded threaded(bodies(WTF::move(host), WTF::move(guest), framedAfter(3, [](CodeSketch& sketch) {
        addSecondaryEntry(sketch);
    })));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 1, 3 }));

    const EntryAlias& alias = findByFunction(threaded.aliases, 2);
    AOT_TEST_CHECK(alias.body == FunctionID(1u) && alias.placement == AliasPlacement::AfterBody);
    AOT_TEST_CHECK(alias.start >= hostSize && alias.start < hostSize + imageFunctionAlignment);
    AOT_TEST_CHECK(!((alias.start + alias.prefixSize - hostEntry) % imageFunctionAlignment));
    const CompiledCode& code = findByFunction(threaded.functions, 1);
    AOT_TEST_CHECK(code.body.primaryEntryOffset() == hostEntry);
    AOT_TEST_CHECK(std::ranges::equal(code.body.bytes.subspan(alias.start), guestBytes));
    AOT_TEST_CHECK(verifyCodeUnits(code) == CodeUnitsError::None);
}
#endif

static bool isProvenAfterThreading(Vector<CompiledCode>&& code, uint32_t target, size_t numberOfAliases = 1)
{
    Threaded threaded(WTF::move(code));
    AOT_TEST_CHECK(threaded.aliases.size() == numberOfAliases);
    return findByFunction(threaded.functions, target).frame.isProvenToFitOnStack;
}

static void checkNothingRemoved(Vector<CompiledCode>&& code)
{
    Vector<uint32_t> ids = code.map([](const CompiledCode& body) {
        return body.function.index();
    });
    Threaded threaded(WTF::move(code));
    AOT_TEST_CHECK(idsOf(threaded.functions) == ids);
    AOT_TEST_CHECK(threaded.aliases.isEmpty());
    AOT_TEST_CHECK(!statisticOf(threaded.context, StatisticID::JumpOnlyFunctionsRemoved));
}

using EditSketch = void (*)(CodeSketch&);

static void checkJumpBodyStays(EditSketch editBefore, EditSketch editJump = [](CodeSketch&) { })
{
    CodeSketch sketch(2);
    editBefore(sketch);
    sketch.plain(5);
    sketch.jumpToFunction(3);
    editJump(sketch);
    checkNothingRemoved(bodies(framed(1), sketch.take(), framed(3)));
}

static void testAddressTaken()
{
    CompiledCode user = framedAfter(4, [](CodeSketch& sketch) {
        appendRelocatedSite(sketch, RelocationKind::AddressNear, { SymbolSpace::Function, sketch.calleeIndex(2) });
    });
    checkNothingRemoved(bodies(framed(1), movesAndJump(2, 3), framed(3), WTF::move(user)));
}

static void jumpToThunk(CodeSketch& sketch)
{
    sketch.lastRelocation().space = SymbolSpace::ArgumentThunk;
    sketch.lastRelocation().symbol = static_cast<uint32_t>(sketch.code().body.argumentMoves.size());
    sketch.code().body.argumentMoves.append(stubArgumentMoves(catchException(), 1));
}

static void testFirstBodyHasNoHost()
{
    CompiledCode first = movesAndJump(1, 2);
    Vector<uint8_t> bytes = first.body.bytes;
    Threaded threaded(bodies(WTF::move(first), oneJump(2, 3), framedAfter(3, [](CodeSketch& sketch) {
        addSecondaryEntry(sketch);
    })));
    AOT_TEST_CHECK(hasIds(threaded.functions, { 1, 3 }));
    AOT_TEST_CHECK(threaded.aliases.size() == 1);
    AOT_TEST_CHECK(isAtPrimaryEntryOf(findByFunction(threaded.aliases, 2), 3));
    const CodeBody& body = findByFunction(threaded.functions, 1).body;
    AOT_TEST_CHECK(body.bytes == bytes);
    AOT_TEST_CHECK(isJumpToFunction(body, body.relocations[0], sizeOfPlainWord, 2));
}

struct Entrypoint {
    uint32_t function;
    std::optional<unsigned> frameSize { 32 };
    uint32_t callee { entrypointCallee };
};

static CompiledCode entrypointBody(const TestBuild& build, const Entrypoint& entrypoint)
{
    CodeSketch sketch(build.genericEntrypointOf(entrypoint.function).index());
    if (entrypoint.frameSize)
        sketch.withFrame(*entrypoint.frameSize);
    sketch.code().entry.isGenericEntrypoint = true;
    sketch.plain(7);
    sketch.callFunction(entrypoint.callee, sketch.addSite());
    sketch.end();
    return sketch.take();
}

static size_t foldEntrypoints(const Entrypoint& first, const Entrypoint& second, CompiledCode&& callee = duplicateBody(entrypointCallee), Vector<EntryAlias>&& aliases = { })
{
    TestBuild build(makeString(entrypointProgramText, runtimeFunctionsText(libraryFunction)), { });
    AOT_TEST_CHECK(build.program().at(FunctionID(asyncFunction)).role == Typed::FunctionRole::CoroutineRamp);
    LinkedFunctions functions = createLinkedFunctions(bodies(entrypointBody(build, first), entrypointBody(build, second), WTF::move(callee)), build.context());
    size_t numberOfAliases = aliases.size();
    Vector<FoldedFunction> folded = foldIdenticalFunctions(functions, aliases, build.program(), build.context());
    AOT_TEST_CHECK(functions.size() == 3 - folded.size());
    AOT_TEST_CHECK(aliases.size() == numberOfAliases + folded.size());
    return folded.size();
}

static CompiledCode hostDuplicate(uint32_t function, uint32_t& guestOffset)
{
    CodeSketch sketch(function);
    sketch.plain(7);
    sketch.jumpToStub(catchException());
    guestOffset = sketch.pinned(1, 2);
    return sketch.takeUnchecked();
}

static void testDuplicates()
{
    CompiledCode second = withParameters(duplicateBody(2), 2);
    EntryDescription entry = second.entry;
    uint32_t size = static_cast<uint32_t>(second.body.bytes.size());
    Folded folded(bodies(withParameters(duplicateBody(1), 1), WTF::move(second)));
    AOT_TEST_CHECK(hasIds(folded.functions, { 1 }));

    const EntryAlias& alias = findByFunction(folded.aliases, 2);
    AOT_TEST_CHECK(isAtPrimaryEntryOf(alias, 1));
    AOT_TEST_CHECK(entriesAreEqual(alias.entry, entry));
    AOT_TEST_CHECK(findByFunction(folded.functions, 1).entry.numberOfParameters == 1);

    AOT_TEST_CHECK(folded.folded.size() == 1);
    AOT_TEST_CHECK(folded.folded[0].function == FunctionID(2u));
    AOT_TEST_CHECK(folded.folded[0].canonicalFunction == FunctionID(1u));
    AOT_TEST_CHECK(folded.folded[0].size == size);
    AOT_TEST_CHECK(folded.build.statistic(StatisticID::FunctionsFolded) == 1);
    AOT_TEST_CHECK(folded.build.statistic(StatisticID::BytesFolded) == size);
}

static void testCyclesFoldInPairs()
{
    Folded folded(bodies(movesAndJump(1, 2, 1), movesAndJump(2, 1, 2), movesAndJump(3, 4, 1), movesAndJump(4, 3, 2)));
    AOT_TEST_CHECK(hasIds(folded.functions, { 1, 2 }));
    AOT_TEST_CHECK(findByFunction(folded.aliases, 3).body == FunctionID(1u));
    AOT_TEST_CHECK(findByFunction(folded.aliases, 4).body == FunctionID(2u));
}

static void testAliasMatchesTarget()
{
    Folded folded(bodies(movesAndJump(1, 9), movesAndJump(2, 5), framed(5)), { aliasAt(9, 5) });
    AOT_TEST_CHECK(hasIds(folded.functions, { 1, 5 }));
    AOT_TEST_CHECK(findByFunction(folded.aliases, 2).body == FunctionID(1u));
}

static void testHostsWithEqualGuests()
{
    uint32_t offset = 0;
    CompiledCode first = hostDuplicate(1, offset);
    CompiledCode second = hostDuplicate(2, offset);
    Folded folded(bodies(WTF::move(first), WTF::move(second)), { appendedAliasAt(8, 1, offset), appendedAliasAt(9, 2, offset) });
    AOT_TEST_CHECK(hasIds(folded.functions, { 1 }));
    AOT_TEST_CHECK(isPlacedAt(findByFunction(folded.aliases, 8), 1, AliasPlacement::AfterBody, offset));
    AOT_TEST_CHECK(isPlacedAt(findByFunction(folded.aliases, 9), 1, AliasPlacement::AfterBody, offset));
}

static void testProofIsRetained()
{
    Folded folded(bodies(duplicateBody(1), withStackProof(duplicateBody(2))));
    AOT_TEST_CHECK(hasIds(folded.functions, { 1 }));
    AOT_TEST_CHECK(findByFunction(folded.functions, 1).frame.isProvenToFitOnStack);
}

static void checkFolded(Vector<CompiledCode>&& code, std::initializer_list<uint32_t> retained)
{
    size_t numberOfBodies = code.size();
    Folded folded(WTF::move(code));
    AOT_TEST_CHECK(hasIds(folded.functions, retained));
    AOT_TEST_CHECK(folded.folded.size() == numberOfBodies - retained.size());
    AOT_TEST_CHECK(folded.aliases.size() == folded.folded.size());
}

static void checkNotFolded(Vector<CompiledCode>&& code, Vector<EntryAlias>&& aliases = { })
{
    Vector<uint32_t> ids = code.map([](const CompiledCode& body) {
        return body.function.index();
    });
    size_t numberOfAliases = aliases.size();
    Folded folded(WTF::move(code), WTF::move(aliases));
    AOT_TEST_CHECK(idsOf(folded.functions) == ids);
    AOT_TEST_CHECK(folded.folded.isEmpty());
    AOT_TEST_CHECK(folded.aliases.size() == numberOfAliases);
    AOT_TEST_CHECK(!folded.build.statistic(StatisticID::FunctionsFolded));
    AOT_TEST_CHECK(!folded.build.statistic(StatisticID::BytesFolded));
}

template<typename Edit>
static CompiledCode editedDuplicate(uint32_t function, const Edit& edit)
{
    CodeSketch sketch(function);
    edit(sketch);
    sketch.plain(7);
    sketch.jumpToStub(catchException());
    return sketch.take();
}

template<typename Edit>
static void checkRecordsNotFolded(const Edit& edit)
{
    checkNotFolded(bodies(editedDuplicate(1, edit), editedDuplicate(2, edit)));
}

static CompiledCode thunkCallBody(uint32_t function, uint64_t constant)
{
    return editedDuplicate(function, [&](CodeSketch& sketch) {
        sketch.callThunk(stubArgumentMoves(writeBarrier(), constant));
    });
}

static CompiledCode siteCacheBody(uint32_t function, std::initializer_list<uint32_t> siteCaches)
{
    return editedDuplicate(function, [&](CodeSketch& sketch) {
        for (uint32_t siteCache : siteCaches)
            appendRelocatedSite(sketch, RelocationKind::InstanceOffset, { SymbolSpace::SiteCache, siteCache });
    });
}

static CompiledCode constantBody(uint32_t function, uint64_t value, uint32_t alignment = sizeof(uint64_t))
{
    return editedDuplicate(function, [&](CodeSketch& sketch) {
        sketch.code().body.constants.append(ConstantData { Vector<uint8_t>(asByteSpan(value)), alignment });
        appendRelocatedSite(sketch, RelocationKind::Load64, { SymbolSpace::Constant, 0 });
    });
}

static CompiledCode jumpTableBody(uint32_t function, std::initializer_list<uint32_t> targets)
{
    return editedDuplicate(function, [&](CodeSketch& sketch) {
        sketch.plain(1, 2);
        sketch.code().body.jumpTables.append(JumpTable { Vector<uint32_t>(targets) });
        appendRelocatedSite(sketch, RelocationKind::AddressFar, { SymbolSpace::JumpTable, 0 });
    });
}

static CompiledCode addressBody(uint32_t function, uint32_t callee)
{
    return editedDuplicate(function, [&](CodeSketch& sketch) {
        appendRelocatedSite(sketch, RelocationKind::AddressNear, { SymbolSpace::Function, sketch.calleeIndex(callee) });
    });
}

static void testOtherEntryOffset()
{
    CompiledCode second = editedDuplicate(2, [](CodeSketch& sketch) {
        addDataPrefix(sketch, 9);
    });
    CompiledCode first = editedDuplicate(1, [](CodeSketch& sketch) {
        sketch.pinned(9);
    });
    AOT_TEST_CHECK(first.body.bytes == second.body.bytes);
    checkNotFolded(bodies(WTF::move(first), WTF::move(second)));
}

static CompiledCode visibleBodyWithSite(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.plain(7);
    sketch.callStub(writeBarrier(), sketch.addSite());
    sketch.jumpToStub(catchException());
    return sketch.take();
}

static void testHostAgainstPlainBody()
{
    uint32_t offset = 0;
    CompiledCode first = hostDuplicate(1, offset);
    checkNotFolded(bodies(WTF::move(first), hostDuplicate(2, offset)), { appendedAliasAt(8, 1, offset) });
}

static void testOtherStackUse()
{
    CompiledCode other = duplicateBody(2);
    other.frame.stackUsedAtCalls = 8;
    checkNotFolded(bodies(duplicateBody(1), WTF::move(other)));
}

struct FoldingResult {
    Vector<uint32_t> ids;
    Vector<EntryAlias> aliases;
    Vector<FoldedFunction> folded;
};

static FoldingResult foldManyBodies(unsigned threadCount)
{
    TestBuild build(401, TestOptions { .threadCount = threadCount });
    AOT_TEST_CHECK(build.context().threadCount() == threadCount);
    Vector<CompiledCode> code;
    code.reserveInitialCapacity(400);
    for (uint32_t function = 1; function <= 400; ++function)
        code.append(function % 2 ? duplicateBody(function, function % 7) : movesAndJump(function, 1 + function % 11, function % 3));
    LinkedFunctions functions = createLinkedFunctions(WTF::move(code), build.context());
    Vector<EntryAlias> aliases;
    Vector<FoldedFunction> folded = foldIdenticalFunctions(functions, aliases, build.program(), build.context());
    return { idsOf(functions), WTF::move(aliases), WTF::move(folded) };
}

static void testThreads()
{
    FoldingResult serial = foldManyBodies(1);
    FoldingResult parallel = foldManyBodies(8);
    AOT_TEST_CHECK(!serial.folded.isEmpty());
    AOT_TEST_CHECK(serial.ids == parallel.ids);
    AOT_TEST_CHECK(serial.aliases.size() == parallel.aliases.size());
    for (size_t i = 0; i < serial.aliases.size(); ++i) {
        const EntryAlias& first = serial.aliases[i];
        const EntryAlias& second = parallel.aliases[i];
        AOT_TEST_CHECK(first.function == second.function);
        AOT_TEST_CHECK(isPlacedAt(second, first.body.index(), first.placement, first.start, first.prefixSize));
        AOT_TEST_CHECK(entriesAreEqual(first.entry, second.entry));
    }
    AOT_TEST_CHECK(serial.folded.size() == parallel.folded.size());
    for (size_t i = 0; i < serial.folded.size(); ++i) {
        AOT_TEST_CHECK(serial.folded[i].function == parallel.folded[i].function);
        AOT_TEST_CHECK(serial.folded[i].canonicalFunction == parallel.folded[i].canonicalFunction);
        AOT_TEST_CHECK(serial.folded[i].size == parallel.folded[i].size);
    }
}

static void testCalleesResolveAfterFolding()
{
    Folded folded(bodies(movesAndJump(1, 3), movesAndJump(2, 4), duplicateBody(3), duplicateBody(4), framedCaller(5, 4)));
    AOT_TEST_CHECK(hasIds(folded.functions, { 1, 3, 5 }));

    CodeIndex index(folded.functions.span());
    ResolvedCallees callees = resolveCallees(folded.functions, index, folded.aliases.span(), folded.build.context());
    OptionalIndex targetPosition { *index.indexOf(FunctionID(3u)) };

    uint32_t jumpPosition = *index.indexOf(FunctionID(1u));
    ResolvedCallee jumpTarget = callees.at(jumpPosition, folded.functions[jumpPosition].body.relocations[0].symbol);
    AOT_TEST_CHECK(jumpTarget.body == targetPosition);
    AOT_TEST_CHECK(!jumpTarget.alias);

    uint32_t caller = *index.indexOf(FunctionID(5u));
    ResolvedCallee callee = callees.at(caller, folded.functions[caller].body.relocations[0].symbol);
    AOT_TEST_CHECK(callee.body == targetPosition);
    AOT_TEST_CHECK(!!callee.alias);
    AOT_TEST_CHECK(folded.aliases[*callee.alias].function == FunctionID(4u));
    AOT_TEST_CHECK(!folded.build.count(InternalErrorKind::CallToFunctionWithoutCode));
}

static constexpr NativeTest mergingTests[] = {
    { "image/code-index/lookup"_s, testCodeIndex },
    { "image/code-index/alias-lookup"_s, testAliasIndex },
    { "image/trim/padding-goes"_s, testPaddingGoes },
    { "image/trim/one-unit-after-returning-call"_s, testOneUnitAfterReturningCall },
    { "image/trim/fault-at-end-grows-body"_s, testFaultAtEndGrowsBody },
    { "image/trim/nothing-to-trim"_s, [] { checkBodyUnchanged(framed(1)); } },
    { "image/trim/trimmed-twice"_s, [] { checkBodyUnchanged(trimmedBody()); } },
    { "image/jump-threading/chain-of-three"_s, testChainOfThree },
    { "image/jump-threading/chain-of-forty"_s, testChainOfForty },
    { "image/jump-threading/caller-callees-unchanged"_s, testCallerCalleesUnchanged },
    { "image/jump-threading/prepended"_s, [] { checkHeadIsPrepended(movesAndJump, 0); } },
    { "image/jump-threading/prepended-head-is-padded"_s, testPrependedHeadIsPadded },
    { "image/jump-threading/entrance-ends-in-jump-to-next-body"_s, testEntranceEndsInJumpToNextBody },
#if CPU(X86_64)
    { "image/jump-threading/entrance-of-even-size-has-no-prefix"_s, testEntranceOfEvenSizeHasNoPrefix },
#endif
    { "image/jump-threading/head-moves-with-entrance"_s, testHeadMovesWithEntrance },
    { "image/jump-threading/entrance-far-from-its-body-stays"_s, testEntranceFarFromItsBodyStays },
    { "image/jump-threading/prefix-and-jump"_s, [] { checkHeadIsPrepended(prefixAndJump, sizeOfDataPrefix); } },
    { "image/jump-threading/second-head-is-appended"_s, [] { checkSecondHeadIsAppended(movesAndJump, 0); } },
    { "image/jump-threading/second-prefix-is-appended"_s, [] { checkSecondHeadIsAppended(prefixAndJump, sizeOfDataPrefix); } },
    { "image/jump-threading/two-guests"_s, testTwoGuests },
    { "image/jump-threading/appended-jump-through-chain"_s, testAppendedJumpThroughChain },
    { "image/jump-threading/target-with-two-entries-takes-no-head"_s, [] { checkTargetTakesNoHead([](CodeSketch& sketch) { addSecondaryEntry(sketch); }); } },
#if CPU(X86_64)
    { "image/jump-threading/appended-to-host-with-odd-entry"_s, testAppendedToHostWithOddEntry },
#endif
    { "image/jump-threading/proof-moves-to-target"_s, [] { AOT_TEST_CHECK(isProvenAfterThreading(bodies(withStackProof(oneJump(1, 2)), withoutStackCheck(framed(2))), 2)); } },
    { "image/jump-threading/prepended-proof-moves-to-target"_s, [] { AOT_TEST_CHECK(isProvenAfterThreading(bodies(framed(1), withStackProof(movesAndJump(2, 3)), withoutStackCheck(framed(3))), 3)); } },
    { "image/jump-threading/appended-proof-moves-to-target"_s, [] { AOT_TEST_CHECK(isProvenAfterThreading(bodies(framed(1), withStackProof(movesAndJump(2, 3)), oneJump(3, 4), withoutStackCheck(framed(4))), 4, 2)); } },
    { "image/jump-threading/two-jumps-in-cycle"_s, [] { checkNothingRemoved(bodies(oneJump(1, 2), oneJump(2, 1))); } },
    { "image/jump-threading/self-jump"_s, [] { checkNothingRemoved(bodies(oneJump(1, 1))); } },
    { "image/jump-threading/moves-and-jump-with-caller"_s, [] { checkNothingRemoved(bodies(framed(1), movesAndJump(2, 3), framed(3), framedCaller(4, 2))); } },
    { "image/jump-threading/address-taken"_s, testAddressTaken },
    { "image/jump-threading/instruction-after-jump"_s, [] { checkJumpBodyStays([](CodeSketch&) { }, [](CodeSketch& sketch) { sketch.end(); }); } },
    { "image/jump-threading/has-frame"_s, [] { checkJumpBodyStays([](CodeSketch& sketch) { sketch.withFrame(); }); } },
    { "image/jump-threading/jump-to-stub"_s, [] { checkNothingRemoved(bodies(framed(1), duplicateBody(2, 5), framed(3))); } },
    { "image/jump-threading/has-site"_s, [] { checkJumpBodyStays([](CodeSketch& sketch) { sketch.addSite(); }); } },
    { "image/jump-threading/has-secondary-entry"_s, [] { checkJumpBodyStays([](CodeSketch& sketch) { addSecondaryEntry(sketch); }); } },
    { "image/jump-threading/jump-to-thunk"_s, [] { checkJumpBodyStays([](CodeSketch&) { }, jumpToThunk); } },
    { "image/jump-threading/jump-to-secondary-entry"_s, [] { checkJumpBodyStays([](CodeSketch&) { }, [](CodeSketch& sketch) { sketch.lastRelocation().entry = 1; }); } },
    { "image/jump-threading/first-body-has-no-host"_s, testFirstBodyHasNoHost },
    { "image/jump-threading/target-without-code"_s, [] { checkNothingRemoved(bodies(framed(1), movesAndJump(2, 9))); } },
    { "image/jump-threading/checked-target-stays-unproven"_s, [] { AOT_TEST_CHECK(!isProvenAfterThreading(bodies(withStackProof(oneJump(1, 2)), framed(2)), 2)); } },
    { "image/jump-threading/unproven-jump-adds-no-proof"_s, [] { AOT_TEST_CHECK(!isProvenAfterThreading(bodies(oneJump(1, 2), withoutStackCheck(framed(2))), 2)); } },
    { "image/folding/duplicates"_s, testDuplicates },
    { "image/folding/foldable-callees"_s, [] { checkFolded(bodies(movesAndJump(1, 3), movesAndJump(2, 4), duplicateBody(3), duplicateBody(4)), { 1, 3 }); } },
    { "image/folding/cycles-fold-in-pairs"_s, testCyclesFoldInPairs },
    { "image/folding/alias-matches-target"_s, testAliasMatchesTarget },
    { "image/folding/hosts-with-equal-guests"_s, testHostsWithEqualGuests },
    { "image/folding/proof-is-retained"_s, testProofIsRetained },
    { "image/folding/entrypoints-with-frames"_s, [] { AOT_TEST_CHECK(foldEntrypoints({ 1 }, { 2 }) == 1); } },
    { "image/folding/frameless-entrypoints"_s, [] { AOT_TEST_CHECK(foldEntrypoints({ 1, std::nullopt }, { 2, std::nullopt }) == 1); } },
    { "image/folding/three-classes"_s, [] { checkFolded(bodies(duplicateBody(1), duplicateBody(2, 8), duplicateBody(3), duplicateBody(4, 8), duplicateBody(5, 9)), { 1, 2, 5 }); } },
    { "image/folding/one-site-cache-each"_s, [] { checkFolded(bodies(siteCacheBody(1, { 10 }), siteCacheBody(2, { 11 })), { 1 }); } },
    { "image/folding/equal-site-cache-patterns"_s, [] { checkFolded(bodies(siteCacheBody(1, { 10, 11, 10 }), siteCacheBody(2, { 12, 3, 12 })), { 1 }); } },
    { "image/folding/equal-constants"_s, [] { checkFolded(bodies(constantBody(1, 0x3fb999999999999a), constantBody(2, 0x3fb999999999999a)), { 1 }); } },
    { "image/folding/equal-jump-tables"_s, [] { checkFolded(bodies(jumpTableBody(1, { 0, 4 }), jumpTableBody(2, { 0, 4 })), { 1 }); } },
    { "image/folding/equal-addresses"_s, [] { checkFolded(bodies(addressBody(1, 5), addressBody(2, 5), framed(5)), { 1, 5 }); } },
    { "image/folding/equal-moves"_s, [] { checkFolded(bodies(thunkCallBody(1, 1), thunkCallBody(2, 1)), { 1 }); } },
    { "image/folding/other-bytes"_s, [] { checkNotFolded(bodies(duplicateBody(1), duplicateBody(2, 8))); } },
    { "image/folding/other-size"_s, [] { checkNotFolded(bodies(duplicateBody(1), duplicateBody(2, 7, catchException(), 2))); } },
    { "image/folding/other-stub"_s, [] { checkNotFolded(bodies(duplicateBody(1), duplicateBody(2, 7, rejectInFrame(false)))); } },
    { "image/folding/other-callee"_s, [] { checkNotFolded(bodies(movesAndJump(1, 3), movesAndJump(2, 4), framed(3), framed(4))); } },
    { "image/folding/other-moves"_s, [] { checkNotFolded(bodies(thunkCallBody(1, 1), thunkCallBody(2, 2))); } },
    { "image/folding/other-site-cache-pattern"_s, [] { checkNotFolded(bodies(siteCacheBody(1, { 10, 10 }), siteCacheBody(2, { 10, 11 }))); } },
    { "image/folding/other-constant"_s, [] { checkNotFolded(bodies(constantBody(1, 1), constantBody(2, 2))); } },
    { "image/folding/other-constant-alignment"_s, [] { checkNotFolded(bodies(constantBody(1, 1), constantBody(2, 1, 16))); } },
    { "image/folding/other-jump-table"_s, [] { checkNotFolded(bodies(jumpTableBody(1, { 0, 4 }), jumpTableBody(2, { 4, 0 }))); } },
    { "image/folding/other-address"_s, [] { checkNotFolded(bodies(addressBody(1, 5), addressBody(2, 6), framed(5), framed(6))); } },
    { "image/folding/other-entry-offset"_s, testOtherEntryOffset },
    { "image/folding/visible-function-with-site"_s, [] { checkNotFolded(bodies(visibleBodyWithSite(1), visibleBodyWithSite(2))); } },
    { "image/folding/ordinary-frames"_s, [] { checkRecordsNotFolded([](CodeSketch& sketch) { sketch.withFrame(); }); } },
    { "image/folding/other-frame-size"_s, [] { AOT_TEST_CHECK(!foldEntrypoints({ 1, 32 }, { 2, 48 })); } },
    { "image/folding/entrypoints-of-framed-callee"_s, [] { AOT_TEST_CHECK(!foldEntrypoints({ 1 }, { 2 }, framed(entrypointCallee))); } },
    { "image/folding/entrypoint-of-alias"_s, [] { AOT_TEST_CHECK(!foldEntrypoints({ 1, 32, 1 }, { 2, 32, 2 }, framed(1), { aliasAt(2, 1) })); } },
    { "image/folding/async-entrypoint"_s, [] { AOT_TEST_CHECK(!foldEntrypoints({ 1 }, { asyncFunction })); } },
    { "image/folding/frameless-async-entrypoint"_s, [] { AOT_TEST_CHECK(!foldEntrypoints({ 1, std::nullopt }, { asyncFunction, std::nullopt })); } },
    { "image/folding/sloppy-entrypoint"_s, [] { AOT_TEST_CHECK(!foldEntrypoints({ 1 }, { sloppyFunction })); } },
    { "image/folding/host-against-plain-body"_s, testHostAgainstPlainBody },
    { "image/folding/handler"_s, [] { checkRecordsNotFolded([](CodeSketch& sketch) { sketch.code().sites.handlers.append(SiteHandler { 1, 0, HandlerType::Catch }); }); } },
    { "image/folding/frame-maps"_s, [] { checkRecordsNotFolded([](CodeSketch& sketch) { sketch.code().sites.frameMapWords.append(1); }); } },
    { "image/folding/other-stack-use"_s, testOtherStackUse },
    { "image/folding/other-check"_s, [] { checkNotFolded(bodies(duplicateBody(1), withoutStackCheck(duplicateBody(2)))); } },
    { "image/folding/callee-without-code"_s, [] { checkNotFolded(bodies(movesAndJump(1, 9), movesAndJump(2, 9))); } },
    { "image/folding/callee-is-inner-entry"_s, [] { checkNotFolded(bodies(movesAndJump(1, 9), movesAndJump(2, 9), framed(5)), { appendedAliasAt(9, 5, 8) }); } },
    { "image/folding/threads"_s, testThreads },
    { "image/folding/callees-resolve-after-folding"_s, testCalleesResolveAfterFolding },
};

std::span<const NativeTest> functionMergingTests()
{
    return mergingTests;
}

} // namespace JSC::AOT::ImageTests

#endif // ENABLE(AOT)
