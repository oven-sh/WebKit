/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTPropertyCachesWords.h"
#include "AOTFieldTable.h"
#include "AOTRuntimeRecords.h"
#include "AOTSlotAccess.h"
#include "AOTSlotRepresentation.h"
#include "AOTSlowPaths.h"
#include "AOTStubRunner.h"
#include "AOTStubsPropertyKeys.h"
#include "GetterSetter.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include "MarkedBlockCage.h"
#include "StructureID.h"
#include <array>
#include <bit>
#include <optional>
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static uint64_t bitsOf(JSValue value) { return static_cast<uint64_t>(JSValue::encode(value)); }

static constexpr uint64_t untouched = 0xa5a5a5a5a5a5a5a5ULL;

namespace {

class PropertyReads {
    WTF_MAKE_NONCOPYABLE(PropertyReads);
public:
    static constexpr uint16_t name = 4;
    static constexpr unsigned numberOfNames = 8;
    static constexpr uint32_t structureID = 0x1000;

    PropertyReads()
    {
        Instance& instance = runner.instance();
        for (SlotAccess access : allSlotAccesses)
            instance.propertyCaches.front[static_cast<unsigned>(access)] = { m_rows.data(), m_planes[static_cast<unsigned>(access)].data() };
        instance.propertyCaches.hashedSlots = m_hashedSlots.data();
        instance.propertyCaches.hashedSlotOffsetMask = HashedSlot::offsetMaskFor(m_hashedSlots.size());
        for (unsigned i = 0; i < numberOfNames; ++i)
            m_identifiers[i] = std::bit_cast<UniquedStringImpl*>(m_uids[i].address());
        instance.image.programIdentifiers = m_identifiers.data();
        instance.cellCageBase = MarkedBlockCage::baseOf(this);
    }

    void file(const Slot& slot)
    {
        m_hashedSlots.fill(HashedSlot { });
        uint32_t offset = HashedSlot::offsetOfFirstWay(slot.structureID, m_identifiers[name], runner.instance().propertyCaches.hashedSlotOffsetMask);
        HashedSlot& entry = m_hashedSlots[offset >> HashedSlot::sizeShift];
        entry.slot = slot;
        entry.key = HashedSlot::keyFor(m_identifiers[name], SlotAccess::Read);
    }

    StubRun read(uint64_t receiver) { return runner.run(getById(), { receiver }, name); }
    StubRun read(uint64_t receiver, uint16_t otherName) { return runner.run(getById(), { receiver }, otherName); }

    StubRunner runner;

private:
    std::array<uint32_t, (structureID >> StructureRowWords::granuleShift) + 1> m_rows { };
    std::array<std::array<PackedSlot, numberOfNames>, numberOfSlotAccesses> m_planes { };
    std::array<HashedSlot, 64> m_hashedSlots { };
    std::array<FakeObject<16>, numberOfNames> m_uids;
    std::array<UniquedStringImpl*, numberOfNames> m_identifiers { };
};

struct FakeTypedObject : FakeObject<offsetOfFirstField + 2 * sizeof(uint64_t)> {
    static constexpr uint32_t offsetOfField = offsetOfFirstField;

    FakeTypedObject()
    {
        setHeader(PropertyReads::structureID, CompiledObjectType);
        field() = untouched;
    }

    uint64_t& field() { return at<uint64_t>(offsetOfField); }
};

} // anonymous namespace

static Slot createFieldSlot(SlotRepresentation representation)
{
    return Slot::createField(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::Field, Slot::isOwn, SlotData::create(FakeTypedObject::offsetOfField, representation), 0, 0);
}

