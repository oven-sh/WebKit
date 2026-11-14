/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTIRParser.h"
#include "AOTImageTestBodies.h"
#include "AOTNumbering.h"
#include "AOTObjectVisiting.h"
#include "AOTPlan.h"
#include "AOTSupportTestCommands.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "MacroAssembler.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/HashSet.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringHash.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

namespace {

enum class ArrivalOrder : uint8_t { Forward, Backward };

struct MentionedThing {
    NumberSpace space;
    Vector<uint32_t, 8> key;
};

struct NumberedThings {
    Vector<uint32_t> numbers;
    Vector<uint8_t> bytes;
};

struct PlacedTypes {
    Vector<uint32_t> ids;
    Vector<uint32_t> places;
};

struct ProgramWithSourcesSize {
    constexpr uint32_t numberOfFunctions() const { return numberOfModules * functionsInModule; }
    constexpr uint32_t moduleOf(uint32_t function) const { return (function - 1) / functionsInModule; }
    constexpr uint32_t placeOf(uint32_t function) const { return (function - 1) % functionsInModule; }

    uint32_t numberOfModules;
    uint32_t functionsInModule;
};

struct SourcesAtHost {
    Vector<Vector<uint8_t>> modules;
    uint32_t releases { 0 };
};

struct RecordsReadBack {
    unsigned objects { 0 };
    unsigned objectsWithSeveralOrders { 0 };
    unsigned environmentsWithVariables { 0 };
    unsigned listsOfSubclasses { 0 };
};

#if CPU(ARM64)
enum class EntryShape : uint8_t { AtStart, AfterPrefix, TwoEntries };
#endif

} // anonymous namespace

static constexpr std::array<unsigned, 3> threadCounts { 1, 2, 16 };
static constexpr unsigned mostThreads = threadCounts.back();

static constexpr uint32_t numberOfMentioningBodies = 600;
static constexpr unsigned numberOfMentionedSpaces = 5;
static constexpr unsigned thingsInMentionedSpace = 24;
static constexpr unsigned numberOfMentionedThings = numberOfMentionedSpaces * thingsInMentionedSpace;
static constexpr unsigned thingsMentionedByBody = 16;
static constexpr unsigned firstThingStride = 7;
static constexpr unsigned mostPartsOfTemplate = 3;
static constexpr unsigned numberOfTypesBorn = 32;
static constexpr uint32_t functionsPerSourceLine = 25;
static constexpr uint32_t numberOfSharedSources = 2;
static constexpr uint32_t timesEachConstantIsHeld = 3;
static constexpr ProgramWithSourcesSize fewSources { 3, 2 * quotedSourceGroupSize + 6 };
static constexpr ProgramWithSourcesSize manySources { 3, 4400 };
static constexpr uint32_t closuresHandedOutByCall = 50;
static constexpr uint32_t numberOfKeysInLongList = manySources.numberOfFunctions() / 2 + 1;
static constexpr size_t fewestSortedOnPool = 1 << 13;
static_assert(manySources.numberOfFunctions() > fewestSortedOnPool && numberOfKeysInLongList > indicesInRangeOfLoop);
static_assert(manySources.numberOfFunctions() / 3 * 2 / numberOfSharedSources > indicesInRangeOfLoop);
static_assert(indicesInRangeOfLoop % timesEachConstantIsHeld && manySources.numberOfFunctions() > 3 * indicesInRangeOfLoop);
static constexpr uint32_t numberOfGroupsOfLayouts = 1100;
static constexpr uint32_t layoutsInGroup = 4;
static constexpr uint32_t functionsInGroup = 5;
static constexpr uint32_t valuesOfGroupInModuleBody = 12;
static_assert(numberOfGroupsOfLayouts * layoutsInGroup > indicesInRangeOfLoop);
static_assert(thingsMentionedByBody < numberOfMentionedThings);
static_assert(firstThingStride <= thingsMentionedByBody && numberOfMentioningBodies * firstThingStride >= numberOfMentionedThings, "every thing is mentioned");

#if CPU(ARM64)
static constexpr size_t reachOfNearCall = MacroAssembler::relocationReach(RelocationKind::Call);
static constexpr size_t sizeOfLongBody = reachOfNearCall / 8 * 5;
static constexpr uint32_t numberOfLongBodies = 2;
static constexpr uint32_t numberOfBodiesOnEachSide = 150;
static constexpr uint32_t numberOfChainedBodies = 2 * numberOfBodiesOnEachSide;
static constexpr uint32_t numberOfFarBodies = numberOfChainedBodies + numberOfLongBodies;
static constexpr uint32_t stepOverLongBodies = numberOfBodiesOnEachSide + numberOfLongBodies;
static constexpr std::array entryShapes { EntryShape::AtStart, EntryShape::AfterPrefix, EntryShape::TwoEntries };
static constexpr uint32_t prefixWord = 0x0105;
static constexpr uint16_t secondEntry = 1;
static_assert(sizeOfLongBody < reachOfNearCall && numberOfLongBodies * sizeOfLongBody > reachOfNearCall);
#endif

static Vector<uint8_t> copyBytes(const TestImage& image)
{
    Vector<uint8_t> bytes(image.bytes.span());
    relocateImage(bytes.mutableSpan(), 0);
    return bytes;
}

template<typename BuildBytes>
static void checkSameForAllThreadCounts(const BuildBytes& buildBytes)
{
    Vector<uint8_t> expected = buildBytes(threadCounts[0], nullptr);
    for (unsigned threadCount : std::span(threadCounts).subspan(1))
        AOT_TEST_CHECK(buildBytes(threadCount, nullptr) == expected);
    AOT_TEST_CHECK(buildBytes(mostThreads, &Typed::poolOfNewThreads) == expected);
}

static Vector<uint8_t> mixedImageBytes(unsigned threadCount, const HostThreadPool* hostPool = nullptr)
{
    TestBuild build(numberOfMixedBodies + 1, { .threadCount = threadCount, .hostPool = hostPool });
    TestImage image = builtImage(build, mixedBodies(numberOfMixedBodies));
    AOT_TEST_CHECK(image.built.layout.fallsIntoNext.bitCount());
    return copyBytes(image);
}

