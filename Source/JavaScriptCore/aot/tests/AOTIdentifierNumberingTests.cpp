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

static uint32_t numberForRank(const IdentifierNumbering& numbering, uint32_t rank)
{
    return RowDisplacementTable::numberForRank(rank, numbering.countWithLookupTableColumn() - 1);
}

static uint32_t rankOf(const IdentifierNumbering& numbering, PropertyKeyID key)
{
    std::optional<uint32_t> number = numbering.numberOf(key);
    AOT_TEST_CHECK(number);
    for (uint32_t rank = 1; rank < numbering.count(); ++rank) {
        if (numberForRank(numbering, rank) == *number)
            return rank;
    }
    AOT_TEST_CHECK(false);
    return 0;
}

static unsigned numberOfIdentifiersWithText(const IdentifierNumbering& numbering, StringID text)
{
    unsigned result = 0;
    for (uint32_t number = 1; number < numbering.count(); ++number)
        result += numbering.at(number).text == text;
    return result;
}

static void testMoreUsesFirst()
{
    NamedProgram named;
    PropertyKeyID a = named.keyWithColumn("a"_s, 3);
    PropertyKeyID b = named.keyWithColumn("b"_s, 5);
    PropertyKeyID c = named.keyWithColumn("c"_s, 1);
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(numbering.numberOf(b) == numberForRank(numbering, 1));
    AOT_TEST_CHECK(numbering.numberOf(a) == numberForRank(numbering, 2));
    AOT_TEST_CHECK(numbering.numberOf(c) == numberForRank(numbering, 3));
}

static void testEqualCountsByKeyID()
{
    NamedProgram named;
    PropertyKeyID q = named.keyWithColumn("q"_s, 2);
    PropertyKeyID p = named.keyWithColumn("p"_s, 2);
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(rankOf(numbering, std::min(q, p)) == 1);
    AOT_TEST_CHECK(rankOf(numbering, std::max(q, p)) == 2);
}

static void testZeroIsNoName()
{
    NamedProgram named;
    std::array keys { named.keyWithColumn("a"_s, 3), named.keyWithColumn("b"_s, 5), named.keyMentionedBy("c"_s, 1) };
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(!numbering.at(0).key);
    AOT_TEST_CHECK(!numbering.at(0).text);
    for (PropertyKeyID key : keys) {
        const NumberedIdentifier& identifier = numbering.at(*numbering.numberOf(key));
        AOT_TEST_CHECK(identifier.key == key);
        AOT_TEST_CHECK(identifier.text == named.program->at(key).text);
        AOT_TEST_CHECK(numbering.numberOfText(identifier.text) == numbering.numberOf(key));
    }

    uint32_t greatestNumber = 0;
    for (uint32_t index = 0; index < named.program->types.numberOfPropertyKeys(); ++index)
        greatestNumber = std::max(greatestNumber, numbering.numberOf(PropertyKeyID(index)).value_or(0));
    AOT_TEST_CHECK(numbering.count() == greatestNumber + 1);
    AOT_TEST_CHECK(IdentifierNumbering().count() == 1);
    AOT_TEST_CHECK(IdentifierNumbering().countWithLookupTableColumn() == 1);
}

static void testTextWithoutKeyHasNoNumber()
{
    NamedProgram named;
    named.keyMentionedBy("a"_s, 1);
    StringID onlyText = named.text("only text"_s);
    PropertyKeyID unusedKey = named.key("never used"_s);
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(!numbering.numberOfText(onlyText));
    AOT_TEST_CHECK(!numbering.numberOfText(StringID()));
    AOT_TEST_CHECK(!numbering.numberOfText(StringID(named.program->strings.size())));
    AOT_TEST_CHECK(!numbering.numberOf(unusedKey));
    AOT_TEST_CHECK(!numbering.numberOf(PropertyKeyID()));
    AOT_TEST_CHECK(!numbering.numberOf(PropertyKeyID(named.program->types.numberOfPropertyKeys())));
}