static bool tryWrite(SlotRepresentation representation, void* slot, JSValue value, uintptr_t cageBase)
{
    SlotTraits traits = traitsOf(representation);
    if (traits.valueKind == SlotValueKind::DeferredClosure)
        return false;
    if (traits.valueKind == SlotValueKind::Boxed) {
        *static_cast<uint64_t*>(slot) = bitsOf(value);
        return true;
    }
    if (value.isCell()) {
        if (!traits.holdsCell)
            return false;
        if (traits.isNarrowReference)
            *static_cast<uint32_t*>(slot) = MarkedBlockCage::encode(cageBase, value.asCell());
        else
            *static_cast<uint64_t*>(slot) = bitsOf(value);
        return true;
    }
    if (value.isInt32())
        return tryWriteInt32(representation, slot, value.asInt32());
    if (value.isDouble())
        return traits.valueKind != SlotValueKind::Int32OrDouble && tryWriteDouble(representation, slot, value.asDouble());
    if (value.isBoolean())
        return tryWriteBoolean(representation, slot, value.asBoolean());
    return false;
}

static void testTypedFields()
{
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);
    AOT_TEST_CHECK(MarkedBlockCage::baseOf(cell.bytes.data()) == cageBase);
    const JSValue values[] = {
        jsNumber(0), jsNumber(7), jsNumber(-1), jsNumber(200), jsNumber(-200), jsNumber(70000), jsNumber(INT32_MAX), jsNumber(INT32_MIN),
        jsDoubleNumber(0.5), jsDoubleNumber(1e300), jsDoubleNumber(-0.0), jsDoubleNumber(4294967295.0),
        jsBoolean(true), jsBoolean(false), JSValue(std::bit_cast<JSCell*>(cell.address())),
    };
    unsigned representationsRead = 0;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        reads.file(createFieldSlot(representation));
        bool wasRead = false;
        for (JSValue value : values) {
            FakeTypedObject object;
            if (!tryWrite(representation, &object.field(), value, cageBase))
                continue;
            StubRun run = reads.read(object.address());
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(value));
            wasRead = true;
        }
        representationsRead += wasRead;
    }
    AOT_TEST_CHECK(representationsRead == numberOfEnumerators<SlotRepresentation> - 1);
}

namespace {

struct FakeHolder : FakeObject<JSObject::offsetOfInlineStorage() + 2 * sizeof(uint64_t)> {
    static constexpr PropertyOffset inlineOffset = 1;
    static constexpr PropertyOffset outOfLineOffset = firstOutOfLineOffset + 1;

    FakeHolder()
    {
        setHeader(0, FinalObjectType);
        at<uint64_t>(JSObject::butterflyOffset()) = std::bit_cast<uint64_t>(m_storage.data() + m_storage.size());
    }

    uint64_t& property(PropertyOffset offset)
    {
        int32_t wordIndex = Slot::wordIndexFor(offset);
        return isInlineOffset(offset) ? at<uint64_t>(wordIndex * sizeof(uint64_t)) : m_storage[m_storage.size() + wordIndex];
    }

private:
    std::array<uint64_t, 4> m_storage { };
};

} // anonymous namespace

static Slot createFieldOrInheritedSlot(SlotRepresentation representation, FakeHolder& holder, PropertyOffset offset, uint8_t flags, uintptr_t cageBase)
{
    SlotData field = SlotData::create(FakeTypedObject::offsetOfField, representation);
    return *Slot::tryCreateFieldAndHolder(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::FieldOrInherited, flags | Slot::dependsOnChain, field, std::bit_cast<JSCell*>(holder.address()), offset, cageBase);
}

