/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTIRParser.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImageTestSupport.h"
#include "AOTImageText.h"
#include "AOTLinkedFunctions.h"
#include "AOTNameClass.h"
#include "AOTNumbering.h"
#include "AOTPlan.h"
#include "AOTPropertyCachesFront.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC::AOT {

using namespace ImageTests;

namespace {

struct NamedProgram {
    static constexpr uint32_t numberOfFunctions = 10;

    struct MentionedID {
        NumberSpace space;
        uint32_t id;
        int32_t addend;
    };

    explicit NamedProgram(const TestOptions& = { }, const String& textOfProgram = programText(numberOfFunctions));

    StringID text(StringView);
    PropertyKeyID key(StringView, PropertyKeyKind = PropertyKeyKind::Text);
    void mention(NumberSpace, uint32_t id, uint32_t numberOfMentioningFunctions, int32_t addend = 0);
    PropertyKeyID keyMentionedBy(StringView, uint32_t numberOfMentioningFunctions);
    PropertyKeyID keyWithColumn(StringView, uint32_t numberOfMentioningFunctions);
    Vector<PropertyKeyID> addKeysWithColumn(uint32_t count);
    void number(ImageNumbers&, std::initializer_list<void (ImageNumbersBuilder::*)()> steps);
    IdentifierNumbering number();
    uint64_t numberOfNamesWithoutColumn();

    BuildContext context;
    std::unique_ptr<Typed::Program> program;
    // number() destroys it before it calls computeFacts(): a key interned after that has no stable number.
    std::optional<Typed::TypeArena> types;
    std::array<Vector<MentionedID>, numberOfFunctions> mentionsOfFunctions;
    BitVector keysNamedByTables;
    BitVector textsNamedAsKeysByTables;
    BitVector keysWithColumn;
    // Numbered from 1 on, behind the empty string.
    Vector<StringID> literalStrings;
};

} // anonymous namespace

NamedProgram::NamedProgram(const TestOptions& options, const String& textOfProgram)
    : context(wireOptions(options))
    , program(Typed::parseProgram(textOfProgram, context))
{
    AOT_TEST_CHECK(program);
    types.emplace(program->types, Typed::ArenaKeeps::InProgram);
}

StringID NamedProgram::text(StringView characters)
{
    return program->strings.intern(characters);
}

PropertyKeyID NamedProgram::key(StringView characters, PropertyKeyKind kind)
{
    Typed::PropertyKey key;
    key.kind = kind;
    key.text = text(characters);
    return types->intern(key);
}

void NamedProgram::mention(NumberSpace space, uint32_t id, uint32_t numberOfMentioningFunctions, int32_t addend)
{
    AOT_TEST_CHECK(numberOfMentioningFunctions <= numberOfFunctions);
    for (uint32_t function = 0; function < numberOfMentioningFunctions; ++function)
        mentionsOfFunctions[function].append(MentionedID { space, id, addend });
}

PropertyKeyID NamedProgram::keyMentionedBy(StringView characters, uint32_t numberOfMentioningFunctions)
{
    PropertyKeyID result = key(characters);
    mention(NumberSpace::Name, result.index(), numberOfMentioningFunctions);
    return result;
}

PropertyKeyID NamedProgram::keyWithColumn(StringView characters, uint32_t numberOfMentioningFunctions)
{
    PropertyKeyID result = keyMentionedBy(characters, numberOfMentioningFunctions);
    keysWithColumn.set(result.index());
    return result;
}

Vector<PropertyKeyID> NamedProgram::addKeysWithColumn(uint32_t count)
{
    Vector<PropertyKeyID> keys;
    keys.reserveInitialCapacity(count);
    for (uint32_t index = 0; index < count; ++index)
        keys.append(keyWithColumn(makeString("n"_s, index), 1));
    return keys;
}

void NamedProgram::number(ImageNumbers& numbers, std::initializer_list<void (ImageNumbersBuilder::*)()> steps)
{
    types.reset();
    std::unique_ptr<Typed::Facts> facts = Typed::computeFacts(*program, context, Typed::FactsRequest { .mechanisms = Typed::mechanismsOfProcess() });
    AOT_TEST_CHECK(facts);
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());

    Typed::SlotPlanParts slots;
    slots.keysWithColumn = keysWithColumn;
    Typed::PlanParts parts;
    parts.slots = Typed::SlotPlan(WTF::move(slots));
    Vector<StringID> numberedStrings { program->strings.emptyString() };
    numberedStrings.appendVector(literalStrings);
    Vector<uint32_t> numbersOfStringsPlusOne(FillWith { }, program->strings.size(), 0u);
    for (uint32_t number = 0; number < numberedStrings.size(); ++number)
        numbersOfStringsPlusOne[numberedStrings[number].index()] = number + 1;
    parts.literalStrings = Typed::LiteralStringTable(WTF::move(numberedStrings), { }, WTF::move(numbersOfStringsPlusOne));
    Typed::Plan plan(WTF::move(parts));
    program->types.freeze();

    Vector<CompiledCode> code;
    code.reserveInitialCapacity(numberOfFunctions);
    for (uint32_t function = 0; function < numberOfFunctions; ++function) {
        MentionList mentionsOfFunction;
        for (const MentionedID& mentioned : mentionsOfFunctions[function])
            mentionsOfFunction.add(mentioned.space, mentioned.id, mentioned.addend);
        CompiledCode compiled = CodeSketch(function).takeUnchecked();
        mentionsOfFunction.moveTo(compiled.body);
        code.append(WTF::move(compiled));
    }
    LinkedFunctions functions = createLinkedFunctions(WTF::move(code), context);

    NumberingInputs inputs { *program, *facts, plan, functions };
    GatheredMentions mentions = gatherMentions(inputs, context);
    NamesOfTables tables(*program);
    tables.keys.merge(keysNamedByTables);
    tables.textsNamedAsKeys.merge(textsNamedAsKeysByTables);

    ImageNumbersBuilder builder { inputs, mentions, tables, numbers, context };
    for (auto step : steps)
        (builder.*step)();
}

