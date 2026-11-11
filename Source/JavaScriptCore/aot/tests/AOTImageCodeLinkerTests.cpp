/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageBuildSteps.h"
#include "AOTVector.h"

#include "AOTBodyFactsForTables.h"
#include "AOTCodeLayout.h"
#include "AOTImageTestSupport.h"
#include "AOTNumbering.h"
#include "AOTOperationData.h"
#include "AOTStubRegisters.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsOperations.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTableParts.h"
#include "GPRInfo.h"
#include "MacroAssembler.h"
#include <bit>
#include <optional>
#include <span>
#include <wtf/BitSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringView.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace ImageTests {

namespace {

struct BrokenRelocation {
    SymbolSpace space;
    void (*damage)(Relocation&);
};

} // anonymous namespace

static constexpr uint32_t numberOfCalleesOfLinkedBody = 3;
static constexpr uint16_t entryThatNoLinkedBodyHas = 7;

static constexpr uint32_t linkedStringMention = 0;
static constexpr uint32_t linkedStringCellMention = 1;
static constexpr uint32_t linkedRegExpMention = 2;

template<typename Emit>
static uint32_t appendRelocated(CodeSketch& sketch, const Emit& emit)
{
    CCallHelpers jit;
    RelocationRecorder recorder(jit, testAssemblerPolicy());
    emit(recorder);
    auto unit = recorder.finalize(LinkBuffer::Profile::FTL, nullptr);
    AOT_TEST_CHECK(unit);
    AOT_TEST_CHECK(unit->relocations.size() == 1);
    CodeBody& body = sketch.code().body;
    uint32_t offset = sketch.pinnedBytes(static_cast<unsigned>(unit->bytes.size()));
    memcpySpan(body.bytes.mutableSpan().subspan(offset), unit->bytes.span());
    Relocation relocation = unit->relocations[0];
    relocation.offset += offset;
    body.relocations.append(relocation);
    return offset;
}

static ConstantData constantOf(uint64_t word)
{
    return { Vector<uint8_t>(asByteSpan(word)), sizeof(uint64_t) };
}

static CompiledCode bodyWithManySpaces(uint32_t function, uint32_t callee)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.pinned(0x2000 + function);
    uint32_t functionCall = sketch.callFunction(callee, sketch.addSite());
    sketch.callStub(writeBarrier(), sketch.addSite());
    sketch.callThunk(stubArgumentMoves(writeBarrier(), 1), sketch.addSite());
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.moveAddress({ SymbolSpace::RegExp, linkedRegExpMention, 1 }, GPRInfo::regT0, PCRelativeReach::Far);
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.moveAddress({ SymbolSpace::Constant, 0 }, GPRInfo::regT0, PCRelativeReach::Far);
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.load64({ SymbolSpace::Constant, 1 }, GPRInfo::regT0);
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.moveAddress({ SymbolSpace::JumpTable, 0 }, GPRInfo::regT0, PCRelativeReach::Far);
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.moveNumber({ SymbolSpace::Number, linkedStringMention }, GPRInfo::regT0);
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.emitNumber32({ SymbolSpace::Number, linkedStringMention });
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.loadInstanceWord({ SymbolSpace::InstanceWord, linkedStringCellMention }, InstanceWordWidth::Word32, instanceGPR, GPRInfo::regT0);
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.moveInstanceAddress({ SymbolSpace::InstanceWord, linkedStringCellMention }, instanceGPR, GPRInfo::regT0);
    });
    uint32_t end = sketch.end();
    RareListsOfBody& rareLists = sketch.code().body.rareLists.ensure();
    rareLists.constants = { constantOf(0x1122334455667788), constantOf(0x99aabbccddeeff00) };
    rareLists.jumpTables = { JumpTable { { functionCall, end } } };
    return sketch.take();
}

static CompiledCode calleeOfLinkedBody(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.plain(0x1000 + function);
    sketch.end();
    return sketch.take();
}

static Vector<CompiledCode> bodiesWithEverySpace(uint32_t& function)
{
    Vector<CompiledCode> code;
    for (uint32_t callee = 1; callee <= numberOfCalleesOfLinkedBody; ++callee)
        code.append(calleeOfLinkedBody(callee));
    function = static_cast<uint32_t>(code.size()) + 1;
    code.append(bodyWithManySpaces(function, 1));
    return code;
}

static RegExpSection createRegExpSection()
{
    RegExpSection section;
    while (section.bytes.size() < 64)
        section.bytes.appendVector(testTemplates().breakpoint);
    section.entries.append({ 0, 32 });
    return section;
}

static uint32_t positionOfLinkedBody(const Steps& steps, uint32_t function)
{
    for (uint32_t position = 0; position < steps.functions.size(); ++position) {
        if (steps.functions[position].unit == bodyUnitOf(function))
            return position;
    }
    AOT_TEST_CHECK(false);
    return 0;
}

static void addMentionsOfLinkedBody(Steps& steps, uint32_t function)
{
    CompiledCode& code = steps.functions[positionOfLinkedBody(steps, function)];
    StringID text = steps.build.program().strings.intern(StringView { "linked"_s });
    MentionList mentions;
    uint32_t string = mentions.add(NumberSpace::String, text);
    uint32_t stringCell = mentions.add(NumberSpace::StringCell, text);
    uint32_t regExp = mentions.regExp(text, { });
    AOT_TEST_CHECK(string == linkedStringMention && stringCell == linkedStringCellMention && regExp == linkedRegExpMention);
    mentions.moveTo(code.body);
}