static void testFieldsOrInheritedValues()
{
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeHolder holder;
    AOT_TEST_CHECK(MarkedBlockCage::baseOf(holder.bytes.data()) == cageBase);
    holder.property(FakeHolder::inlineOffset) = bitsOf(jsNumber(11));
    holder.property(FakeHolder::outOfLineOffset) = bitsOf(jsNumber(12));
    unsigned representationsRead = 0;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        SlotTraits traits = traitsOf(representation);
        if (!traits.hasAbsent || traits.valueKind == SlotValueKind::DeferredClosure)
            continue;
        for (PropertyOffset offset : { FakeHolder::inlineOffset, FakeHolder::outOfLineOffset }) {
            reads.file(createFieldOrInheritedSlot(representation, holder, offset, 0, cageBase));

            FakeTypedObject objectWithoutField;
            writeSlotBits(representation, &objectWithoutField.field(), traits.absentBits);
            StubRun inherited = reads.read(objectWithoutField.address());
            AOT_TEST_CHECK(!inherited.calledOperation && inherited.result == holder.property(offset));

            for (JSValue value : { jsNumber(7), jsBoolean(true) }) {
                FakeTypedObject object;
                if (!tryWrite(representation, &object.field(), value, cageBase))
                    continue;
                StubRun own = reads.read(object.address());
                AOT_TEST_CHECK(!own.calledOperation && own.result == bitsOf(value));
                ++representationsRead;
            }
        }
    }
    AOT_TEST_CHECK(representationsRead);
}

static void testFieldsInExtensionOrInheritedValues()
{
    constexpr uint32_t offsetOfExtensionWord = offsetOfFirstField + sizeof(uint64_t);
    constexpr uint32_t offsetInExtension = 8;
    PropertyReads reads;
    Instance& instance = reads.runner.instance();
    uintptr_t cageBase = instance.cellCageBase;
    FakeObject<sizeof(CompiledStructure)> structure;
    instance.prefix().structureIDBase = structure.address() - PropertyReads::structureID;
    structure.at<uint32_t>(CompiledStructure::offsetOfExtensionSlot()) = SlotData::create(offsetOfExtensionWord, SlotRepresentation::Cell32).encoded();
    FakeHolder holder;
    holder.property(FakeHolder::inlineOffset) = bitsOf(jsNumber(11));
    SlotData field = SlotData::create(offsetInExtension, SlotRepresentation::JSValue);
    reads.file(*Slot::tryCreateFieldAndHolder(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::FieldOrInherited, Slot::isOutOfLine | Slot::dependsOnChain, field, std::bit_cast<JSCell*>(holder.address()), FakeHolder::inlineOffset, cageBase));

    // The page of zeros that stands for "no Extension" belongs to a real cage. Here an Extension without a granule takes its place.
    FakeTypedObject object;
    FakeObject<offsetInExtension + sizeof(uint64_t)> extension;
    extension.at<uint64_t>(offsetOfExtensionGranuleBits) = 0;
    object.at<uint32_t>(offsetOfExtensionWord) = MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(extension.address() - offsetOfEmptyExtensionInCage));
    extension.at<uint64_t>(offsetInExtension) = bitsOf(jsNumber(7));
    StubRun run = reads.read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(11)));

    extension.at<uint64_t>(offsetOfExtensionGranuleBits) = granuleBitOf(offsetInExtension);
    run = reads.read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));

    extension.at<uint64_t>(offsetInExtension) = JSValue::ValueEmpty;
    run = reads.read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(11)));
}

static void testFieldsOrInheritedGetters()
{
    constexpr uint32_t slimClosureStructureID = 0x1080;
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    reads.runner.instance().slimClosureHeaders[0] = slimClosureStructureID;
    FakeObject<offsetOfSlimClosureWord + sizeof(uint64_t)> getter;
    getter.setHeader(slimClosureStructureID, SlimClosureType);
    getter.at<uint64_t>(offsetOfSlimClosureWord) = reads.runner.closureWord(0);
    FakeObject<sizeof(GetterSetter)> accessors;
    accessors.at<uint64_t>(GetterSetter::offsetOfGetter()) = getter.address();
    FakeHolder holder;
    holder.property(FakeHolder::inlineOffset) = accessors.address();
    reads.file(createFieldOrInheritedSlot(SlotRepresentation::JSValue, holder, FakeHolder::inlineOffset, Slot::inheritsGetter, cageBase));

    FakeTypedObject objectWithoutField;
    objectWithoutField.field() = JSValue::ValueEmpty;
    StubRun run = reads.read(objectWithoutField.address());
    AOT_TEST_CHECK(!run.calledOperation && run.landedCall.place == LandedCall::Place::CompiledFunction && run.result == StubRunner::resultOfCompiledFunction);
    AOT_TEST_CHECK(run.landedCall.values.callee == getter.address() && run.landedCall.values.thisValue == objectWithoutField.address() && !static_cast<uint32_t>(run.landedCall.count));

    FakeTypedObject object;
    object.field() = bitsOf(jsNumber(7));
    run = reads.read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.landedCall.place == LandedCall::Place::Nowhere && run.result == bitsOf(jsNumber(7)));
}