static void testDeterminismThreads() { checkSameForAllThreadCounts(mixedImageBytes); }
static void testDeterminismTwoBuilds() { AOT_TEST_CHECK(mixedImageBytes(mostThreads) == mixedImageBytes(mostThreads)); }

static unsigned placeInArrivalOrder(ArrivalOrder order, unsigned step, unsigned count)
{
    switch (order) {
    case ArrivalOrder::Forward:
        return step;
    case ArrivalOrder::Backward:
        return count - 1 - step;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StringID internNumberedText(Typed::Program& program, ASCIILiteral prefix, unsigned number, ASCIILiteral suffix)
{
    String text = makeString(prefix, number, suffix);
    return program.strings.intern(StringView(text));
}

static Vector<MentionedThing> createMentionedThings(Typed::Program& program)
{
    constexpr uint32_t module = 0;
    constexpr uint32_t noFlags = 0;
    Vector<MentionedThing> things;
    things.reserveInitialCapacity(numberOfMentionedThings);
    for (unsigned index = 0; index < thingsInMentionedSpace; ++index) {
        uint32_t name = internNumberedText(program, "name"_s, index, ""_s).index();
        uint32_t text = internNumberedText(program, "text"_s, index, ""_s).index();
        uint32_t pattern = internNumberedText(program, "(?<group"_s, index, ">a+)"_s).index();
        Vector<uint32_t, 8> request { static_cast<uint32_t>(RequestKind::TemplateObject) };
        for (unsigned part = 0; part <= index % mostPartsOfTemplate; ++part) {
            request.append(text);
            request.append(text);
        }
        things.append(MentionedThing { NumberSpace::NameOfText, { name } });
        things.append(MentionedThing { NumberSpace::StringCell, { text } });
        things.append(MentionedThing { NumberSpace::RegExp, { pattern, noFlags } });
        things.append(MentionedThing { NumberSpace::RegExpLiteralCell, { module, index } });
        things.append(MentionedThing { NumberSpace::Request, WTF::move(request) });
    }
    AOT_TEST_CHECK(things.size() == numberOfMentionedThings);
    return things;
}

static Vector<CompiledCode> mentioningBodies(std::span<const MentionedThing> things, ArrivalOrder order)
{
    Vector<CompiledCode> result;
    result.reserveInitialCapacity(numberOfMentioningBodies);
    MentionList mentions;
    for (uint32_t place = 0; place < numberOfMentioningBodies; ++place) {
        uint32_t body = placeInArrivalOrder(order, place, numberOfMentioningBodies);
        unsigned firstThing = body * firstThingStride;
        for (unsigned step = 0; step < thingsMentionedByBody; ++step) {
            const MentionedThing& thing = things[(firstThing + placeInArrivalOrder(order, step, thingsMentionedByBody)) % things.size()];
            mentions.add(thing.space, thing.key.span());
        }
        CompiledCode code = plainBody(body + 1);
        mentions.moveTo(code.body);
        result.append(WTF::move(code));
    }
    return result;
}

static NumberedThings numberThingsMentioned(ArrivalOrder order, unsigned threadCount)
{
    TestBuild build(numberOfMentioningBodies + 1, { .threadCount = threadCount });
    Vector<MentionedThing> things = createMentionedThings(build.program());
    TestImage image = builtImage(build, mentioningBodies(things.span(), order));

    NumberedThings result;
    for (const MentionedThing& thing : things) {
        std::optional<uint32_t> number = image.built.numbers->numberOf(thing.space, thing.key.span());
        AOT_TEST_CHECK(number);
        result.numbers.append(*number);
    }
    for (size_t first = 0; first < things.size(); ++first) {
        for (size_t second = first + 1; second < things.size(); ++second)
            AOT_TEST_CHECK(things[first].space != things[second].space || result.numbers[first] != result.numbers[second]);
    }
    result.bytes = copyBytes(image);
    return result;
}

static void testMentionsInTwoArrivalOrders()
{
    NumberedThings expected = numberThingsMentioned(ArrivalOrder::Forward, threadCounts[0]);
    for (unsigned threadCount : threadCounts) {
        NumberedThings numbered = numberThingsMentioned(ArrivalOrder::Backward, threadCount);
        AOT_TEST_CHECK(numbered.numbers == expected.numbers);
        AOT_TEST_CHECK(numbered.bytes == expected.bytes);
    }
}

static PlacedTypes placeTypesBornIn(ArrivalOrder order)
{
    BuildContext context(wireOptions());
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(programText(2), context);
    AOT_TEST_CHECK(program);

    Vector<StringID> texts;
    for (unsigned index = 0; index < numberOfTypesBorn; ++index)
        texts.append(internNumberedText(*program, "literal"_s, index, ""_s));
    Vector<TypeID> types(FillWith { }, numberOfTypesBorn, TypeID());
    {
        Typed::TypeArena arena(program->types, Typed::ArenaKeeps::InProgram);
        for (unsigned step = 0; step < numberOfTypesBorn; ++step) {
            unsigned index = placeInArrivalOrder(order, step, numberOfTypesBorn);
            types[index] = arena.stringLiteral(texts[index]);
        }
    }
    std::unique_ptr<Typed::Facts> facts = Typed::computeFacts(*program, context, Typed::FactsRequest { .mechanisms = Typed::mechanismsOfProcess() });
    AOT_TEST_CHECK(facts);
    std::unique_ptr<Typed::Plan> plan = Typed::planProgram(*program, *facts, context);
    AOT_TEST_CHECK(plan);
    program->types.freeze();
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());

    MentionList mentionsOfTypes;
    for (TypeID type : types)
        mentionsOfTypes.add(NumberSpace::Type, type.bits());
    CompiledCode code = CodeSketch(1).takeUnchecked();
    mentionsOfTypes.moveTo(code.body);
    LinkedFunctions functions = createLinkedFunctions(bodies(WTF::move(code)), context);

    NumberingInputs inputs { *program, *facts, *plan, functions };
    GatheredMentions mentions = gatherMentions(inputs, context);
    NamesOfTables tables(*program);
    ImageNumbers numbers;
    ImageNumbersBuilder builder { inputs, mentions, tables, numbers, context };
    builder.numberTypes();
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());

    PlacedTypes result;
    for (TypeID type : types) {
        std::optional<uint32_t> place = numbers.placeOfType(type);
        AOT_TEST_CHECK(place);
        result.ids.append(type.bits());
        result.places.append(*place);
    }
    return result;
}

