/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTCellFormat.h"
#include "AOTCompiledCode.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTNameClass.h"
#include "AOTOperationData.h"
#include "AOTPlan.h"
#include "AOTSlotAccess.h"
#include "AOTTableInImage.h"
#include "AOTTextEncoder.h"
#include "AOTTypeClosure.h"
#include "AOTVector.h"
#include "IndexingHeader.h"
#include "IndexingType.h"
#include "JSCJSValueInlines.h"
#include "PureNaN.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static constexpr FieldIndex noFieldOfDataMember = std::numeric_limits<FieldIndex>::max();

namespace {

enum class FormOfDataObject : uint8_t {
    Compiled,
    Ordinary,
    Unplanned,
};

struct DataObjectAllocationRow {
    std::optional<Typed::PlacedAllocation> allocation;
    uint32_t firstField { 0 };
    uint32_t numberOfFields { 0 };
    uint32_t firstKeyWord { 0 };
    uint32_t numberOfKeyWords { 0 };
    uint32_t nextRowPlusOne { 0 };
    bool isNumbered { false };
    std::optional<uint32_t> number;
};

class DataObjectAllocationRowCache {
    WTF_MAKE_NONCOPYABLE(DataObjectAllocationRowCache);
public:
    explicit DataObjectAllocationRowCache(const TableInputs& inputs)
        : m_inputs(inputs)
    {
        m_firstRowPlusOneOfLayout.fill(0, inputs.program.layouts.size());
    }

    std::span<const FieldIndex> fieldsOf(const DataObjectAllocationRow& row) const { return m_fields.span().subspan(row.firstField, row.numberOfFields); }
    std::span<const uint32_t> keyWordsOf(const DataObjectAllocationRow& row) const { return m_keyWords.span().subspan(row.firstKeyWord, row.numberOfKeyWords); }

    DataObjectAllocationRow& rowFor(LayoutID layout, std::span<const FieldIndex> fieldsInOrderOfMembers)
    {
        uint32_t& firstRowPlusOne = m_firstRowPlusOneOfLayout[layout.index()];
        for (uint32_t rowPlusOne = firstRowPlusOne; rowPlusOne; rowPlusOne = m_rows[rowPlusOne - 1].nextRowPlusOne) {
            if (std::ranges::equal(fieldsOf(m_rows[rowPlusOne - 1]), fieldsInOrderOfMembers))
                return m_rows[rowPlusOne - 1];
        }

        DataObjectAllocationRow row;
        row.firstField = static_cast<uint32_t>(m_fields.size());
        row.numberOfFields = static_cast<uint32_t>(fieldsInOrderOfMembers.size());
        m_fields.append(fieldsInOrderOfMembers);
        row.firstKeyWord = static_cast<uint32_t>(m_keyWords.size());
        row.allocation = m_inputs.plan.allocation(m_inputs.program, layout, fieldsInOrderOfMembers);
        if (row.allocation) {
            m_mentionsOfOneRow.mentions.shrink(0);
            m_mentionsOfOneRow.mentionWords.shrink(0);
            m_mentions.allocation(*row.allocation, fieldsInOrderOfMembers);
            m_mentions.moveTo(m_mentionsOfOneRow);
            m_keyWords.append(keyOf(m_mentionsOfOneRow.mentions.last(), m_mentionsOfOneRow.mentionWords.span()));
        }
        row.numberOfKeyWords = static_cast<uint32_t>(m_keyWords.size()) - row.firstKeyWord;
        row.nextRowPlusOne = firstRowPlusOne;
        m_rows.append(row);
        firstRowPlusOne = static_cast<uint32_t>(m_rows.size());
        return m_rows.last();
    }

    void appendKeysTo(Vector<uint32_t>& keys) const
    {
        for (const DataObjectAllocationRow& row : m_rows) {
            if (!row.allocation)
                continue;
            keys.append(row.numberOfKeyWords);
            keys.append(keyWordsOf(row));
        }
    }

private:
    const TableInputs& m_inputs;
    Vector<uint32_t> m_firstRowPlusOneOfLayout;
    Vector<DataObjectAllocationRow> m_rows;
    Vector<FieldIndex> m_fields;
    Vector<uint32_t> m_keyWords;
    MentionList m_mentions;
    CodeBody m_mentionsOfOneRow;
};

} // anonymous namespace