static void testDeferredClosures()
{
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeObject<16> closure;
    closure.setHeader(0, JSFunctionType);
    const Slot slot = createFieldSlot(SlotRepresentation::DeferredClosure);
    reads.file(slot);

    FakeTypedObject objectWithClosure;
    objectWithClosure.at<uint32_t>(FakeTypedObject::offsetOfField) = MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(static_cast<uintptr_t>(closure.address())));
    StubRun run = reads.read(objectWithClosure.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == closure.address());

    FakeTypedObject objectBeforeFirstRead;
    objectBeforeFirstRead.at<uint32_t>(FakeTypedObject::offsetOfField) = 0;
    reads.runner.recordCallsOf(Entry::operationAOTReadField);
    reads.runner.setResultOfRecordedOperation(closure.address());
    run = reads.read(objectBeforeFirstRead.address());
    reads.runner.countCallsOf(Entry::operationAOTReadField);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == closure.address());
    AOT_TEST_CHECK(run.recordedArguments[0] == objectBeforeFirstRead.address() && run.recordedArguments[1] == slot.headerBits() && run.recordedArguments[2] == slot.payloadBits());
    AOT_TEST_CHECK(!objectBeforeFirstRead.at<uint32_t>(FakeTypedObject::offsetOfField));
}

static void testTypedArrayLengths()
{
    constexpr uint64_t maxLengthRead = INT32_MAX;
    PropertyReads reads;
    StubRunner& runner = reads.runner;
    reads.file(Slot::createWithoutPayload(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::TypedArrayLength, 0));
    FakeObject<sizeof(JSArrayBufferView)> view;
    view.setHeader(PropertyReads::structureID, Uint8ArrayType);
    auto readLength = [&](TypedArrayMode mode, uint64_t length) {
        view.at<uint8_t>(JSArrayBufferView::offsetOfMode()) = mode;
        view.at<uint64_t>(JSArrayBufferView::offsetOfLength()) = length;
        return reads.read(view.address());
    };
    auto isLeftToOperation = [&](TypedArrayMode mode, uint64_t length) {
        runner.recordCallsOf(Entry::operationAOTGetById);
        runner.setResultOfRecordedOperation(bitsOf(jsNumber(5)));
        StubRun run = readLength(mode, length);
        runner.countCallsOf(Entry::operationAOTGetById);
        return run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == view.address() && static_cast<uint32_t>(run.recordedArguments[1]) == PropertyReads::name && run.result == bitsOf(jsNumber(5));
    };

    for (TypedArrayMode mode : { FastTypedArray, OversizeTypedArray, WastefulTypedArray }) {
        for (uint64_t length : { uint64_t(0), uint64_t(7), maxLengthRead }) {
            StubRun run = readLength(mode, length);
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(static_cast<int32_t>(length))));
        }
        AOT_TEST_CHECK(isLeftToOperation(mode, maxLengthRead + 1));
    }
    for (TypedArrayMode mode : { GrowableSharedWastefulTypedArray, GrowableSharedAutoLengthWastefulTypedArray, ResizableNonSharedWastefulTypedArray, ResizableNonSharedAutoLengthWastefulTypedArray })
        AOT_TEST_CHECK(isLeftToOperation(mode, 7));
}