static void testTypesBornInTwoOrders()
{
    PlacedTypes forward = placeTypesBornIn(ArrivalOrder::Forward);
    PlacedTypes backward = placeTypesBornIn(ArrivalOrder::Backward);
    AOT_TEST_CHECK(forward.ids != backward.ids);
    AOT_TEST_CHECK(forward.places == backward.places);

    Vector<uint32_t> sortedPlaces = forward.places;
    std::ranges::sort(sortedPlaces);
    AOT_TEST_CHECK(std::ranges::adjacent_find(sortedPlaces) == sortedPlaces.end());
}

static bool hasUniqueSource(uint32_t place) { return !(place % 3); }

static String quotedSourceAt(uint32_t module, uint32_t place)
{
    if (hasUniqueSource(place))
        return makeString("own"_s, module, '_', place, ".member"_s);
    return makeString("shared"_s, place % numberOfSharedSources, ".member"_s);
}

static String functionSourceAt(uint32_t module, uint32_t place)
{
    if (hasUniqueSource(place))
        return makeString("function own"_s, module, '_', place, "() { }"_s);
    return makeString("function shared"_s, place % numberOfSharedSources, "() { }"_s);
}

static LineColumn lineColumnOfPlace(uint32_t place)
{
    return { place / functionsPerSourceLine + 1, place % functionsPerSourceLine + 1 };
}

static void appendModuleWithSources(StringBuilder& text, ProgramWithSourcesSize size, uint32_t module)
{
    text.append("module m"_s, module, " path=\"m"_s, module, ".js\" displayPath=\"m"_s, module, ".js\""_s);
    if (!module) {
        text.append(" body=f0 requests=["_s);
        for (uint32_t other = 1; other < size.numberOfModules; ++other)
            text.append(other > 1 ? ", "_s : ""_s, "{target=m"_s, other, " completes=true}"_s);
        text.append(']');
    }
    text.append(" positions={lineStarts=["_s);
    for (uint32_t start = 0; start < size.functionsInModule; start += functionsPerSourceLine)
        text.append(start ? ", "_s : ""_s, start);
    text.append("] quotedSources=["_s);
    StringBuilder quotedSourceText;
    for (uint32_t place = 0; place < size.functionsInModule; ++place) {
        unsigned start = quotedSourceText.length();
        quotedSourceText.append(quotedSourceAt(module, place));
        text.append(place ? ", "_s : ""_s, "{position="_s, place, " start="_s, start, " end="_s, quotedSourceText.length(), '}');
    }
    text.append("] quotedSourceText=\""_s, quotedSourceText.toString(), "\"} functionSource=\""_s);
    for (uint32_t place = 0; place < size.functionsInModule; ++place)
        text.append(functionSourceAt(module, place), ' ');
    text.append("\"\n"_s);
}

