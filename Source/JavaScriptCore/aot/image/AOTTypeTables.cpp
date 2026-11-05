/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTImageFormat.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestPlan.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static_assert(!offsetof(LayoutSetData, members));
static_assert(!(RuntimeCellTest::offsetOfOperand() % sizeof(uint32_t)) && !(sizeof(RuntimeType) % sizeof(uint32_t)) && !(sizeof(RuntimeCellTest) % sizeof(uint32_t)));

using TypesSeenByTypeTables = UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

static void nameWhatTypeRecordNames(const Typed::Program& program, const Typed::Type& type, NamesOfTables& names)
{
    program.types.at(type.stringLiterals).forEach([&](StringID string) {
        names.strings.set(string.index());
        names.stringsWithOtherUsesThanShowing.set(string.index());
    });
    program.types.at(type.symbolLiterals).forEach([&](PropertyKeyID symbol) {
        names.keys.set(symbol.index());
    });
}

void nameWhatTypeTablesName(const TableInputs& inputs, const GatheredMentions& mentions, NamesOfTables& names)
{
    const Typed::Program& program = inputs.program;
    TypesSeenByTypeTables seen;
    Vector<TypeID> unread;
    auto add = [&](TypeID type) {
        if (type && !Typed::liesInThread(type) && seen.add(type.bits()).isNewEntry)
            unread.append(type);
    };

    const NumberedKeys& mentioned = mentions.keys(NumberSpace::Type);
    for (uint32_t index = 0; index < mentioned.size(); ++index)
        add(TypeID::fromBits(mentioned.keyAt(index)[0]));
    for (TypeID type : names.types)
        add(type);

    const Typed::SlotPlan& slots = inputs.plan.slots();
    for (unsigned index = 0; index < slots.numberOfLayouts(); ++index) {
        for (const Typed::PlacedSlot& slot : slots.layout(LayoutID(index)).slots) {
            if (slot.indexInLayoutRecord)
                add(slot.typeInLayoutRecord());
        }
    }

    names.types.append(Typed::TypeTable::anything);

    while (!unread.isEmpty()) {
        Typed::Type type = program.at(unread.takeLast());
        nameWhatTypeRecordNames(program, type, names);
        for (const Typed::ClassPart& part : Typed::classPartsOf(program, type)) {
            for (TypeID contents : part.writtenContents)
                add(contents);
        }
    }

    names.strings.set(program.strings.emptyString().index());
}

namespace {

struct LayoutSetTestedByTypeRecords {
    ShapeTestData members { 0, 0, noBitSet };
    Vector<uint64_t> bits;
    PagedBitSet::Entries entries { PagedBitSet::Entries::None };
    uint32_t row { 0 };
};

struct OperandOfShapeTestInTypeRecords {
    uint32_t word;
    uint32_t set;
};

struct TypeRecordsBeingEncoded {
    static constexpr uint32_t noShapePasses = UINT32_MAX;

    Vector<uint32_t> words;
    Vector<uint32_t> firstWord;
    Vector<OperandOfShapeTestInTypeRecords> operandsOfShapeTests;
    Vector<LayoutSetTestedByTypeRecords> sets;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> setOfLayouts;
};

} // anonymous namespace

template<typename T>
static uint32_t setUnsharedTableOfTypeTables(TablePart& part, ProgramTableIndex index, std::span<const T> items)
{
    static_assert(std::has_unique_object_representations_v<T>, "Padding would make the bytes differ from build to build.");
    uint32_t offset = part.appendUnshared(asBytes(items), alignof(T));
    part.setTableRange(index, offset, static_cast<uint32_t>(items.size()));
    return offset;
}

template<typename Record>
static void appendWordsOfTypeRecord(Vector<uint32_t>& words, const Record& record)
{
    auto recordWords = std::bit_cast<std::array<uint32_t, sizeof(Record) / sizeof(uint32_t)>>(record);
    words.append(std::span<const uint32_t>(recordWords));
}