static void numberMentionsOfLinkedBodies(Steps& steps, ImageNumbers& numbers, FunctionID functionWhoseClosureCellTablesName = { })
{
    TestBuild& build = steps.build;
    BuildContext& context = build.context();
    BodyFactsForTables bodyFacts = collectBodyFactsForTables(build.program(), build.plan(), steps.functions, context);
    internPiecesOfModulePaths(build.program());
    NumberingInputs numberingInputs { build.program(), build.facts(), build.plan(), steps.functions };
    GatheredMentions mentions = gatherMentions(numberingInputs, context);
    TableInputs tableInputs { build.program(), build.facts(), build.plan(), steps.functions, bodyFacts, context.options(), &context };
    NamesOfTables names = nameWhatTablesName(tableInputs, mentions, context);
    if (functionWhoseClosureCellTablesName)
        names.functionsWithClosureCell.set(functionWhoseClosureCellTablesName.index());
    numberMentions(numberingInputs, mentions, names, numbers, context);
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
}

static void prepareToLink(Steps& steps, uint32_t functionWithManySpaces, ImageNumbers& numbers)
{
    addMentionsOfLinkedBody(steps, functionWithManySpaces);
    numberMentionsOfLinkedBodies(steps, numbers);
    steps.regExpSection = createRegExpSection();
    steps.removeJumps();
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());
}

static Vector<uint8_t> link(Steps& steps, const ImageNumbers& numbers)
{
    Vector<uint8_t> code(FillWith { }, static_cast<size_t>(steps.layout.codeSize), static_cast<uint8_t>(0));
    CodeLayoutInputs layoutInputs = steps.layoutInputs();
    linkCode({ layoutInputs, steps.layout, steps.constants, numbers }, code.mutableSpan(), steps.build.context());
    return code;
}

static uint32_t valueOfLinkedMention(const ImageNumbers& numbers, const CodeBody& body, uint32_t mention, bool asInstanceWord)
{
    std::optional<uint32_t> value = numbers.valueOf(body.mentions[mention], body.mentionWords.span(), asInstanceWord);
    AOT_TEST_CHECK(value);
    return *value;
}

static size_t expectedTarget(const Steps& steps, const ImageNumbers& numbers, uint32_t position, const Relocation& relocation)
{
    const CodeBody& body = steps.functions[position].body;
    const CodeLayout& layout = steps.layout;
    const SharedCode& sharedCode = steps.sharedCode;
    size_t copy = layout.sharedCodeCopyByBody[position];
    uint32_t symbol = relocation.symbol;
    switch (relocation.space) {
    case SymbolSpace::Function: {
        uint32_t callee = *steps.callees.at(position, symbol);
        return layout.bodyStarts[callee] + *steps.functions[callee].body.offsetOfEntry(relocation.entry);
    }
    case SymbolSpace::Stub:
        return copy + *sharedCode.blob.offsetOf(body.stubKeys[symbol]);
    case SymbolSpace::ArgumentThunk:
        return copy + sharedCode.argumentThunksOffset + sharedCode.thunkOffset(body.argumentMoves[symbol]);
    case SymbolSpace::Constant:
        return layout.constantsOffset + steps.constants.constantOffsetsByBody[position][symbol];
    case SymbolSpace::JumpTable:
        return layout.constantsOffset + steps.constants.jumpTableOffsetsByBody[position][symbol];
    case SymbolSpace::Number:
        return valueOfLinkedMention(numbers, body, symbol, false);
    case SymbolSpace::InstanceWord:
        return valueOfLinkedMention(numbers, body, symbol, true);
    case SymbolSpace::RegExp:
        return layout.regExpCodeOffset + steps.regExpSection.entries[valueOfLinkedMention(numbers, body, symbol, false)][relocation.entry];
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        break;
    }
    AOT_TEST_CHECK(false);
    return 0;
}

static bool fitsInBody(const CodeBody& body, const Relocation& relocation)
{
    return relocation.offset + MacroAssembler::relocationSize(relocation.kind) <= body.bytes.size();
}

static void checkLinkedBody(const Steps& steps, const ImageNumbers& numbers, uint32_t position, std::span<const uint8_t> code)
{
    const CodeBody& body = steps.functions[position].body;
    size_t start = steps.layout.bodyStarts[position];
    AOT_TEST_CHECK(steps.layout.veneersByBody[position].isEmpty());
    Vector<uint8_t> expected(FillWith { }, code.size(), static_cast<uint8_t>(0));
    memcpySpan(expected.mutableSpan().subspan(start), body.bytes.span());
    for (const Relocation& relocation : body.relocations) {
        if (fitsInBody(body, relocation))
            AOT_TEST_CHECK(applyRelocation(expected.mutableSpan(), start, relocation, expectedTarget(steps, numbers, position, relocation)));
    }
    AOT_TEST_CHECK(equalSpans(code.subspan(start, body.bytes.size()), expected.span().subspan(start, body.bytes.size())));
}

static void checkLinkedData(const Steps& steps, uint32_t position, std::span<const uint8_t> code)
{
    const RareListsOfBody& rareLists = *steps.functions[position].body.rareLists;
    size_t constantsOffset = steps.layout.constantsOffset;
    for (size_t index = 0; index < rareLists.constants.size(); ++index) {
        size_t offset = constantsOffset + steps.constants.constantOffsetsByBody[position][index];
        AOT_TEST_CHECK(!(offset % rareLists.constants[index].alignment));
        AOT_TEST_CHECK(equalSpans(code.subspan(offset, rareLists.constants[index].bytes.size()), rareLists.constants[index].bytes.span()));
    }
    for (size_t index = 0; index < rareLists.jumpTables.size(); ++index) {
        size_t table = constantsOffset + steps.constants.jumpTableOffsetsByBody[position][index];
        std::span<const uint32_t> targets = rareLists.jumpTables[index].targets.span();
        for (size_t word = 0; word < targets.size(); ++word) {
            int64_t distance = static_cast<int64_t>(steps.layout.bodyStarts[position]) + targets[word] - static_cast<int64_t>(table);
            AOT_TEST_CHECK(WTF::unalignedLoad<int32_t>(code.subspan(table + word * sizeof(int32_t)).data()) == distance);
        }
    }
}