static void testTableNamesComeLast()
{
    NamedProgram named;
    std::array used { named.keyMentionedBy("a"_s, 3), named.keyMentionedBy("b"_s, 5), named.keyMentionedBy("c"_s, 1) };
    PropertyKeyID namedByTables = named.key("field"_s);
    named.keysNamedByTables.set(namedByTables.index());
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(rankOf(numbering, namedByTables) > used.size());

    BitVector numbersOfUsedKeys;
    for (uint32_t rank = 1; rank <= used.size(); ++rank)
        numbersOfUsedKeys.set(numberForRank(numbering, rank));
    unsigned numberOfBuiltinNames = 0;
    for (uint32_t index = 0; index < named.program->types.numberOfPropertyKeys(); ++index) {
        PropertyKeyID key(index);
        if (!named.program->builtinNameOf(key) || std::ranges::find(used, key) != used.end())
            continue;
        std::optional<uint32_t> number = numbering.numberOf(key);
        AOT_TEST_CHECK(number);
        AOT_TEST_CHECK(!numbersOfUsedKeys.get(*number));
        ++numberOfBuiltinNames;
    }
    AOT_TEST_CHECK(numberOfBuiltinNames);
}

static void testPromisedColumnsFirst()
{
    NamedProgram named;
    PropertyKeyID seldom = named.keyMentionedBy("seldom"_s, 2);
    PropertyKeyID often = named.keyMentionedBy("often"_s, 9);
    PropertyKeyID promised = named.keyWithColumn("promised"_s, 1);
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(rankOf(numbering, promised) == 1);
    AOT_TEST_CHECK(*numbering.numberOf(promised) < numbering.countWithLookupTableColumn());
    AOT_TEST_CHECK(rankOf(numbering, often) == 2);
    AOT_TEST_CHECK(rankOf(numbering, seldom) == 3);
    AOT_TEST_CHECK(!named.numberOfNamesWithoutColumn());
}

static void testFunctionsCountNotMentions()
{
    NamedProgram named;
    PropertyKeyID rare = named.keyWithColumn("rare"_s, 1);
    for (int32_t addend = 1; addend <= 5; ++addend)
        named.mention(NumberSpace::Name, rare.index(), 1, addend);
    PropertyKeyID often = named.keyWithColumn("often"_s, 2);
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(rankOf(numbering, often) == 1);
    AOT_TEST_CHECK(rankOf(numbering, rare) == 2);
}

static void testLookupTableFull()
{
    NamedProgram named;
    Vector<PropertyKeyID> promised = named.addKeysWithColumn(RowDisplacementTable::maxColumn);
    PropertyKeyID often = named.keyMentionedBy("often"_s, 9);
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(!named.context.diagnostics().hasErrors());
    AOT_TEST_CHECK(numbering.countWithLookupTableColumn() == RowDisplacementTable::maxColumn + 1);
    for (PropertyKeyID key : promised) {
        std::optional<uint32_t> number = numbering.numberOf(key);
        AOT_TEST_CHECK(number && RowDisplacementTable::hasColumn(*number));
    }
    std::optional<uint32_t> numberWithoutColumn = numbering.numberOf(often);
    AOT_TEST_CHECK(numberWithoutColumn == numbering.countWithLookupTableColumn());
    AOT_TEST_CHECK(numbering.hasFieldTableColumn(*numberWithoutColumn));
    AOT_TEST_CHECK(named.numberOfNamesWithoutColumn() == 1);
}

static void testMorePromisesThanColumns()
{
    NamedProgram named;
    named.addKeysWithColumn(RowDisplacementTable::maxColumn + 1);
    named.number();
    AOT_TEST_CHECK(named.context.diagnostics().hasErrors());
}

