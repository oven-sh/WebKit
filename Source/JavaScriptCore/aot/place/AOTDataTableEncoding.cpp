/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTSlotAccess.h"
#include "AOTSort.h"
#include "AOTTableInImage.h"
#include "AOTTextEncoder.h"
#include "AOTVector.h"
#include "IndexingHeader.h"
#include "IndexingType.h"
#include "JSCJSValueInlines.h"
#include "PureNaN.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t wordSize = sizeof(uint64_t);
static constexpr FieldIndex noField = std::numeric_limits<FieldIndex>::max();

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

uint64_t encodeDataWord(TableEncoder& encoder, const DataNode& leaf)
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
    case DataNode::Kind::BigInt: {
        std::optional<uint32_t> number = encoder.placement.bigIntConstant(leaf.as<DataNode::BigInt>().digits);
        if (!number) {
            encoder.fail(ProgramTableRule::DataNotEncoded);
            return DataWord::hole;
        }
        return DataWord::encodeBigIntConstant(*number);
    }
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

static uint64_t doubleBitsOf(const DataNode& numberOrHole)
{
    const auto* number = numberOrHole.tryAs<DataNode::Number>();
    return std::bit_cast<uint64_t>(number ? number->value : PNaN);
}

static bool hasOnlyPairs(const DataNode::Array& entries)
{
    return std::ranges::all_of(entries.elements, [](const DataNode& entry) {
        const auto* pair = entry.tryAs<DataNode::Array>();
        return pair && pair->elements.size() == 2;
    });
}

namespace {

struct OpenNode {
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
        , m_tableOffset(encoder.writer.allocate(wordSize))
    {
    }

    uint32_t tableOffset() const { return m_tableOffset; }
    uint32_t numberOfWords() const { return static_cast<uint32_t>((m_encoder.writer.size() - m_tableOffset) / wordSize); }

    uint32_t encode(const DataNode& root);

private:
    void openNode(const DataNode&);
    void visitChild(OpenNode parent, uint32_t index);
    void visitValue(const DataNode&);
    void closeNode(const OpenNode&);
    uint32_t appendArray(const OpenNode&, std::span<const uint64_t> contents);
    uint32_t appendObject(const OpenNode&, std::span<const uint64_t> contents);
    std::optional<uint32_t> tryAppendObjectBody(const DataNode::Object&, AllocationID, std::span<const FieldIndex> fieldOfEachMember, std::span<const uint64_t> contents);
    bool tryWriteSlot(const PlacedSlot&, void* slot, const DataNode&, uint64_t word);
    uint32_t appendMembersByName(DataNodeHeader::Kind, std::span<const uint64_t> values, std::span<const uint32_t> identifierNumbers);
    uint32_t appendNode(DataNodeHeader::Kind, uint32_t operand, std::span<const uint64_t> contents, std::span<const uint16_t> fieldIndices, std::optional<uint32_t> otherMembers = std::nullopt);