namespace {

// PropertyReads::name is a value in the cell of shape 1, a narrow reference in the cell of shape 2, a value in the Extension of shape 3, some other field of shape 4 and an integer in the cell of shape 5. No other name has an entry.
class FieldTable {
    WTF_MAKE_NONCOPYABLE(FieldTable);
public:
    static constexpr uint32_t shapeWithValue = 1;
    static constexpr uint32_t shapeWithNarrowCell = 2;
    static constexpr uint32_t shapeWithOutOfLineValue = 3;
    static constexpr uint32_t shapeWithOtherField = 4;
    static constexpr uint32_t shapeWithInt32 = 5;
    static constexpr uint32_t highestShape = 5;
    static constexpr uint32_t offsetInExtension = 2 * sizeof(uint64_t);

    explicit FieldTable(PropertyReads& reads)
    {
        using Entry = FieldTableEntry;
        auto entryOf = [&](uint32_t shape) -> uint32_t& {
            m_table[highestShape - shape] = shape;
            return m_table[highestShape + shape + PropertyReads::name];
        };
        entryOf(shapeWithValue) = Entry::create(PropertyReads::name, Entry::Form::InlineValue, FakeTypedObject::offsetOfField / sizeof(uint64_t)).bits;
        entryOf(shapeWithNarrowCell) = Entry::create(PropertyReads::name, Entry::Form::InlineNarrowCell, FakeTypedObject::offsetOfField / sizeof(uint32_t)).bits;
        entryOf(shapeWithOutOfLineValue) = Entry::create(PropertyReads::name, Entry::Form::OutOfLineValue, offsetInExtension / sizeof(uint64_t)).bits;
        entryOf(shapeWithOtherField) = Entry::create(PropertyReads::name, Entry::Form::Field, 0).bits;
        entryOf(shapeWithInt32) = Entry::create(PropertyReads::name, Entry::Form::InlineInt32, FakeTypedObject::offsetOfField / sizeof(uint32_t)).bits;
        reads.runner.instance().image.fieldTableEntries = m_table.data() + highestShape;
    }

private:
    std::array<uint32_t, highestShape + highestShape + PropertyReads::numberOfNames> m_table { };
};

struct FakeObjectWithShape : FakeTypedObject {
    explicit FakeObjectWithShape(uint32_t shape) { at<uint32_t>(offsetOfShapeWord) = shapeWordOf(shape); }
};

// Holds one granule.
struct FakeExtension : FakeObject<extensionCellSizeFor(1)> {
    static constexpr uint64_t bitOfSlot = granuleBitOf(FieldTable::offsetInExtension);

    explicit FakeExtension(uint64_t granuleBit) { at<uint64_t>(offsetOfExtensionGranuleBits) = granuleBit; }

    uint64_t& granule() { return at<uint64_t>(offsetOfFirstExtensionSlot); }
    uint32_t encode(uintptr_t cageBase) const { return MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(address() - offsetOfEmptyExtensionInCage)); }
};

} // anonymous namespace

static void testFieldsFoundInFieldTable()
{
    PropertyReads reads;
    FieldTable table(reads);
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);
    const JSValue values[] = { jsNumber(7), jsDoubleNumber(0.5), jsUndefined(), jsNull(), jsBoolean(true), JSValue(std::bit_cast<JSCell*>(cell.address())) };

    FakeObjectWithShape withValue(FieldTable::shapeWithValue);
    for (JSValue value : values) {
        withValue.field() = bitsOf(value);
        StubRun run = reads.read(withValue.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(value));
    }

    FakeObjectWithShape withNarrowCell(FieldTable::shapeWithNarrowCell);
    auto readNarrow = [&](uint32_t word) {
        withNarrowCell.field() = untouched << 32 | word;
        StubRun run = reads.read(withNarrowCell.address());
        AOT_TEST_CHECK(!run.calledOperation);
        return run.result;
    };
    AOT_TEST_CHECK(readNarrow(MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(cell.address()))) == cell.address());
    AOT_TEST_CHECK(readNarrow(undefinedCell32) == bitsOf(jsUndefined()) && readNarrow(nullCell32) == bitsOf(jsNull()));

    FakeObjectWithShape withInt32(FieldTable::shapeWithInt32);
    for (int32_t value : { 0, 7, -1, INT32_MAX, minInlineInt32 }) {
        withInt32.field() = untouched << 32 | static_cast<uint32_t>(value);
        StubRun run = reads.read(withInt32.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(value)));
    }

    FakeExtension extension(FakeExtension::bitOfSlot);
    FakeObjectWithShape withOutOfLineValue(FieldTable::shapeWithOutOfLineValue);
    withOutOfLineValue.field() = untouched << 32 | extension.encode(cageBase);
    for (JSValue value : values) {
        extension.granule() = bitsOf(value);
        StubRun run = reads.read(withOutOfLineValue.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(value));
    }
}