static void testPrivateAndTextName()
{
    NamedProgram named;
    PropertyKeyID textKey = named.keyMentionedBy("p"_s, 1);
    PropertyKeyID privateKey = named.key("p"_s, PropertyKeyKind::EnginePrivateName);
    named.mention(NumberSpace::Name, privateKey.index(), 4);
    AOT_TEST_CHECK(textKey != privateKey);

    IdentifierNumbering numbering = named.number();
    std::optional<uint32_t> textNumber = numbering.numberOf(textKey);
    std::optional<uint32_t> privateNumber = numbering.numberOf(privateKey);
    AOT_TEST_CHECK(textNumber && privateNumber);
    AOT_TEST_CHECK(textNumber != privateNumber);
    AOT_TEST_CHECK(numbering.at(*textNumber).text == numbering.at(*privateNumber).text);
    AOT_TEST_CHECK(numbering.at(*privateNumber).key == privateKey);
    AOT_TEST_CHECK(numbering.numberOfText(named.text("p"_s)) == textNumber);
}

static void testPrivateNameAlone()
{
    NamedProgram named;
    PropertyKeyID privateKey = named.key("name of no built-in"_s, PropertyKeyKind::EnginePrivateName);
    named.mention(NumberSpace::Name, privateKey.index(), 1);
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(numbering.numberOf(privateKey));
    AOT_TEST_CHECK(!numbering.numberOfText(named.text("name of no built-in"_s)));
}

static void testExportNameWithoutKey()
{
    NamedProgram named;
    PropertyKeyID used = named.keyMentionedBy("used"_s, 1);
    StringID exported = named.text("exported"_s);
    named.textsNamedAsKeysByTables.set(exported.index());
    named.textsNamedAsKeysByTables.set(named.text("used"_s).index());

    IdentifierNumbering numbering = named.number();
    std::optional<uint32_t> number = numbering.numberOfText(exported);
    AOT_TEST_CHECK(number);
    AOT_TEST_CHECK(!numbering.at(*number).key);
    AOT_TEST_CHECK(numbering.at(*number).text == exported);
    AOT_TEST_CHECK(numbering.numberOfText(named.text("used"_s)) == numbering.numberOf(used));
    AOT_TEST_CHECK(numberOfIdentifiersWithText(numbering, named.text("used"_s)) == 1);
}

static void testTextMentionedWithoutKey()
{
    NamedProgram named;
    PropertyKeyID used = named.keyMentionedBy("used"_s, 1);
    StringID compared = named.text("compared"_s);
    named.mention(NumberSpace::NameOfText, compared.index(), 2);
    named.mention(NumberSpace::NameOfText, named.text("used"_s).index(), 1);

    IdentifierNumbering numbering = named.number();
    std::optional<uint32_t> number = numbering.numberOfText(compared);
    AOT_TEST_CHECK(number);
    AOT_TEST_CHECK(numbering.at(*number).text == compared);
    AOT_TEST_CHECK(numberOfIdentifiersWithText(numbering, compared) == 1);
    AOT_TEST_CHECK(numbering.numberOfText(named.text("used"_s)) == numbering.numberOf(used));
    AOT_TEST_CHECK(numberOfIdentifiersWithText(numbering, named.text("used"_s)) == 1);
}

static IdentifierNumbering numberWithThreads(unsigned threadCount)
{
    NamedProgram named(TestOptions { .threadCount = threadCount });
    named.keyWithColumn("a"_s, 3);
    named.keyWithColumn("b"_s, 5);
    named.keyMentionedBy("c"_s, 1);
    return named.number();
}

static void testSameResultTwice()
{
    IdentifierNumbering serial = numberWithThreads(1);
    IdentifierNumbering parallel = numberWithThreads(8);
    AOT_TEST_CHECK(serial.count() > 3);
    AOT_TEST_CHECK(serial.count() == parallel.count());
    AOT_TEST_CHECK(serial.countWithLookupTableColumn() == parallel.countWithLookupTableColumn());
    for (uint32_t number = 1; number < serial.count(); ++number) {
        AOT_TEST_CHECK(serial.at(number).key == parallel.at(number).key);
        AOT_TEST_CHECK(serial.at(number).text == parallel.at(number).text);
    }
}

