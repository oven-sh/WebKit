/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinTable.h"
#include "AOTChooseCallingConventions.h"
#include "AOTClassMemberTable.h"
#include "AOTCreateEntrypoints.h"
#include "AOTDefinitions.h"
#include "AOTDerivedFunctions.h"
#include "AOTFieldTable.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFlowResults.h"
#include "AOTFunctionKinds.h"
#include "AOTIdentifierTable.h"
#include "AOTLayoutData.h"
#include "AOTLiteralStringDigest.h"
#include "AOTModuleGraph.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTObjectVisiting.h"
#include "AOTOperationData.h"
#include "AOTPlacementSteps.h"
#include "AOTRepresentation.h"
#include "AOTRuntimeType.h"
#include "AOTSchemaWalkers.h"
#include "AOTSlotAccess.h"
#include "AOTSlotPlanSteps.h"
#include "AOTSort.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTTableInImage.h"
#include "AOTTextEncoder.h"
#include "AOTThreads.h"
#include "AOTTypeTestPlan.h"
#include "AOTVector.h"
#include "IndexingHeader.h"
#include "IndexingType.h"
#include "JSCJSValueInlines.h"
#include "PureNaN.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <iterator>
#include <limits>
#include <tuple>
#include <wtf/ASCIICType.h>
#include <wtf/FastBitVector.h>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxFieldOffset, hostArchitecture()) == 1u << SlotData::offsetBits);
static_assert(defaultLimitValue(LimitID::FieldsInKeyOrder, hostArchitecture()) == maxFieldsInOrder);
static_assert(defaultLimitValue(LimitID::MaxIdentifiers, hostArchitecture()) == 1u << MethodData::nameBits);
static_assert(defaultLimitValue(LimitID::MaxCaptures, hostArchitecture()) <= UINT8_MAX);

template<typename ID>
static uint32_t indexOrNone(ID id)
{
    return id ? id.index() : UINT32_MAX;
}

TableEncoder::TableEncoder(const ProgramTableInputs& inputs, BuildContext& build)
    : inputs(inputs)
    , program(*inputs.pipeline.program)
    , flow(*inputs.pipeline.flow)
    , slots(*inputs.pipeline.slots)
    , placement(*inputs.pipeline.placement)
    , identifiers(build.identifierNumbering())
    , build(build)
    , writer(build)
{
    namedStrings.ensureSize(program.strings.size());
    stringsWithOtherUses.ensureSize(program.strings.size());
    writtenTables.ensureSize(numberOfProgramTables);
    uint32_t headerOffset = writer.allocate(sizeof(ProgramTables), 16);
    RELEASE_ASSERT(!headerOffset);
}

uint32_t TableEncoder::stringNumber(StringID id)
{
    uint32_t number = shownStringNumber(id);
    stringsWithOtherUses.quickSet(number);
    return number;
}

uint32_t TableEncoder::shownStringNumber(StringID id)
{
    if (!id)
        id = program.strings.emptyString();
    namedStrings.quickSet(id.index());
    return id.index();
}

uint32_t TableEncoder::stringConstantNumber(StringID id)
{
    stringNumber(id);
    return numberOrFail(inputs.stringConstants.numberOf(id), ProgramTableRule::StringWithoutConstantNumber);
}

uint32_t TableEncoder::typeOffset(TypeID id) const
{
    if (uint32_t offset = typeOffsets[(id ? id : TypeTable::anything).index()])
        return offset;
    fail(ProgramTableRule::TypeNotEncoded);
    return typeOffsets[TypeTable::anything.index()];
}

uint32_t TableEncoder::dataNodeOf(DataID id) const
{
    if (dataNodes[id.index()] != UINT32_MAX)
        return dataNodes[id.index()];
    fail(ProgramTableRule::DataNotEncoded);
    return 0;
}

FunctionID TableEncoder::findEntrypoint(FunctionID function) const
{
    if (!function)
        return { };
    return inputs.pipeline.derivedFunctions->find(derivedFunctionKey(FunctionRole::GenericEntrypoint, function));
}

uint32_t TableEncoder::entrypointOf(FunctionID function) const
{
    FunctionID entrypoint = findEntrypoint(function);
    if (function && !entrypoint)
        fail(ProgramTableRule::FunctionWithoutEntrypoint, { }, function);
    return indexOrNone(entrypoint);
}

uint32_t TableEncoder::checkedOffset(uint32_t offset) const
{
    if (offset <= SlotData::maxOffset)
        return offset;
    build.reportLimitExceeded(LimitID::MaxFieldOffset, offset);
    return 0;
}

void TableEncoder::fail(ProgramTableRule rule, LayoutID layout, FunctionID function) const
{
    build.diagnostics().add(InternalError { .kind = InternalErrorKind::ProgramTableCheckFailed, .function = function, .layout = layout, .count = static_cast<uint32_t>(rule) });
}

ASCIILiteral nameOf(ProgramTableRule rule)
{
    static constexpr ASCIILiteral names[] = {
        "None"_s,
#define AOT_PROGRAM_TABLE_RULE_NAME(name) #name ""_s,
        FOR_EACH_AOT_PROGRAM_TABLE_RULE(AOT_PROGRAM_TABLE_RULE_NAME)
#undef AOT_PROGRAM_TABLE_RULE_NAME
    };
    RELEASE_ASSERT(static_cast<size_t>(rule) < std::size(names));
    return names[static_cast<size_t>(rule)];
}

SlotData placedVariableSlot(const TableEncoder& encoder, VariableID id)
{
    const PlacedVariable& placed = encoder.placement.variable(id);
    RELEASE_ASSERT(placed.encoding != VariableEncoding::UndefinedAsZero);
    if (placed.storage == VariableStorage::Statics)
        return SlotData::create(encoder.checkedOffset(placed.offset), placed.representation);
    RELEASE_ASSERT(placed.storage == VariableStorage::ClosureTable);
    uint32_t entrypoint = encoder.entrypointOf(placed.function);
    return entrypoint == UINT32_MAX ? SlotData { } : SlotData::create(encoder.checkedOffset(entrypoint), SlotRepresentation::DeferredClosure);
}

// Two words for every 32 indices: which of them have an entry, and how many entries lie in front of them.
static Vector<uint32_t> groupWordsFor(size_t count, size_t numberOfGroupWords, NOESCAPE const Invocable<bool(size_t)> auto& hasEntry)
{
    Vector<uint32_t> groups(FillWith { }, numberOfGroupWords, 0);
    uint32_t numberOfEntries = 0;
    for (size_t index = 0; index < count; ++index) {
        if (!(index % 32))
            groups[index / 32 * 2 + 1] = numberOfEntries;
        if (!hasEntry(index))
            continue;
        groups[index / 32 * 2] |= 1u << index % 32;
        ++numberOfEntries;
    }
    return groups;
}

static size_t appendGroupsAndOffsets(Vector<uint32_t>& words, size_t numberOfGroupWords, const Vector<uint32_t>& offsets)
{
    words.appendVector(groupWordsFor(offsets.size(), numberOfGroupWords, [&](size_t index) {
        return !!offsets[index];
    }));
    size_t sizeBefore = words.size();
    for (uint32_t offset : offsets) {
        if (offset)
            words.append(offset);
    }
    return words.size() - sizeBefore;
}

static_assert(!offsetof(LayoutSetData, members));

namespace {

struct LayoutSetRecords {
    std::optional<uint32_t> recordOf(LayoutSetID set) const { return set ? recordOfSet[set.index()] : std::nullopt; }

    Vector<std::optional<uint32_t>> recordOfSet;
    uint32_t tableOffset { 0 };
};

struct PlannedType {
    TypeID id;
    TypeTestPlan plan;
};

} // anonymous namespace

// Only the report of a mismatch puts a type into words: what a container holds, the name of a set of layouts, where a type was written. No test reads any of them.
static bool describesTypes(const TableEncoder& encoder)
{
    return encoder.build.recordedOptions().policy() == MismatchPolicy::Report;
}

static LayoutSetRecords encodeLayoutSets(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    const ShapeTable& shapes = encoder.placement.shapes();
    unsigned numberOfSets = program.types.numberOfLayoutSets();
    BitVector setsInNeededTypes(numberOfSets);
    for (size_t index : describesTypes(encoder) ? encoder.placement.typesNeededAtRunTime() : BitVector()) {
        if (LayoutSetID set = program.at(TypeID(index)).layouts)
            setsInNeededTypes.quickSet(set.index());
    }

    Vector<LayoutID> namingLayouts(FillWith { }, numberOfSets, LayoutID());
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        if (!isObjectLayout(program.at(id).kind))
            continue;
        LayoutSetID set = program.declaredMembersOf(id);
        if (set && !program.at(set).members.isEmpty() && !namingLayouts[set.index()])
            namingLayouts[set.index()] = id;
    }

    LayoutSetRecords records { Vector<std::optional<uint32_t>>(FillWith { }, numberOfSets, std::nullopt) };
    Vector<LayoutSetData> table;
    for (size_t index : setsInNeededTypes) {
        records.recordOfSet[index] = static_cast<uint32_t>(table.size());
        LayoutSetData data { };
        data.members = shapes.dataOf(shapes.testOf(LayoutSetID(index)));
        data.name = encoder.shownNameNumber(namingLayouts[index] ? program.at(namingLayouts[index]).name : StringID());
        table.append(data);
    }
    records.tableOffset = encoder.setTable(ProgramTableIndex::layoutSets, table);
    return records;
}

static uint32_t shapeTestOffsetOf(TableEncoder& encoder, const LayoutSetRecords& records, LayoutSetID set)
{
    if (std::optional<uint32_t> record = records.recordOf(set))
        return records.tableOffset + *record * static_cast<uint32_t>(sizeof(LayoutSetData));
    const ShapeTable& shapes = encoder.placement.shapes();
    const ShapeTestData test = shapes.dataOf(shapes.testOf(set));
    return encoder.writer.append(singleElementSpan(test));
}

static std::optional<RuntimeCellTest> encodeCellTest(TableEncoder& encoder, const LayoutSetRecords& records, const CellTest& test)
{
    switch (test.kind) {
    case CellTestKind::IsObject:
        return RuntimeCellTest::isObject();
    case CellTestKind::IsCallable:
        return RuntimeCellTest::isCallable();
    case CellTestKind::CellTypeIs:
        return RuntimeCellTest::cellTypeIs(test.cellTypes.first, test.cellTypes.last);
    case CellTestKind::ShapeIn:
        return RuntimeCellTest::shapeIn(shapeTestOffsetOf(encoder, records, test.layouts));
    case CellTestKind::InheritsClassInfo:
        return RuntimeCellTest::inheritsClassInfo(test.builtinClass);
    case CellTestKind::IsPristine:
        return RuntimeCellTest::isPristine(test.builtinClass);
    case CellTestKind::IsBuiltinCell:
        return RuntimeCellTest::isBuiltinCell(encoder.numberOrFail(encoder.placement.builtinCell(test.builtin), ProgramTableRule::TypeNotEncoded));
    case CellTestKind::IsClosureOf:
        if (FunctionID entrypoint = encoder.findEntrypoint(test.function))
            return RuntimeCellTest::isClosureOf(entrypoint);
        return std::nullopt;
    case CellTestKind::IsSymbol:
        return RuntimeCellTest::isSymbol(encoder.identifierNumber(test.symbol));
    case CellTestKind::SharingModeIs:
        return RuntimeCellTest::sharingModeIs(test.sharingMode);
    case CellTestKind::IsNamespaceOf:
        return RuntimeCellTest::isNamespaceOf(test.module);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID describedContentsOf(const Program& program, TypeID id)
{
    const Type& type = program.at(id);
    if (type.classes.size() != 1)
        return { };
    TypeID contents = type.classes[0].contentsAt(program.builtins(), ContentsSlot::Element);
    return contents == TypeTable::anything ? TypeID() : contents;
}

template<typename Record>
static void appendRecord(Vector<uint32_t, 16>& words, const Record& record)
{
    auto recordWords = std::bit_cast<std::array<uint32_t, sizeof(Record) / sizeof(uint32_t)>>(record);
    words.append(std::span { recordWords });
}

static void appendInt64(Vector<uint32_t, 16>& words, int64_t integer)
{
    words.appendList({ static_cast<uint32_t>(integer), static_cast<uint32_t>(static_cast<uint64_t>(integer) >> 32) });
}

static void appendRuntimeType(TableEncoder& encoder, const LayoutSetRecords& records, const PlannedType& planned)
{
    const TypeTestPlan& plan = planned.plan;
    const LiteralStringTable& numbered = *encoder.inputs.pipeline.literalStrings;
    RuntimeType header { };
    Vector<RuntimeCellTest, 4> cellTests;
    Vector<StringID, 4> literalStrings;
    std::optional<Range> integers;
    uint32_t contentsOffset = 0;
    std::optional<uint32_t> layoutSet;
    switch (plan.verdict) {
    case TypeTestVerdict::AlwaysPasses:
        header.primitives = encoder.program.at(TypeTable::anything).primitives;
        cellTests.append(RuntimeCellTest::isObject());
        break;
    case TypeTestVerdict::NeverPasses:
        break;
    case TypeTestVerdict::NeedsTest:
        header.primitives = plan.stringLiterals.isEmpty() ? plan.passingPrimitives : (plan.passingPrimitives & ~Primitive::String) | Primitive::LiteralString;
        for (const CellTest& test : plan.cellTests) {
            if (std::optional<RuntimeCellTest> encoded = encodeCellTest(encoder, records, test))
                cellTests.append(*encoded);
        }
        for (StringID literal : plan.stringLiterals) {
            if (!numbered.isLeftOut(literal))
                literalStrings.append(literal);
        }
        integers = plan.integers;
        if (describesTypes(encoder)) {
            if (TypeID contents = describedContentsOf(encoder.program, planned.id))
                contentsOffset = encoder.typeOffsets[contents.index()];
            layoutSet = records.recordOf(encoder.program.at(planned.id).layouts);
        }
        break;
    }
    if (cellTests.size() > std::numeric_limits<decltype(RuntimeType::numberOfCellTests)>::max() || literalStrings.size() > std::numeric_limits<decltype(RuntimeType::numberOfLiteralStrings)>::max()) {
        encoder.fail(ProgramTableRule::TypeNotEncoded);
        return;
    }
    header.flags = (integers ? RuntimeType::hasIntegerRange : 0) | (contentsOffset ? RuntimeType::hasContents : 0) | (layoutSet ? RuntimeType::hasLayoutSet : 0);
    for (StringID string : literalStrings)
        header.flags |= numbered.isWithoutText(string) ? RuntimeType::hasLiteralStringWithoutText : numbered.indexOf(string) ? 0 : RuntimeType::hasLiteralStringWithoutWord;
    header.numberOfCellTests = static_cast<uint16_t>(cellTests.size());
    header.numberOfLiteralStrings = static_cast<uint16_t>(literalStrings.size());

    Vector<uint32_t, 16> words;
    appendRecord(words, header);
    for (const RuntimeCellTest& test : cellTests)
        appendRecord(words, test);
    for (StringID string : literalStrings)
        words.append(numbered.isWithoutText(string) ? string.index() : encoder.stringNumber(string));
    if (integers) {
        appendInt64(words, integers->minimum);
        appendInt64(words, integers->maximum);
    }
    if (contentsOffset)
        words.append(contentsOffset);
    if (layoutSet)
        words.append(*layoutSet);
    RELEASE_ASSERT(words.sizeInBytes() == RuntimeType::sizeInBytes(header.numberOfCellTests, header.numberOfLiteralStrings, header.flags));
    encoder.typeOffsets[planned.id.index()] = encoder.writer.append(words);
}

static void encodeTypes(TableEncoder& encoder, const LayoutSetRecords& records)
{
    const Program& program = encoder.program;
    const BitVector& neededTypes = encoder.placement.typesNeededAtRunTime();
    auto plannedTypes = collectInParallel<PlannedType>(encoder.build.parallelLoops(), program.types.size(), [&](unsigned index, Vector<PlannedType>& collected) {
        TypeID id(index);
        if (neededTypes.get(index))
            collected.append({ id, computeTypeTestPlanFromAnything(program, program.at(id).withoutClassProofs().withUntestableBuiltinsWidened()) });
    });
    auto findPlannedType = [&](TypeID id) -> const PlannedType* {
        auto found = std::ranges::lower_bound(plannedTypes, id, { }, &PlannedType::id);
        return id && found != plannedTypes.end() && found->id == id ? &*found : nullptr;
    };
    encoder.typeOffsets = Vector<uint32_t>(FillWith { }, program.types.size(), 0u);
    Vector<const PlannedType*, 8> typesBeforeContents;
    for (const PlannedType& planned : plannedTypes) {
        for (const PlannedType* next = &planned; next && !encoder.typeOffsets[next->id.index()]; next = findPlannedType(describedContentsOf(program, next->id))) {
            RELEASE_ASSERT(typesBeforeContents.size() < plannedTypes.size());
            typesBeforeContents.append(next);
        }
        while (!typesBeforeContents.isEmpty())
            appendRuntimeType(encoder, records, *typesBeforeContents.takeLast());
    }
    uint32_t numberOfTypes = program.types.size();
    Vector<uint32_t> index { numberOfTypes };
    encoder.build.statistics().count(StatisticID::TypeRecordsEmitted, appendGroupsAndOffsets(index, ProgramTables::numberOfTypeGroupWords(numberOfTypes), encoder.typeOffsets));
    encoder.setTable(ProgramTableIndex::types, index);
}

static FastBitVector findUsedTypeSites(const Program& program, const ParallelLoops& loops)
{
    FastBitVector usedSites(program.typeSites.size());
    auto markUsed = [&](TypeSiteID site) {
        usedSites.atomicSetAndCheck(site.index(), true);
    };
    forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values)
                forEachTypeSite(function.at(id), markUsed);
            forEachTypeSite(block.terminator, markUsed);
        }
    });
    for (const Layout& layout : program.layouts) {
        for (const StaticField& field : layout.staticFields) {
            if (TypeSiteID site = program.at(field.variable).declared.site)
                markUsed(site);
        }
    }
    return usedSites;
}