static String textOfProgramWithSources(ProgramWithSourcesSize size)
{
    StringBuilder text;
    for (uint32_t module = 0; module < size.numberOfModules; ++module)
        appendModuleWithSources(text, size, module);
    text.append("entry module=m0\n"
        "function f0 name=\"main\" module=m0 kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Builtin builtin=GlobalThis @0\n"
        "    v1 = PropGet v0 key=\"sink\" [isFreeName] @0\n"_s);
    for (uint32_t function = 1; function <= size.numberOfFunctions(); ++function)
        text.append("    v"_s, function + 1, " = NewClosure function=f"_s, function, " @0\n"_s);
    uint32_t nextValue = size.numberOfFunctions() + 2;
    for (uint32_t first = 1; first <= size.numberOfFunctions(); first += closuresHandedOutByCall) {
        text.append("    v"_s, nextValue++, " = Call v1, -"_s);
        for (uint32_t function = first; function < first + closuresHandedOutByCall && function <= size.numberOfFunctions(); ++function)
            text.append(", v"_s, function + 1);
        text.append(" @0\n"_s);
    }
    text.append("    Return @0\n}\n"_s);
    unsigned start = 0;
    for (uint32_t function = 1; function <= size.numberOfFunctions(); ++function) {
        uint32_t place = size.placeOf(function);
        if (!place)
            start = 0;
        unsigned end = start + functionSourceAt(size.moduleOf(function), place).length();
        text.append("function f"_s, function, " name=\"f"_s, function, "\" module=m"_s, size.moduleOf(function), " sourceRange={start="_s, start, " end="_s, end, "} results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
        start = end + 1;
    }
    text.append(runtimeFunctionsText(size.numberOfFunctions() + 1));
    return text.toString();
}

static std::array<uint32_t, 2> keyInLongList(uint32_t number)
{
    constexpr uint32_t module = 0;
    return { module, number % numberOfKeysInLongList };
}

static Vector<CompiledCode> codeOfProgramWithSources(const TestBuild& build, ProgramWithSourcesSize size)
{
    Vector<CompiledCode> result;
    result.reserveInitialCapacity(2 * size.numberOfFunctions());
    MentionList mentions;
    for (uint32_t function = 1; function <= size.numberOfFunctions(); ++function) {
        LineColumn place = lineColumnOfPlace(size.placeOf(function));
        CodeSketch sketch = framedSketch(function);
        OptionalIndex site = sketch.addSite(0, place.line, place.column);
        sketch.code().sites.positions.last().source = ModuleID(size.moduleOf(function));
        sketch.callStub(writeBarrier(), site);
        uint64_t constant = function % (size.numberOfFunctions() / timesEachConstantIsHeld);
        sketch.code().body.rareLists.ensure().constants.append(ConstantData { Vector<uint8_t>(asByteSpan(constant)), sizeof(constant) });
        sketch.end();
        CompiledCode code = sketch.take();
        mentions.add(NumberSpace::RegExpLiteralCell, std::span<const uint32_t>(keyInLongList(function)));
        mentions.add(NumberSpace::RegExpLiteralCell, std::span<const uint32_t>(keyInLongList(7 * function)));
        mentions.moveTo(code.body);
        result.append(WTF::move(code));
        result.append(entryOfBody(build, function));
    }
    return result;
}

static String readQuotedSource(const ProgramTables& tables, std::span<const uint8_t> quotedSourceText, uint32_t module, LineColumn place)
{
    std::tuple wanted { module, static_cast<uint32_t>(place.line), static_cast<uint32_t>(place.column) };
    std::span<const QuotedSourceGroupData> groups = tables.quotedSourceGroups();
    auto next = std::ranges::upper_bound(groups, wanted, { }, [](const QuotedSourceGroupData& group) {
        return std::tuple { group.module, group.line, group.column };
    });
    if (next == groups.begin() || std::prev(next)->module != module)
        return { };
    const QuotedSourceGroupData& group = *std::prev(next);
    std::span<const uint8_t> rows = tables.quotedSourceRows();
    const uint8_t* cursor = rows.subspan(group.offsetOfRows).data();
    const uint8_t* end = next == groups.end() ? std::to_address(rows.end()) : rows.subspan(next->offsetOfRows).data();
    uint32_t line = group.line;
    uint32_t column = group.column;
    int64_t start = 0;
    while (cursor < end) {
        QuotedSourceRow row = QuotedSourceRow::read(cursor);
        line += row.lineChange;
        column = row.isOnSameLine ? column + row.columnChange : row.column;
        start += row.startChange;
        if (line == place.line && column == place.column)
            return String(byteCast<Latin1Character>(quotedSourceText.subspan(static_cast<size_t>(start), row.length)));
    }
    return { };
}

static size_t distinctSourcesSize(ProgramWithSourcesSize size, String (*sourceAt)(uint32_t module, uint32_t place))
{
    size_t result = 0;
    HashSet<String> sources;
    for (uint32_t function = 1; function <= size.numberOfFunctions(); ++function) {
        String text = sourceAt(size.moduleOf(function), size.placeOf(function));
        if (sources.add(text).isNewEntry)
            result += text.length();
    }
    return result;
}

static void checkSourcesRoundTrip(const TestImage& image, ProgramWithSourcesSize size)
{
    const ProgramTables& tables = image.image->programTables();
    uint32_t groupsInModule = (size.functionsInModule + quotedSourceGroupSize - 1) / quotedSourceGroupSize;
    AOT_TEST_CHECK(tables.quotedSourceGroups().size() == size.numberOfModules * groupsInModule);
    std::span<const uint8_t> quotedSourceText = tables.quotedSourceText();
    AOT_TEST_CHECK(quotedSourceText.size() == distinctSourcesSize(size, quotedSourceAt));
    for (uint32_t function = 1; function <= size.numberOfFunctions(); ++function) {
        uint32_t module = size.moduleOf(function);
        AOT_TEST_CHECK(readQuotedSource(tables, quotedSourceText, module, lineColumnOfPlace(size.placeOf(function))) == quotedSourceAt(module, size.placeOf(function)));
    }
    AOT_TEST_CHECK(readQuotedSource(tables, quotedSourceText, 0, lineColumnOfPlace(size.functionsInModule)).isNull());
    AOT_TEST_CHECK(readQuotedSource(tables, quotedSourceText, size.numberOfModules, lineColumnOfPlace(0)).isNull());

    AOT_TEST_CHECK(tables.functionSources().size() == 1);
    const FunctionSources& sources = tables.functionSources()[0];
    AOT_TEST_CHECK(sources.numberOfEntries == size.numberOfFunctions());
    std::span entries { tables.at<FunctionSourceEntry>(sources.entriesOffset), sources.numberOfEntries };
    AOT_TEST_CHECK(sources.sourceSize == distinctSourcesSize(size, functionSourceAt));
    std::span functionSources { tables.at<Latin1Character>(sources.sourceOffset), sources.sourceSize };
    uint32_t endOfText = 0;
    for (uint32_t function = 1; function <= size.numberOfFunctions(); ++function) {
        const FunctionSourceEntry& entry = entries[function - 1];
        AOT_TEST_CHECK(tables.sourceFunctionOf(entry.function) == function);
        String text(functionSources.subspan(entry.start, entry.end - entry.start));
        AOT_TEST_CHECK(text == functionSourceAt(size.moduleOf(function), size.placeOf(function)));
        endOfText = std::max<uint32_t>(endOfText, entry.end);
    }
    AOT_TEST_CHECK(endOfText == sources.sourceSize);
}

static void testSourcesRoundTrip()
{
    for (ProgramWithSourcesSize size : { fewSources, manySources }) {
        TestBuild build(textOfProgramWithSources(size), { });
        checkSourcesRoundTrip(builtImage(build, codeOfProgramWithSources(build, size)), size);
    }
}

static HostFunctionSources hostOf(SourcesAtHost& sources)
{
    return {
        .owner = &sources,
        .size = [](void* owner, uint32_t module) { return static_cast<SourcesAtHost*>(owner)->modules[module].size(); },
        .copy = [](void* owner, uint32_t module, uint8_t* destination) {
            const Vector<uint8_t>& source = static_cast<SourcesAtHost*>(owner)->modules[module];
            memcpy(destination, source.span().data(), source.size());
        },
        .view = [](void* owner, uint32_t module) -> const uint8_t* { return module % 2 ? static_cast<SourcesAtHost*>(owner)->modules[module].span().data() : nullptr; },
        .release = [](void* owner) {
            static_cast<SourcesAtHost*>(owner)->modules = { };
            ++static_cast<SourcesAtHost*>(owner)->releases;
        },
    };
}

static void testHostSourcesRoundTrip()
{
    for (ProgramWithSourcesSize size : { fewSources, manySources }) {
        TestBuild build(textOfProgramWithSources(size), { });
        SourcesAtHost sources;
        for (Typed::Module& module : build.program().modules) {
            sources.modules.append(Vector<uint8_t>(std::span<const uint8_t>(module.functionSource)));
            if (sources.modules.size() > 1)
                module.functionSource = { };
        }
        build.context().setFunctionSources(hostOf(sources));
        TestImage image = builtImage(build, codeOfProgramWithSources(build, size));
        AOT_TEST_CHECK(sources.releases == 1);
        checkSourcesRoundTrip(image, size);
    }
}

static Vector<uint8_t> bytesOfImageWithLongLists(unsigned threadCount, const HostThreadPool* hostPool)
{
    TestBuild build(textOfProgramWithSources(manySources), { .threadCount = threadCount, .hostPool = hostPool });
    TestImage image = builtImage(build, codeOfProgramWithSources(build, manySources));
    checkSourcesRoundTrip(image, manySources);
    Vector<uint8_t> bytes = copyBytes(image);
    for (uint32_t number = 0; number < numberOfKeysInLongList; ++number) {
        std::optional<uint32_t> numberOfKey = image.built.numbers->numberOf(NumberSpace::RegExpLiteralCell, std::span<const uint32_t>(keyInLongList(number)));
        AOT_TEST_CHECK(numberOfKey);
        bytes.append(asByteSpan(*numberOfKey));
    }
    return bytes;
}

static void testListsLongerThanRange() { checkSameForAllThreadCounts(bytesOfImageWithLongLists); }

static void appendLayoutsOfGroup(StringBuilder& text, uint32_t group)
{
    uint32_t layout = group * layoutsInGroup;
    uint32_t function = 1 + group * functionsInGroup;
    text.append("layout L"_s, layout, " name=\"Record"_s, group, "\" fields=[{key=\"a\" declared={type=anything} flags=[IsRequired]}, {key=\"b"_s, group % 7, "\" declared={type=anything} flags=[IsRequired]}]\n"_s);
    text.append("layout L"_s, layout + 1, " kind=Environment name=\"scope"_s, group, "\" fields=[{key=\"held\" declared={type=anything}}]\n"_s);
    text.append("layout L"_s, layout + 2, " kind=Instance name=\"Base"_s, group, "\" constructor=f"_s, function + 3, '\n');
    text.append("layout L"_s, layout + 3, " kind=Instance name=\"Derived"_s, group, "\" base=L"_s, layout + 2, " constructor=f"_s, function + 4, " fields=[{key=\"level\" declared={type=anything}}] flags=[HasHeritage]\n"_s);
}

static void appendStatementsOfGroup(StringBuilder& text, uint32_t group)
{
    uint32_t layout = group * layoutsInGroup;
    uint32_t function = 1 + group * functionsInGroup;
    uint32_t value = 2 + group * valuesOfGroupInModuleBody;
    text.append("    v"_s, value, " = NewClosure function=f"_s, function, '\n');
    text.append("    v"_s, value + 1, " = Call v1, -, v"_s, value, '\n');
    text.append("    v"_s, value + 2, " = NewClosure function=f"_s, function + 3, '\n');
    text.append("    v"_s, value + 3, " = NewClass v"_s, value + 2, " layout=L"_s, layout + 2, '\n');
    text.append("    v"_s, value + 4, " = FinishClass v"_s, value + 3, " layout=L"_s, layout + 2, '\n');
    text.append("    v"_s, value + 5, " = NewClosure function=f"_s, function + 4, '\n');
    text.append("    v"_s, value + 6, " = ResolveHeritage v"_s, value + 4, '\n');
    text.append("    v"_s, value + 7, " = NewClass v"_s, value + 5, ", v"_s, value + 4, ", v"_s, value + 6, " layout=L"_s, layout + 3, '\n');
    text.append("    v"_s, value + 8, " = FinishClass v"_s, value + 7, " layout=L"_s, layout + 3, '\n');
    text.append("    v"_s, value + 9, " = Construct v"_s, value + 4, '\n');
    text.append("    v"_s, value + 10, " = Construct v"_s, value + 8, ", -, v0\n"_s);
    text.append("    v"_s, value + 11, " = Call v1, -, v"_s, value + 9, ", v"_s, value + 10, '\n');
    static_assert(valuesOfGroupInModuleBody == 12);
}

static void appendFunctionsOfGroup(StringBuilder& text, uint32_t group)
{
    uint32_t layout = group * layoutsInGroup;
    uint32_t function = 1 + group * functionsInGroup;
    text.append("function f"_s, function, " name=\"make"_s, group, "\" parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
    text.append("    v1 = NewObject -, v0, v0 layout=L"_s, layout, " fields=[0, 1]\n    v2 = NewObject -, v0, v0 layout=L"_s, layout, " fields=[1, 0]\n"_s);
    text.append("    v3 = NewObject -, v0 layout=L"_s, layout + 1, " fields=[0]\n    v4 = NewClosure v3 function=f"_s, function + 1, "\n    v5 = NewClosure v3 function=f"_s, function + 2, '\n');
    text.append("    v6 = Call v0, -, v1, v2, v4, v5\n    Return\n}\n"_s);
    text.append("function f"_s, function + 1, " name=\"set"_s, group, "\" kind=Arrow captures=[{type=env L"_s, layout + 1, "}] parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: env L"_s, layout + 1, ", v1: anything):\n"_s);
    text.append("    PutEnvironmentField v0, v1 layout=L"_s, layout + 1, " field=0\n    Return v1\n}\n"_s);
    text.append("function f"_s, function + 2, " name=\"get"_s, group, "\" kind=Arrow captures=[{type=env L"_s, layout + 1, "}] results=[{type=anything}] {\n  b0(v0: env L"_s, layout + 1, "):\n"_s);
    text.append("    v1 = GetEnvironmentField v0 layout=L"_s, layout + 1, " field=0\n    Return v1\n}\n"_s);
    text.append("function f"_s, function + 3, " name=\"Base"_s, group, "\" kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] constructedClass=L"_s, layout + 2, " {\n  b0(v0: anything):\n"_s);
    text.append("    v1 = GetConstructPrototype v0 layout=L"_s, layout + 2, "\n    v2 = NewObject v1 layout=L"_s, layout + 2, "\n    Return v2\n}\n"_s);
    text.append("function f"_s, function + 4, " name=\"Derived"_s, group, "\" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=anything}] results=[{type=anything}] {\n"_s);
    text.append("    v3 = GetSuperBase v0 homeKind=Object\n    v4 = SuperConstruct v3, v1, v2 layout=L"_s, layout + 3, "\n    v5 = Integer integer=2\n    PropDefine v4, v5 key=\"level\"\n    Return v4\n}\n"_s);
}

static String textOfProgramWithManyLayouts()
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    for (uint32_t group = 0; group < numberOfGroupsOfLayouts; ++group)
        appendLayoutsOfGroup(text, group);
    text.append("function f0 name=\"main\" kind=Module results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    for (uint32_t group = 0; group < numberOfGroupsOfLayouts; ++group)
        appendStatementsOfGroup(text, group);
    text.append("    Return\n}\n"_s);
    for (uint32_t group = 0; group < numberOfGroupsOfLayouts; ++group)
        appendFunctionsOfGroup(text, group);
    text.append(runtimeFunctionsText(1 + numberOfGroupsOfLayouts * functionsInGroup));
    return text.toString();
}

template<typename Record>
static void checkReferencesReadBack(const ProgramTables& tables, const Typed::PlannedLayout& planned, const Record& record)
{
    Vector<uint32_t> expected;
    for (FieldIndex row = 0; row < planned.slots.size(); ++row) {
        const Typed::PlacedSlot& slot = planned.slots[row];
        if (row != planned.extensionWordIndex() && slot.hasStorage() && !slot.isOutOfLine() && traitsOf(slot.representation).holdsCell)
            expected.append(slot.offset);
    }
    std::ranges::sort(expected);
    if (!record.referencesOffset) {
        AOT_TEST_CHECK(expected.isEmpty() && !planned.hasExtension());
        return;
    }
    const uint32_t* counts = tables.at<uint32_t>(static_cast<uint32_t>(static_cast<int64_t>(tables.offsetOf(&record)) + record.referencesOffset));
    Vector<uint32_t> found(std::span { counts + numberOfInlineReferenceCounts, static_cast<size_t>(counts[0]) + counts[1] + counts[2] });
    std::ranges::sort(found);
    AOT_TEST_CHECK(found == expected);
}

static void checkObjectRecordReadBack(const TestBuild& build, const ProgramTables& tables, LayoutID id, uint32_t shape, RecordsReadBack& counts)
{
    const Typed::Plan& plan = build.plan();
    const Typed::PlannedLayout& planned = plan.layout(id);
    const Typed::PlacedShape& placed = plan.shapes().shape(shape);
    const LayoutData& record = tables.layoutForShape(shape);
    AOT_TEST_CHECK(record.index == id.index() && record.shape == shape && record.kind() == build.program().at(id).kind);
    AOT_TEST_CHECK(record.size == planned.size && record.numberOfFields == planned.numberOfRecordFields);
    AOT_TEST_CHECK(record.impossibleMutations() == placed.impossibleMutations);

    const Typed::KeyOrders& keyOrders = plan.keyOrders();
    AOT_TEST_CHECK(record.numberOfOrders == keyOrders.numberOfOrdersOf(id));
    AOT_TEST_CHECK(!record.ordersOffset == !record.numberOfOrders);
    for (uint32_t order = 0; order < record.numberOfOrders; ++order) {
        std::span<const FieldIndex> fields = keyOrders.order(id, order);
        std::span<const uint16_t> entries = tables.keyOrder(record, order).entries();
        AOT_TEST_CHECK(entries.size() == fields.size());
        for (size_t place = 0; place < fields.size(); ++place)
            AOT_TEST_CHECK(plan.slot(id, fields[place]).indexInLayoutRecord == std::optional<uint16_t>(entries[place]));
    }
    checkReferencesReadBack(tables, planned, record);
    if (uint32_t offsetOfClass = record.isClass() ? tables.offsetOfClass(id.index()) : 0)
        AOT_TEST_CHECK(!memcmp(&record.classData(), &tables.at<LayoutData>(offsetOfClass)->classData(), sizeof(ClassData)));
    ++counts.objects;
    counts.objectsWithSeveralOrders += record.numberOfOrders > 1;
}

static void checkEnvironmentRecordReadBack(const TestBuild& build, const ProgramTables& tables, LayoutID id, uint32_t shape, RecordsReadBack& counts)
{
    const Typed::PlannedLayout& planned = build.plan().layout(id);
    const EnvironmentData& record = tables.environmentForShape(shape);
    AOT_TEST_CHECK(record.index == id.index() && record.shape == shape && record.kind == build.program().at(id).kind && record.size == planned.size);
    std::span<const VariableData> described = record.describedVariables();
    size_t next = 0;
    for (const Typed::PlacedSlot& slot : planned.slots.span().first(planned.numberOfFields())) {
        SlotTraits traits = traitsOf(slot.representation);
        if (!slot.hasStorage() || !(traits.holdsCell || (traits.hasAbsent && traits.absentBits)))
            continue;
        AOT_TEST_CHECK(next < described.size() && described[next].slot == slot.data());
        ++next;
    }
    AOT_TEST_CHECK(next == described.size());
    checkReferencesReadBack(tables, planned, record);
    counts.environmentsWithVariables += record.kind == LayoutKind::Environment && !described.empty();
}

static void checkListsOfSubclassesReadBack(const ProgramTables& tables, RecordsReadBack& counts)
{
    Vector<Vector<uint32_t>> classesBelow(tables.numberOfLayoutIndices);
    for (uint32_t index = 0; index < tables.numberOfLayoutIndices; ++index) {
        uint32_t parent = tables.offsetOfClass(index) ? tables.at<LayoutData>(tables.offsetOfClass(index))->assumedBase() : LayoutData::noBase;
        if (parent != LayoutData::noBase)
            classesBelow[parent].append(index);
    }
    for (uint32_t index = 0; index < tables.numberOfLayoutIndices; ++index) {
        if (!tables.offsetOfClass(index))
            continue;
        uint32_t offsetOfList = tables.at<LayoutData>(tables.offsetOfClass(index))->classData().subclassesOffset;
        AOT_TEST_CHECK(!offsetOfList == classesBelow[index].isEmpty());
        if (!offsetOfList)
            continue;
        const uint32_t* list = tables.at<uint32_t>(offsetOfList);
        AOT_TEST_CHECK(std::ranges::equal(std::span { list + 1, static_cast<size_t>(list[0]) }, classesBelow[index]));
        ++counts.listsOfSubclasses;
    }
}

static void checkLayoutRecordsReadBack(const TestBuild& build, const ProgramTables& tables)
{
    const Typed::ShapeTable& shapes = build.plan().shapes();
    RecordsReadBack counts;
    AOT_TEST_CHECK(tables.layouts().size() == static_cast<size_t>(shapes.numberOfShapes()) + 1);
    for (uint32_t shape = 1; shape <= shapes.numberOfShapes(); ++shape) {
        bool hasRecord = shapes.isAssigned(shape) && build.plan().isInUse(shapes.shape(shape).layout);
        AOT_TEST_CHECK(!!tables.layouts()[shape] == hasRecord);
        if (!hasRecord)
            continue;
        if (shape < shapes.firstEnvironmentShape())
            checkObjectRecordReadBack(build, tables, shapes.shape(shape).layout, shape, counts);
        else
            checkEnvironmentRecordReadBack(build, tables, shapes.shape(shape).layout, shape, counts);
    }
    checkListsOfSubclassesReadBack(tables, counts);
    AOT_TEST_CHECK(counts.objects >= numberOfGroupsOfLayouts);
    AOT_TEST_CHECK(counts.environmentsWithVariables >= numberOfGroupsOfLayouts && counts.listsOfSubclasses >= numberOfGroupsOfLayouts);
}

static Vector<uint8_t> bytesOfImageWithManyLayouts(unsigned threadCount, const HostThreadPool* hostPool, Wire::CheckSet checks)
{
    TestBuild build(textOfProgramWithManyLayouts(), { .threadCount = threadCount, .hostPool = hostPool, .checks = checks });
    TestImage image = builtImage(build, bodies(plainBody(1)));
    checkLayoutRecordsReadBack(build, image.image->programTables());
    return copyBytes(image);
}

static void testRecordsOfManyLayouts()
{
    checkSameForAllThreadCounts([](unsigned threadCount, const HostThreadPool* hostPool) {
        return bytesOfImageWithManyLayouts(threadCount, hostPool, { });
    });
}

static void testRecordsOfManyLayoutsOfValidatingBuild()
{
    checkSameForAllThreadCounts([](unsigned threadCount, const HostThreadPool* hostPool) {
        return bytesOfImageWithManyLayouts(threadCount, hostPool, Wire::CheckSet::Validate);
    });
}

static bool isAllZero(std::span<const uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](uint8_t byte) {
        return !byte;
    });
}