static void checkLinkedBodies(const Steps& steps, const ImageNumbers& numbers, std::span<const uint8_t> code)
{
    for (uint32_t position = 0; position < steps.functions.size(); ++position) {
        checkLinkedBody(steps, numbers, position, code);
        checkLinkedData(steps, position, code);
    }
}

static WTF::BitSet<numberOfSymbolSpaces> spacesNamedBy(const CodeBody& body)
{
    WTF::BitSet<numberOfSymbolSpaces> spaces;
    for (const Relocation& relocation : body.relocations)
        spaces.set(static_cast<size_t>(relocation.space));
    return spaces;
}

static void testEverySpaceLinks()
{
    uint32_t function = 0;
    Steps steps(bodiesWithEverySpace(function));
    ImageNumbers numbers;
    prepareToLink(steps, function, numbers);
    const CodeBody& body = steps.functions[positionOfLinkedBody(steps, function)].body;
    WTF::BitSet<numberOfSymbolSpaces> spaces = spacesNamedBy(body);
    AOT_TEST_CHECK(spaces.count() == numberOfSymbolSpaces - 2);
    AOT_TEST_CHECK(!spaces.get(static_cast<size_t>(SymbolSpace::WebAssembly)));
    AOT_TEST_CHECK(!spaces.get(static_cast<size_t>(SymbolSpace::FFI)));
    AOT_TEST_CHECK(valueOfLinkedMention(numbers, body, linkedStringMention, false) != linkedStringMention);
    AOT_TEST_CHECK(valueOfLinkedMention(numbers, body, linkedRegExpMention, false) != linkedRegExpMention);

    Vector<uint8_t> code = link(steps, numbers);
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());
    checkLinkedBodies(steps, numbers, code.span());
}

static Relocation& relocationIn(Steps& steps, uint32_t position, SymbolSpace space)
{
    for (Relocation& relocation : steps.functions[position].body.relocations) {
        if (relocation.space == space)
            return relocation;
    }
    AOT_TEST_CHECK(false);
    return steps.functions[position].body.relocations[0];
}

static const InternalError& onlyError(Steps& steps, InternalErrorKind kind)
{
    AOT_TEST_CHECK(steps.build.count(kind) == 1);
    const Vector<InternalError>& errors = steps.build.context().diagnostics().internalErrors();
    AOT_TEST_CHECK(errors.size() == 1);
    return errors[0];
}

static void checkLinkerReported(Steps& steps, const Relocation& relocation)
{
    const InternalError& error = onlyError(steps, InternalErrorKind::RelocationNotApplied);
    AOT_TEST_CHECK(error.function == FunctionID(1u));
    AOT_TEST_CHECK(error.count == relocation.offset);
}

static void testMissingStub()
{
    Steps steps(bodies(bodyWithManySpaces(1, 2), calleeOfLinkedBody(2)));
    ImageNumbers numbers;
    prepareToLink(steps, 1, numbers);
    AOT_TEST_CHECK(exists(toPropertyKey()));
    AOT_TEST_CHECK(!steps.sharedCode.blob.offsetOf(toPropertyKey()));
    steps.functions[0].body.stubKeys[relocationIn(steps, 0, SymbolSpace::Stub).symbol] = toPropertyKey();
    link(steps, numbers);
    AOT_TEST_CHECK(onlyError(steps, InternalErrorKind::CallToStubNotGenerated).function == FunctionID(1u));
}

static void testRelocationPastEnd()
{
    Steps steps(bodies(bodyWithManySpaces(1, 2), calleeOfLinkedBody(2)));
    ImageNumbers numbers;
    prepareToLink(steps, 1, numbers);
    CodeBody& body = steps.functions[0].body;
    Relocation& relocation = body.relocations.last();
    relocation.offset = static_cast<uint32_t>(body.bytes.size() - codeOffsetUnit);
    AOT_TEST_CHECK(!fitsInBody(body, relocation));
    Vector<uint8_t> code = link(steps, numbers);
    checkLinkerReported(steps, relocation);
    checkLinkedBodies(steps, numbers, code.span());
}

static void checkLinkerReportsEach(std::span<const BrokenRelocation> cases)
{
    for (const BrokenRelocation& broken : cases) {
        Steps steps(bodies(bodyWithManySpaces(1, 2), calleeOfLinkedBody(2)));
        ImageNumbers numbers;
        prepareToLink(steps, 1, numbers);
        Relocation& relocation = relocationIn(steps, 0, broken.space);
        broken.damage(relocation);
        link(steps, numbers);
        checkLinkerReported(steps, relocation);
    }
}

static void nameSymbolPastEnd(Relocation& relocation)
{
    relocation.symbol = 1000;
}

static void testRelocationsThatNameNothing()
{
    static constexpr BrokenRelocation cases[] = {
        { SymbolSpace::Stub, [](Relocation& relocation) { relocation.space = SymbolSpace::WebAssembly; } },
        { SymbolSpace::Stub, [](Relocation& relocation) { relocation.space = SymbolSpace::FFI; } },
        { SymbolSpace::RegExp, [](Relocation& relocation) { relocation.entry = 2; } },
        { SymbolSpace::Function, nameSymbolPastEnd },
        { SymbolSpace::Stub, nameSymbolPastEnd },
        { SymbolSpace::ArgumentThunk, nameSymbolPastEnd },
        { SymbolSpace::Constant, nameSymbolPastEnd },
        { SymbolSpace::JumpTable, nameSymbolPastEnd },
        { SymbolSpace::Number, nameSymbolPastEnd },
        { SymbolSpace::InstanceWord, nameSymbolPastEnd },
        { SymbolSpace::RegExp, nameSymbolPastEnd },
    };
    checkLinkerReportsEach(cases);
}

