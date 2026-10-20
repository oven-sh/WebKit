/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTTableInImage.h"
#include "AOTTextEncoder.h"
#include "IndexingType.h"
#include "JSCJSValueInlines.h"
#include "PureNaN.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t wordSize = sizeof(uint64_t);

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

IndexingType indexingTypeOf(const DataNode::Array& array)
{
    bool areInt32 = true;
    bool areDoublesOrHoles = true;
    for (const DataNode& element : array.elements) {
        const auto* number = element.tryAs<DataNode::Number>();
        areInt32 &= number && jsNumber(purifyNaN(number->value)).isInt32();
        areDoublesOrHoles &= number ? !std::isnan(number->value) : element.is<DataNode::Hole>();
    }
    if (areInt32)
        return ArrayWithInt32;
    if (areDoublesOrHoles)
        return ArrayWithDouble;
    return ArrayWithContiguous;
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
    uint32_t appendObject(const OpenNode&, std::span<const uint64_t> contents);
    uint32_t appendNode(DataNodeHeader::Kind, uint32_t operand, std::span<const uint64_t> contents, std::span<const uint16_t> fieldIndices);

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
        opened.indexingType = indexingTypeOf(*array);
        opened.numberOfChildren = array->elements.size();
    } else {
        auto& object = node.as<DataNode::Object>();
        const Layout& layout = m_encoder.program.at(object.layout);
        for (const DataMember& member : object.members) {
            std::optional<uint32_t> field = layout.findField(member.key);
            RELEASE_ASSERT(field);
            m_fields.append(*field);
        }
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
    if (m_encoder.slots.slot(object.layout, m_fields[parent.firstField + index]).hasStorage())
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
    uint32_t node = closed.node->is<DataNode::Object>() ? appendObject(closed, contents) : appendNode(DataNodeHeader::Kind::Array, closed.indexingType, contents, { });
    m_words.shrink(closed.firstWord);
    m_fields.shrink(closed.firstField);
    m_words.append(DataWord::encodeNode(node));
}

uint32_t DataNodeEncoder::appendObject(const OpenNode& closed, std::span<const uint64_t> contents)
{
    const Placement& placement = m_encoder.placement;
    LayoutID layout = closed.node->as<DataNode::Object>().layout;
    std::span<const FieldIndex> fields = m_fields.span().subspan(closed.firstField);
    AllocationID allocation = placement.allocations().find(layout, placement.shapes().generalShapeOf(layout), fields);
    if (!allocation) {
        m_encoder.fail(ProgramTableRule::DataNotEncoded, layout);
        return 0;
    }
    Vector<uint16_t, 16> fieldIndices;
    for (FieldIndex field : fields) {
        const PlacedSlot& slot = m_encoder.slots.slot(layout, field);
        if (slot.hasStorage())
            fieldIndices.append(*slot.indexInLayoutRecord);
    }
    ASSERT(fieldIndices.size() == contents.size());
    return appendNode(DataNodeHeader::Kind::Object, allocation.index(), contents, fieldIndices.span());
}

uint32_t DataNodeEncoder::appendNode(DataNodeHeader::Kind kind, uint32_t operand, std::span<const uint64_t> contents, std::span<const uint16_t> fieldIndices)
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
    for (uint32_t index = 0; index < count; ++index)
        writer.at<uint64_t>(contentsOffset + index * wordSize) = contents[index];

    uint32_t fieldIndicesOffset = contentsOffset + count * wordSize;
    for (uint32_t index = 0; index < fieldIndices.size(); ++index)
        writer.at<uint16_t>(fieldIndicesOffset + index * static_cast<uint32_t>(sizeof(uint16_t))) = fieldIndices[index];

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
    std::ranges::sort(strings);
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

void encodeDataTable(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    encoder.dataNodes.fill(UINT32_MAX, program.data.size());
    BitVector recordsWithNodes = findRecordsWithNodes(encoder);

    DataNodeEncoder nodes(encoder);
    for (size_t index : recordsWithNodes) {
        const DataNode& root = program.data[index].root;
        // TableEncoder::dataNodeOf() reports a record that a request names and that has no node.
        if (!isLeaf(root))
            encoder.dataNodes[index] = nodes.encode(root);
    }
    encoder.setTableRange(ProgramTableIndex::data, nodes.tableOffset(), nodes.numberOfWords());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