static void copyHeaderMembers(ImageHeader& copy, const ImageHeader& header)
{
    static_assert(std::has_unique_object_representations_v<RecordedOptions> && std::has_unique_object_representations_v<ImageCounts> && std::has_unique_object_representations_v<ImageCodeRanges>);
    copy.magic = header.magic;
    copy.stamp = header.stamp;
    copy.identity = header.identity;
    copy.size = header.size;
    copy.codeOffset = header.codeOffset;
    copy.codeSize = header.codeSize;
    copy.address = header.address;
    copy.executableSize = header.executableSize;
    copy.formatVersion = header.formatVersion;
    copy.options = header.options;
    copy.heap.sizeClassProgression = header.heap.sizeClassProgression;
    copy.heap.markedBlockCageShift = header.heap.markedBlockCageShift;
    std::ranges::copy(header.sections, copy.sections);
    copy.counts = header.counts;
    copy.code = header.code;
    copy.flags = header.flags;
    std::ranges::copy(header.entriesUsed, copy.entriesUsed);
    std::ranges::copy(header.singleReturnSites, copy.singleReturnSites);
    copy.numberOfStubsFoundByKey = header.numberOfStubsFoundByKey;
    copy.numbers = header.numbers;
}

static void testGapsAreZero()
{
    TestBuild build(numberOfMixedBodies + 1, { .threadCount = mostThreads });
    TestImage image = builtImage(build, mixedBodies(numberOfMixedBodies));
    std::span<const uint8_t> bytes = std::as_const(image.bytes).span();
    const ImageHeader& header = image.image->header();

    size_t end = sizeof(ImageHeader);
    for (const ImageSectionRange& range : header.sections) {
        AOT_TEST_CHECK(isAllZero(bytes.subspan(end, range.offset - end)));
        end = range.offset + range.size;
    }
    AOT_TEST_CHECK(isAllZero(bytes.subspan(end, header.codeOffset - end)));
    size_t endOfCode = header.codeOffset + header.codeSize;
    AOT_TEST_CHECK(isAllZero(bytes.subspan(endOfCode, header.size - endOfCode)));

    for (const StubAddressInImage& address : image.image->section<StubAddressInImage>(ImageSection::StubAddresses))
        AOT_TEST_CHECK(address.usesCallConvention <= 1);

    alignas(ImageHeader) std::array<uint8_t, sizeof(ImageHeader)> storage { };
    copyHeaderMembers(*reinterpret_cast<ImageHeader*>(storage.data()), header);
    AOT_TEST_CHECK(!memcmp(storage.data(), &header, sizeof(ImageHeader)));
}