static void nameNumberOfLinkedString(Relocation& relocation)
{
    relocation.space = SymbolSpace::Number;
    relocation.symbol = linkedStringMention;
}

static void nameWordOfLinkedStringCell(Relocation& relocation)
{
    relocation.space = SymbolSpace::InstanceWord;
    relocation.symbol = linkedStringCellMention;
}

static void testKindsThatDoNotFitTheirSpace()
{
    static constexpr BrokenRelocation cases[] = {
        { SymbolSpace::Stub, nameNumberOfLinkedString },
        { SymbolSpace::Stub, nameWordOfLinkedStringCell },
        { SymbolSpace::Number, nameWordOfLinkedStringCell },
    };
    checkLinkerReportsEach(cases);
}

static void testMentionWithoutInstanceWord()
{
    Steps steps(bodies(bodyWithManySpaces(1, 2), calleeOfLinkedBody(2)));
    ImageNumbers numbers;
    prepareToLink(steps, 1, numbers);
    const CodeBody& body = steps.functions[0].body;
    Relocation& relocation = relocationIn(steps, 0, SymbolSpace::InstanceWord);
    AOT_TEST_CHECK(relocation.symbol == linkedStringCellMention);
    AOT_TEST_CHECK(!numbers.valueOf(body.mentions[linkedStringMention], body.mentionWords.span(), true));
    relocation.symbol = linkedStringMention;
    link(steps, numbers);
    checkLinkerReported(steps, relocation);
}

static void testEntryThatCalleeLacks()
{
    Steps steps(bodies(bodyWithManySpaces(1, 2), calleeOfLinkedBody(2)));
    ImageNumbers numbers;
    prepareToLink(steps, 1, numbers);
    Relocation& call = relocationIn(steps, 0, SymbolSpace::Function);
    AOT_TEST_CHECK(!steps.functions[*steps.callees.at(0, call.symbol)].body.offsetOfEntry(entryThatNoLinkedBodyHas));
    call.entry = entryThatNoLinkedBodyHas;
    link(steps, numbers);
    AOT_TEST_CHECK(steps.build.context().diagnostics().hasErrors());
}

static uint32_t firstThrowWordOfSiteWithName()
{
    ThrowSiteWord word { };
    word.kind = static_cast<uint32_t>(ThrowKind::NullishRead);
    word.representation = static_cast<uint32_t>(Typed::Representation::JSValue);
    word.registerNumber = 3;
    word.hasWideA = true;
    return std::bit_cast<uint32_t>(word);
}

static CompiledCode bodyThatThrowsWithName(uint32_t function, uint32_t mentionOfName)
{
    ArgumentMoves moves;
    moves.append(throwAtSite(ThrowSiteSource::WordInRegister, CallerFrame::HasFrame).bits());
    moves.append(0);
    appendArgumentMove(moves, throwWordGPR, ArgumentSource::ThrowWordWithMention, ThrowSiteWord::inRegister(firstThrowWordOfSiteWithName(), mentionOfName));

    CodeSketch sketch(function);
    sketch.withFrame();
    uint32_t access = sketch.pinned(0x5000 + function);
    sketch.code().sites.faultingAccesses.append(FaultingAccess { access, { }, { firstThrowWordOfSiteWithName(), mentionOfName }, 1 << 1 });
    sketch.callThunk(moves, sketch.addSite());
    sketch.end();
    return sketch.take();
}

static void testThrowWordsThatNameOneStringShareThunk()
{
    constexpr uint32_t mentionInFirstBody = 1;
    constexpr uint32_t mentionInSecondBody = 0;
    Steps steps(bodies(bodyThatThrowsWithName(1, mentionInFirstBody), bodyThatThrowsWithName(2, mentionInSecondBody)));
    Typed::StringTable& strings = steps.build.program().strings;
    StringID name = strings.intern(StringView { "thrown"_s });
    StringID otherName = strings.intern(StringView { "other"_s });
    MentionList mentions;
    AOT_TEST_CHECK(!mentions.add(NumberSpace::String, otherName) && mentions.add(NumberSpace::String, name) == mentionInFirstBody);
    mentions.moveTo(steps.functions[positionOfLinkedBody(steps, 1)].body);
    AOT_TEST_CHECK(mentions.add(NumberSpace::String, name) == mentionInSecondBody);
    mentions.moveTo(steps.functions[positionOfLinkedBody(steps, 2)].body);

    ImageNumbers numbers;
    numberMentionsOfLinkedBodies(steps, numbers);
    applyNumbersToFunctions(steps.functions, numbers, steps.build.context());
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());

    uint32_t numberOfName = valueOfLinkedMention(numbers, steps.functions[positionOfLinkedBody(steps, 2)].body, mentionInSecondBody, false);
    std::optional<uint32_t> expected = ThrowSiteWord::withNarrowA(firstThrowWordOfSiteWithName(), numberOfName);
    AOT_TEST_CHECK(expected && std::bit_cast<ThrowSiteWord>(*expected).narrowA() == numberOfName);
    for (uint32_t position = 0; position < steps.functions.size(); ++position) {
        const CompiledCode& code = steps.functions[position];
        std::span<const uint64_t> moves = code.body.argumentMoves[0];
        AOT_TEST_CHECK(sourceOfArgumentMove(moves, numberOfHeaderWords) == ArgumentSource::Constant && moves[numberOfHeaderWords + 1] == *expected);
        const FaultingAccess& access = code.sites.faultingAccesses[0];
        AOT_TEST_CHECK(!access.wordsThatAreMentions && access.throwSite.size() == 1 && access.throwSite[0] == *expected);
    }
    steps.select();
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());
    AOT_TEST_CHECK(steps.sharedCode.argumentMoves.size() == 1);

    AOT_TEST_CHECK(ThrowSiteWord::withNarrowA(firstThrowWordOfSiteWithName(), ThrowSiteWord::maxNarrowA));
    AOT_TEST_CHECK(!ThrowSiteWord::withNarrowA(firstThrowWordOfSiteWithName(), ThrowSiteWord::narrowNone));
}