static void testLookupsLeftToTheCaches()
{
    PropertyReads reads;
    FieldTable table(reads);
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    auto fieldSlot = [](SlotRepresentation representation, uint8_t flags) {
        return Slot::createField(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::Field, Slot::isOwn | flags, SlotData::create(FakeTypedObject::offsetOfField, representation), 0, 0);
    };

    // An empty slot says nothing about the chain.
    for (auto [shape, representation] : { std::pair { FieldTable::shapeWithValue, SlotRepresentation::JSValue }, std::pair { FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32 } }) {
        FakeObjectWithShape object(shape);
        object.field() = 0;
        reads.file(fieldSlot(representation, Slot::absentIsUndefined));
        StubRun run = reads.read(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
        reads.file(Slot { });
        AOT_TEST_CHECK(reads.read(object.address()).calledOperation);
    }

    // So does an Extension that is not there, one that holds another granule, and an empty slot in it.
    FakeObjectWithShape withOutOfLineValue(FieldTable::shapeWithOutOfLineValue);
    withOutOfLineValue.field() = 0;
    AOT_TEST_CHECK(reads.read(withOutOfLineValue.address()).calledOperation);
    for (uint64_t granuleBit : { FakeExtension::bitOfSlot >> 1, FakeExtension::bitOfSlot << 1, FakeExtension::bitOfSlot }) {
        FakeExtension extension(granuleBit);
        extension.granule() = granuleBit == FakeExtension::bitOfSlot ? 0 : bitsOf(jsNumber(7));
        withOutOfLineValue.field() = extension.encode(cageBase);
        AOT_TEST_CHECK(reads.read(withOutOfLineValue.address()).calledOperation);
    }

    // A word below the integers says that the slot is empty, undefined or null, or is an integer of a field that has no such words.
    FakeObjectWithShape withInt32(FieldTable::shapeWithInt32);
    for (int32_t word : { absentInt32, undefinedInt32, nullInt32, INT32_MIN, minInlineInt32 - 1 }) {
        withInt32.at<int32_t>(FakeTypedObject::offsetOfField) = word;
        reads.file(Slot { });
        AOT_TEST_CHECK(reads.read(withInt32.address()).calledOperation);
        reads.file(fieldSlot(SlotRepresentation::Int32, 0));
        StubRun run = reads.read(withInt32.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(word)));
    }
    withInt32.at<int32_t>(FakeTypedObject::offsetOfField) = nullInt32;
    reads.file(fieldSlot(SlotRepresentation::OptionalInt32, 0));
    AOT_TEST_CHECK(reads.read(withInt32.address()).result == bitsOf(jsNull()));

    FakeObjectWithShape withOtherField(FieldTable::shapeWithOtherField);
    withOtherField.at<int32_t>(FakeTypedObject::offsetOfField) = 9;
    reads.file(fieldSlot(SlotRepresentation::Int32, 0));
    StubRun run = reads.read(withOtherField.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(9)));

    // The entries that another name meets are those of other rows.
    reads.file(Slot { });
    for (uint32_t shape = 1; shape <= FieldTable::highestShape; ++shape) {
        FakeObjectWithShape object(shape);
        object.field() = bitsOf(jsNumber(7));
        for (uint16_t otherName = 1; otherName < PropertyReads::numberOfNames; ++otherName)
            AOT_TEST_CHECK(otherName == PropertyReads::name || reads.read(object.address(), otherName).calledOperation);
    }

    FakeTypedObject withoutShape;
    withoutShape.field() = bitsOf(jsNumber(7));
    AOT_TEST_CHECK(reads.read(withoutShape.address()).calledOperation);
}