#if CPU(ARM64)
static bool isLongBody(uint32_t function) { return function > numberOfBodiesOnEachSide && function <= stepOverLongBodies; }
static uint32_t calleeOf(uint32_t function) { return function <= numberOfBodiesOnEachSide ? function + stepOverLongBodies : function - stepOverLongBodies; }
static EntryShape entryShapeOf(uint32_t function) { return entryShapes[function % entryShapes.size()]; }
static uint16_t calledEntryOf(uint32_t function) { return entryShapeOf(function) == EntryShape::TwoEntries ? secondEntry : primaryEntry; }

static CompiledCode chainedBody(uint32_t function)
{
    uint32_t callee = calleeOf(function);
    CodeSketch sketch(function);
    sketch.withFrame();
    if (entryShapeOf(function) == EntryShape::AfterPrefix) {
        sketch.plain(prefixWord);
        sketch.code().body.entries[0].offset = sketch.size();
    }
    sketch.plain(0x1000 + function);
    if (entryShapeOf(function) == EntryShape::TwoEntries)
        sketch.code().body.entries.append(CodeEntry { sketch.size(), secondEntry });
    sketch.callFunction(callee, sketch.addSite(), calledEntryOf(callee));
    sketch.end();
    return sketch.take();
}

static CompiledCode longBody(uint32_t function)
{
    constexpr size_t roomForWhatFollows = 1 * KB;
    CodeSketch sketch = framedSketch(function);
    auto& bytes = sketch.code().body.bytes;
    size_t start = bytes.size();
    bytes.reserveCapacity(start + sizeOfLongBody + roomForWhatFollows);
    bytes.grow(start + sizeOfLongBody);
    zeroSpan(bytes.mutableSpan().subspan(start));
    sketch.end();
    return sketch.take();
}