static CompiledCode bodyThatReadsByName(uint32_t function, uint32_t mentionOfName)
{
    ArgumentMoves words;
    words.append(getById().bits());
    words.append(hasImmediate | immediateIsMention | mentionOfName);

    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.callThunk(words, sketch.addSite());
    sketch.end();
    return sketch.take();
}

static void testReadsOfOneNameShareThunk()
{
    static_assert(descriptorOf(StubFamily::GetById).binding == ImmediateBinding::InThunk);
    constexpr uint32_t mentionInFirstBody = 1;
    constexpr uint32_t mentionInSecondBody = 0;
    Steps steps(bodies(bodyThatReadsByName(1, mentionInFirstBody), bodyThatReadsByName(2, mentionInSecondBody)));
    Typed::StringTable& strings = steps.build.program().strings;
    StringID name = strings.intern(StringView { "read"_s });
    StringID otherName = strings.intern(StringView { "other"_s });
    MentionList mentions;
    AOT_TEST_CHECK(!mentions.add(NumberSpace::String, otherName) && mentions.add(NumberSpace::String, name) == mentionInFirstBody);
    mentions.moveTo(steps.functions[positionOfLinkedBody(steps, 1)].body);
    AOT_TEST_CHECK(mentions.add(NumberSpace::String, name) == mentionInSecondBody);
    mentions.moveTo(steps.functions[positionOfLinkedBody(steps, 2)].body);

    ImageNumbers numbers;
    numberMentionsOfLinkedBodies(steps, numbers);
    applyNumbersToFunctions(steps.functions, numbers, steps.build.context());
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());

    uint32_t numberOfName = valueOfLinkedMention(numbers, steps.functions[positionOfLinkedBody(steps, 2)].body, mentionInSecondBody, false);
    for (const CompiledCode& code : steps.functions) {
        std::span<const uint64_t> words = code.body.argumentMoves[0];
        AOT_TEST_CHECK(words.size() == numberOfHeaderWords && words[1] == (hasImmediate | numberOfName));
    }
    steps.select();
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());
    AOT_TEST_CHECK(steps.sharedCode.argumentMoves.size() == 1);
}

static CompiledCode closureEntryOfLinkedBody(uint32_t function)
{
    CodeSketch sketch(CodeUnit { FunctionID(function), Typed::CodeRole::GenericCallEntry });
    sketch.withFrame();
    sketch.plain(0x6000 + function);
    sketch.end();
    return sketch.take();
}

static void testClosureCellsThatTablesNameComeFirst()
{
    constexpr uint32_t numberOfClosures = 3;
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= numberOfClosures; ++function) {
        code.append(calleeOfLinkedBody(function));
        code.append(closureEntryOfLinkedBody(function));
    }
    Steps steps(WTF::move(code));
    for (uint32_t function = 1; function <= numberOfClosures; ++function)
        AOT_TEST_CHECK(steps.build.plan().hasClosureCell(FunctionID(function)));

    MentionList mentions;
    mentions.add(NumberSpace::ClosureCell, FunctionID(1u));
    mentions.add(NumberSpace::ClosureCell, FunctionID(2u));
    mentions.moveTo(steps.functions[positionOfLinkedBody(steps, 1)].body);

    ImageNumbers numbers;
    numberMentionsOfLinkedBodies(steps, numbers, FunctionID(numberOfClosures));
    AOT_TEST_CHECK(numbers.closureCellNumber(FunctionID(3u)) == 1);
    AOT_TEST_CHECK(numbers.closureCellNumber(FunctionID(1u)) == 2);
    AOT_TEST_CHECK(numbers.closureCellNumber(FunctionID(2u)) == 3);
    AOT_TEST_CHECK(numbers.numberOfClosureCells() == numberOfClosures + 1);
}

#if CPU(ARM64)
static constexpr uint64_t reachOfLinkedCall = MacroAssembler::relocationReach(RelocationKind::Call);
static constexpr size_t sizeOfFillerOfFarBody = reachOfLinkedCall / 8 * 5;
static constexpr uint32_t numberOfBodiesWithFarCalls = 4;
static constexpr uint16_t laterEntryOfFarBody = 1;
static_assert((numberOfBodiesWithFarCalls - 2) * sizeOfFillerOfFarBody > reachOfLinkedCall);

static void appendFillerOfFarBody(CodeSketch& sketch)
{
    constexpr size_t roomForWhatFollows = 1 * KB;
    auto& bytes = sketch.code().body.bytes;
    size_t start = bytes.size();
    bytes.reserveCapacity(start + sizeOfFillerOfFarBody + roomForWhatFollows);
    bytes.grow(start + sizeOfFillerOfFarBody);
    zeroSpan(bytes.mutableSpan().subspan(start));
}

static Vector<CompiledCode> bodiesWithFarCalls()
{
    constexpr uint32_t lastBody = numberOfBodiesWithFarCalls;
    Vector<CompiledCode> code;
    CodeSketch caller(1);
    caller.withFrame();
    caller.pinned(0x3000);
    caller.callFunction(lastBody, { }, laterEntryOfFarBody);
    caller.callFunction(lastBody);
    caller.callFunction(lastBody - 1, { }, laterEntryOfFarBody);
    caller.end();
    code.append(caller.take());
    for (uint32_t function = 2; function <= lastBody; ++function) {
        CodeSketch sketch(function);
        sketch.withFrame();
        sketch.pinned(0x3000 + function);
        if (function < lastBody)
            appendFillerOfFarBody(sketch);
        sketch.code().body.entries.append(CodeEntry { sketch.pinned(0x4000 + function), laterEntryOfFarBody });
        sketch.end();
        code.append(sketch.take());
    }
    return code;
}

