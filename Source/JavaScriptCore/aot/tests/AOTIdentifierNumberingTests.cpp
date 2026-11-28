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

    NamedProgram();

    StringID text(StringView);
    PropertyKeyID key(StringView, PropertyKeyKind = PropertyKeyKind::Text);
    void mention(PropertyKeyID, uint32_t numberOfMentioningFunctions);
    PropertyKeyID keyMentionedBy(StringView, uint32_t numberOfMentioningFunctions);
    PropertyKeyID keyWithColumn(StringView, uint32_t numberOfMentioningFunctions);
    Vector<PropertyKeyID> addKeysWithColumn(uint32_t count);
    void number(ImageNumbers&, std::initializer_list<void (ImageNumbersBuilder::*)()> steps);
    IdentifierNumbering number();
    uint64_t numberOfNamesWithoutColumn();

    BuildContext context;
    std::unique_ptr<Typed::Program> program;
    std::optional<Typed::TypeArena> types;
    std::array<Vector<MentionedID>, numberOfFunctions> mentionsOfFunctions;
    BitVector keysNamedByTables;
    BitVector textsNamedAsKeysByTables;
    Typed::BitVector keysWithColumn;
};

} // anonymous namespace

NamedProgram::NamedProgram()
    : context(wireOptions())
    , program(Typed::parseProgram(programText(numberOfFunctions), context))
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

void NamedProgram::mention(PropertyKeyID key, uint32_t numberOfMentioningFunctions)
{
    AOT_TEST_CHECK(numberOfMentioningFunctions <= numberOfFunctions);
    for (uint32_t function = 0; function < numberOfMentioningFunctions; ++function)
        mentionsOfFunctions[function].append(MentionedID { NumberSpace::Name, key.index(), 0 });
}

PropertyKeyID NamedProgram::keyMentionedBy(StringView characters, uint32_t numberOfMentioningFunctions)
{
    PropertyKeyID result = key(characters);
    mention(result, numberOfMentioningFunctions);
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

std::span<const NativeTest> identifierNumberingTests()
{
    static constexpr NativeTest tests[] = {
        { "image/identifier-numbering/more-promises-than-columns"_s, testMorePromisesThanColumns },
        { "image/identifier-numbering/first-function-is-first-in-source"_s, testFirstFunctionIsFirstInSource },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