void encodeTypeTables(TableEncoder& encoder)
{
    encodeTypes(encoder, encodeLayoutSets(encoder));

    Vector<TypeSiteData> sites;
    if (describesTypes(encoder)) {
        sites.reserveInitialCapacity(encoder.inputs.bodyFacts.usedTypeSites.bitCount());
        encoder.inputs.bodyFacts.usedTypeSites.forEachSetBit([&](size_t index) {
            const TypeSite& site = encoder.program.typeSites[index];
            TypeSiteData data { };
            data.site = static_cast<uint32_t>(index);
            data.module = site.module.index();
            data.position = site.position;
            data.kind = static_cast<uint32_t>(site.kind);
            data.text = TypeSiteData::noText;
            if (site.text && !encoder.obfuscates()) {
                if (uint32_t text = encoder.shownStringNumber(site.text); text < TypeSiteData::noText)
                    data.text = text;
                else
                    encoder.build.statistics().count(StatisticID::TypeSiteTextsLeftOut);
            }
            sites.append(data);
        });
    }
    encoder.setTable(ProgramTableIndex::typeSites, sites);
}

static constexpr uint32_t dataWordSize = sizeof(uint64_t);
static constexpr FieldIndex noFieldIndex = std::numeric_limits<FieldIndex>::max();