static void checkLinksTo(std::span<uint8_t> code, size_t offset, std::span<const uint8_t> emitted, RelocationKind kind, size_t target)
{
    std::span<uint8_t> place = code.subspan(offset, emitted.size());
    Vector<uint8_t> linked(place);
    memcpySpan(place, emitted);
    AOT_TEST_CHECK(applyRelocation(code, offset, Relocation { 0, kind, SymbolSpace::Function, primaryEntry, 0 }, target));
    AOT_TEST_CHECK(equalSpans(place, linked.span()));
}

static void testFarCallsGoThroughVeneers()
{
    Steps steps(bodiesWithFarCalls());
    steps.removeJumps();
    const CodeLayout& layout = steps.layout;
    const CodeBody& caller = steps.functions[0].body;
    const Vector<VeneerTarget>& veneers = layout.veneersByBody[0];
    AOT_TEST_CHECK(veneers.size() == 3 && caller.relocations.size() == 3);

    ImageNumbers numbers;
    Vector<uint8_t> code = link(steps, numbers);
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());

    const RelocatableCode& veneer = testTemplates().veneer;
    auto offsetOfVeneer = [&](size_t index) {
        return layout.firstVeneerOffset(0, caller.bytes.size(), 0) + index * testTemplates().sizeOfVeneer();
    };
    for (const Relocation& call : caller.relocations) {
        size_t index = veneers.find(VeneerTarget { SymbolSpace::Function, call.entry, *steps.callees.at(0, call.symbol) });
        AOT_TEST_CHECK(index != notFound);
        std::span<const uint8_t> emitted = caller.bytes.span().subspan(call.offset, MacroAssembler::relocationSize(call.kind));
        checkLinksTo(code.mutableSpan(), static_cast<size_t>(layout.bodyStarts[0]) + call.offset, emitted, call.kind, offsetOfVeneer(index));
    }
    for (size_t index = 0; index < veneers.size(); ++index) {
        const VeneerTarget& target = veneers[index];
        size_t entry = static_cast<size_t>(layout.bodyStarts[target.index]) + *steps.functions[target.index].body.offsetOfEntry(target.entry);
        checkLinksTo(code.mutableSpan(), offsetOfVeneer(index), veneer.bytes.span(), veneer.relocations[0].kind, entry);
    }
}

static constexpr int32_t addendOfFarInstanceWord = 48 * MB;
static constexpr uint32_t instanceWordFarAbove = 0;
static constexpr uint32_t instanceWordFarBelow = 1;
static constexpr uint32_t nearInstanceWord = 2;

static CompiledCode bodyWithFarInstanceWords(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.pinned(0x7000 + function);
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.loadInstanceWord({ SymbolSpace::InstanceWord, instanceWordFarAbove }, InstanceWordWidth::Word32, instanceGPR, GPRInfo::regT0);
    });
    auto& bytes = sketch.code().body.bytes;
    size_t startOfFiller = bytes.size();
    bytes.grow(startOfFiller + reachOfReadOfFarInstanceWord + 64 * KB);
    zeroSpan(bytes.mutableSpan().subspan(startOfFiller));
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.moveInstanceAddress({ SymbolSpace::InstanceWord, instanceWordFarAbove }, instanceGPR, GPRInfo::regT0);
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.storeInstanceWord({ SymbolSpace::InstanceWord, instanceWordFarBelow }, InstanceWordWidth::Word64, GPRInfo::regT0, instanceGPR);
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.loadInstanceWord({ SymbolSpace::InstanceWord, instanceWordFarBelow }, InstanceWordWidth::Byte, instanceGPR, GPRInfo::regT0);
    });
    appendRelocated(sketch, [](RelocationRecorder& recorder) {
        recorder.loadInstanceWord({ SymbolSpace::InstanceWord, nearInstanceWord }, InstanceWordWidth::Word32, instanceGPR, GPRInfo::regT0);
    });
    sketch.end();
    return sketch.take();
}

static uint32_t addMentionsOfFarInstanceWords(Steps& steps, uint32_t function)
{
    uint32_t position = positionOfLinkedBody(steps, function);
    StringID text = steps.build.program().strings.intern(StringView { "far"_s });
    MentionList mentions;
    AOT_TEST_CHECK(mentions.add(NumberSpace::StringCell, text, addendOfFarInstanceWord) == instanceWordFarAbove);
    AOT_TEST_CHECK(mentions.add(NumberSpace::StringCell, text, -addendOfFarInstanceWord) == instanceWordFarBelow);
    AOT_TEST_CHECK(mentions.add(NumberSpace::StringCell, text) == nearInstanceWord);
    mentions.moveTo(steps.functions[position].body);
    return position;
}