    TableEncoder& m_encoder;
    uint32_t m_tableOffset;
    Vector<OpenNode, 16> m_openNodes;
    Vector<uint64_t> m_words;
    Vector<FieldIndex> m_fields;
};

uint32_t DataNodeEncoder::encode(const DataNode& root)
{
    ASSERT(m_openNodes.isEmpty() && m_words.isEmpty() && m_fields.isEmpty());
    openNode(root);
    while (!m_openNodes.isEmpty()) {
        OpenNode& innermost = m_openNodes.last();
        if (innermost.nextChild == innermost.numberOfChildren) {
            closeNode(m_openNodes.takeLast());
            continue;
        }
        uint32_t index = innermost.nextChild++;
        visitChild(innermost, index);
    }
    return DataWord::operandOf(m_words.takeLast());
}

void DataNodeEncoder::openNode(const DataNode& node)
{
    OpenNode opened { .node = &node, .firstWord = static_cast<uint32_t>(m_words.size()), .firstField = static_cast<uint32_t>(m_fields.size()) };
    if (const auto* array = node.tryAs<DataNode::Array>()) {
        opened.indexingType = indexingTypeOf(m_encoder.program, *array);
        opened.numberOfChildren = array->elements.size();
    } else {
        auto& object = node.as<DataNode::Object>();
        const Layout* layout = object.layout ? &m_encoder.program.at(object.layout) : nullptr;
        for (const DataMember& member : object.members)
            m_fields.append(layout ? layout->findField(member.key).value_or(noField) : noField);
        opened.numberOfChildren = object.members.size();
    }
    m_openNodes.append(opened);
}

void DataNodeEncoder::visitChild(OpenNode parent, uint32_t index)
{
    if (const auto* array = parent.node->tryAs<DataNode::Array>()) {
        const DataNode& element = array->elements[index];
        if (parent.indexingType == ArrayWithDouble)
            m_words.append(doubleBitsOf(element));
        else
            visitValue(element);
        return;
    }
    auto& object = parent.node->as<DataNode::Object>();
    FieldIndex field = m_fields[parent.firstField + index];
    if (field == noField || m_encoder.slots.slot(object.layout, field).hasStorage())
        visitValue(object.members[index].value);
}

void DataNodeEncoder::visitValue(const DataNode& value)
{
    if (isLeaf(value))
        m_words.append(encodeDataWord(m_encoder, value));
    else
        openNode(value);
}

void DataNodeEncoder::closeNode(const OpenNode& closed)
{
    std::span<const uint64_t> contents = m_words.span().subspan(closed.firstWord);
    uint32_t node = closed.node->is<DataNode::Object>() ? appendObject(closed, contents) : appendArray(closed, contents);
    m_words.shrink(closed.firstWord);
    m_fields.shrink(closed.firstField);
    m_words.append(DataWord::encodeNode(node));
}

uint32_t DataNodeEncoder::appendArray(const OpenNode& closed, std::span<const uint64_t> contents)
{
    uint32_t operand = closed.indexingType | (isStorageOfArray(closed.node->as<DataNode::Array>()) ? DataNodeHeader::isStorage : 0);
    return appendNode(DataNodeHeader::Kind::Array, operand, contents, { });
}

uint32_t DataNodeEncoder::appendObject(const OpenNode& closed, std::span<const uint64_t> contents)
{
    const Placement& placement = m_encoder.placement;
    auto& object = closed.node->as<DataNode::Object>();
    LayoutID layout = object.layout;
    std::span<const FieldIndex> fieldOfEachMember = m_fields.span().subspan(closed.firstField);

    Vector<FieldIndex, 16> fields;
    Vector<uint16_t, 16> fieldIndices;
    Vector<uint64_t, 16> fieldValues;
    Vector<uint32_t, 16> otherKeys;
    Vector<uint64_t, 16> otherValues;
    size_t nextWord = 0;
    for (size_t index = 0; index < fieldOfEachMember.size(); ++index) {
        FieldIndex field = fieldOfEachMember[index];
        if (field == noField) {
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
    if (!layout)
        return appendMembersByName(DataNodeHeader::Kind::OrdinaryObject, otherValues.span(), otherKeys.span());

    AllocationID allocation = placement.allocations().find(layout, placement.shapes().generalShapeOf(layout), fields.span());
    if (!allocation) {
        m_encoder.fail(ProgramTableRule::DataNotEncoded, layout);
        return 0;
    }
    if (otherKeys.isEmpty()) {
        if (std::optional<uint32_t> node = tryAppendObjectBody(object, allocation, fieldOfEachMember, contents))
            return *node;
        return appendNode(DataNodeHeader::Kind::Object, allocation.index(), fieldValues.span(), fieldIndices.span());
    }
    uint32_t otherMembers = appendMembersByName(DataNodeHeader::Kind::OtherMembers, otherValues.span(), otherKeys.span());
    return appendNode(DataNodeHeader::Kind::ObjectWithOtherMembers, allocation.index(), fieldValues.span(), fieldIndices.span(), otherMembers);
}

bool DataNodeEncoder::tryWriteSlot(const PlacedSlot& slot, void* bytes, const DataNode& value, uint64_t word)
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
    if (!traits.isNarrowReference) {
        writeSlotBits(representation, bytes, word);
        return true;
    }
    if (DataWord::operandOf(word) > NarrowDataWord::maxOperand)
        return false;
    writeSlotBits(representation, bytes, NarrowDataWord::encode(DataWord::kindOf(word), DataWord::operandOf(word)));
    return true;
}

// Every member has a slot in the cell, and a store of its value does nothing but fill the slot.
std::optional<uint32_t> DataNodeEncoder::tryAppendObjectBody(const DataNode::Object& object, AllocationID allocation, std::span<const FieldIndex> fieldOfEachMember, std::span<const uint64_t> contents)
{
    const PlannedLayout& layout = m_encoder.slots.layout(object.layout);
    uint32_t size = m_encoder.placement.allocations().allocations[allocation.index()].size;
    if (size != layout.size || size < offsetOfFirstField || size % wordSize)
        return std::nullopt;

    Vector<uint64_t, 16> body(FillWith { }, (size - offsetOfFirstField) / wordSize, 0);
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
    return appendNode(DataNodeHeader::Kind::ObjectBody, allocation.index(), body.span(), { });
}

uint32_t DataNodeEncoder::appendMembersByName(DataNodeHeader::Kind kind, std::span<const uint64_t> values, std::span<const uint32_t> identifierNumbers)
{
    ASSERT(values.size() == identifierNumbers.size());
    uint32_t node = appendNode(kind, 0, values, { });
    uint32_t numbersOffset = m_tableOffset + (node + 1 + static_cast<uint32_t>(values.size())) * wordSize;
    for (uint32_t index = 0; index < identifierNumbers.size(); ++index)
        m_encoder.writer.at<uint32_t>(numbersOffset + index * static_cast<uint32_t>(sizeof(uint32_t))) = identifierNumbers[index];
    return node;
}

uint32_t DataNodeEncoder::appendNode(DataNodeHeader::Kind kind, uint32_t operand, std::span<const uint64_t> contents, std::span<const uint16_t> fieldIndices, std::optional<uint32_t> otherMembers)
{
    TableWriter& writer = m_encoder.writer;
    uint32_t count = static_cast<uint32_t>(contents.size());
    uint32_t offset = writer.allocate(static_cast<size_t>(DataNodeHeader::numberOfWords(kind, count)) * wordSize);

    DataNodeHeader header { };
    header.count = count;
    header.kindBits = static_cast<uint32_t>(kind);
    header.operand = operand;
    writer.at<uint64_t>(offset) = header.encoded();

    uint32_t contentsOffset = offset + wordSize;
    if (kind == DataNodeHeader::Kind::Array) {
        static_assert(!IndexingHeader::offsetOfPublicLength() && IndexingHeader::offsetOfVectorLength() == sizeof(uint32_t) && sizeof(IndexingHeader) == wordSize);
        writer.at<uint64_t>(contentsOffset) = count | static_cast<uint64_t>(count) << 32;
        contentsOffset += wordSize;
    }
    for (uint32_t index = 0; index < count; ++index)
        writer.at<uint64_t>(contentsOffset + index * wordSize) = contents[index];

    uint32_t fieldIndicesOffset = contentsOffset + count * wordSize;
    for (uint32_t index = 0; index < fieldIndices.size(); ++index)
        writer.at<uint16_t>(fieldIndicesOffset + index * static_cast<uint32_t>(sizeof(uint16_t))) = fieldIndices[index];

    if (otherMembers)
        writer.at<uint64_t>(offset + (DataNodeHeader::numberOfWords(kind, count) - 1) * wordSize) = DataWord::encodeNode(*otherMembers);

    return (offset - m_tableOffset) / wordSize;
}

} // anonymous namespace

static bool fitsTableInImage(const DataNode::Array& entries, bool hasValues)
{
    return std::ranges::all_of(entries.elements, [&](const DataNode& entry) {
        if (!hasValues)
            return entry.is<DataNode::String>();
        auto& pair = entry.as<DataNode::Array>();
        return pair.elements[0].is<DataNode::String>() && isLeaf(pair.elements[1]);
    });
}

static bool hasRepeatedKey(std::span<const TableInImage::Key> keys)
{
    Vector<uint32_t> strings(keys.size(), [&](size_t index) {
        return keys[index].string;
    });
    AOT::sort(strings);
    return std::ranges::adjacent_find(strings) != strings.end();
}

static std::optional<uint32_t> tryEncodeTableInImage(TableEncoder& encoder, const DataNode::Array& elements, bool hasValues)
{
    if (hasValues && !hasOnlyPairs(elements))
        return std::nullopt;
    if (!fitsTableInImage(elements, hasValues))
        return std::nullopt;

    Vector<TableInImage::Key> keys;
    Vector<uint64_t> values;
    keys.reserveInitialCapacity(elements.elements.size());
    values.reserveInitialCapacity(hasValues ? elements.elements.size() : 0);
    for (const DataNode& entry : elements.elements) {
        const DataNode& key = hasValues ? entry.as<DataNode::Array>().elements[0] : entry;
        StringID string = key.as<DataNode::String>().value;
        keys.append(TableInImage::Key { encoder.stringNumber(string), decodeForImage(encoder.program.strings[string]).hash });
        if (!hasValues)
            continue;
        const DataNode& value = entry.as<DataNode::Array>().elements[1];
        values.append(value.is<DataNode::Hole>() ? DataWord::encodeImmediate(jsUndefined()) : encodeDataWord(encoder, value));
    }
    if (hasRepeatedKey(keys.span()))
        return std::nullopt;

    std::optional<Vector<uint8_t>> bytes = TableInImage::tryEncode(keys.span(), values.span());
    if (!bytes)
        return std::nullopt;
    return encoder.writer.append(bytes->span(), alignof(TableInImage));
}

static BitVector findRecordsWithNodes(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    BitVector recordsWithNodes(program.data.size());
    std::span<const RequestRecord> records = encoder.placement.requests().records();
    encoder.tablesInImage.fill(std::nullopt, records.size());
    for (size_t index = 0; index < records.size(); ++index) {
        WTF::switchOn(records[index],
            [&](const DataRecord& request) {
                recordsWithNodes.quickSet(request.data.index());
            },
            [&](const CollectionDataRecord& request) {
                const auto* entries = program.at(request.data).root.tryAs<DataNode::Array>();
                if (!entries || (request.hasValues && !hasOnlyPairs(*entries))) {
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
    return recordsWithNodes;
}

static Vector<std::pair<uint32_t, DataID>> findPlacedNodes(TableEncoder& encoder)
{
    Vector<std::pair<uint32_t, DataID>> placed;
    for (const RequestRecord& record : encoder.placement.requests().records()) {
        const auto* request = std::get_if<DataRecord>(&record);
        if (request && request->node)
            placed.append({ *request->node, request->data });
    }
    AOT::sort(placed, { }, &std::pair<uint32_t, DataID>::first);
    placed.shrink(std::ranges::unique(placed).begin() - placed.begin());
    return placed;
}

void encodeDataTable(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    encoder.dataNodes.fill(UINT32_MAX, program.data.size());
    BitVector recordsWithNodes = findRecordsWithNodes(encoder);

    DataNodeEncoder nodes(encoder);
    // Compiled code has the places of these.
    for (auto [node, data] : findPlacedNodes(encoder)) {
        encoder.dataNodes[data.index()] = nodes.encode(program.at(data).root);
        if (encoder.dataNodes[data.index()] != node)
            encoder.fail(ProgramTableRule::DataNotEncoded);
    }
    for (size_t index : recordsWithNodes) {
        const DataNode& root = program.data[index].root;
        // TableEncoder::dataNodeOf() reports a record that a request names and that has no node.
        if (!isLeaf(root) && encoder.dataNodes[index] == UINT32_MAX)
            encoder.dataNodes[index] = nodes.encode(root);
    }
    encoder.setTableRange(ProgramTableIndex::data, nodes.tableOffset(), nodes.numberOfWords());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