static bool isLeaf(const DataNode& node)
{
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Hole:
    case DataNode::Kind::Boolean:
    case DataNode::Kind::Number:
    case DataNode::Kind::String:
    case DataNode::Kind::BigInt:
        return true;
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint64_t encodeDataWord(TableEncoder& encoder, const DataNode& leaf)
{
    switch (leaf.kind()) {
    case DataNode::Kind::Undefined:
        return DataWord::encodeImmediate(jsUndefined());
    case DataNode::Kind::Null:
        return DataWord::encodeImmediate(jsNull());
    case DataNode::Kind::Hole:
        return DataWord::hole;
    case DataNode::Kind::Boolean:
        return DataWord::encodeImmediate(jsBoolean(leaf.as<DataNode::Boolean>().value));
    case DataNode::Kind::Number:
        return DataWord::encodeImmediate(jsNumber(purifyNaN(leaf.as<DataNode::Number>().value)));
    case DataNode::Kind::String:
        return DataWord::encodeStringConstant(encoder.stringConstantNumber(leaf.as<DataNode::String>().value));
    case DataNode::Kind::BigInt:
        if (std::optional<uint32_t> number = encoder.placement.bigIntConstant(leaf.as<DataNode::BigInt>().digits))
            return DataWord::encodeBigIntConstant(*number);
        encoder.fail(ProgramTableRule::DataNotEncoded);
        return DataWord::hole;
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

IndexingType indexingTypeOf(const Program& program, const DataNode::Array& array)
{
    return arrayStorageFormOf(arrayStorageOfData(program, array)).indexingType;
}

namespace {

struct OpenEncodedNode {
    const DataNode* node { nullptr };
    IndexingType indexingType { ArrayWithContiguous };
    uint32_t numberOfChildren { 0 };
    uint32_t nextChild { 0 };
    uint32_t firstWord { 0 };
    uint32_t firstField { 0 };
};

class DataNodeEncoder {
public:
    explicit DataNodeEncoder(TableEncoder& encoder)
        : m_encoder(encoder)
        , m_tableOffset(encoder.writer.allocate(dataWordSize))
    {
    }

    uint32_t tableOffset() const { return m_tableOffset; }
    uint32_t numberOfWords() const { return static_cast<uint32_t>((m_encoder.writer.size() - m_tableOffset) / dataWordSize); }

    uint32_t encode(const DataNode& root)
    {
        ASSERT(m_openNodes.isEmpty() && m_words.isEmpty() && m_fields.isEmpty());
        openNode(root);
        while (!m_openNodes.isEmpty()) {
            OpenEncodedNode& innermost = m_openNodes.last();
            if (innermost.nextChild == innermost.numberOfChildren) {
                closeNode(m_openNodes.takeLast());
                continue;
            }
            uint32_t index = innermost.nextChild++;
            visitChild(innermost, index);
        }
        return DataWord::operandOf(m_words.takeLast());
    }

private:
    void openNode(const DataNode& node)
    {
        OpenEncodedNode opened { .node = &node, .firstWord = static_cast<uint32_t>(m_words.size()), .firstField = static_cast<uint32_t>(m_fields.size()) };
        if (const auto* array = node.tryAs<DataNode::Array>()) {
            opened.indexingType = indexingTypeOf(m_encoder.program, *array);
            opened.numberOfChildren = array->elements.size();
        } else {
            auto& object = node.as<DataNode::Object>();
            const Layout* layout = object.layout ? &m_encoder.program.at(object.layout) : nullptr;
            for (const DataMember& member : object.members)
                m_fields.append(layout ? layout->findField(member.key).value_or(noFieldIndex) : noFieldIndex);
            opened.numberOfChildren = object.members.size();
        }
        m_openNodes.append(opened);
    }

    void visitChild(OpenEncodedNode parent, uint32_t index)
    {
        const DataNode* value;
        if (const auto* array = parent.node->tryAs<DataNode::Array>()) {
            value = &array->elements[index];
            if (parent.indexingType == ArrayWithDouble) {
                const auto* number = value->tryAs<DataNode::Number>();
                m_words.append(std::bit_cast<uint64_t>(number ? number->value : PNaN));
                return;
            }
        } else {
            auto& object = parent.node->as<DataNode::Object>();
            FieldIndex field = m_fields[parent.firstField + index];
            if (field != noFieldIndex && !m_encoder.slots.slot(object.layout, field).hasStorage())
                return;
            value = &object.members[index].value;
        }
        if (isLeaf(*value))
            m_words.append(encodeDataWord(m_encoder, *value));
        else
            openNode(*value);
    }

    void closeNode(const OpenEncodedNode& closed)
    {
        std::span<const uint64_t> contents = m_words.span().subspan(closed.firstWord);
        uint32_t node;
        if (const auto* array = closed.node->tryAs<DataNode::Array>())
            node = appendNode(DataNodeHeader::Kind::Array, closed.indexingType | (isStorageOfArray(*array) ? DataNodeHeader::isStorage : 0), contents);
        else
            node = appendObject(closed.node->as<DataNode::Object>(), m_fields.span().subspan(closed.firstField), contents);
        m_words.shrink(closed.firstWord);
        m_fields.shrink(closed.firstField);
        m_words.append(DataWord::encodeNode(node));
    }

    uint32_t appendObject(const DataNode::Object& object, std::span<const FieldIndex> fieldOfEachMember, std::span<const uint64_t> contents)
    {
        const Placement& placement = m_encoder.placement;
        LayoutID layout = object.layout;
        Vector<FieldIndex, 16> fields;
        Vector<uint16_t, 16> fieldIndices;
        Vector<uint64_t, 16> fieldValues;
        Vector<uint32_t, 16> otherKeys;
        Vector<uint64_t, 16> otherValues;
        size_t nextWord = 0;
        for (size_t index = 0; index < fieldOfEachMember.size(); ++index) {
            FieldIndex field = fieldOfEachMember[index];
            if (field == noFieldIndex) {
                otherKeys.append(m_encoder.identifierNumber(object.members[index].key));
                otherValues.append(contents[nextWord++]);
                continue;
            }
            fields.append(field);
            const PlacedSlot& slot = m_encoder.slots.slot(layout, field);
            if (!slot.hasStorage())
                continue;
            fieldIndices.append(*slot.indexInLayoutRecord);
            fieldValues.append(contents[nextWord++]);
        }
        ASSERT(nextWord == contents.size());
        ASSERT(otherValues.size() == otherKeys.size());
        if (!layout)
            return appendNode(DataNodeHeader::Kind::OrdinaryObject, 0, otherValues.span(), asBytes(otherKeys.span()));

        AllocationID allocation = placement.allocations().find(layout, placement.shapes().generalShapeOf(layout), fields.span());
        if (!allocation) {
            m_encoder.fail(ProgramTableRule::DataNotEncoded, layout);
            return 0;
        }
        if (otherKeys.isEmpty()) {
            if (std::optional<uint32_t> node = tryAppendObjectBody(object, allocation, fieldOfEachMember, contents))
                return *node;
            return appendNode(DataNodeHeader::Kind::Object, allocation.index(), fieldValues.span(), asBytes(fieldIndices.span()));
        }
        uint32_t otherMembers = appendNode(DataNodeHeader::Kind::OtherMembers, 0, otherValues.span(), asBytes(otherKeys.span()));
        return appendNode(DataNodeHeader::Kind::ObjectWithOtherMembers, allocation.index(), fieldValues.span(), asBytes(fieldIndices.span()), otherMembers);
    }

    bool tryWriteSlot(const PlacedSlot& slot, void* bytes, const DataNode& value, uint64_t word)
    {
        SlotRepresentation representation = slot.representation;
        SlotTraits traits = traitsOf(representation);
        switch (value.kind()) {
        case DataNode::Kind::Undefined:
            if (traits.hasUndefined)
                writeSlotBits(representation, bytes, traits.undefinedBits);
            return traits.hasUndefined;
        case DataNode::Kind::Null:
            if (traits.hasNull)
                writeSlotBits(representation, bytes, traits.nullBits);
            return traits.hasNull;
        case DataNode::Kind::Boolean:
            return tryWriteBoolean(representation, bytes, value.as<DataNode::Boolean>().value);
        case DataNode::Kind::Number:
            return tryWriteDouble(representation, bytes, value.as<DataNode::Number>().value);
        case DataNode::Kind::String:
            if (traits.valueKind == SlotValueKind::LiteralString) {
                StringID string = value.as<DataNode::String>().value;
                const LiteralStringTable& literalStrings = *m_encoder.inputs.pipeline.literalStrings;
                if (m_encoder.program.string(string).empty() || literalStrings.indexOf(string)) {
                    writeSlotBits(representation, bytes, m_encoder.program.string(string).empty() ? emptyLiteralStringWord : literalStringWord(literalStrings.numberOf(string)));
                    return true;
                }
            }
            break;
        case DataNode::Kind::BigInt:
        case DataNode::Kind::Array:
        case DataNode::Kind::Object:
            break;
        case DataNode::Kind::Hole:
            return false;
        }

        if (!traits.holdsCell || traits.valueKind == SlotValueKind::DeferredClosure)
            return false;
        if (traits.isNarrowReference && DataWord::operandOf(word) > NarrowDataWord::maxOperand)
            return false;
        writeSlotBits(representation, bytes, traits.isNarrowReference ? NarrowDataWord::encode(DataWord::kindOf(word), DataWord::operandOf(word)) : word);
        return true;
    }

    std::optional<uint32_t> tryAppendObjectBody(const DataNode::Object& object, AllocationID allocation, std::span<const FieldIndex> fieldOfEachMember, std::span<const uint64_t> contents)
    {
        const PlannedLayout& layout = m_encoder.slots.layout(object.layout);
        uint32_t size = m_encoder.placement.allocations().allocations[allocation.index()].size;
        if (size != layout.size || size < offsetOfFirstField || size % dataWordSize)
            return std::nullopt;

        Vector<uint64_t, 16> body(FillWith { }, (size - offsetOfFirstField) / dataWordSize, 0);
        auto bytesOf = [&](const PlacedSlot& slot) {
            RELEASE_ASSERT(slot.offset >= offsetOfFirstField && slot.end() <= size);
            return reinterpret_cast<uint8_t*>(body.mutableSpan().data()) + (slot.offset - offsetOfFirstField);
        };
        for (FieldIndex field = 0; field < layout.numberOfFields(); ++field) {
            const PlacedSlot& slot = layout.slots[field];
            if (slot.hasStorage() && !slot.isOutOfLine())
                clearSlot(slot.representation, bytesOf(slot));
        }

        size_t nextWord = 0;
        for (size_t index = 0; index < fieldOfEachMember.size(); ++index) {
            FieldIndex field = fieldOfEachMember[index];
            const PlacedSlot& slot = layout.slots[field];
            if (!slot.hasStorage())
                continue;
            const DataNode& value = object.members[index].value;
            bool setsMarkOfCell = (value.is<DataNode::Undefined>() || value.is<DataNode::Null>()) && holdsRawValueFromBirth(m_encoder.program, { object.layout, field });
            if (slot.isOutOfLine() || setsMarkOfCell || !tryWriteSlot(slot, bytesOf(slot), value, contents[nextWord++]))
                return std::nullopt;
        }
        return appendNode(DataNodeHeader::Kind::ObjectBody, allocation.index(), body.span());
    }

    uint32_t appendNode(DataNodeHeader::Kind kind, uint32_t operand, std::span<const uint64_t> contents, std::span<const uint8_t> behindContents = { }, std::optional<uint32_t> otherMembers = std::nullopt)
    {
        TableWriter& writer = m_encoder.writer;
        uint32_t count = static_cast<uint32_t>(contents.size());
        uint32_t numberOfWords = DataNodeHeader::numberOfWords(kind, count);
        uint32_t offset = writer.allocate(static_cast<size_t>(numberOfWords) * dataWordSize);

        DataNodeHeader header { };
        header.count = count;
        header.kindBits = static_cast<uint32_t>(kind);
        header.operand = operand;
        writer.at<uint64_t>(offset) = header.encoded();

        uint32_t contentsOffset = offset + dataWordSize;
        if (kind == DataNodeHeader::Kind::Array) {
            static_assert(!IndexingHeader::offsetOfPublicLength() && IndexingHeader::offsetOfVectorLength() == sizeof(uint32_t) && sizeof(IndexingHeader) == dataWordSize);
            writer.at<uint64_t>(contentsOffset) = count | static_cast<uint64_t>(count) << 32;
            contentsOffset += dataWordSize;
        }
        writer.write(writer.write(contentsOffset, contents), behindContents);
        if (otherMembers)
            writer.at<uint64_t>(offset + (numberOfWords - 1) * dataWordSize) = DataWord::encodeNode(*otherMembers);
        return (offset - m_tableOffset) / dataWordSize;
    }

    TableEncoder& m_encoder;
    uint32_t m_tableOffset;
    Vector<OpenEncodedNode, 16> m_openNodes;
    Vector<uint64_t> m_words;
    Vector<FieldIndex> m_fields;
};

} // anonymous namespace

static std::optional<uint32_t> tryEncodeTableInImage(TableEncoder& encoder, const DataNode::Array& entries, bool hasValues)
{
    Vector<TableInImage::Key> keys;
    Vector<uint64_t> values;
    Vector<uint32_t> strings;
    for (const DataNode& entry : entries.elements) {
        const auto* key = (hasValues ? entry.as<DataNode::Array>().elements[0] : entry).tryAs<DataNode::String>();
        if (!key || (hasValues && !isLeaf(entry.as<DataNode::Array>().elements[1])))
            return std::nullopt;
    }
    keys.reserveInitialCapacity(entries.elements.size());
    values.reserveInitialCapacity(hasValues ? entries.elements.size() : 0);
    strings.reserveInitialCapacity(entries.elements.size());
    for (const DataNode& entry : entries.elements) {
        StringID string = (hasValues ? entry.as<DataNode::Array>().elements[0] : entry).as<DataNode::String>().value;
        keys.append(TableInImage::Key { encoder.stringNumber(string), decodeForImage(encoder.program.strings[string]).hash });
        strings.append(keys.last().string);
        if (!hasValues)
            continue;
        const DataNode& value = entry.as<DataNode::Array>().elements[1];
        values.append(value.is<DataNode::Hole>() ? DataWord::encodeImmediate(jsUndefined()) : encodeDataWord(encoder, value));
    }
    AOT::sort(strings);
    if (std::ranges::adjacent_find(strings) != strings.end())
        return std::nullopt;

    std::optional<Vector<uint8_t>> bytes = TableInImage::tryEncode(keys.span(), values.span());
    if (!bytes)
        return std::nullopt;
    return encoder.writer.append(bytes->span(), alignof(TableInImage));
}

void encodeDataTable(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    std::span<const RequestRecord> records = encoder.placement.requests().records();
    encoder.dataNodes.fill(UINT32_MAX, program.data.size());
    encoder.tablesInImage.fill(std::nullopt, records.size());
    BitVector recordsWithNodes(program.data.size());
    Vector<std::pair<uint32_t, DataID>> placedNodes;
    for (size_t index = 0; index < records.size(); ++index) {
        WTF::switchOn(records[index],
            [&](const DataRecord& request) {
                recordsWithNodes.quickSet(request.data.index());
                if (request.node)
                    placedNodes.append({ *request.node, request.data });
            },
            [&](const CollectionDataRecord& request) {
                const auto* entries = program.at(request.data).root.tryAs<DataNode::Array>();
                auto isPair = [](const DataNode& entry) {
                    const auto* pair = entry.tryAs<DataNode::Array>();
                    return pair && pair->elements.size() == 2;
                };
                if (!entries || (request.hasValues && !std::ranges::all_of(entries->elements, isPair))) {
                    encoder.fail(ProgramTableRule::DataNotEncoded);
                    return;
                }
                encoder.tablesInImage[index] = tryEncodeTableInImage(encoder, *entries, request.hasValues);
                if (!encoder.tablesInImage[index])
                    recordsWithNodes.quickSet(request.data.index());
            },
            [](const NewClassRecord&) { },
            [](const CloneAsRecord&) { },
            [](const TemplateObjectRecord&) { },
            [](const DirectEvalRecord&) { });
    }
    AOT::sort(placedNodes, { }, &std::pair<uint32_t, DataID>::first);
    placedNodes.shrink(std::ranges::unique(placedNodes).begin() - placedNodes.begin());

    DataNodeEncoder nodes(encoder);
    // Compiled code has the places of these.
    for (auto [node, data] : placedNodes) {
        encoder.dataNodes[data.index()] = nodes.encode(program.at(data).root);
        if (encoder.dataNodes[data.index()] != node)
            encoder.fail(ProgramTableRule::DataNotEncoded);
    }
    for (size_t index : recordsWithNodes) {
        const DataNode& root = program.data[index].root;
        if (!isLeaf(root) && encoder.dataNodes[index] == UINT32_MAX)
            encoder.dataNodes[index] = nodes.encode(root);
    }
    encoder.setTableRange(ProgramTableIndex::data, nodes.tableOffset(), nodes.numberOfWords());
}

static uint16_t fieldFlagsOf(TableEncoder& encoder, FieldReference reference)
{
    const Program& program = encoder.program;
    const Field& declaration = fieldAt(program, declarationOf(program, reference));
    OptionSet<FieldStability> stability = flowOf(program, reference).stability;
    const PlacedSlot& slot = encoder.slots.slot(reference);
    const std::pair<bool, uint16_t> flags[] = {
        { !declaration.flags.contains(FieldFlags::IsRequired), fieldIsOptional },
        { slot.isOutOfLine(), fieldIsOutOfLine },
        // Object::tryStore(): a slot without an absent state holds a placeholder until its only store, which may come by name: such a store is like any other.
        { flowOf(program, reference).location.stores == StoreCount::AtMostOnce && canLeaveSlot(slot.representation), fieldIsStoredOnce },
        { slot.mayExistWithoutStorage, fieldMayExistWithoutStorage },
        { slot.deferredClosureCapturesObject, fieldDeferredClosureCapturesObject },
        { slot.deferredClosureCapturesEnvironment, fieldDeferredClosureCapturesEnvironment },
        { declaration.flags.contains(FieldFlags::IsPrivate) || (declaration.flags.contains(FieldFlags::IsHidden) && !declaration.key), fieldIsPrivate },
        { declaration.flags.contains(FieldFlags::IsBrand), fieldIsBrand },
        { stability.contains(FieldStability::IsNeverDeleted), fieldIsNeverDeleted },
        { stability.contains(FieldStability::IsNeverRedefined), fieldIsNeverRedefined },
        { stability.contains(FieldStability::IsNeverStoredWithOtherType), fieldIsNeverStoredWithOtherType },
        { declaration.form.contains(FieldForm::IsReadOnly), fieldIsReadOnly },
        { declaration.form.contains(FieldForm::IsNotEnumerable), fieldIsNotEnumerable },
        { declaration.form.contains(FieldForm::IsNotConfigurable), fieldIsNotConfigurable },
        { declaration.form.contains(FieldForm::IsAccessor), fieldIsAccessor },
    };
    uint16_t result = 0;
    for (auto [holds, flag] : flags)
        result |= holds ? flag : 0;
    return result;
}

static std::optional<uint32_t> fieldOperandOf(TableEncoder& encoder, const PlacedSlot& slot)
{
    switch (traitsOf(slot.representation).valueKind) {
    case SlotValueKind::DeferredClosure:
        return indexOrNone(encoder.findEntrypoint(slot.deferredFunction));
    case SlotValueKind::SignedInteger:
    case SlotValueKind::UnsignedInteger:
    case SlotValueKind::Float:
    case SlotValueKind::Double:
    case SlotValueKind::Boolean:
    case SlotValueKind::Boxed:
    case SlotValueKind::Cell:
    case SlotValueKind::LiteralString:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<FieldData> collectFields(TableEncoder& encoder, LayoutID id, Vector<uint32_t>& operands, LayoutData& layoutData)
{
    const Program& program = encoder.program;
    const PlannedLayout& planned = encoder.slots.layout(id);
    RELEASE_ASSERT(planned.numberOfRecordFields <= maxFieldsInOrder);
    Vector<FieldData> fields(FillWith { }, planned.numberOfRecordFields, FieldData { });
    layoutData.hasOnlyPlainSlots = true;
    layoutData.isCopiedByWords = true;
    for (FieldIndex index = 0; index < planned.numberOfFields(); ++index) {
        const PlacedSlot& slot = planned.slots[index];
        if (!slot.indexInLayoutRecord)
            continue;
        FieldReference reference { id, index };
        const Field& declaration = fieldAt(program, declarationOf(program, reference));

        FieldData& data = fields[*slot.indexInLayoutRecord];
        data.name = declaration.key ? encoder.identifierNumber(declaration.key) : 0;
        data.isFilledAtBirth = isFilledAtBirth(program, reference);
        data.holdsRawValueFromBirth = holdsRawValueFromBirth(program, reference);
        data.keepsStateOfBirth = keepsStateOfBirth(program, reference);
        data.hasGuardOfStores = declaration.key && program.at(id).kind == LayoutKind::Instance && !!encoder.placement.guardBits().find(declaredLayoutOf(program, id), ClassSide::Prototype, MemberGuardKind::PassedByStores, declaration.key);
        data.flags = fieldFlagsOf(encoder, reference);
        data.slot = slot.data();
        data.type = encoder.typeOffset(recordedTypeOf(program, declaration));
        if (std::optional<uint32_t> operand = fieldOperandOf(encoder, slot)) {
            data.operandIndex = static_cast<uint16_t>(operands.size());
            operands.append(*operand);
        }
        if (slot.deferredClosureCapturesEnvironment) {
            std::optional<uint32_t> environmentField = program.at(id).flow.closureEnvironmentField;
            RELEASE_ASSERT(environmentField);
            const PlacedSlot& environmentSlot = encoder.slots.slot(id, *environmentField);
            RELEASE_ASSERT(environmentSlot.storage == SlotStorage::Inline && environmentSlot.representation == SlotRepresentation::Cell32);
            operands.append(environmentSlot.data().encoded());
        }
        bool isCopiedByWords = !(data.flags & (fieldIsPrivate | fieldIsBrand | fieldAttributeFlags)) && data.slot.representation() != SlotRepresentation::DeferredClosure;
        layoutData.isCopiedByWords &= isCopiedByWords;
        layoutData.hasOnlyPlainSlots &= isCopiedByWords && !(data.flags & fieldIsStoredOnce);
        layoutData.hasFieldsWithSymbolKeys |= declaration.key && isSymbolKind(program.at(declaration.key).kind);
        layoutData.hasFieldThatKeepsStateOfBirth |= data.keepsStateOfBirth;
    }
    return fields;
}

static uint32_t appendReferences(TableEncoder& encoder, LayoutID id)
{
    enum { WideValues, WideCells, Narrow, WideOutOfLine, NarrowOutOfLine, NumberOfLists };
    static_assert(numberOfInlineReferenceCounts == WideOutOfLine && numberOfExtensionHeaderWords == 7);
    const PlannedLayout& planned = encoder.slots.layout(id);
    Vector<uint32_t> offsets[NumberOfLists];
    for (FieldIndex row = 0; row < planned.slots.size(); ++row) {
        const PlacedSlot& slot = planned.slots[row];
        SlotTraits traits = traitsOf(slot.representation);
        if (row == planned.extensionWordIndex() || !slot.hasStorage() || !traits.holdsCell)
            continue;
        if (slot.isOutOfLine())
            offsets[traits.isNarrowReference ? NarrowOutOfLine : WideOutOfLine].append(slot.offset);
        else
            offsets[traits.isNarrowReference ? Narrow : traits.valueKind == SlotValueKind::Cell ? WideCells : WideValues].append(slot.offset);
    }

    // In front of the counts, read backwards from there: how many inner objects, their offsets, how many allocations make such objects, their numbers.
    Vector<uint32_t> references;
    if (!planned.innerObjects.isEmpty()) {
        for (const PlacedInnerObject& inner : planned.innerObjects) {
            if (!inner.layout)
                continue;
            for (AllocationID allocation : encoder.placement.allocations().ofLayout(inner.layout))
                references.append(allocation.index());
        }
        references.append(references.size());
        for (const PlacedInnerObject& inner : planned.innerObjects)
            references.append(inner.offset);
        references.append(planned.innerObjects.size());
    }
    uint32_t wordsInFront = references.size();
    for (unsigned list = 0; list < numberOfInlineReferenceCounts; ++list)
        references.append(offsets[list].size());
    for (unsigned list = 0; list < numberOfInlineReferenceCounts; ++list) {
        AOT::sort(offsets[list]);
        references.appendVector(offsets[list]);
    }
    if (planned.hasExtension()) {
        ExtensionReferences sets { };
        for (uint32_t offset : offsets[WideOutOfLine]) {
            RELEASE_ASSERT(!offsetInGranule(offset));
            sets.wide |= granuleBitOf(offset);
        }
        for (uint32_t offset : offsets[NarrowOutOfLine])
            (offsetInGranule(offset) ? sets.narrowInSecondHalf : sets.narrowInFirstHalf) |= granuleBitOf(offset);
        references.append(encoder.slots.extensionWord(id).data().encoded());
        for (uint64_t set : { sets.wide, sets.narrowInFirstHalf, sets.narrowInSecondHalf })
            references.appendList({ static_cast<uint32_t>(set), static_cast<uint32_t>(set >> 32) });
    }
    return references.size() > numberOfInlineReferenceCounts ? encoder.writer.append(references) + wordsInFront * sizeof(uint32_t) : 0;
}

static int32_t distanceFromRecord(uint32_t recordOffset, uint32_t targetOffset)
{
    return targetOffset ? static_cast<int32_t>(targetOffset) - static_cast<int32_t>(recordOffset) : 0;
}

static uint32_t appendKeyOrders(TableEncoder& encoder, LayoutID id)
{
    Vector<uint32_t> orders;
    for (const Vector<FieldIndex>& order : encoder.placement.keyOrders().of(id)) {
        Vector<uint16_t> list { static_cast<uint16_t>(order.size()) };
        for (FieldIndex field : order) {
            if (std::optional<uint16_t> indexInLayoutRecord = encoder.slots.slot(id, field).indexInLayoutRecord)
                list.append(*indexInLayoutRecord);
        }
        if (list.size() == order.size() + 1)
            orders.append(encoder.writer.append(list));
        else
            encoder.fail(ProgramTableRule::TableOutOfBounds, id);
    }
    return orders.isEmpty() ? 0 : encoder.writer.append(orders);
}

static uint32_t appendEnvironment(TableEncoder& encoder, LayoutID id, uint32_t shape)
{
    const Layout& layout = encoder.program.at(id);
    const PlannedLayout& planned = encoder.slots.layout(id);
    RELEASE_ASSERT(layout.isEnvironment());
    Vector<VariableData> described;
    for (FieldIndex index = 0; index < planned.numberOfFields(); ++index) {
        const PlacedSlot& slot = encoder.slots.slot(id, index);
        SlotTraits traits = traitsOf(slot.representation);
        if (!slot.hasStorage() || !(traits.holdsCell || (traits.hasAbsent && traits.absentBits)))
            continue;
        if (slot.isOutOfLine())
            encoder.fail(ProgramTableRule::TableOutOfBounds, id);
        PropertyKeyID key = layout.fields[index].key;
        described.append({ key ? encoder.identifiers.numberOf(key).value_or(0) : 0, slot.data() });
    }
    uint32_t referencesOffset = appendReferences(encoder, id);

    uint32_t offset = encoder.writer.allocate(sizeof(EnvironmentData) + described.sizeInBytes(), alignof(EnvironmentData));
    auto& data = encoder.writer.at<EnvironmentData>(offset);
    data.kind = layout.kind;
    data.index = id.index();
    data.shape = shape;
    data.name = encoder.shownNameNumber(layout.name);
    data.size = planned.size;
    data.numberOfDescribedVariables = static_cast<uint32_t>(described.size());
    data.referencesOffset = distanceFromRecord(offset, referencesOffset);
    encoder.writer.write(offset + sizeof(EnvironmentData), described.span());
    return offset;
}

// The numbers of the sets, which is all that clearing the guard looks at, then the offset of the list of members of each: a set is set with the last of its members.
// A list says how many members the set has, then their numbers. The writer keeps one copy of equal lists, so every member names the same one.
static MemberGuardAggregates appendAggregatesOf(TableEncoder& encoder, GuardBitID guard)
{
    const GuardBitTable& table = encoder.placement.guardBits();
    std::span<const GuardBitID> sets = table.setsContaining(guard);
    if (sets.empty())
        return { 0, 0 };
    Vector<uint32_t, 8> words;
    for (GuardBitID set : sets) {
        const PlacedGuardBit& placed = encoder.placement.guardBit(set);
        words.append(numberInStaticsOf(placed) | (placed.holdsWhileClear ? MemberGuardAggregates::holdsWhileClear : 0));
    }
    for (GuardBitID set : sets) {
        Vector<uint32_t, 8> list { static_cast<uint32_t>(table.membersOf(set).size()) };
        for (GuardBitID member : table.membersOf(set)) {
            const PlacedGuardBit& placed = encoder.placement.guardBit(member);
            uint32_t number = numberInStaticsOf(placed);
            RELEASE_ASSERT(MemberGuardAggregates::numberInListEntry(number) == number);
            list.append(number | (isAwaitedBeforeClassExists(encoder.program, placed) ? MemberGuardAggregates::isAwaitedBeforeClassExists : 0) | (placed.holdsWhileClear ? MemberGuardAggregates::holdsWhileClear : 0));
        }
        words.append(encoder.writer.append(list));
    }
    return { encoder.writer.append(words), static_cast<uint32_t>(sets.size()) };
}

namespace {

struct ClassRecordParts {
    Vector<MethodData> methods;
    Vector<FieldData> staticFields;
    Vector<MemberGuardData> memberGuards;
    Vector<MemberGuardAggregates> memberGuardAggregates;
    ClassData data { };
};

} // anonymous namespace

static ClassRecordParts collectClassRecordParts(TableEncoder& encoder, LayoutID id)
{
    const Program& program = encoder.program;
    const Layout& layout = program.at(id);
    const ClassCreationPlan& creation = encoder.slots.classCreation(id);
    FunctionID classConstructor = encoder.findEntrypoint(layout.constructor);
    ClassRecordParts parts;
    ClassData& data = parts.data;

    if (classConstructor || layout.has(LayoutFlags::IsOfCreatedObject))
        parts.methods.reserveInitialCapacity(layout.classFlow.members.size());
    for (ClassSide side : { ClassSide::Prototype, ClassSide::Static }) {
        for (size_t index = 0; index < layout.classFlow.members.size(); ++index) {
            const ClassMember& member = layout.classFlow.members[index];
            if (member.side != side || !(classConstructor || layout.has(LayoutFlags::IsOfCreatedObject)))
                continue;
            MemberCreation memberCreation = index < creation.members.size() ? creation.members[index] : MemberCreation::AtDefinition;
            MethodData method { };
            method.name = member.key ? encoder.identifierNumber(member.key) : 0;
            method.kindBits = static_cast<uint32_t>(member.kind);
            method.flags = (member.key ? 0 : methodHasComputedName) | (program.at(member.function).captures.isEmpty() ? 0 : methodTakesEnvironment) | (memberCreation == MemberCreation::AtDefinition ? methodIsCreatedAtDefinition : 0);
            method.function = memberCreation == MemberCreation::ByClassRecord ? encoder.entrypointOf(member.function) : indexOrNone(encoder.findEntrypoint(member.function));
            parts.methods.append(method);
            ++(side == ClassSide::Static ? data.numberOfStaticMethods : data.numberOfMethods);
        }
    }

    for (const StaticField& field : layout.staticFields) {
        if (!field.key || encoder.placement.variable(field.variable).storage == VariableStorage::None)
            continue;
        parts.staticFields.append(FieldData {
            .name = encoder.identifierNumber(field.key),
            .flags = 0,
            .operandIndex = 0,
            .slot = placedVariableSlot(encoder, field.variable),
            .type = encoder.typeOffset(TypeTable::valuesOf(program.provenTypeOf(program.at(field.variable).declared))),
        });
    }

    const GuardBitTable& table = encoder.placement.guardBits();
    Vector<std::pair<MemberGuardData, MemberGuardAggregates>> guards;
    for (const PlacedGuardBit& placed : table.ofClass(id)) {
        MemberGuardData guard { };
        guard.name = placed.key ? encoder.identifierNumber(placed.key) : 0;
        guard.kindBits = static_cast<uint32_t>(placed.kind);
        guard.sideBits = static_cast<uint32_t>(placed.side);
        guard.bit = placed.bit;
        guards.append({ guard, appendAggregatesOf(encoder, GuardBitID(static_cast<size_t>(&placed - table.bits.begin()))) });
        data.guardWordsOffset[static_cast<unsigned>(placed.side)] = placed.wordOffset - placed.bit / MemberGuardData::bitsPerWord * static_cast<uint32_t>(sizeof(uint64_t));
    }
    AOT::sort(guards, { }, [](const auto& entry) {
        const MemberGuardData& guard = entry.first;
        return std::tuple<uint32_t, uint32_t, uint32_t> { guard.sideBits, guard.kindBits, guard.name };
    });
    data.hasMemberGuardAggregates = std::ranges::any_of(guards, [](const auto& entry) {
        return entry.second.count;
    });
    for (const auto& [guard, aggregates] : guards) {
        parts.memberGuards.append(guard);
        if (data.hasMemberGuardAggregates)
            parts.memberGuardAggregates.append(aggregates);
    }

    LayoutID environment;
    if (layout.constructor) {
        const Vector<DeclaredType>& captures = program.at(layout.constructor).captures;
        if (captures.size() == 1 && captures[0].type)
            environment = program.at(captures[0].type).environment;
    }
    static_assert(LayoutData::noBase == UINT32_MAX);
    bool hasProvenBase = layout.flow.baseState == BaseState::Layout;
    data.base = hasProvenBase ? expectedBaseOf(layout).index() : LayoutData::noBase;
    data.expectedBase = hasProvenBase ? LayoutData::noBase : indexOrNone(expectedBaseOf(layout));
    data.builtinBase = indexOrNone(builtinParentNamedByClassRecord(program, layout));
    data.classConstructor = indexOrNone(classConstructor);
    data.environment = indexOrNone(environment);
    data.numberOfStaticFields = static_cast<uint32_t>(parts.staticFields.size());
    data.numberOfMemberGuards = static_cast<uint32_t>(parts.memberGuards.size());
    data.guardsHoldWhileClear = creation.guardsHoldWhileClear;
    if (const Vector<uint32_t>& subclasses = encoder.subclasses[id.index()]; !subclasses.isEmpty()) {
        Vector<uint32_t, 8> list { static_cast<uint32_t>(subclasses.size()) };
        list.appendVector(subclasses);
        data.subclassesOffset = encoder.writer.append(list);
    }
    return parts;
}

static bool isUntyped(const Program& program, const Layout& layout)
{
    switch (layout.kind) {
    case LayoutKind::Instance:
        if (layout.isDerived())
            return isUntyped(program, program.at(layout.flow.base));
        return layout.constructor && !program.at(layout.constructor).flags.contains(FunctionFlags::IsChecked);
    case LayoutKind::Record:
        return (!layout.name || program.string(layout.name).empty()) && std::ranges::all_of(layout.fields, [&](const Field& field) {
            return recordedTypeOf(program, field) == TypeTable::anything;
        });
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint32_t appendObjectLayout(TableEncoder& encoder, LayoutID id, uint32_t shapeNumber)
{
    const Program& program = encoder.program;
    const Layout& layout = program.at(id);
    const PlannedLayout& planned = encoder.slots.layout(id);
    const ClassCreationPlan& creation = encoder.slots.classCreation(id);
    RELEASE_ASSERT(isObjectLayout(layout.kind));
    PlacedShape shape = shapeNumber ? encoder.placement.shapes().shape(shapeNumber) : PlacedShape { };
    bool isClass = layout.kind == LayoutKind::Instance;

    LayoutData data { };
    Vector<uint32_t> fieldOperands;
    Vector<FieldData> fields = collectFields(encoder, id, fieldOperands, data);
    if (!shape.keysAreNeverEnumerated) {
        data.numberOfOrders = static_cast<uint32_t>(encoder.placement.keyOrders().of(id).size());
        data.ordersOffset = appendKeyOrders(encoder, id);
    }
    uint32_t referencesOffset = appendReferences(encoder, id);
    ClassRecordParts classParts;
    if (isClass) {
        classParts = collectClassRecordParts(encoder, id);
        const Layout& classLayout = layout.isDerived() ? program.at(declaredLayoutOf(program, layout.flow.base)) : layout;
        data.evaluationBits = static_cast<uint32_t>(isEvaluatedOnce(program, classLayout) ? EvaluationState::ProvenOnce : EvaluationState::PredictedOnce);
    }
    data.kindBits = static_cast<uint32_t>(layout.kind);
    data.isDerived = layout.isDerived();
    data.impossibleMutationsRaw = shape.impossibleMutations.toRaw();
    data.hasInnerObjects = !planned.innerObjects.isEmpty();
    data.holdsInnerObjectsInPlace = std::ranges::any_of(planned.innerObjects, &PlacedInnerObject::isAlwaysInPlace);
    data.mayBeInnerObject = planned.mayBeInnerObject;
    data.keysAreNeverEnumerated = shape.keysAreNeverEnumerated;
    data.isUntyped = isUntyped(program, layout);
    data.hasUntypedBase = layout.has(LayoutFlags::HasHeritage) && layout.flow.baseState != BaseState::Layout;
    data.createsMembersLazily = creation.createsMembersLazily;
    data.createsPrototypeBeforeClassObject = creation.createsPrototypeBeforeClassObject;
    data.createsClassObjectOnMutation = creation.createsClassObjectOnMutation;
    data.hasExtensionData = planned.hasExtension();
    data.hasExtraPropertiesSlot = planned.hasExtraPropertiesWord();
    data.index = id.index();
    data.shape = shapeNumber;
    data.name = encoder.shownNameNumber(layout.name);
    data.size = planned.size;
    data.numberOfFields = static_cast<uint16_t>(fields.size());
    data.numberOfFieldOperands = static_cast<uint16_t>(fieldOperands.size());

    size_t prefixSize = LayoutData::prefixSize(isClass, data.hasExtensionData, data.hasExtraPropertiesSlot);
    uint64_t trailingSize = LayoutData::trailingSize({
        .fields = fields.size(),
        .fieldOperands = fieldOperands.size(),
        .methods = classParts.methods.size(),
        .staticFields = classParts.staticFields.size(),
        .memberGuards = classParts.memberGuards.size(),
        .memberGuardAggregates = classParts.memberGuardAggregates.size(),
    });
    TableWriter& writer = encoder.writer;
    uint32_t offset = writer.allocate(prefixSize + sizeof(LayoutData) + trailingSize, alignof(LayoutData)) + prefixSize;
    data.referencesOffset = distanceFromRecord(offset, referencesOffset);
    auto& record = writer.at<LayoutData>(offset) = data;
    if (isClass)
        record.classData() = classParts.data;
    if (data.hasExtensionData) {
        uint64_t granules = 0;
        for (const PlacedSlot& slot : planned.slots.span().first(planned.numberOfFields())) {
            if (slot.isOutOfLine() && slot.isFilledByConstructor)
                granules |= granuleBitOf(slot.offset);
        }
        record.extensionData() = ExtensionLayoutData {
            .extension = encoder.slots.extensionWord(id).data(),
            .extensionSize = planned.extensionSize,
            .granulesFilledByConstructorLow = static_cast<uint32_t>(granules),
            .granulesFilledByConstructorHigh = static_cast<uint32_t>(granules >> 32),
        };
    }
    if (data.hasExtraPropertiesSlot)
        record.slotOfExtraProperties() = encoder.slots.extraPropertiesWord(id).data();

    uint32_t end = writer.write(offset + sizeof(LayoutData), fields.span());
    end = writer.write(end, fieldOperands.span());
    end = writer.write(end, classParts.methods.span());
    end = writer.write(end, classParts.staticFields.span());
    end = writer.write(end, classParts.memberGuards.span());
    end = writer.write(end, classParts.memberGuardAggregates.span());
    RELEASE_ASSERT(end == offset + sizeof(LayoutData) + trailingSize);
    return offset;
}

static void encodeLayoutRecords(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    const ShapeTable& shapes = encoder.placement.shapes();
    auto isClassInUse = [&](uint32_t index) {
        return program.layouts[index].kind == LayoutKind::Instance && encoder.placement.isInUse(LayoutID(index));
    };
    encoder.subclasses = Vector<Vector<uint32_t>>(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID assumedBase = isClassInUse(index) ? expectedBaseOf(program.layouts[index]) : LayoutID();
        if (assumedBase && program.at(assumedBase).kind == LayoutKind::Instance)
            encoder.subclasses[assumedBase.index()].append(index);
    }

    Vector<uint32_t> layouts(FillWith { }, shapes.numberOfShapes() + 1, 0);
    for (uint32_t shape = 1; shape <= shapes.numberOfShapes(); ++shape) {
        if (!shapes.isAssigned(shape) || !encoder.placement.isInUse(shapes.shape(shape).layout))
            continue;
        LayoutID id = shapes.shape(shape).layout;
        layouts[shape] = shape < shapes.firstEnvironmentShape() ? appendObjectLayout(encoder, id, shape) : appendEnvironment(encoder, id, shape);
    }

    Vector<uint32_t> classes(FillWith { }, program.layouts.size(), 0);
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        if (isClassInUse(index))
            classes[index] = shapes.shapesOf(id).isEmpty() ? appendObjectLayout(encoder, id, 0) : layouts[shapes.generalShapeOf(id)];
    }
    encoder.build.statistics().count(StatisticID::LayoutRecordsEmitted, std::ranges::count_if(layouts, std::identity { }));
    encoder.setTable(ProgramTableIndex::layouts, layouts);

    encoder.header.numberOfLayoutIndices = static_cast<uint32_t>(classes.size());
    Vector<uint32_t> classGroups;
    appendGroupsAndOffsets(classGroups, (classes.size() + 31) / 32 * 2, classes);
    encoder.setTable(ProgramTableIndex::classes, classGroups);
}

static bool leavesFieldOfBirthEmpty(const Program& program, const PlacedAllocation& allocation)
{
    if (!isObjectLayout(program.at(allocation.layout).kind))
        return false;
    for (FieldIndex field = 0; field < program.at(allocation.layout).fields.size(); ++field) {
        if (isFilledAtBirth(program, { allocation.layout, field }) && !allocation.initialFields.contains(field))
            return true;
    }
    return false;
}

static void encodeAllocations(TableEncoder& encoder)
{
    Vector<AllocationData> allocations;
    Vector<ConstructedAllocationData> constructedAllocations;
    allocations.reserveInitialCapacity(encoder.placement.numberOfAllocations());
    for (const PlacedAllocation& allocation : encoder.placement.allocations().allocations) {
        if (allocation.constructedBy)
            constructedAllocations.append({ Number24(allocations.size()), Number24(encoder.entrypointOf(allocation.constructedBy)) });
        allocations.append(AllocationData::create(allocation.shape, allocation.orderWord, allocation.size, leavesFieldOfBirthEmpty(encoder.program, allocation), !!allocation.constructedBy));
    }
    encoder.setTable(ProgramTableIndex::allocations, allocations);
    encoder.setTable(ProgramTableIndex::constructedAllocations, constructedAllocations);
}

static void encodeShapeSetBits(TableEncoder& encoder)
{
    // Not shared with equal bytes further in front: the image builder takes the table off the end.
    std::span<const uint64_t> words = encoder.placement.shapes().shapeSetWords();
    encoder.setTableRange(ProgramTableIndex::shapeSetBits, encoder.writer.appendUnshared(asBytes(words), alignof(uint64_t)), static_cast<uint32_t>(words.size()));
}

template<typename T>
static void setSortedTable(TableEncoder& encoder, ProgramTableIndex index, Vector<T>& items, auto... order)
{
    AOT::sort(items, order...);
    encoder.setTable(index, items);
}

static void encodeRemovedFields(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<RemovedFieldData> removedFields;
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        if (program.at(id).isEnvironment())
            continue;
        bool everyObjectHadRemovedField = false;
        for (FieldIndex field = 0; field < program.at(id).fields.size(); ++field) {
            const Field& declaration = fieldAt(program, declarationOf(program, { id, field }));
            if (!declaration.flags.contains(FieldFlags::IsRemovedAsUnread) || !declaration.key)
                continue;
            everyObjectHadRemovedField |= declaration.flow.occupancy == Occupancy::AlwaysFilled;
            if (std::optional<uint32_t> name = encoder.identifiers.numberOf(declaration.key))
                removedFields.append({ id.index(), *name });
        }
        if (everyObjectHadRemovedField)
            removedFields.append({ id.index(), RemovedFieldData::everyObjectHadOne });
    }
    setSortedTable(encoder, ProgramTableIndex::removedFields, removedFields);
}

static void encodeAddedNames(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<AddedNameData> addedNames;
    auto haveNumbers = [&](const Vector<PropertyKeyID>& keys) {
        return std::ranges::all_of(keys, [&](PropertyKeyID key) {
            return encoder.identifiers.numberOf(key) && !encoder.flow.standsForSeveralSymbols(key);
        });
    };
    const Vector<PropertyKeyID>* sharedKeys = program.keysStoredThroughUnknownReceivers();
    bool canListSharedKeys = sharedKeys && haveNumbers(*sharedKeys);
    bool listsSharedKeys = false;
    for (unsigned index = 0; encoder.build.validates() && index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const LayoutFlow& flow = program.at(id).flow;
        if (!flow.addedKeysAreComplete || encoder.slots.storageOf(id) != LayoutStorage::CompiledObject)
            continue;
        if (!haveNumbers(flow.addedKeys) || (flow.isReachedThroughUnknownReceivers && !canListSharedKeys))
            continue;
        for (PropertyKeyID key : flow.addedKeys)
            addedNames.append({ id.index(), *encoder.identifiers.numberOf(key) });
        if (flow.isReachedThroughUnknownReceivers) {
            addedNames.append({ id.index(), AddedNameData::getsSharedNames });
            listsSharedKeys = true;
        }
        addedNames.append({ id.index(), AddedNameData::listIsComplete });
    }
    if (listsSharedKeys) {
        for (PropertyKeyID key : *sharedKeys)
            addedNames.append({ AddedNameData::layoutOfSharedNames, *encoder.identifiers.numberOf(key) });
    }
    setSortedTable(encoder, ProgramTableIndex::addedNames, addedNames);
}

static void encodeClassPrototypeNames(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<AddedNameData> names;
    for (unsigned index = 0; encoder.build.validates() && index < program.layouts.size(); ++index) {
        const ClassFlow& flow = program.layouts[index].classFlow;
        if (!flow.prototypeNamesAreComplete)
            continue;
        size_t sizeBefore = names.size();
        bool everyNameHasNumber = true;
        for (const Vector<ClassMember>* members : { &flow.members, &flow.namedMembers }) {
            for (const ClassMember& member : *members) {
                if (member.side != ClassSide::Prototype || !member.key)
                    continue;
                std::optional<uint32_t> name = encoder.identifiers.numberOf(member.key);
                everyNameHasNumber &= name && program.at(member.key).kind != PropertyKeyKind::CreatedSymbol;
                names.append({ index, name.value_or(0) });
            }
        }
        if (everyNameHasNumber)
            names.append({ index, AddedNameData::listIsComplete });
        else
            names.shrink(sizeBefore);
    }
    setSortedTable(encoder, ProgramTableIndex::classPrototypeNames, names);
}

static bool appendMembersForValidators(TableEncoder& encoder, Vector<ParsedRecordFieldData>& rows)
{
    Vector<Vector<uint32_t, 8>> validatedObjects;
    for (const ExpectedResult& expected : encoder.program.expectedResults) {
        for (const ValidationNode& node : expected.validation) {
            if (node.shape.kind() == ValidationShape::Kind::AnyObject || node.shape.kind() == ValidationShape::Kind::Dictionary)
                return false;
            if (node.shape.kind() != ValidationShape::Kind::Object)
                continue;
            Vector<uint32_t, 8> names;
            bool canBeMet = true;
            for (const ValidationMember& member : node.shape.as<ValidationShape::Object>().members) {
                if (member.isOptional)
                    continue;
                std::optional<uint32_t> name = encoder.identifiers.numberOf(member.key);
                if (!name) {
                    canBeMet = false;
                    break;
                }
                names.appendIfNotContains(*name);
            }
            if (!canBeMet)
                continue;
            if (names.isEmpty() || names.size() > ParsedRecordFieldData::maxRequiredMembers)
                return false;
            AOT::sort(names);
            validatedObjects.appendIfNotContains(WTF::move(names));
        }
    }
    if (validatedObjects.size() > ParsedRecordFieldData::maxValidatedObjects)
        return false;
    for (uint32_t index = 0; index < validatedObjects.size(); ++index) {
        for (uint32_t name : validatedObjects[index])
            rows.append({ name, ParsedRecordFieldData::memberForValidator(index, validatedObjects[index].size()) });
    }
    return true;
}

static void encodeParsedRecordFields(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    const ShapeTable& shapes = encoder.placement.shapes();
    Vector<ParsedRecordFieldData> rows;
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Record || !layout.flow.observedUses.contains(ObservedUse::ReachesTypedParse) || shapes.shapesOf(id).isEmpty())
            continue;
        uint32_t shape = shapes.generalShapeOf(id);
        const PlacedShape& placed = shapes.shape(shape);
        if (placed.keysAreNeverEnumerated || !placed.impossibleMutations.isEmpty())
            continue;
        size_t sizeBefore = rows.size();
        for (FieldIndex fieldIndex = 0; fieldIndex < layout.fields.size(); ++fieldIndex) {
            const Field& field = layout.fields[fieldIndex];
            if (!encoder.slots.slot(id, fieldIndex).indexInLayoutRecord)
                continue;
            std::optional<uint32_t> name = field.key ? encoder.identifiers.numberOf(field.key) : std::nullopt;
            if (!name) {
                rows.shrink(sizeBefore);
                break;
            }
            rows.append({ *name, shape });
        }
    }
    if (!rows.isEmpty() && !appendMembersForValidators(encoder, rows))
        rows.clear();
    setSortedTable(encoder, ProgramTableIndex::parsedRecordFields, rows);
}

static void encodeConstructors(TableEncoder& encoder)
{
    Vector<ConstructorData> constructors;
    for (unsigned index = 0; index < encoder.program.layouts.size(); ++index) {
        if (!encoder.placement.isInUse(LayoutID(index)))
            continue;
        if (FunctionID entrypoint = encoder.findEntrypoint(encoder.program.layouts[index].constructor))
            constructors.append({ entrypoint.index(), index });
    }
    setSortedTable(encoder, ProgramTableIndex::constructors, constructors, [](const ConstructorData& a, const ConstructorData& b) {
        return std::tie(a.function, a.layoutIndex) < std::tie(b.function, b.layoutIndex);
    });
}

static void encodeBoundFunctions(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<BoundFunctionData> boundFunctions;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (function.role != FunctionRole::BoundFunction)
            continue;
        FunctionID entrypoint = encoder.findEntrypoint(id);
        if (!entrypoint || !program.at(entrypoint).hasBody())
            continue;
        const auto& record = function.creator.as<CreatorRecord::BoundFunction>();
        if (record.numberOfBoundArguments > UINT16_MAX)
            encoder.fail(ProgramTableRule::TableOutOfBounds, { }, id);
        BoundFunctionData data { };
        data.function = entrypoint.index();
        data.target = encoder.entrypointOf(record.target);
        data.numberOfBoundArguments = record.numberOfBoundArguments;
        if (record.capturesTargetClosure) {
            if (!function.captures.isEmpty() && declaredRepresentationOf(program, function.captures.last()) == Representation::Cell)
                data.targetClosureSlotPlusOne = closureCaptureSlotOf(program, function, static_cast<unsigned>(function.captures.size() - 1)) + 1;
            else
                encoder.fail(ProgramTableRule::TableOutOfBounds, { }, id);
        }
        const Function& target = program.at(record.target);
        if (target.constructorForm() != ConstructorForm::NotConstructor) {
            unsigned firstArgument = static_cast<unsigned>(target.captures.size()) + target.has(FunctionFlags::HasThis);
            data.constructs = true;
            data.numberOfCapturedArguments = record.numberOfCapturedArguments;
            data.firstArgumentSlot = record.numberOfCapturedArguments ? closureCaptureSlotOf(program, function, firstArgument) : 0;
            bool isAsRuntimeExpects = !!data.targetClosureSlotPlusOne;
            for (unsigned argument = 0; argument < record.numberOfCapturedArguments; ++argument)
                isAsRuntimeExpects &= holdsCells(declaredRepresentationOf(program, function.captures[firstArgument + argument])) && closureCaptureSlotOf(program, function, firstArgument + argument) == data.firstArgumentSlot + argument;
            if (!isAsRuntimeExpects)
                encoder.fail(ProgramTableRule::TableOutOfBounds, { }, id);
        }
        boundFunctions.append(data);
    }
    setSortedTable(encoder, ProgramTableIndex::boundFunctions, boundFunctions, std::ranges::less { }, &BoundFunctionData::function);
}

void encodeLayoutTables(TableEncoder& encoder)
{
    encodeLayoutRecords(encoder);
    encodeRemovedFields(encoder);
    encodeAddedNames(encoder);
    encodeClassPrototypeNames(encoder);
    encodeParsedRecordFields(encoder);
    encodeAllocations(encoder);
    encodeConstructors(encoder);
    encodeBoundFunctions(encoder);
}

namespace {

struct NamedField {
    friend auto operator<=>(const NamedField&, const NamedField&) = default;

    uint32_t name;
    uint32_t entry;
    uint16_t field;
};

struct FieldTableRow {
    uint32_t size() const { return end - begin; }

    uint32_t shape;
    uint32_t rankOfClass;
    uint32_t begin;
    uint32_t end;
};

struct NamedFields {
    std::span<const NamedField> fieldsOf(const FieldTableRow& row) const { return fields.span().subspan(row.begin, row.size()); }
    void endRow(uint32_t shape, uint32_t rankOfClass, size_t begin)
    {
        if (begin != fields.size())
            rows.append({ shape, rankOfClass, static_cast<uint32_t>(begin), static_cast<uint32_t>(fields.size()) });
    }

    Vector<NamedField> fields;
    Vector<FieldTableRow> rows;
};

class Bitmap {
public:
    static constexpr uint32_t bitsPerBatch = 64;

    uint64_t clearBitsFrom(uint32_t index) const
    {
        size_t word = index / 64;
        unsigned shift = index % 64;
        uint64_t bits = wordAt(word) >> shift;
        if (shift)
            bits |= wordAt(word + 1) << (64 - shift);
        return ~bits;
    }

    void set(uint32_t index)
    {
        size_t word = index / 64;
        if (word >= m_words.size())
            m_words.insertFill(m_words.size(), 0, std::max(word + 1, m_words.size() * 2) - m_words.size());
        m_words[word] |= 1ull << index % 64;
    }

    uint32_t firstBitThatMayBeClear()
    {
        while (m_firstWordWithClearBit < m_words.size() && !~m_words[m_firstWordWithClearBit])
            ++m_firstWordWithClearBit;
        return static_cast<uint32_t>(m_firstWordWithClearBit * 64);
    }

private:
    uint64_t wordAt(size_t word) const { return word < m_words.size() ? m_words[word] : 0; }

    Vector<uint64_t> m_words;
    size_t m_firstWordWithClearBit { 0 };
};

} // anonymous namespace

static uint32_t numberOfFieldTableColumns(const TableEncoder& encoder)
{
    return static_cast<uint32_t>(std::min<uint64_t>(encoder.identifiers.count(), FieldTableEntry::maxName + 1));
}

static std::optional<uint32_t> findNumberOfConstructorName(const TableEncoder& encoder)
{
    for (uint32_t number = 1; number < numberOfFieldTableColumns(encoder); ++number) {
        PropertyKeyID key = encoder.identifiers.at(number).key;
        if (key && encoder.program.nameClassOf(key) == NameClass::Constructor)
            return number;
    }
    return std::nullopt;
}

static std::optional<Vector<DeclaredClassMember>> collectDeclaredClassMembers(const TableEncoder& encoder, const LayoutData& classLayout, std::optional<uint32_t> nameOfConstructor)
{
    Vector<DeclaredClassMember> members;
    auto indexOfMemberNamed = [&](uint32_t name) {
        return members.findIf([&](const DeclaredClassMember& member) {
            return member.name == name;
        });
    };
    auto cellOf = [&](uint32_t function) -> uint32_t {
        uint32_t cell = encoder.placement.closureCellNumber(FunctionID(function));
        return cell <= ClassRowEntry::maxNumber ? cell : 0;
    };

    if (!classLayout.classData().classConstructor)
        return std::nullopt;
    // ClassTable::DeclaredMember::define(): a later definition of a name replaces an earlier one, and the halves of an accessor add up.
    for (const MethodData& method : classLayout.methods()) {
        uint32_t cell = cellOf(method.function);
        if (method.flags & (methodHasComputedName | methodIsCreatedAtDefinition) || !cell)
            return std::nullopt;
        if (!FieldTableEntry::hasColumn(method.name))
            continue;
        if (indexOfMemberNamed(method.name) == notFound)
            members.append(DeclaredClassMember { method.name });
        DeclaredClassMember& member = members[indexOfMemberNamed(method.name)];
        member.method = 0;
        switch (method.kind()) {
        case MethodKind::Method:
            member = DeclaredClassMember { method.name, cell };
            break;
        case MethodKind::Getter:
            member.getter = cell;
            break;
        case MethodKind::Setter:
            member.setter = cell;
            break;
        }
    }
    if (nameOfConstructor && indexOfMemberNamed(*nameOfConstructor) == notFound) {
        uint32_t cell = cellOf(classLayout.classData().classConstructor);
        if (!cell)
            return std::nullopt;
        members.append(DeclaredClassMember { *nameOfConstructor, cell });
    }
    AOT::sort(members, { }, &DeclaredClassMember::name);
    return members;
}

std::optional<Vector<DeclaredClassMember>> collectDeclaredClassMembers(const TableEncoder& encoder, const LayoutData& classLayout)
{
    return collectDeclaredClassMembers(encoder, classLayout, findNumberOfConstructorName(encoder));
}

void encodeFieldTable(TableEncoder& encoder)
{
    TableWriter& writer = encoder.writer;
    const uint32_t highestObjectShape = encoder.placement.shapes().firstEnvironmentShape() - 1;
    const uint32_t typeOfAnyValue = encoder.typeOffset(TypeTable::anything);
    TableRange layouts = encoder.header.ranges[static_cast<unsigned>(ProgramTableIndex::layouts)];
    NamedFields named;
    for (uint32_t shape = 1; shape <= highestObjectShape; ++shape) {
        uint32_t offset = writer.at<uint32_t>(layouts.offset + shape * sizeof(uint32_t));
        if (!offset)
            continue;
        const LayoutData& layout = writer.at<LayoutData>(offset);
        size_t begin = named.fields.size();
        for (const FieldData& field : layout.fields()) {
            if (FieldTableEntry::hasColumn(field.name))
                named.fields.append({ field.name, FieldTableEntry::forField(layout, field).bits, FieldRecordIndex::create(field, layout.indexOf(field), writer.at<RuntimeType>(field.type), typeOfAnyValue).bits });
        }
        AOT::sort(named.fields.mutableSpan().subspan(begin));
        named.endRow(shape, 0, begin);
    }

    // A shape without fields has a row of its own, where no other row begins: what it finds there are the entries of other rows under other names.
    constexpr uint32_t rowWithoutFields = 0;
    const TableRange classes = encoder.header.ranges[static_cast<unsigned>(ProgramTableIndex::classes)];
    const uint32_t numberOfGroupWords = (encoder.header.numberOfLayoutIndices + 31) / 32 * 2;
    const std::optional<uint32_t> nameOfConstructor = findNumberOfConstructorName(encoder);
    Vector<uint32_t> firstEntryOfShape(FillWith { }, highestObjectShape + 1, rowWithoutFields);
    Vector<uint32_t> firstEntryOfClass(FillWith { }, classes.count - numberOfGroupWords, ClassRowEntry::noRow);
    Vector<uint64_t> accessors;
    for (uint32_t rank = 0; rank < firstEntryOfClass.size(); ++rank) {
        uint32_t offset = writer.at<uint32_t>(classes.offset + (numberOfGroupWords + rank) * sizeof(uint32_t));
        std::optional<Vector<DeclaredClassMember>> members = collectDeclaredClassMembers(encoder, writer.at<LayoutData>(offset), nameOfConstructor);
        if (!members)
            continue;
        firstEntryOfClass[rank] = rowWithoutFields;
        size_t begin = named.fields.size();
        for (const DeclaredClassMember& member : *members) {
            uint32_t number = member.method ? member.method : static_cast<uint32_t>(accessors.size());
            if (!member.method)
                accessors.append(std::bit_cast<uint64_t>(ClassAccessorData { member.getter, member.setter }));
            named.fields.append({ member.name, ClassRowEntry::create(member.name, member.method ? ClassRowEntry::Form::Method : ClassRowEntry::Form::Accessor, number).bits, ClassRowEntry::upperBitsOf(number) });
        }
        named.endRow(0, rank, begin);
    }

    AOT::sort(named.rows, [&](const FieldTableRow& a, const FieldTableRow& b) {
        if (a.size() != b.size())
            return a.size() > b.size();
        auto fieldsOfA = named.fieldsOf(a);
        auto fieldsOfB = named.fieldsOf(b);
        auto order = std::lexicographical_compare_three_way(fieldsOfA.begin(), fieldsOfA.end(), fieldsOfB.begin(), fieldsOfB.end());
        return order != 0 ? order < 0 : std::tie(a.shape, a.rankOfClass) < std::tie(b.shape, b.rankOfClass);
    });

    auto firstEntryOf = [&](const FieldTableRow& row) -> uint32_t& {
        return row.shape ? firstEntryOfShape[row.shape] : firstEntryOfClass[row.rankOfClass];
    };
    Bitmap occupiedEntries;
    Bitmap firstEntries;
    firstEntries.set(rowWithoutFields);
    uint32_t highestFirstEntry = rowWithoutFields;

    uint32_t entryOfFirstFieldOfLastRow = 0;
    const FieldTableRow* lastRow = nullptr;
    for (const FieldTableRow& row : named.rows) {
        auto fields = named.fieldsOf(row);
        if (lastRow && std::ranges::equal(fields, named.fieldsOf(*lastRow))) {
            firstEntryOf(row) = firstEntryOf(*lastRow);
            continue;
        }
        if (!lastRow || lastRow->size() != row.size() || row.size() == 1)
            entryOfFirstFieldOfLastRow = 0;
        uint32_t lowestEntryOfFirstField = std::max(occupiedEntries.firstBitThatMayBeClear(), entryOfFirstFieldOfLastRow);
        uint32_t firstEntry = lowestEntryOfFirstField > fields[0].name ? lowestEntryOfFirstField - fields[0].name : 0;
        for (;; firstEntry += Bitmap::bitsPerBatch) {
            uint64_t fitting = firstEntries.clearBitsFrom(firstEntry);
            for (size_t i = 0; i < fields.size() && fitting; ++i)
                fitting &= occupiedEntries.clearBitsFrom(firstEntry + fields[i].name);
            if (fitting) {
                firstEntry += WTF::ctz(fitting);
                break;
            }
        }
        firstEntries.set(firstEntry);
        for (const NamedField& field : fields)
            occupiedEntries.set(firstEntry + field.name);
        firstEntryOf(row) = firstEntry;
        highestFirstEntry = std::max(highestFirstEntry, firstEntry);
        entryOfFirstFieldOfLastRow = firstEntry + fields[0].name;
        lastRow = &row;
    }

    // A lookup tests no bounds: every row has an entry for every name that has a column.
    const uint32_t numberOfEntries = highestFirstEntry + numberOfFieldTableColumns(encoder);
    Vector<uint32_t> table(FillWith { }, highestObjectShape + numberOfEntries, 0);
    Vector<uint16_t> recordIndices(FillWith { }, numberOfEntries, 0);
    for (uint32_t shape = 1; shape <= highestObjectShape; ++shape)
        table[highestObjectShape - shape] = firstEntryOfShape[shape];
    for (const FieldTableRow& row : named.rows) {
        for (const NamedField& field : named.fieldsOf(row)) {
            uint32_t index = firstEntryOf(row) + field.name;
            table[highestObjectShape + index] = field.entry;
            recordIndices[index] = field.field;
        }
    }
    encoder.header.numberOfFieldTableRows = highestObjectShape;
    encoder.setTable(ProgramTableIndex::fieldTable, table);
    encoder.setTable(ProgramTableIndex::fieldRecordIndices, recordIndices);
    encoder.setTable(ProgramTableIndex::classMemberRows, firstEntryOfClass);
    encoder.setTable(ProgramTableIndex::classAccessors, accessors);
}

static void appendFunctionNames(TableEncoder& encoder, std::span<const FunctionTraceRecord> traces, size_t firstDerived)
{
    const Program& program = encoder.program;
    bool numbersFit = program.strings.size() <= Number24::max;
    if (!numbersFit)
        encoder.build.reportLimitExceeded(LimitID::MaxListedStrings, program.strings.size());
    BitVector isShown(traces.size());
    for (size_t index : encoder.inputs.bodyFacts.functionsWithFrames)
        isShown.quickSet(traces[index].sourceFunctionIndex);
    auto nameOf = [&](size_t index) {
        StringID name = isShown.quickGet(index) ? program.functions[index].name : StringID();
        std::optional<BuiltinID> builtin = traces[index].implements();
        if (builtin && name && program.builtins().at(*builtin).kind != BuiltinKind::Accessor) {
            BuiltinName builtinName = program.builtins().at(*builtin).name;
            if (StringID nameOfBuiltin = builtinName ? program.stringOf(builtinName) : StringID())
                name = nameOfBuiltin;
        }
        uint32_t number = encoder.shownNameNumber(name);
        return Number24(numbersFit ? number : 0);
    };
    Vector<Number24> names;
    Vector<uint32_t> groups = groupWordsFor(firstDerived, ProgramTables::numberOfFunctionNameGroupWords(firstDerived), [&](size_t index) {
        Number24 name = isShown.quickGet(index) ? nameOf(index) : Number24(0);
        if (name)
            names.append(name);
        return !!name;
    });
    encoder.setTable(ProgramTableIndex::functionNames, names);
    encoder.setTable(ProgramTableIndex::functionNameGroups, groups);

    Vector<FunctionNameRunData> runs;
    for (size_t index = firstDerived; index < traces.size(); ++index) {
        // ProgramTables::nameOfFunction() looks for the run in front of any function.
        if (traces[index].sourceFunctionIndex != index || (!isShown.quickGet(index) && !runs.isEmpty()))
            continue;
        uint32_t name = nameOf(index);
        if (runs.isEmpty() || runs.last().name != name)
            runs.append({ static_cast<uint32_t>(index), name });
    }
    encoder.setTable(ProgramTableIndex::functionNameRuns, runs);
}

NumberedFunctionRecords numberFunctionRecords(std::span<const FunctionRecordData> recordsOfFunctions)
{
    struct DistinctRecord {
        uint32_t bits;
        uint32_t count;
        uint32_t number;
    };
    Vector<uint32_t> sortedBits(recordsOfFunctions.size(), [&](size_t index) {
        return std::bit_cast<uint32_t>(recordsOfFunctions[index]);
    });
    AOT::sort(sortedBits);
    Vector<DistinctRecord> distinct;
    for (uint32_t bits : sortedBits) {
        if (distinct.isEmpty() || distinct.last().bits != bits)
            distinct.append({ bits, 0, 0 });
        ++distinct.last().count;
    }
    AOT::stableSort(distinct, std::ranges::greater { }, &DistinctRecord::count);
    for (uint32_t number = 0; number < distinct.size(); ++number)
        distinct[number].number = number;

    NumberedFunctionRecords result;
    result.records = Vector<FunctionRecordData>(std::min<size_t>(distinct.size(), functionRecordIsListedByFunction), [&](size_t number) {
        return std::bit_cast<FunctionRecordData>(distinct[number].bits);
    });
    AOT::sort(distinct, { }, &DistinctRecord::bits);
    result.numbers = Vector<uint8_t>(recordsOfFunctions.size(), [&](size_t index) {
        uint32_t number = std::ranges::lower_bound(distinct, std::bit_cast<uint32_t>(recordsOfFunctions[index]), { }, &DistinctRecord::bits)->number;
        if (number < functionRecordIsListedByFunction)
            return static_cast<uint8_t>(number);
        result.recordsByFunction.append({ static_cast<uint32_t>(index), recordsOfFunctions[index] });
        return functionRecordIsListedByFunction;
    });
    return result;
}

static uint32_t countClosureCells(const TableEncoder& encoder, ClosureFunctionRange range)
{
    uint32_t numberOfFunctions = encoder.program.functions.size();
    BitVector usedNumbers(numberOfFunctions + 1);
    uint32_t numberOfCells = 1;
    uint32_t maxNumber = 0;
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        FunctionID id(index);
        uint32_t number = encoder.placement.closureCellNumber(id);
        if (!number)
            continue;
        if (id < range.first || id >= range.end || number > numberOfFunctions || usedNumbers.quickSet(number)) {
            encoder.fail(ProgramTableRule::TableOutOfBounds, { }, id);
            return numberOfCells;
        }
        maxNumber = std::max(maxNumber, number);
        ++numberOfCells;
    }
    if (maxNumber + 1 != numberOfCells)
        encoder.fail(ProgramTableRule::TableOutOfBounds);
    return numberOfCells;
}

static void appendClosureFunctionTables(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    ClosureFunctionRange range = encoder.placement.closureFunctions();
    Vector<FunctionKindData> kinds;
    Vector<uint8_t> lengths;
    Vector<uint8_t> captureCounts;
    Vector<FunctionWithLargeCountsData> functionsWithLargeCounts;
    Vector<Number24> cellNumbers;
    size_t count = range.end.index() - range.first.index();
    kinds.reserveInitialCapacity(count);
    lengths.reserveInitialCapacity(count);
    captureCounts.reserveInitialCapacity(count);
    cellNumbers.reserveInitialCapacity(count);
    for (uint32_t index = range.first.index(); index < range.end.index(); ++index) {
        FunctionID function(index);
        const Function& target = program.at(closureTargetOf(program, function));
        uint32_t length = target.length;
        if (length > UINT16_MAX) {
            encoder.fail(ProgramTableRule::TableOutOfBounds, { }, function);
            length = 0;
        }
        CaptureCounts counts { 0, 0 };
        if (program.at(function).role == FunctionRole::GenericEntrypoint) {
            ClosureCaptureCounts countsOfTarget = closureCaptureCountsOf(program, target);
            if (countsOfTarget.captures <= UINT8_MAX)
                counts = { static_cast<uint8_t>(countsOfTarget.references), static_cast<uint8_t>(countsOfTarget.captures) };
            else
                encoder.fail(ProgramTableRule::TableOutOfBounds, { }, function);
        }
        kinds.append(encoder.placement.functionKind(function));
        lengths.append(length < countsAreListed ? static_cast<uint8_t>(length) : countsAreListed);
        captureCounts.append(counts.encoded());
        if (lengths.last() == countsAreListed || captureCounts.last() == countsAreListed)
            functionsWithLargeCounts.append({ Number24(index), counts, { static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8) } });
        cellNumbers.append(Number24(encoder.placement.closureCellNumber(function)));
    }
    encoder.setTable(ProgramTableIndex::closureFunctionKinds, kinds);
    encoder.setTable(ProgramTableIndex::functionLengths, lengths);
    encoder.setTable(ProgramTableIndex::closureCaptureCounts, captureCounts);
    encoder.setTable(ProgramTableIndex::functionsWithLargeCounts, functionsWithLargeCounts);
    encoder.setTable(ProgramTableIndex::closureCellNumbers, cellNumbers);
    encoder.header.numberOfClosureCells = countClosureCells(encoder, range);
}