static void testFarInstanceWordsLink()
{
    Steps steps(bodies(bodyWithFarInstanceWords(1), calleeOfLinkedBody(2)));
    uint32_t position = addMentionsOfFarInstanceWords(steps, 1);
    ImageNumbers numbers;
    numberMentionsOfLinkedBodies(steps, numbers);
    steps.farInstanceWords = findFarInstanceWords(steps.functions, numbers, steps.build.context());
    steps.removeJumps();
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());

    const CodeBody& body = steps.functions[position].body;
    int32_t above = static_cast<int32_t>(valueOfLinkedMention(numbers, body, instanceWordFarAbove, true));
    int32_t below = static_cast<int32_t>(valueOfLinkedMention(numbers, body, instanceWordFarBelow, true));
    uint32_t distanceOfNearWord = valueOfLinkedMention(numbers, body, nearInstanceWord, true);
    AOT_TEST_CHECK(isFarInstanceWord(above) && isFarInstanceWord(below) && !isFarInstanceWord(static_cast<int32_t>(distanceOfNearWord)));
    AOT_TEST_CHECK(steps.farInstanceWords.size() == steps.functions.size());
    for (uint32_t other = 0; other < steps.functions.size(); ++other)
        AOT_TEST_CHECK(steps.farInstanceWords[other].isEmpty() == (other != position));
    const FarInstanceWords& words = steps.farInstanceWords[position];
    AOT_TEST_CHECK(words.distances.size() == 2 && words.distances[0] == below && words.distances[1] == above && words.numberOfDetours == 1);

    size_t start = steps.layout.bodyStarts[position];
    size_t firstDistance = start + FarInstanceWords::offsetBehind(body.bytes.size());
    size_t detour = firstDistance + words.sizeOfDistances();
    AOT_TEST_CHECK(!steps.layout.fallsIntoNext.get(position));
    for (uint32_t other = 0; other < steps.functions.size(); ++other)
        AOT_TEST_CHECK(steps.layout.bodyStarts[other] <= start || steps.layout.bodyStarts[other] >= firstDistance + words.size());

    Vector<uint8_t> code = link(steps, numbers);
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());
    AOT_TEST_CHECK(WTF::unalignedLoad<int32_t>(code.span().subspan(firstDistance).data()) == below);
    AOT_TEST_CHECK(WTF::unalignedLoad<int32_t>(code.span().subspan(firstDistance + sizeof(int32_t)).data()) == above);

    std::span<const Relocation> relocations = body.relocations.span();
    AOT_TEST_CHECK(relocations.size() == 5);
    Vector<uint8_t> expected(code);
    memcpySpan(expected.mutableSpan().subspan(start), body.bytes.span());
    zeroSpan(expected.mutableSpan().subspan(detour, sizeOfDetourToFarInstanceWord));
    AOT_TEST_CHECK(applyRelocationThroughDetour(expected.mutableSpan(), start, relocations[0], detour, above));
    AOT_TEST_CHECK(applyRelocationReadingDistance(expected.mutableSpan(), start, relocations[1], firstDistance + sizeof(int32_t)));
    AOT_TEST_CHECK(applyRelocationReadingDistance(expected.mutableSpan(), start, relocations[2], firstDistance));
    AOT_TEST_CHECK(applyRelocationReadingDistance(expected.mutableSpan(), start, relocations[3], firstDistance));
    AOT_TEST_CHECK(applyRelocation(expected.mutableSpan(), start, relocations[4], distanceOfNearWord));
    AOT_TEST_CHECK(equalSpans(code.span(), expected.span()));
}

static void testFarInstanceWordWithoutRoomIsReported()
{
    Steps steps(bodies(bodyWithFarInstanceWords(1), calleeOfLinkedBody(2)));
    addMentionsOfFarInstanceWords(steps, 1);
    ImageNumbers numbers;
    numberMentionsOfLinkedBodies(steps, numbers);
    steps.removeJumps();
    AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());
    link(steps, numbers);
    AOT_TEST_CHECK(steps.build.count(InternalErrorKind::RelocationNotApplied) == 4);
}

static void testBodyWithFarInstanceWordsKeepsItsFinalJump()
{
    for (bool findsFarWords : { false, true }) {
        CodeSketch sketch(1);
        sketch.plain(0x7100);
        appendRelocated(sketch, [](RelocationRecorder& recorder) {
            recorder.loadInstanceWord({ SymbolSpace::InstanceWord, instanceWordFarAbove }, InstanceWordWidth::Word32, instanceGPR, GPRInfo::regT0);
        });
        sketch.jumpToFunction(2);
        Steps steps(bodies(sketch.take(), calleeOfLinkedBody(2)));
        uint32_t position = addMentionsOfFarInstanceWords(steps, 1);
        ImageNumbers numbers;
        numberMentionsOfLinkedBodies(steps, numbers);
        if (findsFarWords)
            steps.farInstanceWords = findFarInstanceWords(steps.functions, numbers, steps.build.context());
        steps.layOut();
        AOT_TEST_CHECK(!steps.build.context().diagnostics().hasErrors());
        AOT_TEST_CHECK(steps.finalJumps[position].jumpsToNextBody);
        AOT_TEST_CHECK(steps.layout.fallsIntoNext.get(position) == !findsFarWords);
    }
}