// As programText(), but for f1, which is the body of a second module.
static String textOfNamedProgramWithTwoModules()
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0 requests=[{target=m1 completes=true}]\n"
        "module m1 path=\"other.js\" displayPath=\"other.js\" body=f1\n"
        "entry\n"
        "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Builtin builtin=GlobalThis\n"
        "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    for (unsigned k = 2; k < NamedProgram::numberOfFunctions; ++k) {
        text.append("    v"_s, 2 * k - 2, " = NewClosure function=f"_s, k, "\n"_s);
        text.append("    v"_s, 2 * k - 1, " = Call v1, -, v"_s, 2 * k - 2, "\n"_s);
    }
    text.append("    Return\n}\n"
        "function f1 name=\"other\" module=m1 kind=Module results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
    for (unsigned k = 2; k < NamedProgram::numberOfFunctions; ++k)
        text.append("function f"_s, k, " name=\"f"_s, k, "\" results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
    text.append(runtimeFunctionsText(NamedProgram::numberOfFunctions));
    return text.toString();
}

static void testKeyCellsByFirstFunction()
{
    constexpr uint32_t functionOfLaterModule = 1;
    constexpr uint32_t earlierFunction = 3;
    constexpr uint32_t laterFunction = 7;
    NamedProgram named({ }, textOfNamedProgramWithTwoModules());
    AOT_TEST_CHECK(named.program->at(FunctionID(earlierFunction)).module == named.program->at(FunctionID(laterFunction)).module);
    AOT_TEST_CHECK(named.program->at(FunctionID(laterFunction)).module < named.program->at(FunctionID(functionOfLaterModule)).module);

    PropertyKeyID keyOfLaterModule = named.key("cell of the later module"_s);
    std::array keysOfLaterFunction { named.key("cell of the later function"_s), named.key("cell of the later function and the later module"_s) };
    std::array keysOfEarlierFunction { named.key("cell of the earlier function"_s), named.key("cell of both functions"_s) };
    std::ranges::sort(keysOfLaterFunction);
    std::ranges::sort(keysOfEarlierFunction);
    auto mentionCell = [&](uint32_t function, PropertyKeyID key) {
        named.mentionsOfFunctions[function].append(NamedProgram::MentionedID { NumberSpace::KeyCell, key.index(), 0 });
    };
    mentionCell(functionOfLaterModule, keysOfLaterFunction[1]);
    mentionCell(functionOfLaterModule, keyOfLaterModule);
    mentionCell(laterFunction, keysOfEarlierFunction[1]);
    mentionCell(laterFunction, keysOfLaterFunction[1]);
    mentionCell(laterFunction, keysOfLaterFunction[0]);
    mentionCell(earlierFunction, keysOfEarlierFunction[1]);
    mentionCell(earlierFunction, keysOfEarlierFunction[0]);

    ImageNumbers numbers;
    named.number(numbers, { &ImageNumbersBuilder::numberConstants });
    AOT_TEST_CHECK(!named.context.diagnostics().hasErrors());

    std::array mentionedCells { keysOfEarlierFunction[0], keysOfEarlierFunction[1], keysOfLaterFunction[0], keysOfLaterFunction[1], keyOfLaterModule };
    std::span<const PropertyKeyID> cells = numbers.keyCells();
    AOT_TEST_CHECK(cells.size() >= mentionedCells.size());
    AOT_TEST_CHECK(std::ranges::equal(cells.first(mentionedCells.size()), mentionedCells));
    AOT_TEST_CHECK(std::ranges::all_of(cells.subspan(mentionedCells.size()), [&](PropertyKeyID key) {
        return Typed::isSymbolKind(named.program->at(key).kind);
    }));
    for (uint32_t number = 0; number < cells.size(); ++number) {
        uint32_t key = cells[number].index();
        AOT_TEST_CHECK(numbers.numberOf(NumberSpace::KeyCell, singleElementSpan(key)) == number);
    }
}

static void testPromisedNameThatNothingNames()
{
    NamedProgram named;
    PropertyKeyID resolved = named.key("every access was resolved"_s);
    named.keysWithColumn.set(resolved.index());
    PropertyKeyID promised = named.keyWithColumn("promised and looked up"_s, 1);
    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(!named.context.diagnostics().hasErrors());
    AOT_TEST_CHECK(!numbering.numberOf(resolved));
    AOT_TEST_CHECK(!numbering.numberOfText(named.program->at(resolved).text));
    AOT_TEST_CHECK(rankOf(numbering, promised) == 1);
}

static void testColumnsBehindHotNamesByFirstFunction()
{
    constexpr uint32_t earlierFunction = 3;
    constexpr uint32_t laterFunction = 7;
    NamedProgram named;
    for (uint32_t index = 0; index + 1 < RowDisplacementTable::numberOfHotNames; ++index)
        named.keyWithColumn(makeString("hot"_s, index), NamedProgram::numberOfFunctions);
    auto keyLookedUpBy = [&](StringView characters, uint32_t function) {
        PropertyKeyID key = named.key(characters);
        named.keysWithColumn.set(key.index());
        named.mentionsOfFunctions[function].append(NamedProgram::MentionedID { NumberSpace::Name, key.index(), 0 });
        return key;
    };
    std::array keysOfLaterFunction { keyLookedUpBy("name of the later function"_s, laterFunction), keyLookedUpBy("another name of the later function"_s, laterFunction) };
    PropertyKeyID keyOfEarlierFunction = keyLookedUpBy("name of the earlier function"_s, earlierFunction);
    std::ranges::sort(keysOfLaterFunction);

    IdentifierNumbering numbering = named.number();
    AOT_TEST_CHECK(!named.context.diagnostics().hasErrors());
    constexpr uint32_t firstRankBehindHotNames = RowDisplacementTable::numberOfHotNames;
    AOT_TEST_CHECK(rankOf(numbering, keyOfEarlierFunction) == firstRankBehindHotNames);
    AOT_TEST_CHECK(rankOf(numbering, keysOfLaterFunction[0]) == firstRankBehindHotNames + 1);
    AOT_TEST_CHECK(rankOf(numbering, keysOfLaterFunction[1]) == firstRankBehindHotNames + 2);
}

static void testStringsByFirstFunction()
{
    constexpr uint32_t earlierFunction = 3;
    constexpr uint32_t laterFunction = 7;
    NamedProgram named;
    std::array stringsOfLaterFunction { named.text("constant of the later function"_s), named.text("another constant of the later function"_s) };
    StringID stringOfBothFunctions = named.text("constant of both functions"_s);
    std::ranges::sort(stringsOfLaterFunction);
    auto mentionCell = [&](uint32_t function, StringID string) {
        named.mentionsOfFunctions[function].append(NamedProgram::MentionedID { NumberSpace::StringCell, string.index(), 0 });
    };
    mentionCell(laterFunction, stringsOfLaterFunction[1]);
    mentionCell(laterFunction, stringsOfLaterFunction[0]);
    mentionCell(laterFunction, stringOfBothFunctions);
    mentionCell(earlierFunction, stringOfBothFunctions);

    ImageNumbers numbers;
    named.number(numbers, { &ImageNumbersBuilder::collectRegExpRequests, &ImageNumbersBuilder::numberIdentifiersAndStrings });
    AOT_TEST_CHECK(!named.context.diagnostics().hasErrors());

    std::array expected { stringOfBothFunctions, stringsOfLaterFunction[0], stringsOfLaterFunction[1] };
    AOT_TEST_CHECK(std::ranges::equal(numbers.stringConstants().strings(), expected));
    Vector<StringID> textsOfConstants;
    BitVector isListed;
    for (StringID string : numbers.stringsInOrderOfText()) {
        AOT_TEST_CHECK(!isListed.set(string.index()));
        if (std::ranges::contains(expected, string))
            textsOfConstants.append(string);
    }
    AOT_TEST_CHECK(std::ranges::equal(textsOfConstants, expected));

    // The texts of names lie in front of all others.
    const IdentifierNumbering& identifiers = numbers.identifiers();
    AOT_TEST_CHECK(std::ranges::is_partitioned(numbers.stringsInOrderOfText(), [&](StringID string) {
        for (uint32_t number = 1; number < identifiers.count(); ++number) {
            if (identifiers.at(number).text == string)
                return true;
        }
        return false;
    }));
}

// One word says where a short text lies while it begins in the first 16 MB of all text. A long text takes three words anywhere.
static void testLongTextsLieBehindShortOnes()
{
    constexpr uint32_t earlierFunction = 3;
    constexpr uint32_t laterFunction = 7;
    NamedProgram named;
    auto textOfLength = [&](char character, unsigned length) {
        StringBuilder builder;
        for (unsigned index = 0; index < length; ++index)
            builder.append(character);
        return named.text(builder.toString());
    };
    StringID longOfEarlier = textOfLength('a', ImageString::lengthMask + 1);
    StringID longestShortOfEarlier = textOfLength('b', ImageString::lengthMask);
    StringID longOfLater = textOfLength('c', 1000);
    StringID shortOfLater = named.text("short text of the later function"_s);
    auto mentionCell = [&](uint32_t function, StringID string) {
        named.mentionsOfFunctions[function].append(NamedProgram::MentionedID { NumberSpace::StringCell, string.index(), 0 });
    };
    mentionCell(earlierFunction, longOfEarlier);
    mentionCell(earlierFunction, longestShortOfEarlier);
    mentionCell(laterFunction, longOfLater);
    mentionCell(laterFunction, shortOfLater);

    ImageNumbers numbers;
    named.number(numbers, { &ImageNumbersBuilder::collectRegExpRequests, &ImageNumbersBuilder::numberIdentifiersAndStrings });
    AOT_TEST_CHECK(!named.context.diagnostics().hasErrors());

    std::array expected { longestShortOfEarlier, shortOfLater, longOfEarlier, longOfLater };
    Vector<StringID> found;
    for (StringID string : numbers.stringsInOrderOfText()) {
        if (std::ranges::contains(expected, string))
            found.append(string);
    }
    AOT_TEST_CHECK(std::ranges::equal(found, expected));
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
        { "image/identifier-numbering/more-uses-first"_s, testMoreUsesFirst },
        { "image/identifier-numbering/equal-counts-by-key-id"_s, testEqualCountsByKeyID },
        { "image/identifier-numbering/zero-is-no-name"_s, testZeroIsNoName },
        { "image/identifier-numbering/text-without-key-has-no-number"_s, testTextWithoutKeyHasNoNumber },
        { "image/identifier-numbering/table-names-come-last"_s, testTableNamesComeLast },
        { "image/identifier-numbering/promised-columns-first"_s, testPromisedColumnsFirst },
        { "image/identifier-numbering/functions-count-not-mentions"_s, testFunctionsCountNotMentions },
        { "image/identifier-numbering/lookup-table-full"_s, testLookupTableFull },
        { "image/identifier-numbering/more-promises-than-columns"_s, testMorePromisesThanColumns },
        { "image/identifier-numbering/private-and-text-name"_s, testPrivateAndTextName },
        { "image/identifier-numbering/private-name-alone"_s, testPrivateNameAlone },
        { "image/identifier-numbering/export-name-without-key"_s, testExportNameWithoutKey },
        { "image/identifier-numbering/text-mentioned-without-key"_s, testTextMentionedWithoutKey },
        { "image/identifier-numbering/same-result-twice"_s, testSameResultTwice },
        { "image/identifier-numbering/key-cells-by-first-function"_s, testKeyCellsByFirstFunction },
        { "image/identifier-numbering/promised-name-that-nothing-names"_s, testPromisedNameThatNothingNames },
        { "image/identifier-numbering/columns-behind-hot-names-by-first-function"_s, testColumnsBehindHotNamesByFirstFunction },
        { "image/identifier-numbering/strings-by-first-function"_s, testStringsByFirstFunction },
        { "image/identifier-numbering/long-texts-lie-behind-short-ones"_s, testLongTextsLieBehindShortOnes },
        { "image/identifier-numbering/first-function-is-first-in-source"_s, testFirstFunctionIsFirstInSource },
        { "image/identifier-numbering/literal-string-that-nothing-spells"_s, testLiteralStringThatNothingSpells },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