static bool isLeafOfDataTree(const Typed::DataNode& node)
{
    switch (node.kind()) {
    case Typed::DataNode::Kind::Undefined:
    case Typed::DataNode::Kind::Null:
    case Typed::DataNode::Kind::Hole:
    case Typed::DataNode::Kind::Boolean:
    case Typed::DataNode::Kind::Number:
    case Typed::DataNode::Kind::String:
    case Typed::DataNode::Kind::BigInt:
        return true;
    case Typed::DataNode::Kind::Array:
    case Typed::DataNode::Kind::Object:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static IndexingType indexingTypeOfDataArray(const Typed::Program& program, const Typed::DataNode::Array& array)
{
    return Typed::arrayStorageFormOf(Typed::arrayStorageOfData(program, array)).indexingType;
}

static FormOfDataObject formOfDataObject(const Typed::Plan& plan, const Typed::DataNode::Object& object)
{
    if (!object.layout)
        return FormOfDataObject::Ordinary;
    const Typed::PlannedLayout& layout = plan.layout(object.layout);
    switch (layout.storage) {
    case Typed::LayoutStorage::CompiledObject:
        return layout.isRecordAlone ? FormOfDataObject::Unplanned : FormOfDataObject::Compiled;
    case Typed::LayoutStorage::OrdinaryObject:
        return FormOfDataObject::Ordinary;
    case Typed::LayoutStorage::None:
    case Typed::LayoutStorage::Environment:
        return FormOfDataObject::Unplanned;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FieldIndex fieldOfDataMember(const TableInputs& inputs, FormOfDataObject form, const Typed::DataNode::Object& object, const Typed::DataMember& member, bool& sawNamedMemberOutsideFields)
{
    if (form != FormOfDataObject::Compiled || sawNamedMemberOutsideFields)
        return noFieldOfDataMember;
    FieldIndex field = inputs.plan.slots().findField(inputs.program, object.layout, member.key).value_or(noFieldOfDataMember);
    sawNamedMemberOutsideFields = field == noFieldOfDataMember && inputs.program.nameClassOf(member.key) != Typed::NameClass::Index;
    return field;
}

static bool representationTakesDataMember(SlotRepresentation representation, const Typed::DataNode& value)
{
    SlotTraits traits = traitsOf(representation);
    bool defersClosure = traits.valueKind == SlotValueKind::DeferredClosure;
    uint64_t bytesOfSlot = 0;
    switch (value.kind()) {
    case Typed::DataNode::Kind::Undefined:
        return traits.hasUndefined || defersClosure;
    case Typed::DataNode::Kind::Null:
        return traits.hasNull || defersClosure;
    case Typed::DataNode::Kind::Boolean:
        return tryWriteBoolean(representation, &bytesOfSlot, value.as<Typed::DataNode::Boolean>().value);
    case Typed::DataNode::Kind::Number:
        return tryWriteDouble(representation, &bytesOfSlot, value.as<Typed::DataNode::Number>().value);
    case Typed::DataNode::Kind::String:
    case Typed::DataNode::Kind::BigInt:
    case Typed::DataNode::Kind::Array:
    case Typed::DataNode::Kind::Object:
        return traits.holdsCell;
    case Typed::DataNode::Kind::Hole:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void forEachRequestForData(const NumberedKeys& requests, NOESCAPE const Invocable<void(DataID, RequestKind, uint32_t)> auto& functor)
{
    for (uint32_t place = 0; place < requests.size(); ++place) {
        std::span<const uint32_t> key = requests.keyAt(place);
        auto kind = static_cast<RequestKind>(key[0]);
        switch (kind) {
        case RequestKind::Data:
        case RequestKind::CollectionData:
            functor(DataID(key[1]), kind, key[2]);
            break;
        case RequestKind::NewClass:
        case RequestKind::TemplateObject:
        case RequestKind::DirectEval:
        case RequestKind::FormatPattern:
        case RequestKind::ImportChecks:
            break;
        }
    }
}

static BitVector findDataWithNodes(const Typed::Program& program, const NumberedKeys& requests, NOESCAPE const Invocable<bool(DataID)> auto& hasWordInStatics)
{
    BitVector data(program.data.size());
    forEachRequestForData(requests, [&](DataID id, RequestKind, uint32_t) {
        data.set(id.index());
    });
    for (uint32_t index = 0; index < program.data.size(); ++index) {
        if (hasWordInStatics(DataID(index)))
            data.set(index);
    }
    return data;
}

static void nameWhatDataTreeNames(const TableInputs& inputs, const Typed::DataNode& root, DataObjectAllocationRowCache& rows, NamesOfTables& names)
{
    auto nameString = [&](StringID string) {
        names.strings.set(string.index());
        names.stringsWithOtherUsesThanShowing.set(string.index());
    };
    Vector<FieldIndex, 16> fields;
    Vector<const Typed::DataNode*, 32> nodesToVisit { &root };
    while (!nodesToVisit.isEmpty()) {
        const Typed::DataNode& node = *nodesToVisit.takeLast();
        switch (node.kind()) {
        case Typed::DataNode::Kind::Undefined:
        case Typed::DataNode::Kind::Null:
        case Typed::DataNode::Kind::Hole:
        case Typed::DataNode::Kind::Boolean:
        case Typed::DataNode::Kind::Number:
            break;
        case Typed::DataNode::Kind::String:
            nameString(node.as<Typed::DataNode::String>().value);
            names.stringConstants.set(node.as<Typed::DataNode::String>().value.index());
            break;
        case Typed::DataNode::Kind::BigInt:
            nameString(node.as<Typed::DataNode::BigInt>().digits);
            names.bigInts.set(node.as<Typed::DataNode::BigInt>().digits.index());
            break;
        case Typed::DataNode::Kind::Array:
            for (const Typed::DataNode& element : node.as<Typed::DataNode::Array>().elements)
                nodesToVisit.append(&element);
            break;
        case Typed::DataNode::Kind::Object: {
            auto& object = node.as<Typed::DataNode::Object>();
            FormOfDataObject form = formOfDataObject(inputs.plan, object);
            fields.shrink(0);
            bool sawNamedMemberOutsideFields = false;
            for (const Typed::DataMember& member : object.members) {
                FieldIndex field = fieldOfDataMember(inputs, form, object, member, sawNamedMemberOutsideFields);
                if (field == noFieldOfDataMember)
                    names.keys.set(member.key.index());
                else
                    fields.append(field);
                if (field == noFieldOfDataMember || inputs.plan.slot(object.layout, field).hasStorage())
                    nodesToVisit.append(&member.value);
            }
            if (form == FormOfDataObject::Compiled)
                rows.rowFor(object.layout, fields.span());
            break;
        }
        }
    }
}

void nameWhatDataTableNames(const TableInputs& inputs, const GatheredMentions& mentions, NamesOfTables& names)
{
    BitVector data = findDataWithNodes(inputs.program, mentions.keys(NumberSpace::Request), [&](DataID id) {
        return mentions.isMentioned(NumberSpace::DataStorage, id.index());
    });
    DataObjectAllocationRowCache rows(inputs);
    for (size_t index : data) {
        const Typed::DataNode& root = inputs.program.data[index].root;
        if (!isLeafOfDataTree(root))
            nameWhatDataTreeNames(inputs, root, rows, names);
    }
    rows.appendKeysTo(names.allocations);
}

namespace {

struct OpenDataNode {
    const Typed::DataNode* node { nullptr };
    FormOfDataObject form { FormOfDataObject::Unplanned };
    IndexingType indexingType { ArrayWithContiguous };
    uint32_t numberOfChildren { 0 };
    uint32_t nextChild { 0 };
    uint32_t firstWord { 0 };
    uint32_t firstField { 0 };
};

class DataTableWriter {
    WTF_MAKE_NONCOPYABLE(DataTableWriter);
public:
    explicit DataTableWriter(TableEncoder& encoder)
        : m_encoder(encoder)
        , m_rows(encoder.inputs)
    {
        static_assert(DataNodeHeader::firstNode == 1);
        m_table.append(0);
    }

    const Vector<uint64_t>& table() const { return m_table; }

    uint32_t encode(const Typed::DataNode& root)
    {
        ASSERT(m_openNodes.isEmpty() && m_words.isEmpty() && m_fields.isEmpty());
        openNode(root);
        while (!m_openNodes.isEmpty()) {
            OpenDataNode& innermost = m_openNodes.last();
            if (innermost.nextChild == innermost.numberOfChildren) {
                closeNode(m_openNodes.takeLast());
                continue;
            }
            uint32_t index = innermost.nextChild++;
            visitChild(innermost, index);
        }
        return DataWord::operandOf(m_words.takeLast());
    }

    std::optional<Vector<uint8_t>> tryEncodeTableInImage(const Typed::DataNode::Array& entries, bool hasValues) const
    {
        Vector<TableInImage::Key> keys;
        Vector<uint64_t> values;
        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> entryOfString;
        for (const Typed::DataNode& entry : entries.elements) {
            const auto* key = (hasValues ? entry.as<Typed::DataNode::Array>().elements[0] : entry).tryAs<Typed::DataNode::String>();
            const Typed::DataNode* value = hasValues ? &entry.as<Typed::DataNode::Array>().elements[1] : nullptr;
            if (!key || (value && (!isLeafOfDataTree(*value) || value->is<Typed::DataNode::Hole>())))
                return std::nullopt;
            auto place = entryOfString.add(key->value.index(), static_cast<uint32_t>(keys.size()));
            if (place.isNewEntry) {
                keys.append(TableInImage::Key { m_encoder.stringNumber(key->value), decodeForImage(m_encoder.program.string(key->value)).hash, m_encoder.stringConstantNumber(key->value) });
                if (value)
                    values.append(0);
            }
            if (value)
                values[place.iterator->value] = encodeLeaf(*value);
        }
        return TableInImage::tryEncode(keys.span(), values.span());
    }

private:
    COLD void fail(ASCIILiteral rule, LayoutID layout = { }) const { m_encoder.fail(rule, layout); }

    void openNode(const Typed::DataNode& node)
    {
        OpenDataNode opened { .node = &node, .firstWord = static_cast<uint32_t>(m_words.size()), .firstField = static_cast<uint32_t>(m_fields.size()) };
        if (const auto* array = node.tryAs<Typed::DataNode::Array>()) {
            opened.indexingType = indexingTypeOfDataArray(m_encoder.program, *array);
            opened.numberOfChildren = array->elements.size();
        } else {
            auto& object = node.as<Typed::DataNode::Object>();
            opened.form = formOfDataObject(m_encoder.plan, object);
            bool sawNamedMemberOutsideFields = false;
            for (const Typed::DataMember& member : object.members)
                m_fields.append(fieldOfDataMember(m_encoder.inputs, opened.form, object, member, sawNamedMemberOutsideFields));
            opened.numberOfChildren = object.members.size();
        }
        m_openNodes.append(opened);
    }

    void visitChild(OpenDataNode parent, uint32_t index)
    {
        const Typed::DataNode* value;
        if (const auto* array = parent.node->tryAs<Typed::DataNode::Array>()) {
            value = &array->elements[index];
            if (parent.indexingType == ArrayWithDouble) {
                m_words.append(bitsOfDoubleElement(*value));
                return;
            }
        } else {
            auto& object = parent.node->as<Typed::DataNode::Object>();
            FieldIndex field = m_fields[parent.firstField + index];
            if (field != noFieldOfDataMember && !m_encoder.plan.slot(object.layout, field).hasStorage())
                return;
            value = &object.members[index].value;
        }
        if (isLeafOfDataTree(*value))
            m_words.append(encodeLeaf(*value));
        else
            openNode(*value);
    }

    void closeNode(const OpenDataNode& closed)
    {
        std::span<const uint64_t> contents = m_words.span().subspan(closed.firstWord);
        uint32_t node;
        if (const auto* array = closed.node->tryAs<Typed::DataNode::Array>())
            node = appendNode(DataNodeHeader::Kind::Array, closed.indexingType | (Typed::isStorageOfArray(*array) ? DataNodeHeader::isStorage : 0), contents);
        else
            node = appendObject(closed.node->as<Typed::DataNode::Object>(), closed.form, m_fields.span().subspan(closed.firstField), contents);
        m_words.shrink(closed.firstWord);
        m_fields.shrink(closed.firstField);
        m_words.append(DataWord::encodeNode(node));
    }

    uint64_t bitsOfDoubleElement(const Typed::DataNode& element) const
    {
        if (const auto* number = element.tryAs<Typed::DataNode::Number>(); number && number->value == number->value)
            return std::bit_cast<uint64_t>(number->value);
        if (!element.is<Typed::DataNode::Hole>())
            fail("an element of constant data has no place in the storage for doubles that its array was given"_s);
        return std::bit_cast<uint64_t>(PNaN);
    }

    uint64_t encodeLeaf(const Typed::DataNode& leaf) const
    {
        switch (leaf.kind()) {
        case Typed::DataNode::Kind::Undefined:
            return DataWord::encodeImmediate(jsUndefined());
        case Typed::DataNode::Kind::Null:
            return DataWord::encodeImmediate(jsNull());
        case Typed::DataNode::Kind::Hole:
            return DataWord::hole;
        case Typed::DataNode::Kind::Boolean:
            return DataWord::encodeImmediate(jsBoolean(leaf.as<Typed::DataNode::Boolean>().value));
        case Typed::DataNode::Kind::Number:
            return DataWord::encodeImmediate(jsNumber(withoutPayload(leaf.as<Typed::DataNode::Number>().value)));
        case Typed::DataNode::Kind::String:
            return DataWord::encodeStringConstant(m_encoder.stringConstantNumber(leaf.as<Typed::DataNode::String>().value));
        case Typed::DataNode::Kind::BigInt: {
            uint32_t digits = leaf.as<Typed::DataNode::BigInt>().digits.index();
            if (std::optional<uint32_t> number = m_encoder.numbers.numberOf(NumberSpace::BigInt, singleElementSpan(digits)))
                return DataWord::encodeBigIntConstant(*number);
            fail("a BigInt of constant data has no number among the constants"_s);
            return DataWord::hole;
        }
        case Typed::DataNode::Kind::Array:
        case Typed::DataNode::Kind::Object:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    const DataObjectAllocationRow& numberedRowFor(LayoutID layout, std::span<const FieldIndex> fields)
    {
        DataObjectAllocationRow& row = m_rows.rowFor(layout, fields);
        if (row.isNumbered)
            return row;
        row.isNumbered = true;
        if (!row.allocation) {
            fail("no key order of the image begins with the fields of an object of constant data"_s, layout);
            return row;
        }
        row.number = m_encoder.numbers.numberOf(NumberSpace::Allocation, m_rows.keyWordsOf(row));
        if (!row.number)
            fail("the table of allocations has no row for an object of constant data"_s, layout);
        return row;
    }

    bool isPlainPropertyInSlot(Typed::FieldReference field, const Typed::FieldFacts& facts) const
    {
        if (facts.facts.contains(Typed::FieldFact::IsAccessor) || !facts.formOfValue.isEmpty())
            return false;
        const Typed::Program& program = m_encoder.program;
        if (field.field >= program.at(field.layout).fields.size())
            return true;
        return Typed::fieldAt(program, Typed::declarationOf(program, field)).form.isEmpty();
    }

    bool slotTakesMember(const Typed::FieldFacts& facts, const Typed::PlacedSlot& slot, const Typed::DataNode& value) const
    {
        if (!representationTakesDataMember(slot.representation, value))
            return false;
        Typed::TypeReader types(m_encoder.program);
        if (slot.type && !Typed::includesData(types, types.at(slot.type), value))
            return false;
        if (slot.provenType && !Typed::includesData(types, types.at(slot.provenType), value))
            return false;
        if (!facts.slotType || !slot.type)
            return true;
        bool isUndefinedOrNull = value.is<Typed::DataNode::Undefined>() || value.is<Typed::DataNode::Null>();
        if (isUndefinedOrNull && facts.facts.contains(Typed::FieldFact::SlotHoldsUndefinedAndNull))
            return true;
        return Typed::includesData(types, types.at(facts.slotType), value);
    }

    uint32_t appendObject(const Typed::DataNode::Object& object, FormOfDataObject form, std::span<const FieldIndex> fieldOfEachMember, std::span<const uint64_t> contents)
    {
        LayoutID layout = object.layout;
        if (form == FormOfDataObject::Unplanned) {
            fail("the plan keeps no objects of the layout of an object of constant data"_s, layout);
            return 0;
        }

        Vector<FieldIndex, 16> fields;
        Vector<uint16_t, 16> fieldIndices;
        Vector<uint64_t, 16> fieldValues;
        Vector<uint32_t, 16> otherKeys;
        Vector<uint64_t, 16> otherValues;
        bool sawNamedMemberOutsideFields = false;
        size_t nextWord = 0;
        for (size_t index = 0; index < fieldOfEachMember.size(); ++index) {
            FieldIndex field = fieldOfEachMember[index];
            PropertyKeyID key = object.members[index].key;
            if (field == noFieldOfDataMember) {
                sawNamedMemberOutsideFields |= m_encoder.program.nameClassOf(key) != Typed::NameClass::Index;
                otherKeys.append(m_encoder.identifierNumber(key, "a member of constant data outside the fields of"_s, layout));
                otherValues.append(contents[nextWord++]);
                continue;
            }
            if (sawNamedMemberOutsideFields) {
                fail("an object of constant data has a member outside its fields in front of a field, an order of keys that no node holds"_s, layout);
                return 0;
            }
            fields.append(field);
            const Typed::PlacedSlot& slot = m_encoder.plan.slot(layout, field);
            if (!slot.hasStorage())
                continue;
            if (!slot.indexInLayoutRecord) {
                fail("a field of constant data has storage and no record in the layout"_s, layout);
                return 0;
            }
            Typed::FieldFacts facts = m_encoder.facts.field({ layout, field });
            if (!isPlainPropertyInSlot({ layout, field }, facts)) {
                fail("a member of an object of constant data is a plain property, which its field is not while it lies in its slot"_s, layout);
                return 0;
            }
            if (!slotTakesMember(facts, slot, object.members[index].value)) {
                fail("a member of an object of constant data does not fit the slot of its field, which the run time fills without a test"_s, layout);
                return 0;
            }
            fieldIndices.append(*slot.indexInLayoutRecord);
            fieldValues.append(contents[nextWord++]);
        }
        ASSERT(nextWord == contents.size());
        if (form == FormOfDataObject::Ordinary)
            return appendNode(DataNodeHeader::Kind::OrdinaryObject, 0, otherValues.span(), asBytes(otherKeys.span()));

        const DataObjectAllocationRow& row = numberedRowFor(layout, fields.span());
        if (!row.number)
            return 0;
        if (otherKeys.isEmpty()) {
            if (std::optional<uint32_t> node = tryAppendObjectBody(object, *row.allocation, *row.number, fieldOfEachMember, contents))
                return *node;
            return appendNode(DataNodeHeader::Kind::Object, *row.number, fieldValues.span(), asBytes(fieldIndices.span()));
        }
        uint32_t otherMembers = appendNode(DataNodeHeader::Kind::OtherMembers, 0, otherValues.span(), asBytes(otherKeys.span()));
        return appendNode(DataNodeHeader::Kind::ObjectWithOtherMembers, *row.number, fieldValues.span(), asBytes(fieldIndices.span()), otherMembers);
    }

    bool tryWriteDataInSlot(const Typed::PlacedSlot& slot, void* bytes, const Typed::DataNode& value, uint64_t word) const
    {
        SlotRepresentation representation = slot.representation;
        SlotTraits traits = traitsOf(representation);
        switch (value.kind()) {
        case Typed::DataNode::Kind::Undefined:
            if (traits.hasUndefined)
                writeSlotBits(representation, bytes, traits.undefinedBits);
            return traits.hasUndefined;
        case Typed::DataNode::Kind::Null:
            if (traits.hasNull)
                writeSlotBits(representation, bytes, traits.nullBits);
            return traits.hasNull;
        case Typed::DataNode::Kind::Boolean:
            return tryWriteBoolean(representation, bytes, value.as<Typed::DataNode::Boolean>().value);
        case Typed::DataNode::Kind::Number:
            return tryWriteDouble(representation, bytes, withoutPayload(value.as<Typed::DataNode::Number>().value));
        case Typed::DataNode::Kind::String:
        case Typed::DataNode::Kind::BigInt:
        case Typed::DataNode::Kind::Array:
        case Typed::DataNode::Kind::Object:
            break;
        case Typed::DataNode::Kind::Hole:
            return false;
        }

        if (!traits.holdsCell || traits.valueKind == SlotValueKind::DeferredClosure)
            return false;
        if (traits.isNarrowReference && DataWord::operandOf(word) > NarrowDataWord::maxOperand)
            return false;
        writeSlotBits(representation, bytes, traits.isNarrowReference ? NarrowDataWord::encode(DataWord::kindOf(word), DataWord::operandOf(word)) : word);
        return true;
    }

    std::optional<uint32_t> tryAppendObjectBody(const Typed::DataNode::Object& object, const Typed::PlacedAllocation& allocation, uint32_t allocationNumber, std::span<const FieldIndex> fieldOfEachMember, std::span<const uint64_t> contents)
    {
        const Typed::PlannedLayout& layout = m_encoder.plan.layout(object.layout);
        uint32_t size = allocation.size;
        if (size != layout.size || size < offsetOfFirstField || size % sizeof(uint64_t))
            return std::nullopt;

        Vector<uint64_t, 16> body(FillWith { }, (size - offsetOfFirstField) / sizeof(uint64_t), 0);
        auto bytesOf = [&](const Typed::PlacedSlot& slot) -> void* {
            RELEASE_ASSERT(slot.offset >= offsetOfFirstField && slot.end() <= size);
            return asMutableByteSpan(body.mutableSpan()).subspan(slot.offset - offsetOfFirstField, slot.width()).data();
        };
        for (FieldIndex field = 0; field < layout.numberOfFields(); ++field) {
            const Typed::PlacedSlot& slot = layout.slots[field];
            if (slot.hasStorage() && !slot.isOutOfLine())
                clearSlot(slot.representation, bytesOf(slot));
        }

        size_t nextWord = 0;
        for (size_t index = 0; index < fieldOfEachMember.size(); ++index) {
            FieldIndex field = fieldOfEachMember[index];
            const Typed::PlacedSlot& slot = layout.slots[field];
            if (!slot.hasStorage())
                continue;
            const Typed::DataNode& value = object.members[index].value;
            bool setsMarkOfCell = (value.is<Typed::DataNode::Undefined>() || value.is<Typed::DataNode::Null>()) && layout.holdsRawValueFromBirth(field);
            if (slot.isOutOfLine() || setsMarkOfCell || !tryWriteDataInSlot(slot, bytesOf(slot), value, contents[nextWord++]))
                return std::nullopt;
        }
        return appendNode(DataNodeHeader::Kind::ObjectBody, allocationNumber, body.span());
    }

    uint32_t appendNode(DataNodeHeader::Kind kind, uint32_t operand, std::span<const uint64_t> contents, std::span<const uint8_t> behindContents = { }, std::optional<uint32_t> otherMembers = std::nullopt)
    {
        if (m_table.size() + 2 * contents.size() + 4 > std::numeric_limits<uint32_t>::max()) {
            fail("the table of constant data has more words than the number of a node counts"_s);
            return 0;
        }
        uint32_t count = static_cast<uint32_t>(contents.size());
        DataNodeHeader header { };
        header.count = count;
        header.kindBits = static_cast<uint32_t>(kind);
        header.operand = operand;
        if (header.operand != operand) {
            fail("the operand of a node of constant data does not fit its header"_s);
            return 0;
        }

        uint32_t first = static_cast<uint32_t>(m_table.size());
        m_table.grow(first + DataNodeHeader::numberOfWords(kind, count));
        std::span<uint64_t> node = m_table.mutableSpan().subspan(first);
        zeroSpan(node);
        node[0] = header.encoded();
        size_t firstOfContents = 1;
        if (kind == DataNodeHeader::Kind::Array) {
            static_assert(!IndexingHeader::offsetOfPublicLength() && IndexingHeader::offsetOfVectorLength() == sizeof(uint32_t) && sizeof(IndexingHeader) == sizeof(uint64_t));
            static_assert(DataNodeHeader::wordsBeforeElements == 2);
            node[firstOfContents++] = count | static_cast<uint64_t>(count) << 32;
        }
        memcpySpan(node.subspan(firstOfContents), contents);
        memcpySpan(asMutableByteSpan(node.subspan(firstOfContents + count)), behindContents);
        if (otherMembers)
            node.back() = DataWord::encodeNode(*otherMembers);
        return first;
    }

    TableEncoder& m_encoder;
    DataObjectAllocationRowCache m_rows;
    Vector<uint64_t> m_table;
    Vector<OpenDataNode, 16> m_openNodes;
    Vector<uint64_t> m_words;
    Vector<FieldIndex> m_fields;
};

} // anonymous namespace

void encodeDataTable(TableEncoder& encoder, FirstTableParts& firstParts)
{
    const Typed::Program& program = encoder.program;
    const NumberedKeys& requests = encoder.numbers.requests();
    const PlacedStatics& statics = encoder.numbers.statics();
    auto hasOffsetIn = [](const Vector<uint32_t>& offsets, DataID id) {
        return id.index() < offsets.size() && offsets[id.index()] != PlacedStatics::noOffset;
    };
    BitVector data(program.data.size());
    for (uint32_t index = 0; index < program.data.size(); ++index) {
        if (hasOffsetIn(statics.dataStorageOffsets, DataID(index)))
            data.set(index);
    }

    firstParts.tablesInImage.fill(0, requests.size());
    DataTableWriter writer(encoder);
    for (uint32_t place = 0; place < requests.size(); ++place) {
        std::span<const uint32_t> key = requests.keyAt(place);
        auto kind = static_cast<RequestKind>(key[0]);
        if (kind == RequestKind::Data)
            data.set(key[1]);
        if (kind != RequestKind::CollectionData)
            continue;
        bool hasValues = key[2];
        const auto* entries = program.at(DataID(key[1])).root.tryAs<Typed::DataNode::Array>();
        auto isPair = [](const Typed::DataNode& entry) {
            const auto* pair = entry.tryAs<Typed::DataNode::Array>();
            return pair && pair->elements.size() == 2;
        };
        if (!entries || (hasValues && !std::ranges::all_of(entries->elements, isPair))) {
            encoder.fail("the constant data of a collection is no list of its entries"_s);
            continue;
        }
        if (std::optional<Vector<uint8_t>> table = writer.tryEncodeTableInImage(*entries, hasValues))
            firstParts.tablesInImage[place] = encoder.part.append(table->span(), alignof(TableInImage));
        else
            data.set(key[1]);
    }

    firstParts.dataNodes.fill(0, program.data.size());
    for (size_t index : data) {
        const Typed::DataNode& root = program.data[index].root;
        if (!isLeafOfDataTree(root))
            firstParts.dataNodes[index] = writer.encode(root);
    }
    encoder.part.setTable(ProgramTableIndex::data, writer.table());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