static void testFarInstanceWordsAreReached()
{
    constexpr GPRReg instance = GPRInfo::argumentGPR0;
    constexpr GPRReg stored = GPRInfo::argumentGPR1;
    constexpr GPRReg sum = GPRInfo::argumentGPR2;
    constexpr GPRReg word = GPRInfo::argumentGPR3;
    constexpr FPRReg number = FPRInfo::fpRegT0;
    constexpr int32_t megabyte = MB;
    constexpr size_t reach = 32 * MB;
    static constexpr int32_t distances[] = {
        20 * megabyte + 8, -20 * megabyte - 16, 17 * megabyte + 4099, -17 * megabyte + 4, 31 * megabyte + 24,
        -31 * megabyte - 8, 16 * megabyte, -16 * megabyte + 4095, 25 * megabyte + 4088,
    };
    constexpr uint32_t numberOfWords = std::size(distances);
    constexpr uint64_t bitsRead[] = { 0x0123456789abcdefull, 0x1111222233334444ull, 0xa5, 0xdeadbeef, 0x400921fb54442d18ull };
    constexpr uint64_t bitsStored = 0xf1e2d3c4b5a69788ull;

    auto symbol = [](uint32_t index) {
        return CodeSymbol { SymbolSpace::InstanceWord, index };
    };
    CCallHelpers jit;
    RelocationRecorder recorder(jit, testAssemblerPolicy());
    recorder.loadInstanceWord(symbol(0), InstanceWordWidth::Word64, instance, sum);
    recorder.moveInstanceAddress(symbol(1), instance, word);
    jit.load64(CCallHelpers::Address(word), word);
    jit.add64(word, sum);
    recorder.loadInstanceWord(symbol(2), InstanceWordWidth::Byte, instance, word);
    jit.add64(word, sum);
    recorder.loadInstanceWord(symbol(3), InstanceWordWidth::Word32, instance, word);
    jit.add64(word, sum);
    recorder.loadInstanceDouble(symbol(4), instance, number);
    jit.moveDoubleTo64(number, word);
    jit.add64(word, sum);
    recorder.storeInstanceWord(symbol(5), InstanceWordWidth::Word64, stored, instance);
    recorder.storeInstanceWord(symbol(6), InstanceWordWidth::Word32, stored, instance);
    recorder.storeInstanceWord(symbol(7), InstanceWordWidth::Byte, stored, instance);
    recorder.storeInstanceDouble(symbol(8), number, instance);
    jit.move(sum, GPRInfo::returnValueGPR);
    jit.ret();
    auto unit = recorder.finalize(LinkBuffer::Profile::FTL, nullptr);
    AOT_TEST_CHECK(unit && unit->relocations.size() == numberOfWords);

    for (bool takesDetours : { false, true }) {
        Vector<uint8_t> code(unit->bytes);
        size_t sizeOfBody = code.size();
        size_t sizeBehindBodyOfEach = takesDetours ? sizeOfDetourToFarInstanceWord : sizeof(int32_t);
        code.grow(sizeOfBody + numberOfWords * sizeBehindBodyOfEach);
        for (const Relocation& relocation : unit->relocations) {
            int32_t distance = distances[relocation.symbol];
            size_t place = sizeOfBody + relocation.symbol * sizeBehindBodyOfEach;
            AOT_TEST_CHECK(isFarInstanceWord(distance) && !applyRelocation(code.mutableSpan(), 0, relocation, static_cast<uint32_t>(distance)));
            if (takesDetours) {
                AOT_TEST_CHECK(applyRelocationThroughDetour(code.mutableSpan(), 0, relocation, place, distance));
                continue;
            }
            WTF::unalignedStore<int32_t>(code.mutableSpan().subspan(place).data(), distance);
            AOT_TEST_CHECK(applyRelocationReadingDistance(code.mutableSpan(), 0, relocation, place));
        }

        ExecutableCopy executable(code.span());
        Vector<uint8_t> memory(FillWith { }, 2 * reach, static_cast<uint8_t>(0));
        auto wordAt = [&](int64_t distance) {
            return memory.mutableSpan().subspan(static_cast<size_t>(static_cast<int64_t>(reach) + distance)).data();
        };
        uint64_t expectedSum = 0;
        for (uint32_t index = 0; index < std::size(bitsRead); ++index) {
            WTF::unalignedStore<uint64_t>(wordAt(distances[index]), index == 2 ? bitsRead[index] | 0xffffffffffffff00ull : index == 3 ? bitsRead[index] | 0xffffffff00000000ull : bitsRead[index]);
            expectedSum += bitsRead[index];
        }
        AOT_TEST_CHECK(executable.call<uint64_t>(0, wordAt(0), bitsStored) == expectedSum);
        AOT_TEST_CHECK(WTF::unalignedLoad<uint64_t>(wordAt(distances[5])) == bitsStored);
        AOT_TEST_CHECK(WTF::unalignedLoad<uint64_t>(wordAt(distances[6])) == static_cast<uint32_t>(bitsStored));
        AOT_TEST_CHECK(WTF::unalignedLoad<uint64_t>(wordAt(distances[7])) == static_cast<uint8_t>(bitsStored));
        AOT_TEST_CHECK(WTF::unalignedLoad<uint64_t>(wordAt(distances[8])) == bitsRead[4]);
        for (uint32_t index = 5; index < numberOfWords; ++index)
            AOT_TEST_CHECK(!WTF::unalignedLoad<uint64_t>(wordAt(distances[index] - 8)) && !WTF::unalignedLoad<uint64_t>(wordAt(distances[index] + 8)));
    }
}
#endif // CPU(ARM64)

std::span<const NativeTest> codeLinkerTests()
{
    static constexpr NativeTest tests[] = {
        { "image/linker/every-space-links"_s, testEverySpaceLinks },
        { "image/linker/missing-stub"_s, testMissingStub },
        { "image/linker/relocation-past-end"_s, testRelocationPastEnd },
        { "image/linker/relocations-that-name-nothing"_s, testRelocationsThatNameNothing },
        { "image/linker/kinds-that-do-not-fit-their-space"_s, testKindsThatDoNotFitTheirSpace },
        { "image/linker/mention-without-instance-word"_s, testMentionWithoutInstanceWord },
        { "image/linker/entry-that-callee-lacks"_s, testEntryThatCalleeLacks },
        { "image/linker/throw-words-that-name-one-string-share-thunk"_s, testThrowWordsThatNameOneStringShareThunk },
        { "image/linker/reads-of-one-name-share-thunk"_s, testReadsOfOneNameShareThunk },
        { "image/numbers/closure-cells-that-tables-name-come-first"_s, testClosureCellsThatTablesNameComeFirst },
#if CPU(ARM64)
        { "image/linker/far-calls-go-through-veneers"_s, testFarCallsGoThroughVeneers },
        { "image/linker/far-instance-words-link"_s, testFarInstanceWordsLink },
        { "image/linker/far-instance-word-without-room-is-reported"_s, testFarInstanceWordWithoutRoomIsReported },
        { "image/linker/body-with-far-instance-words-keeps-its-final-jump"_s, testBodyWithFarInstanceWordsKeepsItsFinalJump },
        { "image/linker/far-instance-words-are-reached"_s, whenExecutable<testFarInstanceWordsAreReached> },
#endif
    };
    return tests;
}

} } } // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