static size_t startOf(const TestImage& image, uint32_t function) { return image.built.layout.bodyStarts[image.indexOf(function)]; }

static size_t calledAddressOf(const TestImage& image, uint32_t function)
{
    std::optional<uint32_t> offset = image.built.functions[image.indexOf(function)].body.offsetOfEntry(calledEntryOf(function));
    AOT_TEST_CHECK(offset);
    return startOf(image, function) + *offset;
}

static void checkLinked(const TestImage& image, Vector<uint8_t>& expectedCode, size_t start, std::span<const uint8_t> bytes, const Relocation& relocation, size_t target)
{
    size_t site = start + relocation.offset;
    memcpySpan(expectedCode.mutableSpan().subspan(site), bytes);
    AOT_TEST_CHECK(applyRelocation(expectedCode.mutableSpan(), start, relocation, target));
    AOT_TEST_CHECK(equalSpans(image.image->code().subspan(site, bytes.size()), expectedCode.subspan(site, bytes.size())));
}

static void checkChainedBody(const TestImage& image, uint32_t function, Vector<uint8_t>& expectedCode)
{
    uint32_t position = image.indexOf(function);
    const CodeBody& body = image.built.functions[position].body;
    size_t start = startOf(image, function);
    AOT_TEST_CHECK(!((start + body.primaryEntryOffset()) % imageFunctionAlignment));
    if (entryShapeOf(function) == EntryShape::AfterPrefix) {
        AOT_TEST_CHECK(body.primaryEntryOffset() == sizeof(prefixWord));
        AOT_TEST_CHECK(equalSpans(image.image->code().subspan(start, sizeof(prefixWord)), asByteSpan(prefixWord)));
    }

    AOT_TEST_CHECK(image.built.layout.veneersByBody[position].size() == 1);
    const RelocatableCode& veneerTemplate = testTemplates().veneer;
    size_t veneer = image.built.layout.firstVeneerOffset(position, body.bytes.size());
    AOT_TEST_CHECK(veneer >= start + body.bytes.size());
    checkLinked(image, expectedCode, veneer, veneerTemplate.bytes.span(), veneerTemplate.relocations[0], calledAddressOf(image, calleeOf(function)));

    AOT_TEST_CHECK(body.relocations.size() == 1);
    checkLinked(image, expectedCode, start, testTemplates().nearCall.span(), body.relocations[0], veneer);
}