static bool showsSourceText(FunctionRole role)
{
    switch (role) {
    case FunctionRole::Source:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
        return true;
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineBody:
    case FunctionRole::AsyncWorker:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWorker:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasFunctionText(const Program& program, const Function& function)
{
    if (!function.module || function.text.start >= function.text.end)
        return false;
    return showsSourceText(function.role) && !function.enclosingRuntimeFunction && program.at(function.module).kind != ModuleKind::RuntimeSource;
}

static void appendFunctionTexts(TableEncoder& encoder)
{
    struct RetainedTextRange {
        uint32_t start;
        uint32_t end;
        uint32_t textOffset { 0 };
    };
    struct FunctionWithText {
        FunctionID function;
        FunctionID entrypoint;
    };
    const Program& program = encoder.program;
    Vector<FunctionWithText> functions;
    Vector<Vector<RetainedTextRange>> moduleRanges(program.modules.size());
    for (size_t index = 0; !encoder.obfuscates() && index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!hasFunctionText(program, function))
            continue;
        FunctionID id(index);
        FunctionID entrypoint = encoder.findEntrypoint(id);
        if (!entrypoint)
            continue;
        if (function.text.end > program.at(function.module).functionText.size()) {
            encoder.fail(ProgramTableRule::TableOutOfBounds, { }, id);
            continue;
        }
        functions.append({ id, entrypoint });
        moduleRanges[function.module.index()].append({ function.text.start, function.text.end });
    }
    if (functions.isEmpty()) {
        encoder.setTable(ProgramTableIndex::functionTexts, Vector<FunctionTexts> { });
        return;
    }

    Vector<uint8_t> text;
    for (size_t module = 0; module < moduleRanges.size(); ++module) {
        Vector<RetainedTextRange>& ranges = moduleRanges[module];
        AOT::sort(ranges, [](const RetainedTextRange& a, const RetainedTextRange& b) {
            return a.start != b.start ? a.start < b.start : a.end > b.end;
        });
        size_t count = 0;
        for (const RetainedTextRange& next : ranges) {
            if (count && next.start < ranges[count - 1].end)
                ranges[count - 1].end = std::max(ranges[count - 1].end, next.end);
            else
                ranges[count++] = next;
        }
        ranges.shrink(count);
        for (RetainedTextRange& range : ranges) {
            range.textOffset = static_cast<uint32_t>(text.size());
            text.append(program.modules[module].functionText.subspan(range.start, range.end - range.start));
        }
    }

    Vector<FunctionTextEntry> entries(functions.size(), [&](size_t index) {
        const Function& function = program.at(functions[index].function);
        auto next = std::ranges::upper_bound(moduleRanges[function.module.index()], function.text.start, { }, &RetainedTextRange::start);
        const RetainedTextRange& range = *std::prev(next);
        uint32_t start = range.textOffset + function.text.start - range.start;
        return FunctionTextEntry { functions[index].entrypoint.index(), start, start + function.text.end - function.text.start };
    });
    AOT::sort(entries, { }, &FunctionTextEntry::function);

    uint32_t entriesOffset = encoder.writer.append(entries);
    uint32_t textOffset = encoder.writer.append(text);
    encoder.setTable(ProgramTableIndex::functionTexts, Vector<FunctionTexts, 1> { FunctionTexts { static_cast<uint32_t>(entries.size()), entriesOffset, textOffset } });
}

static void appendConstructEntrypoints(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<ConstructEntrypointData> constructEntrypoints;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (function.role != FunctionRole::GenericEntrypoint || !function.hasBody())
            continue;
        FunctionID constructEntrypoint = function.creator.as<CreatorRecord::Entrypoint>().constructEntrypoint;
        if (!constructEntrypoint)
            continue;
        if (!program.at(constructEntrypoint).hasBody()) {
            encoder.fail(ProgramTableRule::FunctionWithoutEntrypoint, { }, FunctionID(index));
            continue;
        }
        bool allocatesThis = program.at(constructEntrypoint).derivation.parameter == allocatingConstructEntrypointParameter;
        constructEntrypoints.append({ index, constructEntrypoint.index() | (allocatesThis ? ConstructEntrypointData::allocatesThis : 0) });
    }
    encoder.setTable(ProgramTableIndex::constructEntrypoints, constructEntrypoints);
}