static void appendInt64ToTypeRecord(Vector<uint32_t>& words, int64_t integer)
{
    words.append(static_cast<uint32_t>(integer));
    words.append(static_cast<uint32_t>(static_cast<uint64_t>(integer) >> 32));
}

static uint32_t findOrAddLayoutSetTestedByTypeRecords(const TableEncoder& encoder, const Typed::TypeArena& arena, TypeRecordsBeingEncoded& records, Typed::LayoutSetID layouts)
{
    RELEASE_ASSERT(layouts);
    return records.setOfLayouts.ensure(layouts.bits(), [&]() -> uint32_t {
        const Typed::ShapeTable& shapeTable = encoder.plan.shapes();
        Typed::ShapeTable::ShapeList shapes;
        shapeTable.appendMemberShapes(Typed::membersOf(arena, layouts), shapes);
        if (shapes.isEmpty())
            return TypeRecordsBeingEncoded::noShapePasses;
        LayoutSetTestedByTypeRecords set;
        set.members.last = shapes.last();
        set.members.count = shapes.last() - shapes.first() + 1;
        if (Typed::hasBits(shapeTable.testOfShapes(shapes.span()).form)) {
            Typed::EncodedShapeSet encoded = Typed::encodeShapeSet(shapes.span());
            set.bits = WTF::move(encoded.words);
            set.entries = encoded.entries;
        }
        records.sets.append(WTF::move(set));
        return static_cast<uint32_t>(records.sets.size() - 1);
    }).iterator->value;
}