static void checkChainedBodies(const TestImage& image)
{
    Vector<uint8_t> expectedCode(FillWith { }, image.image->code().size(), 0);
    for (uint32_t function = 1; function <= numberOfFarBodies; ++function) {
        if (!isLongBody(function))
            checkChainedBody(image, function, expectedCode);
    }
}

static Vector<uint8_t> chainedImageBytes(unsigned threadCount, const HostThreadPool* hostPool)
{
    TestBuild build(numberOfFarBodies + 1, { .threadCount = threadCount, .hostPool = hostPool });
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= numberOfFarBodies; ++function)
        code.append(isLongBody(function) ? longBody(function) : chainedBody(function));
    TestImage image = builtImage(build, WTF::move(code));
    AOT_TEST_CHECK(image.built.functions.size() == numberOfFarBodies);
    AOT_TEST_CHECK(build.statistic(StatisticID::Veneers) == numberOfChainedBodies);

    checkChainedBodies(image);
    return copyBytes(image);
}

static void testVeneers() { checkSameForAllThreadCounts(chainedImageBytes); }
#endif // CPU(ARM64)

std::span<const NativeTest> determinismTests()
{
    static constexpr NativeTest tests[] = {
        { "image/determinism/threads"_s, testDeterminismThreads },
        { "image/determinism/two-builds"_s, testDeterminismTwoBuilds },
        { "image/determinism/mentions-in-two-arrival-orders"_s, testMentionsInTwoArrivalOrders },
        { "image/determinism/types-born-in-two-orders"_s, testTypesBornInTwoOrders },
        { "image/determinism/sources-round-trip"_s, testSourcesRoundTrip },
        { "image/determinism/host-sources-round-trip"_s, testHostSourcesRoundTrip },
        { "image/determinism/lists-longer-than-a-range"_s, testListsLongerThanRange },
        { "image/determinism/records-of-many-layouts"_s, testRecordsOfManyLayouts },
        { "image/determinism/records-of-many-layouts-of-a-validating-build"_s, testRecordsOfManyLayoutsOfValidatingBuild },
        { "image/determinism/gaps-are-zero"_s, testGapsAreZero },
#if CPU(ARM64)
        { "image/determinism/veneers"_s, testVeneers },
#endif
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