void encodeFunctionTables(TableEncoder& encoder)
{
    Vector<FunctionTraceRecord> traces(encoder.program.functions.size(), [&](size_t index) {
        return functionTraceOf(encoder.program, FunctionID(index));
    });
    size_t firstDerived = 0;
    while (firstDerived < traces.size() && traces[firstDerived].sourceFunctionIndex == firstDerived)
        ++firstDerived;
    appendFunctionNames(encoder, traces.span(), firstDerived);

    NumberedFunctionRecords numbered = numberFunctionRecords(Vector<FunctionRecordData>(traces.size(), [&](size_t index) {
        return FunctionRecordData { encoder.placement.functionKind(FunctionID(index)), traces[index] };
    }).span());
    encoder.setTable(ProgramTableIndex::functionRecordNumbers, numbered.numbers);
    encoder.setTable(ProgramTableIndex::functionRecords, numbered.records);
    encoder.setTable(ProgramTableIndex::functionRecordsByFunction, numbered.recordsByFunction);

    encoder.setTableFrom<Number24>(ProgramTableIndex::functionSources, traces.span().subspan(firstDerived), [](const FunctionTraceRecord& trace) {
        return Number24(trace.sourceFunctionIndex);
    });
    Vector<ImplementedBuiltinData> implementedBuiltins;
    for (uint32_t index = 0; index < traces.size(); ++index) {
        if (std::optional<BuiltinID> builtin = traces[index].implements())
            implementedBuiltins.append({ index, builtin->index() });
    }
    encoder.setTable(ProgramTableIndex::implementedBuiltins, implementedBuiltins);

    appendClosureFunctionTables(encoder);
    appendFunctionTexts(encoder);
    appendConstructEntrypoints(encoder);
}