static void testPresenceFromLeafLookup()
{
    PropertyReads reads;
    StubRunner& runner = reads.runner;
    FakeTypedObject object;
    FakeObject<16> keyWithoutName;
    keyWithoutName.setHeader(0, FinalObjectType);
    auto ask = [&](PresenceQuestion question, LeafPresence answer) {
        runner.recordCallsOf(Entry::operationAOTInByValGeneric);
        runner.setResultOfRecordedOperation(static_cast<uint64_t>(answer));
        StubRun run = runner.run(inByValCached(question), { object.address(), keyWithoutName.address() });
        runner.countCallsOf(Entry::operationAOTInByValGeneric);
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && run.recordedArguments[1] == keyWithoutName.address());
        return run.result;
    };

    AOT_TEST_CHECK(ask(PresenceQuestion::In, LeafPresence::Own) == 1 && ask(PresenceQuestion::HasOwn, LeafPresence::Own) == 1);
    AOT_TEST_CHECK(ask(PresenceQuestion::In, LeafPresence::Inherited) == 1 && !ask(PresenceQuestion::HasOwn, LeafPresence::Inherited));
    AOT_TEST_CHECK(!ask(PresenceQuestion::In, LeafPresence::Absent) && !ask(PresenceQuestion::HasOwn, LeafPresence::Absent));

    constexpr std::pair<PresenceQuestion, Entry> cachingOperations[] = {
        { PresenceQuestion::In, Entry::operationAOTInByValAndCache },
        { PresenceQuestion::HasOwn, Entry::operationAOTHasOwnByValAndCache },
    };
    for (auto [question, cachingOperation] : cachingOperations) {
        runner.recordCallsOf(cachingOperation);
        runner.setResultOfRecordedOperation(1);
        StubRun run = runner.run(inByValCached(question), { object.address(), keyWithoutName.address() });
        runner.countCallsOf(cachingOperation);
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == 1);
        AOT_TEST_CHECK(run.recordedArguments[0] == object.address() && run.recordedArguments[1] == keyWithoutName.address());
    }
}

std::span<const NativeTest> stubPropertyReadTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/get-by-id/typed-fields"_s, ImageTests::whenExecutable<testTypedFields> },
        { "stubs/get-by-id/fields-or-inherited-values"_s, ImageTests::whenExecutable<testFieldsOrInheritedValues> },
        { "stubs/get-by-id/fields-in-extension-or-inherited-values"_s, ImageTests::whenExecutable<testFieldsInExtensionOrInheritedValues> },
        { "stubs/get-by-id/fields-or-inherited-getters"_s, ImageTests::whenExecutable<testFieldsOrInheritedGetters> },
        { "stubs/get-by-id/deferred-closures"_s, ImageTests::whenExecutable<testDeferredClosures> },
        { "stubs/get-by-id/typed-array-lengths"_s, ImageTests::whenExecutable<testTypedArrayLengths> },
        { "stubs/get-by-id/fields-found-in-field-table"_s, ImageTests::whenExecutable<testFieldsFoundInFieldTable> },
        { "stubs/get-by-id/lookups-left-to-the-caches"_s, ImageTests::whenExecutable<testLookupsLeftToTheCaches> },
        { "stubs/in-by-val/presence-from-leaf-lookup"_s, ImageTests::whenExecutable<testPresenceFromLeafLookup> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