IdentifierNumbering NamedProgram::number()
{
    ImageNumbers numbers;
    number(numbers, { &ImageNumbersBuilder::collectRegExpRequests, &ImageNumbersBuilder::numberIdentifiersAndStrings });
    if (context.diagnostics().hasErrors())
        return { };
    return numbers.identifiers();
}

uint64_t NamedProgram::numberOfNamesWithoutColumn()
{
    return context.statistics().values()[static_cast<unsigned>(StatisticID::NamesWithoutColumn)];
}

static void testMorePromisesThanColumns()
{
    NamedProgram named;
    named.addKeysWithColumn(RowDisplacementTable::maxColumn + 1);
    named.number();
    AOT_TEST_CHECK(named.context.diagnostics().hasErrors());
}

static void testFirstFunctionIsFirstInSource()
{
    constexpr uint32_t functionWrittenFirst = 7;
    constexpr uint32_t functionWrittenLast = 3;
    NamedProgram named;
    named.program->at(FunctionID(functionWrittenFirst)).position = 10;
    named.program->at(FunctionID(functionWrittenLast)).position = 20;
    StringID stringOfLastFunction = named.text("constant of the function that is written last"_s);
    StringID stringOfFirstFunction = named.text("constant of the function that is written first"_s);
    AOT_TEST_CHECK(stringOfLastFunction < stringOfFirstFunction);
    named.mentionsOfFunctions[functionWrittenLast].append(NamedProgram::MentionedID { NumberSpace::StringCell, stringOfLastFunction.index(), 0 });
    named.mentionsOfFunctions[functionWrittenFirst].append(NamedProgram::MentionedID { NumberSpace::StringCell, stringOfFirstFunction.index(), 0 });

    ImageNumbers numbers;
    named.number(numbers, { &ImageNumbersBuilder::collectRegExpRequests, &ImageNumbersBuilder::numberIdentifiersAndStrings });
    AOT_TEST_CHECK(!named.context.diagnostics().hasErrors());
    AOT_TEST_CHECK(std::ranges::equal(numbers.stringConstants().strings(), std::array { stringOfFirstFunction, stringOfLastFunction }));
}

static void numberLiteralStringsAndCheck(const TestOptions& options, bool keepsEveryText)
{
    NamedProgram named(options);
    StringID madeByCode = named.text("code makes a value of it"_s);
    StringID heldAsCell = named.text("code holds it as a cell"_s);
    PropertyKeyID lookedUp = named.keyMentionedBy("code looks it up"_s, 1);
    StringID neverSpelled = named.text("nothing spells it"_s);
    named.literalStrings = { madeByCode, heldAsCell, named.program->at(lookedUp).text, neverSpelled };
    named.mention(NumberSpace::String, madeByCode.index(), 1);
    named.mention(NumberSpace::StringCell, heldAsCell.index(), 1);

    ImageNumbers numbers;
    named.number(numbers, { &ImageNumbersBuilder::collectRegExpRequests, &ImageNumbersBuilder::numberIdentifiersAndStrings });
    AOT_TEST_CHECK(!named.context.diagnostics().hasErrors());

    AOT_TEST_CHECK(numbers.literalStringsWithoutText().bitCount() == (keepsEveryText ? 0u : 1u));
    AOT_TEST_CHECK(numbers.literalStringsWithoutText().get(neverSpelled.index()) == !keepsEveryText);
    AOT_TEST_CHECK(numbers.namedStrings().get(neverSpelled.index()) == keepsEveryText);
    AOT_TEST_CHECK(std::ranges::contains(numbers.stringsInOrderOfText(), neverSpelled) == keepsEveryText);
    // The header in the image says which literal string it is, so each that has its text has one.
    for (StringID string : named.literalStrings) {
        if (numbers.literalStringsWithoutText().get(string.index()))
            continue;
        AOT_TEST_CHECK(numbers.namedStrings().get(string.index()));
        AOT_TEST_CHECK(!numbers.stringsOnlyShown().get(string.index()));
        AOT_TEST_CHECK(std::ranges::contains(numbers.stringsInOrderOfText(), string));
    }
}

static void testLiteralStringThatNothingSpells()
{
    numberLiteralStringsAndCheck({ }, false);
    numberLiteralStringsAndCheck({ .output = Wire::OutputOptions::RunsInSeveralRealms }, true);
}

std::span<const NativeTest> identifierNumberingTests()
{
    static constexpr NativeTest tests[] = {
        { "image/identifier-numbering/more-promises-than-columns"_s, testMorePromisesThanColumns },
        { "image/identifier-numbering/first-function-is-first-in-source"_s, testFirstFunctionIsFirstInSource },
        { "image/identifier-numbering/literal-string-that-nothing-spells"_s, testLiteralStringThatNothingSpells },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