namespace {

struct ModuleLists {
    void setListStarts(ModuleData& entry) const
    {
        entry.firstRequest = Number24(requests.size());
        entry.firstExport = Number24(exports.size());
        entry.firstSyntheticExport = Number24(syntheticExports.size());
        entry.firstRequiredName = Number24(requiredNames.size());
    }

    Vector<ModuleData> modules;
    Vector<ModuleDirectoryData> directories;
    HashMap<uint64_t, uint32_t> numbersOfDirectories;
    Vector<ModuleDisplayPathData> displayPaths;
    Vector<ModuleRequestData> requests;
    Vector<ModuleExportData> exports;
    Vector<IndirectExportData> indirectExports;
    Vector<SyntheticExportData> syntheticExports;
    Vector<RequiredNameData> requiredNames;
};

struct EncodedRequestTargets {
    const Vector<ModuleRequest>& requests;

    size_t size() const { return requests.size(); }
    ModuleID operator[](size_t index) const { return requests[index].target; }
};

} // anonymous namespace

static BitVector findModuleBodiesThatOnlyReturn(const Program& program)
{
    BitVector result(program.functions.size());
    for (const Module& module : program.modules) {
        if (!module.body)
            continue;
        const Function& body = program.at(module.body);
        if (body.blocks.size() != 1 || body.has(FunctionFlags::IsAsync) || body.blocks.first().terminator.kind != TerminatorKind::Return)
            continue;
        bool hasOnlyUndefined = std::ranges::all_of(body.blocks.first().values, [&](ValueID id) {
            return body.at(id).opcode == Opcode::Undefined;
        });
        if (hasOnlyUndefined)
            result.quickSet(module.body.index());
    }
    return result;
}