static std::optional<RuntimeCellTest> encodeCellTestOfTypeRecord(const TableEncoder& encoder, const Typed::TypeArena& arena, TypeRecordsBeingEncoded& records, const Typed::CellTest& test)
{
    switch (test.kind) {
    case CellTestKind::IsObject:
        return RuntimeCellTest::isObject();
    case CellTestKind::IsCallable:
        return RuntimeCellTest::isCallable();
    case CellTestKind::CellTypeIs:
        return RuntimeCellTest::cellTypeIs(test.cellTypes.first, test.cellTypes.last);
    case CellTestKind::ShapeIn: {
        uint32_t set = findOrAddLayoutSetTestedByTypeRecords(encoder, arena, records, test.layouts);
        if (set == TypeRecordsBeingEncoded::noShapePasses)
            return std::nullopt;
        return RuntimeCellTest::shapeIn(set);
    }
    case CellTestKind::InheritsClassInfo:
        return RuntimeCellTest::inheritsClassInfo(test.builtinClass);
    case CellTestKind::IsPristine:
        return RuntimeCellTest::isPristine(test.builtinClass);
    case CellTestKind::IsBuiltinCell: {
        const uint32_t key = BuiltinCellKey::object(test.builtin).encoded();
        if (std::optional<uint32_t> number = encoder.numbers.numberOf(NumberSpace::BuiltinCell, singleElementSpan(key)))
            return RuntimeCellTest::isBuiltinCell(*number);
        encoder.fail("BuiltinOfTypeRecordHasNoCell"_s);
        return std::nullopt;
    }
    case CellTestKind::IsClosureOf: {
        std::optional<FunctionID> entry = encoder.findEntrypoint(test.function);
        if (!entry)
            return std::nullopt;
        const FunctionNumbers& functions = encoder.numbers.functions();
        if (*entry < functions.firstClosureFunction() || *entry >= functions.endOfClosureFunctions()) {
            encoder.fail("ClosureEntryOfTypeRecordHasNoClosureNumber"_s, test.function);
            return std::nullopt;
        }
        return RuntimeCellTest::isClosureOf(functions.closureNumberOf(*entry));
    }
    case CellTestKind::IsSymbol:
        return RuntimeCellTest::isSymbol(encoder.identifierNumber(test.symbol, "the symbol that a type test compares with"_s));
    case CellTestKind::SharingModeIs:
        return RuntimeCellTest::sharingModeIs(test.sharingMode);
    case CellTestKind::IsNamespaceOf:
        return RuntimeCellTest::isNamespaceOf(test.module);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr uint8_t rankOfCellTestInTypeRecord(CellTestKind kind)
{
    switch (kind) {
    case CellTestKind::CellTypeIs:
        return 0;
    case CellTestKind::IsObject:
        return 1;
    case CellTestKind::ShapeIn:
        return 2;
    case CellTestKind::IsPristine:
        return 3;
    case CellTestKind::IsBuiltinCell:
        return 4;
    case CellTestKind::IsSymbol:
        return 5;
    case CellTestKind::SharingModeIs:
        return 6;
    case CellTestKind::IsCallable:
        return 7;
    case CellTestKind::InheritsClassInfo:
        return 8;
    case CellTestKind::IsNamespaceOf:
        return 9;
    case CellTestKind::IsClosureOf:
        return 10;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static void appendTypeRecord(const TableEncoder& encoder, Typed::TypeArena& arena, TypeRecordsBeingEncoded& records, TypeID type)
{
    Typed::TypeTestPlan plan = Typed::computeTypeTestPlanFromAnything(arena, arena.at(arena.testablePartOf(type)));
    RuntimeType header { };
    Vector<RuntimeCellTest, 4> cellTests;
    Vector<uint32_t, 4> literalStrings;
    std::optional<Typed::Range> integers;
    switch (plan.verdict) {
    case Typed::TypeTestVerdict::AlwaysPasses:
        header.primitives = Typed::Primitive::All;
        cellTests.append(RuntimeCellTest::isObject());
        break;
    case Typed::TypeTestVerdict::NeverPasses:
        break;
    case Typed::TypeTestVerdict::NeedsTest:
        header.primitives = plan.passingPrimitives;
        if (!plan.stringLiterals.isEmpty())
            header.primitives = static_cast<uint16_t>((plan.passingPrimitives & ~Typed::Primitive::String) | Typed::Primitive::LiteralString);
        for (const Typed::CellTest& test : plan.cellTests) {
            if (std::optional<RuntimeCellTest> encoded = encodeCellTestOfTypeRecord(encoder, arena, records, test))
                cellTests.append(*encoded);
        }
        for (StringID string : plan.stringLiterals)
            literalStrings.append(encoder.stringNumber(string));
        integers = plan.integers;
        break;
    }

    auto sortKeyOf = [](const RuntimeCellTest& test) {
        return std::tuple { rankOfCellTestInTypeRecord(test.kind), test.firstCellType, test.lastCellType, test.operand };
    };
    AOT::sortInLine(cellTests.mutableSpan(), { }, sortKeyOf);
    removeRepeatedElements(cellTests, [&](const RuntimeCellTest& a, const RuntimeCellTest& b) {
        return sortKeyOf(a) == sortKeyOf(b);
    });
    AOT::sortInLine(literalStrings.mutableSpan());
    if (cellTests.size() > std::numeric_limits<decltype(RuntimeType::numberOfCellTests)>::max() || literalStrings.size() > std::numeric_limits<decltype(RuntimeType::numberOfLiteralStrings)>::max()) {
        encoder.fail("TypeRecordListsMoreThanItsCountsHold"_s);
        return;
    }

    if (integers)
        header.flags |= RuntimeType::hasIntegerRange;
    size_t numberOfTestsOfMoreThanOneFunction = std::ranges::count_if(cellTests, [](const RuntimeCellTest& test) {
        return test.kind != CellTestKind::IsClosureOf;
    });
    header.numberOfTestsOfMoreThanOneFunctionIfFew = static_cast<uint8_t>(std::min<size_t>(RuntimeType::hasManyTestsOfMoreThanOneFunction, numberOfTestsOfMoreThanOneFunction));
    header.numberOfCellTests = static_cast<uint16_t>(cellTests.size());
    header.numberOfLiteralStrings = static_cast<uint16_t>(literalStrings.size());

    Vector<uint32_t>& words = records.words;
    size_t sizeBefore = words.size();
    appendWordsOfTypeRecord(words, header);
    for (const RuntimeCellTest& test : cellTests) {
        if (test.kind == CellTestKind::ShapeIn)
            records.operandsOfShapeTests.append({ static_cast<uint32_t>(words.size() + RuntimeCellTest::offsetOfOperand() / sizeof(uint32_t)), test.operand });
        appendWordsOfTypeRecord(words, test);
    }
    words.appendVector(literalStrings);
    if (integers) {
        appendInt64ToTypeRecord(words, integers->minimum);
        appendInt64ToTypeRecord(words, integers->maximum);
    }
    RELEASE_ASSERT((words.size() - sizeBefore) * sizeof(uint32_t) == RuntimeType::sizeInBytes(header.numberOfCellTests, header.numberOfLiteralStrings, header.flags));
}

static Vector<uint64_t> placeShapeSetsOfTypeRecords(TableEncoder& encoder, TypeRecordsBeingEncoded& records)
{
    Vector<uint32_t> setsWithBits;
    for (uint32_t set = 0; set < records.sets.size(); ++set) {
        if (!records.sets[set].bits.isEmpty())
            setsWithBits.append(set);
    }
    auto bitsOf = [&](uint32_t set) {
        return records.sets[set].bits.span();
    };
    AOT::sortInLine(setsWithBits.mutableSpan(), [&](uint32_t a, uint32_t b) {
        if (bitsOf(a).size() != bitsOf(b).size())
            return bitsOf(a).size() < bitsOf(b).size();
        return std::ranges::lexicographical_compare(bitsOf(a), bitsOf(b));
    });

    Vector<uint64_t> words;
    uint64_t firstWord = 0;
    for (size_t position = 0; position < setsWithBits.size(); ++position) {
        LayoutSetTestedByTypeRecords& set = records.sets[setsWithBits[position]];
        if (!position || !std::ranges::equal(bitsOf(setsWithBits[position - 1]), set.bits.span())) {
            while (words.sizeInBytes() % alignmentOfConstant(set.bits.sizeInBytes()))
                words.append(0);
            firstWord = words.size();
            words.appendVector(set.bits);
        }
        set.members.bitSetOffset = PagedBitSet::locationOf(firstWord, set.entries);
    }
    uint64_t numberOfBits = static_cast<uint64_t>(words.size()) * PagedBitSet::bitsPerPage;
    if (numberOfBits > encoder.build.limit(LimitID::MaxShapeSetBits))
        encoder.build.reportLimitExceeded(LimitID::MaxShapeSetBits, numberOfBits);
    return words;
}

static void writeLayoutSetsOfTypeRecords(TableEncoder& encoder, TypeRecordsBeingEncoded& records)
{
    auto sortKeyOf = [](const ShapeTestData& members) {
        return std::tuple { members.last, members.count, members.bitSetOffset };
    };
    Vector<uint32_t> setsByKey;
    setsByKey.reserveInitialCapacity(records.sets.size());
    for (uint32_t set = 0; set < records.sets.size(); ++set)
        setsByKey.append(set);
    AOT::sortInLine(setsByKey.mutableSpan(), { }, [&](uint32_t set) {
        return sortKeyOf(records.sets[set].members);
    });

    uint32_t name = encoder.shownNameNumber(StringID());
    Vector<LayoutSetData> table;
    for (uint32_t set : setsByKey) {
        const ShapeTestData& members = records.sets[set].members;
        if (table.isEmpty() || sortKeyOf(table.last().members) != sortKeyOf(members))
            table.append(LayoutSetData { members, name });
        records.sets[set].row = static_cast<uint32_t>(table.size() - 1);
    }
    uint32_t offsetOfTable = setUnsharedTableOfTypeTables<LayoutSetData>(encoder.part, ProgramTableIndex::layoutSets, table.span());
    for (const OperandOfShapeTestInTypeRecords& operand : records.operandsOfShapeTests)
        records.words[operand.word] = offsetOfTable + records.sets[operand.set].row * static_cast<uint32_t>(sizeof(LayoutSetData));
}

static Vector<uint32_t> writeTypeRecords(TablePart& part, const TypeRecordsBeingEncoded& records)
{
    size_t numberOfTypes = records.firstWord.size() - 1;
    std::span<const OperandOfShapeTestInTypeRecords> operands = records.operandsOfShapeTests.span();
    Vector<uint32_t> offsets;
    offsets.reserveInitialCapacity(numberOfTypes);
    size_t nextOperand = 0;
    for (size_t place = 0; place < numberOfTypes; ++place) {
        uint32_t first = records.firstWord[place];
        uint32_t end = records.firstWord[place + 1];
        if (first == end) {
            offsets.append(0);
            continue;
        }
        std::span<const uint32_t> record = records.words.span().subspan(first, end - first);
        uint32_t offset = part.append(record);
        for (; nextOperand < operands.size() && operands[nextOperand].word < end; ++nextOperand)
            part.link(offset + static_cast<uint32_t>((operands[nextOperand].word - first) * sizeof(uint32_t)));
        offsets.append(offset);
    }
    RELEASE_ASSERT(nextOperand == operands.size());
    return offsets;
}

static void writeIndexOfTypeRecords(TablePart& part, std::span<const uint32_t> offsets)
{
    uint32_t numberOfTypes = static_cast<uint32_t>(offsets.size());
    uint32_t numberOfGroupWords = ProgramTables::numberOfTypeGroupWords(numberOfTypes);
    Vector<uint32_t> index(FillWith { }, 1 + numberOfGroupWords, 0u);
    index[0] = numberOfTypes;
    std::span<uint32_t> groups = index.mutableSpan().subspan(1);
    uint32_t numberOfRecords = 0;
    for (uint32_t place = 0; place < numberOfTypes; ++place) {
        if (!(place % 32))
            groups[place / 32 * 2 + 1] = numberOfRecords;
        if (!offsets[place])
            continue;
        groups[place / 32 * 2] |= 1u << place % 32;
        ++numberOfRecords;
    }
    for (uint32_t offset : offsets) {
        if (offset)
            index.append(offset);
    }
    uint32_t offsetOfIndex = setUnsharedTableOfTypeTables<uint32_t>(part, ProgramTableIndex::types, index.span());
    part.linkWords(offsetOfIndex + static_cast<uint32_t>((1 + numberOfGroupWords) * sizeof(uint32_t)), numberOfRecords);
}

void encodeTypeTables(TableEncoder& encoder, FirstTableParts& firstParts)
{
    RELEASE_ASSERT(encoder.program.types.isFrozen());
    Typed::TypeArena arena(const_cast<Typed::TypeTable&>(encoder.program.types), Typed::ArenaKeeps::UntilReset);

    std::span<const TypeID> types = encoder.numbers.typesNeededAtRunTime();
    TypeRecordsBeingEncoded records;
    records.firstWord.reserveInitialCapacity(types.size() + 1);
    for (TypeID type : types) {
        records.firstWord.append(static_cast<uint32_t>(records.words.size()));
        appendTypeRecord(encoder, arena, records, type);
    }
    records.firstWord.append(static_cast<uint32_t>(records.words.size()));

    firstParts.shapeSetWords = placeShapeSetsOfTypeRecords(encoder, records);
    writeLayoutSetsOfTypeRecords(encoder, records);
    firstParts.typeOffsets = writeTypeRecords(encoder.part, records);
    writeIndexOfTypeRecords(encoder.part, firstParts.typeOffsets.span());

    encoder.part.setTable(ProgramTableIndex::typeSites, Vector<TypeSiteData>());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