static bool hasCompiledBody(const Program& program, const Module& module)
{
    return module.body && program.at(module.body).hasBody();
}

static bool isNeverEvaluated(const Program& program, const Module& module)
{
    return module.body && !program.at(module.body).hasBody();
}

static bool isSourceTextOrCommonJs(const Module& module)
{
    return module.kind == ModuleKind::SourceText || module.kind == ModuleKind::CommonJs;
}

static uint32_t bodyEntrypointOf(const TableEncoder& encoder, const Module& module)
{
    bool doesNothing = module.body && isSourceTextOrCommonJs(module) && encoder.inputs.bodyFacts.functionsThatOnlyReturn.get(module.body.index());
    return hasCompiledBody(encoder.program, module) && !doesNothing ? encoder.entrypointOf(module.body) : UINT32_MAX;
}

static bool mayHavePathInPieces(const Module& module)
{
    return module.kind == ModuleKind::SourceText || module.kind == ModuleKind::Data;
}

static void forEachPieceOfPath(std::span<const uint8_t> path, NOESCAPE const Invocable<void(std::span<const uint8_t>)> auto& functor)
{
    if (path.empty() || path.back() == '/' || !std::ranges::contains(path, '/'))
        return;
    while (!path.empty()) {
        auto slash = std::ranges::find(path, '/');
        size_t length = slash == path.end() ? path.size() : static_cast<size_t>(slash - path.begin()) + 1;
        functor(path.first(length));
        path = path.subspan(length);
    }
}

void internPiecesOfModulePaths(Program& program)
{
    for (const Module& module : program.modules) {
        if (!mayHavePathInPieces(module))
            continue;
        forEachPieceOfPath(program.string(module.path), [&](std::span<const uint8_t> piece) {
            program.strings.intern(piece);
        });
    }
}

static void setPublicPath(TableEncoder& encoder, const Module& module, ModuleLists& lists, ModuleData& entry)
{
    Vector<StringID, 16> pieces;
    bool everyPieceIsString = true;
    if (mayHavePathInPieces(module) && entry.isSimple() && encoder.program.strings.size() <= Number24::max) {
        forEachPieceOfPath(encoder.program.string(module.path), [&](std::span<const uint8_t> piece) {
            pieces.append(encoder.program.strings.find(piece));
            everyPieceIsString &= !!pieces.last();
        });
    }
    if (pieces.isEmpty() || !everyPieceIsString) {
        entry.publicPathOrName = encoder.shownStringNumber(module.path);
        entry.directory = Number24(Number24::max);
        return;
    }

    uint32_t directory = Number24::max;
    for (StringID name : pieces.span().first(pieces.size() - 1)) {
        directory = lists.numbersOfDirectories.ensure((static_cast<uint64_t>(directory) << 32 | name.index()) + 1, [&] {
            lists.directories.append({ Number24(directory), Number24(encoder.shownStringNumber(name)) });
            return static_cast<uint32_t>(lists.directories.size() - 1);
        }).iterator->value;
    }
    RELEASE_ASSERT(directory < Number24::max);
    entry.directory = Number24(directory);
    entry.publicPathOrName = encoder.shownStringNumber(pieces.last());
}

static void setDisplayPath(TableEncoder& encoder, ModuleID id, const Module& module, ModuleLists& lists, ModuleData& entry)
{
    std::span<const uint8_t> displayPath = module.displayPath ? encoder.program.string(module.displayPath) : std::span<const uint8_t>();
    entry.charactersInFrontOfDisplayPath = ModuleData::hasNoDisplayPath;
    if (encoder.obfuscates() || displayPath.empty())
        return;
    std::span<const uint8_t> publicPath = encoder.program.string(module.path);
    if (publicPath.size() >= displayPath.size()) {
        std::span<const uint8_t> inFront = publicPath.first(publicPath.size() - displayPath.size());
        bool isCounted = inFront.size() < ModuleData::hasNoDisplayPath && std::ranges::all_of(inFront, isASCII<uint8_t>);
        if (isCounted && std::ranges::equal(publicPath.subspan(inFront.size()), displayPath)) {
            entry.charactersInFrontOfDisplayPath = static_cast<uint8_t>(inFront.size());
            return;
        }
    }
    entry.charactersInFrontOfDisplayPath = ModuleData::displayPathIsListed;
    lists.displayPaths.append({ Number24(id.index()), Number24(std::min(encoder.shownStringNumber(module.displayPath), Number24::max)) });
}

static void appendModule(TableEncoder& encoder, ModuleID id, const Module& module, ModuleLists& lists)
{
    const Program& program = encoder.program;
    using Flag = ModuleData::Flag;
    ModuleData entry { };
    setDisplayPath(encoder, id, module, lists, entry);
    entry.hasInstalledPositions = module.flags.contains(ModuleFlags::HasInstalledPositions);
    entry.kind = module.kind;
    entry.externalKind = module.externalKind;
    entry.exportRule = module.exportRule;
    entry.requireResult = module.requireResult;
    entry.body = optionalNumber24(bodyEntrypointOf(encoder, module));
    RELEASE_ASSERT(module.kind != ModuleKind::External || !module.runtimeModule);
    entry.referrerOrRuntimeModule = optionalNumber24(module.kind == ModuleKind::External ? indexOrNone(module.referrer) : indexOrNone(module.runtimeModule));
    if (module.exportsVariable)
        entry.value = placedVariableSlot(encoder, module.exportsVariable);
    else if (encoder.placement.hasModuleValue(id))
        entry.value = SlotData::create(encoder.checkedOffset(encoder.placement.moduleValueOffset(id)), SlotRepresentation::JSValue);
    const std::pair<bool, Flag> flags[] = {
        { module.kind == ModuleKind::CommonJs && module.syntheticExports.isEmpty(), Flag::CapturesOnDemand },
        { module.flags.contains(ModuleFlags::HasTopLevelAwait), Flag::HasTopLevelAwait },
        { module.kind != ModuleKind::CommonJs || module.flags.contains(ModuleFlags::KeepsError), Flag::KeepsError },
        { module.flags.contains(ModuleFlags::EvaluatesItself) && entry.bodyFunction(), Flag::EvaluatesItself },
        { module.exportsVariable || encoder.placement.hasModuleValue(id), Flag::HasValue },
        { isBundlerHelperModule(program, module), Flag::IsLibraryCodeWithFilePath },
        { module.kind == ModuleKind::RuntimeSource || module.flags.contains(ModuleFlags::IsLibrary) || isBundlerHelperModule(program, module), Flag::IsOfLibrary },
    };
    for (auto [holds, flag] : flags) {
        if (holds)
            entry.add(flag);
    }
    lists.setListStarts(entry);
    lists.modules.append(entry);
}

static bool runsNothingItself(const TableEncoder& encoder, const Module& module)
{
    switch (module.kind) {
    case ModuleKind::SourceText:
        return bodyEntrypointOf(encoder, module) == UINT32_MAX && (module.requiredNames.isEmpty() || isNeverEvaluated(encoder.program, module));
    case ModuleKind::CommonJs:
        return isNeverEvaluated(encoder.program, module);
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BitVector findModulesNotWorthVisiting(const TableEncoder& encoder)
{
    const Vector<Module>& modules = encoder.program.modules;
    StronglyConnectedComponents components = computeStronglyConnectedComponents(modules.size(), [&](unsigned index) {
        return EncodedRequestTargets { modules[index].requests };
    });
    const Vector<uint32_t>& componentOf = components.componentOf;

    // A component has a higher number than every component it requests.
    BitVector componentsWorthVisiting(components.numberOfComponents);
    for (uint32_t index : components.members) {
        bool isWorthVisiting = !runsNothingItself(encoder, modules[index]) || std::ranges::any_of(modules[index].requests, [&](const ModuleRequest& request) {
            return componentsWorthVisiting.quickGet(componentOf[request.target.index()]);
        });
        if (isWorthVisiting)
            componentsWorthVisiting.quickSet(componentOf[index]);
    }

    BitVector modulesNotWorthVisiting(modules.size());
    for (size_t index = 0; index < modules.size(); ++index) {
        if (!componentsWorthVisiting.quickGet(componentOf[index]))
            modulesNotWorthVisiting.quickSet(index);
    }
    return modulesNotWorthVisiting;
}

static bool isAnsweredByCode(const TableEncoder& encoder, ModuleID requester, const ModuleRequest& request)
{
    switch (encoder.program.at(request.target).kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
        return !encoder.placement.requireEdge(requester, request.target);
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void appendExports(TableEncoder& encoder, ModuleID id, const Module& module, ModuleLists& lists)
{
    for (const ModuleExport& moduleExport : module.exports) {
        if (!isDescribedToRunTime(encoder.program, encoder.flow, id, moduleExport))
            continue;
        ModuleExportData entry { };
        entry.name = encoder.identifierNumberOfText(moduleExport.name);
        entry.kindBits = static_cast<uint32_t>(moduleExport.kind);
        entry.isDeferred = moduleExport.phase == ImportPhase::Defer;
        entry.isFromStarExport = moduleExport.isFromStarExport;
        switch (moduleExport.kind) {
        case ModuleExportKind::Variable:
            entry.variable = placedVariableSlot(encoder, moduleExport.variable);
            break;
        case ModuleExportKind::Namespace:
            entry.module = moduleExport.module.index();
            break;
        case ModuleExportKind::Indirect:
            entry.indirectExport = static_cast<uint32_t>(lists.indirectExports.size());
            lists.indirectExports.append({ moduleExport.module.index(), encoder.identifierNumberOfText(moduleExport.importName) });
            break;
        }
        lists.exports.append(entry);
    }
}

static ModuleLists collectModuleLists(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    ModuleLists lists;
    BitVector modulesNotWorthVisiting = findModulesNotWorthVisiting(encoder);
    lists.modules.reserveInitialCapacity(program.modules.size() + 1);
    for (size_t index = 0; index < program.modules.size(); ++index) {
        ModuleID id(index);
        const Module& module = program.modules[index];
        appendModule(encoder, id, module, lists);
        for (const ModuleRequest& request : module.requests) {
            if (modulesNotWorthVisiting.quickGet(request.target.index()) && isNeverEvaluated(program, program.at(request.target)))
                continue;
            bool isRequire = request.kind == ModuleRequestKind::Require;
            if (isRequire && !isAnsweredByCode(encoder, id, request))
                continue;
            lists.requests.append(ModuleRequestData::create(request.target.index(), request.phase == ImportPhase::Defer, request.isDynamicStarExport, isRequire));
        }
        appendExports(encoder, id, module, lists);
        for (const SyntheticExport& syntheticExport : module.syntheticExports) {
            SyntheticExportData entry { };
            entry.name = encoder.identifierNumberOfText(syntheticExport.name);
            entry.absentIsUndefined = syntheticExport.isKnownMember;
            entry.hasVariable = !!syntheticExport.variable;
            entry.variable = syntheticExport.variable ? placedVariableSlot(encoder, syntheticExport.variable) : SlotData { };
            lists.syntheticExports.append(entry);
        }
        for (const RequiredName& requiredName : module.requiredNames) {
            if (!isNeverEvaluated(program, module))
                lists.requiredNames.append({ requiredName.module.index(), requiredName.mayBeAbsent, encoder.identifierNumberOfText(requiredName.name) });
        }
    }
    for (const Entry& entry : program.entries)
        lists.modules[entry.module.index()].add(ModuleData::Flag::IsEntryPoint);
    ModuleData endEntry { };
    lists.setListStarts(endEntry);
    lists.modules.append(endEntry);

    for (const RequiredNameData& requiredName : lists.requiredNames) {
        const ModuleData& exporter = lists.modules[requiredName.exporter];
        if (exporter.exportRule != SyntheticExportRule::RuntimeModule)
            continue;
        uint32_t end = lists.modules[requiredName.exporter + 1].firstSyntheticExport;
        for (uint32_t index = exporter.firstSyntheticExport; index < end; ++index) {
            if (lists.syntheticExports[index].name == requiredName.name)
                lists.syntheticExports[index].isBoundByRequiredName = true;
        }
    }
    return lists;
}

static Vector<uint16_t> encodeModuleNumbers(const TableEncoder& encoder, const Vector<uint32_t>& modules)
{
    // The table of modules ends with one more record.
    bool needsTwoWords = ModuleNumbers::needsTwoWords(encoder.program.modules.size() + 1);
    Vector<uint16_t> words;
    words.reserveInitialCapacity(needsTwoWords ? 2 * modules.size() : modules.size());
    for (uint32_t module : modules) {
        words.append(static_cast<uint16_t>(module));
        if (needsTwoWords)
            words.append(static_cast<uint16_t>(module >> 16));
    }
    return words;
}

static bool isFoundByPath(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        return true;
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void appendTablesOfCompiledModules(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<uint32_t> modulesByPath;
    Vector<CompiledRuntimeModuleData> runtimeModules;
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (!isFoundByPath(module.kind) || (module.kind == ModuleKind::RuntimeSource && !hasCompiledBody(program, module)))
            continue;
        if (module.kind == ModuleKind::RuntimeSource && module.runtimeModule)
            runtimeModules.append({ module.runtimeModule.index(), index });
        if (!isNeverEvaluated(program, module) && (!isBundlerHelperModule(program, module) || isBundlerHelperNamedByProgram(program, ModuleID(index))))
            modulesByPath.append(index);
    }
    AOT::sort(modulesByPath, [&](uint32_t left, uint32_t right) {
        return std::ranges::lexicographical_compare(program.string(program.modules[left].path), program.string(program.modules[right].path));
    });
    encoder.setTable(ProgramTableIndex::modulesByPath, encodeModuleNumbers(encoder, modulesByPath));
    setSortedTable(encoder, ProgramTableIndex::compiledRuntimeModules, runtimeModules, std::ranges::less { }, &CompiledRuntimeModuleData::registryID);

    BitVector isCalledByHost = functionsCalledByHost(program);
    Vector<CompiledRuntimeFunctionData> runtimeFunctions;
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (entry.function && isCalledByHost.get(entry.function.index()) && program.at(entry.function).hasBody())
            runtimeFunctions.append({ entry.id.index(), encoder.entrypointOf(entry.function), static_cast<uint32_t>(entry.visibility) });
    }
    encoder.setTable(ProgramTableIndex::compiledRuntimeFunctions, runtimeFunctions);
}

static ContractedModuleRequests contractRequestLists(TableEncoder& encoder, ModuleLists& lists)
{
    BitVector modulesNamedByRequire(encoder.program.modules.size());
    for (const RequireEdge& edge : encoder.placement.requireEdges())
        modulesNamedByRequire.quickSet(edge.target.index());
    for (const ModuleRequestData& request : lists.requests) {
        if (request.isRequire)
            modulesNamedByRequire.quickSet(request.target());
    }

    uint32_t maxContractedRequests = static_cast<uint32_t>(std::min<uint64_t>(encoder.build.limit(LimitID::MaxContractedModuleRequests), UINT32_MAX));
    ContractedModuleRequests contracted = contractModuleRequests(lists.modules.mutableSpan(), lists.requests.span(), modulesNamedByRequire, maxContractedRequests);
    for (uint32_t module : contracted.modulesOverLimit)
        encoder.build.gaveUp(LimitID::MaxContractedModuleRequests, { .function = encoder.program.modules[module].body });
    return contracted;
}

static Vector<ModuleImportSite> findImportSites(const Program& program, const ParallelLoops& loops)
{
    auto namesModule = [](const Value& value) {
        return value.opcode == Opcode::ImportModule;
    };
    Vector<uint32_t> importers = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return std::ranges::any_of(program.functions[index].values, namesModule);
    });
    Vector<ModuleImportSite> sites;
    for (uint32_t importer : importers) {
        for (const Value& value : program.functions[importer].values) {
            if (namesModule(value))
                sites.append(ModuleImportSite { value.referrer().index(), value.module().index() });
        }
    }
    return sites;
}

void encodeModuleTables(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    ModuleLists lists = collectModuleLists(encoder);
    computeModuleFlags(lists.modules.mutableSpan(), lists.requests.span());
    for (size_t index = 0; index < program.modules.size(); ++index)
        setPublicPath(encoder, program.modules[index], lists, lists.modules[index]);
    ContractedModuleRequests contracted = contractRequestLists(encoder, lists);
    std::optional<ModuleID> plannedEntryPoint = program.entries.isEmpty() ? std::nullopt : std::optional(program.entries.first().module);
    Vector<LinkPlanStepData> entryPointLinkPlan;
    if (plannedEntryPoint)
        entryPointLinkPlan = planLinkOfEntryPoint(lists.modules.mutableSpan(), lists.requests.span(), *plannedEntryPoint);
    LinkPlansOfRoots linkPlansOfRoots = planLinksOfRoots(lists.modules.mutableSpan(), lists.requests.span(), encoder.inputs.bodyFacts.importSites.span(), plannedEntryPoint);
    Vector<uint32_t> modulesOfUncompiledCode;
    for (uint32_t index = 0; index + 1 < lists.modules.size(); ++index) {
        if (lists.modules[index].isUncompiledCode())
            modulesOfUncompiledCode.append(index);
    }
    encoder.setTable(ProgramTableIndex::modulesOfUncompiledCode, modulesOfUncompiledCode);
    encoder.setTable(ProgramTableIndex::modules, lists.modules);
    encoder.setTable(ProgramTableIndex::moduleDirectories, lists.directories);
    encoder.setTable(ProgramTableIndex::moduleDisplayPaths, lists.displayPaths);
    encoder.setTable(ProgramTableIndex::moduleRequests, lists.requests);
    encoder.setTable(ProgramTableIndex::linkRequests, encodeModuleNumbers(encoder, contracted.linkRequests));
    encoder.setTable(ProgramTableIndex::evaluationRequests, encodeModuleNumbers(encoder, contracted.evaluationRequests));
    encoder.setTable(ProgramTableIndex::entryPointLinkPlan, entryPointLinkPlan);
    encoder.setTable(ProgramTableIndex::linkPlanRoots, linkPlansOfRoots.roots);
    encoder.setTable(ProgramTableIndex::linkPlanStepsOfRoots, linkPlansOfRoots.steps);
    encoder.setTable(ProgramTableIndex::moduleExports, lists.exports);
    encoder.setTable(ProgramTableIndex::indirectExports, lists.indirectExports);
    encoder.setTable(ProgramTableIndex::syntheticExports, lists.syntheticExports);
    encoder.setTable(ProgramTableIndex::requiredNames, lists.requiredNames);
    encoder.setTableFrom<ModuleFailureData>(ProgramTableIndex::moduleFailures, program.moduleFailures, [&](const ModuleFailure& failure) {
        return failure.exporter ? ModuleFailureData::ofAbsentExport(failure.exporter, encoder.shownStringNumber(failure.specifier)) : ModuleFailureData { failure.importer.index(), encoder.shownStringNumber(failure.specifier) };
    });
    appendTablesOfCompiledModules(encoder);
}

namespace {

struct TextProperties {
    uint32_t hash { 0 };
    uint32_t length { 0 };
    std::array<uint8_t, 20> digest { };
};

} // anonymous namespace

static Vector<TextProperties> computeTextProperties(const TableEncoder& encoder, std::span<const StringID> strings)
{
    const LiteralStringTable& literalStrings = *encoder.inputs.pipeline.literalStrings;
    Vector<TextProperties> properties(strings.size());
    forEachIndexInParallel(encoder.build.parallelLoops(), static_cast<unsigned>(strings.size()), [&](unsigned index) {
        DecodedString decoded = decodeForImage(encoder.program.strings[strings[index]]);
        properties[index] = { decoded.hash, decoded.length(), { } };
        if (literalStrings.isWithoutText(strings[index]))
            properties[index].digest = digestOfLiteralString(decoded.is16Bit() ? StringView(decoded.units16.span()) : StringView(byteCast<Latin1Character>(decoded.units8.span())));
    });
    return properties;
}

static void encodeLiteralStrings(TableEncoder& encoder)
{
    const LiteralStringTable& numbered = *encoder.inputs.pipeline.literalStrings;
    std::span<const StringID> literalStrings = numbered.strings();
    RELEASE_ASSERT(literalStrings.size() <= maxLiteralStrings);
    encoder.setTableFrom<uint32_t>(ProgramTableIndex::literalStrings, literalStrings, [&](StringID string) {
        return numbered.isWithoutText(string) ? string.index() : encoder.stringNumber(string);
    });

    Vector<TextProperties> properties = computeTextProperties(encoder, literalStrings);
    Vector<uint32_t> table(FillWith { }, std::max<uint32_t>(literalStrings.size(), 4) * 2, 0);
    Vector<LiteralStringLimits, 1> limits;
    Vector<LiteralStringDigest> digests;
    if (!literalStrings.empty())
        limits.append(LiteralStringLimits { 0 });
    for (uint32_t number = 0; number < literalStrings.size(); ++number) {
        uint32_t hash = properties[number].hash;
        uint32_t slot = AOT::LiteralStringTable::firstSlot(hash, table.size());
        while (table[slot])
            slot = slot + 1 == table.size() ? 0 : slot + 1;
        table[slot] = AOT::LiteralStringTable::entryFor(number, hash);
        ASSERT(table[slot]);
        limits[0].maxLength = std::max(limits[0].maxLength, properties[number].length);
        if (numbered.isWithoutText(literalStrings[number]))
            digests.append(LiteralStringDigest { number, properties[number].length, properties[number].digest });
    }
    encoder.setTable(ProgramTableIndex::literalStringTable, table);
    encoder.setTable(ProgramTableIndex::literalStringLimits, limits);
    encoder.setTable(ProgramTableIndex::literalStringDigests, digests);
}

static bool isEngineSymbol(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
    case PropertyKeyKind::EnginePrivateName:
        return true;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void encodeIdentifierTables(TableEncoder& encoder)
{
    Vector<uint32_t> numbers;
    Vector<StringID> texts;
    Vector<uint32_t> engineSymbols;
    for (uint32_t number = 1; number < encoder.identifiers.count(); ++number) {
        const auto& identifier = encoder.identifiers.at(number);
        if (encoder.identifiers.numberOfText(identifier.text) == number) {
            numbers.append(number);
            texts.append(identifier.text);
        }
        if (identifier.key && isEngineSymbol(encoder.program.at(identifier.key).kind))
            engineSymbols.append(number);
    }
    Vector<TextProperties> properties = computeTextProperties(encoder, texts.span());
    Vector<IdentifierTable::Key> keys(numbers.size(), [&](size_t index) {
        return IdentifierTable::Key { properties[index].hash, numbers[index] };
    });
    encoder.setTable(ProgramTableIndex::identifiersByHash, IdentifierTable::create(keys.span()));
    encoder.setTable(ProgramTableIndex::identifiersOfEngineSymbols, engineSymbols);
}

void encodeConstantTables(TableEncoder& encoder)
{
    encoder.setTableFrom<uint32_t>(ProgramTableIndex::builtinCells, encoder.placement.builtinCells(), &BuiltinCellKey::encoded);
    encoder.setTable(ProgramTableIndex::eagerBuiltinCells, encoder.placement.eagerBuiltinCells());
    encoder.setTableFrom<uint32_t>(ProgramTableIndex::constantBuiltinMembers, constantBuiltinMembersToLock(encoder.program, encoder.flow), &BuiltinID::index);
    encoder.setTableFrom<uint32_t>(ProgramTableIndex::bigIntConstants, encoder.placement.bigIntConstants(), [&](StringID digits) {
        return encoder.stringNumber(digits);
    });
    encoder.setTableFrom<uint32_t>(ProgramTableIndex::keyCellIdentifiers, encoder.placement.keyCells(), [&](PropertyKeyID key) {
        return encoder.identifierNumber(key);
    });
    encodeLiteralStrings(encoder);
    encodeIdentifierTables(encoder);
    encoder.setTableFrom<uint32_t>(ProgramTableIndex::entryPoints, encoder.program.entries, [](const Entry& entry) {
        return entry.module.index();
    });
}

BodyFactsForTables collectBodyFactsForTables(const Program& program, const BitVector& bodiesWithoutCode, BuildContext& build)
{
    BodyFactsForTables facts;
    if (build.recordedOptions().policy() == MismatchPolicy::Report)
        facts.usedTypeSites = findUsedTypeSites(program, build.parallelLoops());
    facts.functionsThatOnlyReturn = findModuleBodiesThatOnlyReturn(program);
    facts.functionsWithFrames = BitVector(program.functions.size());
    for (size_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || bodiesWithoutCode.get(index))
            continue;
        facts.functionsWithFrames.quickSet(index);
        for (const InlineFrame& frame : function.inlineFrames) {
            if (frame.callee)
                facts.functionsWithFrames.quickSet(frame.callee.index());
        }
    }
    facts.importSites = findImportSites(program, build.parallelLoops());
    return facts;
}

void encodeTables(TableEncoder& encoder)
{
    struct TableGroup {
        void (*encode)(TableEncoder&);
        StatisticID statistic;
    };
    static constexpr TableGroup groups[] = {
        { encodeTypeTables, StatisticID::ProgramTableBytesTypes },
        { encodeDataTable, StatisticID::ProgramTableBytesData },
        { encodeLayoutTables, StatisticID::ProgramTableBytesLayouts },
        { encodePropertyRunTable, StatisticID::ProgramTableBytesLayouts },
        { encodeFieldTable, StatisticID::ProgramTableBytesFieldTable },
        { encodeMethodTable, StatisticID::ProgramTableBytesOther },
        { encodeStaticsTables, StatisticID::ProgramTableBytesStatics },
        { encodeFunctionTables, StatisticID::ProgramTableBytesFunctions },
        { encodeModuleTables, StatisticID::ProgramTableBytesModules },
        { encodeRequestTable, StatisticID::ProgramTableBytesRequests },
        { encodeConstantTables, StatisticID::ProgramTableBytesOther },
        { encodeGlobalBindingTable, StatisticID::ProgramTableBytesOther },
        // The last: the image builder takes it off the tables and puts it in front of the constants of the code.
        { encodeShapeSetBits, StatisticID::ProgramTableBytesLayouts },
    };

    BuildContext& build = encoder.build;
    build.statistics().count(StatisticID::ProgramTableBytesOther, encoder.writer.size());
    for (auto& group : groups) {
        size_t sizeBefore = encoder.writer.size();
        group.encode(encoder);
        if (build.diagnostics().hasErrors())
            return;
        build.statistics().count(group.statistic, encoder.writer.size() - sizeBefore);
    }
}

EncodedProgramTables encodeProgramTables(const ProgramTableInputs& inputs, BuildContext& build)
{
    TableEncoder encoder(inputs, build);
    encodeTables(encoder);
    if (build.diagnostics().hasErrors())
        return { };
    ProgramTables& header = encoder.header;
    const ShapeTable& shapes = encoder.placement.shapes();
    ClosureFunctionRange closureFunctions = encoder.placement.closureFunctions();
    header.magic = ProgramTables::expectedMagic;
    header.flags = (encoder.flow.programInstances() == ProgramInstances::AtMostOne ? 0 : ProgramTables::mayRunInMultipleInstances) | (encoder.flow.mayReadLegacyRegExpStatics() ? 0 : ProgramTables::neverReadsLegacyRegExpStatics);
    header.instanceSizes = inputs.instanceSizes;
    header.numberOfShapes = shapes.numberOfShapes();
    header.firstEnvironmentShape = shapes.firstEnvironmentShape();
    header.closureFunctions = { closureFunctions.first.index(), closureFunctions.end.index(), closureFunctions.omittedWordCount };
    encoder.writer.at<ProgramTables>(0) = header;

    Vector<uint8_t> bytes = encoder.writer.take();
    if (build.options().verifiesProgram) {
        checkProgramTables(encoder, bytes.span());
        if (build.diagnostics().hasErrors())
            return { };
    }
    BitVector stringsOnlyShown = encoder.namedStrings;
    stringsOnlyShown.exclude(encoder.stringsWithOtherUses);
    return { WTF::move(bytes), WTF::move(encoder.namedStrings), WTF::move(stringsOnlyShown) };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
