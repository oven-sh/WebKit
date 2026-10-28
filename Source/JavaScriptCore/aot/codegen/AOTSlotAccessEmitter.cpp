/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAllocationEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTBlockFrequencies.h"
#include "AOTCellFormat.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardFactDetails.h"
#include "AOTMemoryLocation.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTObjectWords.h"
#include "AOTPlacement.h"
#include "AOTResultRules.h"
#include "AOTRuntimeValidation.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThreshold.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include "GetterSetter.h"
#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "JSType.h"
#include <limits>
#include <optional>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static const PlacedSlot* extensionWordOf(const FieldAccess& access)
{
    return access.slot.isOutOfLine() ? &access.extensionWord : nullptr;
}

const FieldAccess& SlotAccessEmitter::fieldAccessOf(const ValueContext& value) const
{
    return m_placement.fieldAccess(value->fieldAccess());
}

Operand SlotAccessEmitter::failRead(const ValueContext& value, InternalErrorKind kind)
{
    fail(value, kind);
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

// A field gets no slot where nothing stores it and no allocation that is left lists it: those that did were inlined and taken apart.
Operand SlotAccessEmitter::resultBehindTestThatNeverPasses(const ValueContext& value)
{
    if (generator().validates())
        emitter<RuntimeValidationEmitter>().reportUnreachable(value.id());
    m_out.unreachable();
    m_out.appendTo(newBlock());
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

Operand SlotAccessEmitter::undefinedResult(const ValueContext& value)
{
    switch (value.representation()) {
    case Representation::Void:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
        return emitter<ConstantEmitter>().constantOf(jsUndefined(), value.representation());
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::Cell:
    case Representation::RawWord:
        break;
    }
    return failRead(value, InternalErrorKind::OpcodeWithoutCode);
}

Operand SlotAccessEmitter::inResultRepresentation(const ValueContext& value, Operand loaded)
{
    return emitter<ConversionEmitter>().inRepresentation(value, loaded, typeOf(value.id()), value.representation());
}

// The table has an entry for every shape from the first to the last that a member with the field has, the last shape first: the shape word is 0 - shape.
Word32 SlotAccessEmitter::distanceBehindLowestOffset(CellPointer cell, const FieldAccess& access)
{
    AOT_THRESHOLD(uint32_t, maxEntries, 1024);
    const ShapeTable& shapes = m_placement.shapes();
    const SlotPlan& slots = m_placement.slots();
    struct Member {
        ShapeRange shapes;
        uint32_t distance;
    };
    Vector<Member, 8> members;
    uint32_t first = std::numeric_limits<uint32_t>::max();
    uint32_t last = 0;
    uint32_t greatestDistance = 0;
    for (LayoutID layout : m_program.at(access.membersWithField).completeMembers()) {
        ShapeRange range = shapes.shapesOf(layout);
        if (range.isEmpty())
            continue;
        uint32_t distance = slots.slot(layout, *m_program.at(layout).findField(access.key)).offset - access.slot.offset;
        members.append({ range, distance });
        first = std::min(first, range.first);
        last = std::max(last, range.end() - 1);
        greatestDistance = std::max(greatestDistance, distance);
    }
    if (members.isEmpty())
        return word32(0);

    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    Word32 shapeWord = shapeTests.shapeWordOf(cell);
    if (last - first >= maxEntries) {
        Word32 distance = word32(0);
        for (const Member& member : members) {
            if (member.distance)
                distance = select(below(add(shapeWord, static_cast<int32_t>(member.shapes.end() - 1)), static_cast<int32_t>(member.shapes.count)), static_cast<int32_t>(member.distance), distance);
        }
        return distance;
    }

    Word32 index = add(shapeWord, word32(static_cast<int32_t>(last)));
    auto entryOfTableOf = [&]<typename Entry>(Entry) {
        Vector<Entry, 64> entries(FillWith { }, last - first + 1, static_cast<Entry>(0));
        for (const Member& member : members) {
            for (uint32_t shape = member.shapes.first; shape < member.shapes.end(); ++shape)
                entries[last - shape] = static_cast<Entry>(member.distance);
        }
        auto known = std::ranges::find_if(m_offsetTables, [&](const OffsetTable& table) {
            return table.key == access.key && table.members == access.membersWithField;
        });
        uint32_t constant = known != m_offsetTables.end() ? known->constant : m_out.addConstantData(asBytes(entries.span()));
        if (known == m_offsetTables.end())
            m_offsetTables.append({ access.key, access.membersWithField, constant });
        Pointer table(m_out.relocatable(RelocationSpace::Constants, constant));
        return m_out.address(m_heaps.AOTImage_tables, offsetBy(table, zeroExtended(index), std::countr_zero(sizeof(Entry))).value());
    };
    if (greatestDistance <= std::numeric_limits<uint8_t>::max())
        return Word32(m_out.load8ZeroExt32(entryOfTableOf(uint8_t())));
    RELEASE_ASSERT(greatestDistance <= std::numeric_limits<uint16_t>::max());
    return Word32(m_out.load16ZeroExt32(entryOfTableOf(uint16_t())));
}

CellPointer SlotAccessEmitter::baseOfSlot(CellPointer cell, const FieldAccess& access)
{
    if (!access.findsOffsetByShape)
        return cell;
    return CellPointer(m_out.add(cell.value(), zeroExtended(distanceBehindLowestOffset(cell, access)).value()));
}

Operand SlotAccessEmitter::loadSlot(CellPointer cell, const FieldAccess& access)
{
    return emitter<SlotEmitter>().loadSlot(cell, access.slot, MemoryLocation::objectField(access.key, access.receivers), extensionWordOf(access));
}

Operand SlotAccessEmitter::loadFilledSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    return emitter<SlotEmitter>().loadSlot(cell, access.slot, MemoryLocation::objectField(access.key, access.receivers), extensionWordOf(access), typeOf(value.id()));
}

Operand SlotAccessEmitter::loadSlotOrUndefined(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    SlotRepresentation representation = access.slot.representation;
    SlotTraits traits = traitsOf(representation);
    if (!traits.hasAbsent)
        return inResultRepresentation(value, loadSlot(cell, access));
    if (traits.hasUndefined)
        return inResultRepresentation(value, slots.loadSlotOrUndefined(cell, access.slot, MemoryLocation::objectField(access.key, access.receivers), extensionWordOf(access)));

    Operand loaded = loadSlot(cell, access);
    LBasicBlock isFilled = newBlock();
    LBasicBlock isAbsent = newBlock();
    OperandJoin join(m_out, value.representation());
    branch(slots.isAbsent(loaded, representation), { unlikelyEdge(isAbsent), normalEdge(isFilled) });

    m_out.appendTo(isFilled);
    join.add(inResultRepresentation(value, loaded));

    m_out.appendTo(isAbsent);
    join.add(undefinedResult(value));
    return join.finish();
}

Operand SlotAccessEmitter::readIfMemberHasField(const ValueContext& value, CellPointer cell, const FieldAccess& access, NOESCAPE const Invocable<Operand()> auto& read)
{
    LBasicBlock hasField = newBlock();
    LBasicBlock hasNoField = newBlock();
    OperandJoin join(m_out, value.representation());
    emitter<ShapeTestEmitter>().branchOnShapeIn(cell, access.membersWithField, Type::forLayoutSet(access.receivers), { normalEdge(hasField), normalEdge(hasNoField) });

    m_out.appendTo(hasField);
    join.add(read());

    m_out.appendTo(hasNoField);
    join.add(undefinedResult(value));
    return join.finish();
}

Operand SlotAccessEmitter::readSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access, bool absentIsUndefined)
{
    if (value.representation() == Representation::Void)
        return { };
    if (access.slot.representation == SlotRepresentation::DeferredClosure)
        return failRead(value, InternalErrorKind::SlotAccessNotPlaced);

    // A result that is never undefined comes from a slot that is never absent.
    bool mayBeAbsent = absentIsUndefined && typeOf(value.id()).primitives & Primitive::Undefined;
    auto load = [&] {
        return loadSlotOrUndefined(value, baseOfSlot(cell, access), access);
    };
    switch (access.coverage) {
    case FieldAccess::Coverage::NoMember:
        if (absentIsUndefined)
            return undefinedResult(value);
        return guardProvesSlotFilled(analyses(), value.value()) ? resultBehindTestThatNeverPasses(value) : failRead(value, InternalErrorKind::SlotAccessNotPlaced);
    case FieldAccess::Coverage::SomeMembers:
        return mayBeAbsent ? readIfMemberHasField(value, cell, access, load) : inResultRepresentation(value, loadFilledSlot(value, baseOfSlot(cell, access), access));
    case FieldAccess::Coverage::EveryMemberHasSlot:
    case FieldAccess::Coverage::EveryMember:
        return mayBeAbsent ? load() : inResultRepresentation(value, loadFilledSlot(value, baseOfSlot(cell, access), access));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SlotAccessEmitter::generateGetSlot(const ValueContext& value)
{
    define(value, readSlot(value, in<CellPointer>(value->objectOperand()), fieldAccessOf(value), false));
}

void SlotAccessEmitter::generateGetSlotOrUndefined(const ValueContext& value)
{
    define(value, readSlot(value, in<CellPointer>(value->objectOperand()), fieldAccessOf(value), true));
}

// Nothing writes the words of a GetterSetter once it is made, so what its getter and its setter are depends on the slot alone.
std::optional<CellPointer> SlotAccessEmitter::loadHalfOfAccessorInSlot(const ValueContext& value, ptrdiff_t offsetInGetterSetter)
{
    if (value.representation() == Representation::Void) {
        define(value, { });
        return std::nullopt;
    }
    const FieldAccess& access = fieldAccessOf(value);
    if (access.coverage == FieldAccess::Coverage::NoMember) {
        define(value, guardProvesSlotFilled(analyses(), value.value()) ? resultBehindTestThatNeverPasses(value) : failRead(value, InternalErrorKind::SlotAccessNotPlaced));
        return std::nullopt;
    }
    MemoryLocation location = MemoryLocation::objectField(access.key, access.receivers);
    CellPointer object = in<CellPointer>(value->objectOperand());
    Operand held = emitter<SlotEmitter>().loadSlot(object, access.slot, location, extensionWordOf(access), Type::object());
    CellPointer accessor = held.is<CellPointer>() ? held.as<CellPointer>()
        : held.is<NarrowReference>() ? emitter<ConversionEmitter>().decode(held.as<NarrowReference>(), Type::object(), Representation::Cell).as<CellPointer>()
        : unboxCell(held.as<Boxed>());
    return CellPointer(m_out.loadPtr(m_out.address(m_heaps.at(m_program, location), accessor.value(), offsetInGetterSetter)));
}

void SlotAccessEmitter::generateGetSlotGetter(const ValueContext& value)
{
    if (std::optional<CellPointer> getter = loadHalfOfAccessorInSlot(value, GetterSetter::offsetOfGetter()))
        define(value, inResultRepresentation(value, *getter));
}

// The function that stands for an absent setter ignores the call. Strict code calls the one that throws.
void SlotAccessEmitter::generateGetSlotSetter(const ValueContext& value)
{
    std::optional<CellPointer> setter = loadHalfOfAccessorInSlot(value, GetterSetter::offsetOfSetter());
    if (!setter)
        return;
    if (value->isInStrictCode()) {
        CellPointer thrower(loadField<Pointer>(loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject)), m_heaps.JSGlobalObject_nullSetterStrictFunction).value());
        setter = select(hasCellType(*setter, NullSetterFunctionType), thrower, *setter);
    }
    define(value, inResultRepresentation(value, *setter));
}

void SlotAccessEmitter::readDeferringSlot(const ValueContext& value, bool readsEmptySlotAsUndefined)
{
    const FieldAccess& access = fieldAccessOf(value);
    CellPointer cell = in<CellPointer>(value->objectOperand());
    if (access.slot.representation != SlotRepresentation::DeferredClosure) {
        define(value, readSlot(value, cell, access, readsEmptySlotAsUndefined));
        return;
    }
    // A read of a filled slot has a guard that says so, and only a receiver that has the field passes it.
    bool looksOutsideSlot = readsEmptySlotAsUndefined && mayHaveLeftDeferringSlot(value, access);
    auto load = [&] {
        return looksOutsideSlot ? loadDeferringSlotOrFieldOutsideIt(value, cell, access) : loadDeferringSlot(value, cell, access, readsEmptySlotAsUndefined);
    };
    bool testsForField = readsEmptySlotAsUndefined && access.coverage == FieldAccess::Coverage::SomeMembers;
    define(value, testsForField ? readIfMemberHasField(value, cell, access, load) : load());
}

// Where every receiver leaves the closure of one function unmade, over itself or over nothing, a stub makes it as the allocation would have.
CellPointer SlotAccessEmitter::makeDeferredClosure(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    FunctionID function;
    ClosureDeferral deferral = ClosureDeferral::Unknown;
    bool isOneClosure = !access.findsOffsetByShape && !access.slot.isOutOfLine() && std::ranges::all_of(m_program.at(access.receivers).completeMembers(), [&](LayoutID member) {
        std::optional<uint32_t> field = m_program.at(member).findField(access.key);
        if (!field)
            return true;
        FieldReference declaration = declarationOf(m_program, { member, *field });
        const LocationFlow& location = flowOf(m_program, declaration).location;
        if (!fieldAt(m_program, declaration).form.isEmpty() || !location.deferredFunction || (function && (function != location.deferredFunction || deferral != location.deferral)))
            return false;
        function = location.deferredFunction;
        deferral = location.deferral;
        return true;
    });
    if (isOneClosure && (deferral == ClosureDeferral::CapturesNothing || deferral == ClosureDeferral::CapturesObject)) {
        if (std::optional<CellPointer> closure = emitter<ConstantEmitter>().tryMakeClosureOfField(value, function, cell, access.slot.offset, deferral == ClosureDeferral::CapturesObject))
            return *closure;
    }
    return callOperation<CellPointer>(value, RuntimeOperation::MaterializeDeferredClosure, { cell, identifierNumber(access.key) }, { .isOnRarePath = true });
}

Operand SlotAccessEmitter::loadDeferringSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access, bool readsEmptySlotAsUndefined)
{
    // The words for "not made yet" and "absent" come first. Behind them come null, undefined and the references, which are what a narrow reference holds.
    static_assert(uncreatedDeferredClosure < absentDeferredClosure && absentDeferredClosure < nullCell32 && absentDeferredClosure < undefinedCell32);
    NarrowReference word(m_out.castToInt32(loadSlot(cell, access).as<Pointer>().value()));
    NarrowReference held = either<NarrowReference>(above(word, NarrowReference(m_out.constInt32(absentDeferredClosure))), [&] {
        return word;
    }, [&] {
        auto makeClosure = [&] {
            return compress(makeDeferredClosure(value, cell, access));
        };
        if (!readsEmptySlotAsUndefined)
            return makeClosure();
        // One test serves whatever the slot holds: the two words that are no value are told apart on this side of it.
        return either<NarrowReference>(isZero(Word32(word.value())), makeClosure, [&] {
            return NarrowReference(m_out.constInt32(undefinedCell32));
        });
    });
    return decodeWordOfDeferringSlot(value, held);
}

// The slot took every value until it came to defer its closure.
bool SlotAccessEmitter::mayHaveLeftDeferringSlot(const ValueContext& value, const FieldAccess& access) const
{
    bool mayHaveLeft = std::ranges::any_of(m_program.at(access.receivers).completeMembers(), [&](LayoutID member) {
        std::optional<uint32_t> field = m_program.at(member).findField(access.key);
        return field && !isNeverMovedOutByStore(m_program, { member, *field });
    });
    if (!mayHaveLeft || value.representation() == Representation::Void)
        return false;
    if (value.representation() == Representation::JSValue)
        return true;
    // The slot never took every value, so the read stands behind the test that no field lies outside its slot.
    RELEASE_ASSERT(value->guardOperand());
    return false;
}

Operand SlotAccessEmitter::loadDeferringSlotOrFieldOutsideIt(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    NarrowReference word(m_out.castToInt32(loadSlot(cell, access).as<Pointer>().value()));
    Boxed result = either<Boxed>(above(word, NarrowReference(m_out.constInt32(absentDeferredClosure))), [&] {
        return decodeWordOfDeferringSlot(value, word).as<Boxed>();
    }, [&] {
        return either<Boxed>(isZero(Word32(word.value())), [&] {
            return decodeWordOfDeferringSlot(value, compress(makeDeferredClosure(value, cell, access))).as<Boxed>();
        }, [&] {
            return either<Boxed>(hasNoBits(emitter<ShapeTestEmitter>().orderWordOf(cell), orderWordHasFieldOutsideSlot), [&] {
                return undefined();
            }, [&] {
                return callOperation<Boxed>(value, RuntimeOperation::GetFieldOutsideSlot, { cell, identifierNumber(access.key) }, { .isOnRarePath = true });
            });
        });
    });
    return result;
}

Operand SlotAccessEmitter::decodeWordOfDeferringSlot(const ValueContext& value, NarrowReference held)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    switch (value.representation()) {
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
        return conversions.decode(held, typeOf(value.id()), value.representation());
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::LiteralString:
    case Representation::RawWord:
        return inResultRepresentation(value, conversions.decode(held, typeOf(value.id()), Representation::JSValue));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SlotAccessEmitter::writeSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access, ValueID stored)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    MemoryLocation location = MemoryLocation::objectField(access.key, access.receivers);
    // The next read of the slot makes the closure.
    if (access.slot.representation == SlotRepresentation::DeferredClosure && isDeferredClosure(m_function, stored)) {
        LValue uncreated = m_out.constInt32(uncreatedDeferredClosure);
        m_out.store32(uncreated, slots.addressOf(cell, access.slot, location));
        return;
    }
    Operand operand = get(stored);
    const Type& storedType = typeOf(stored);
    if (access.slot.representation == SlotRepresentation::DeferredClosure)
        m_out.store32(operand.as<NarrowReference>().value(), slots.addressOf(cell, access.slot, location));
    else {
        CellPointer base = baseOfSlot(cell, access);
        slots.storeSlot(base, access.slot, location, operand, extensionWordOf(access), storedType);
    }
    barrierAfterStore(value, cell, operand, storedType);
}

void SlotAccessEmitter::markCellIfMarkerReplacesRawValue(CellPointer cell, const FieldAccess& access, ValueID stored)
{
    if (!typeOf(stored).mayBeUndefinedOrNull())
        return;
    for (LayoutID layout : m_program.at(access.receivers).completeMembers()) {
        std::optional<FieldReference> field = findField(m_program, layout, access.key, BaseChain::Proven);
        RELEASE_ASSERT(!field || !holdsRawValueFromBirth(m_program, *field) || !keepsStateOfBirth(m_program, *field));
    }
    SlotEmitter& slots = emitter<SlotEmitter>();
    if (slots.holdsRawValueFromBirthInSomeMember(access.receivers, access.key))
        slots.markCellIfMarkerIsStored(cell, stored);
}

void SlotAccessEmitter::generatePutSlot(const ValueContext& value)
{
    const FieldAccess& access = fieldAccessOf(value);
    // Placement puts a slot out of line only where every PutSlot finds it filled, so the Extension is there and holds its granule, unless the object lists the field without storage.
    if (!access.slot.hasStorage()) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    CellPointer cell = in<CellPointer>(value->objectOperand());
    ValueID stored = value->valueOperand();
    markCellIfMarkerReplacesRawValue(cell, access, stored);
    if (access.slot.isOutOfLine() && access.mayBeFilledWithoutStorage) {
        SlotEmitter& slots = emitter<SlotEmitter>();
        if (!needsStorageFor(access.slot, typeOf(stored))) {
            slots.storeIfExtensionHolds(cell, access.slot, MemoryLocation::objectField(access.key, access.receivers), get(stored), access.extensionWord);
            return;
        }
        slots.ensureExtensionHoldsWithoutCollecting(value, cell, access.extensionWord, granuleBitOf(access.slot.offset));
    }
    writeSlot(value, cell, access, stored);
}

void SlotAccessEmitter::generatePutSlotOrAdd(const ValueContext& value)
{
    const FieldAccess& access = fieldAccessOf(value);
    if (!access.slot.hasStorage()) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }

    SlotEmitter& slots = emitter<SlotEmitter>();
    CellPointer cell = in<CellPointer>(value->objectOperand());
    ValueID stored = value->valueOperand();
    MemoryLocation location = MemoryLocation::objectField(access.key, access.receivers);
    bool needsStorage = needsStorageFor(access.slot, typeOf(stored));
    markCellIfMarkerReplacesRawValue(cell, access, stored);
    if (access.slot.isOutOfLine() && needsStorage) {
        uint64_t granuleBitsBehind = granuleBitsOfStoresBehind(value, access);
        slots.ensureExtensionHolds(value, cell, access.extensionWord, granuleBitOf(access.slot.offset), granuleBitsBehind, emitter<BarrierEmitter>().barrierOfExtension(value));
    }

    std::optional<StateOfNewObject> newObject = stateOfNewObjectBefore(value, value->objectOperand());
    auto store = [&] {
        if (needsStorage)
            writeSlot(value, cell, access, stored);
        else if (!newObject || newObject->mayHaveExtension)
            slots.storeIfExtensionHolds(baseOfSlot(cell, access), access.slot, location, get(stored), access.extensionWord);
    };
    if (!mayCreateListedField(access)) {
        store();
        return;
    }

    auto listFieldIfMissing = [&](const std::optional<RememberedKeyOrderStep>& stepThatDoesNotApply) {
        LBasicBlock creates = newBlock();
        LBasicBlock done = newBlock();
        // A field without storage may be listed already. The run time passes over a key that the object lists.
        CellPointer base = baseOfSlot(cell, access);
        Condition isMissing = needsStorage
            ? slots.holdsAbsent(slots.addressOf(base, access.slot, location, extensionWordOf(access)), access.slot.representation)
            : logicalNot(slots.holdsValue(base, access.slot, location, extensionWordOf(access)));
        branch(isMissing, { unlikelyEdge(creates), normalEdge(done) });
        m_out.appendTo(creates);
        takeRememberedKeyOrderStepOr(value, cell, stepThatDoesNotApply, [&](std::optional<uint32_t> placePlusOne) {
            return didCreateField(value, cell, access, needsStorage, placePlusOne);
        });
        m_out.jump(done);
        m_out.appendTo(done);
    };

    // The key is listed first, while the slot still says whether the field is missing.
    listCreatedFields(value, cell, access.membersWithField, singleElementSpan(access.key), newObject, isExpectedToCreate(value->accessSite()), listFieldIfMissing);
    store();
}

uint64_t SlotAccessEmitter::granuleBitsOfStoresBehind(const ValueContext& store, const FieldAccess& access)
{
    RELEASE_ASSERT(access.slot.isOutOfLine());
    uint64_t bits = granuleBitOf(access.slot.offset);
    auto add = [&](FieldAccessID id, ValueID stored) {
        if (!id)
            return;
        const FieldAccess& later = m_placement.fieldAccess(id);
        if (later.slot.isOutOfLine() && later.extensionWord.hasLocationOf(access.extensionWord) && needsStorageFor(later.slot, typeOf(stored)))
            bits |= granuleBitOf(later.slot.offset);
    };

    ValueID object = store->objectOperand();
    const UseLists& uses = analyses().useLists();
    const BlockFrequencies& frequencies = analyses().blockFrequencies();
    double frequency = frequencies.frequencyOf(store.block()).predicted();
    for (const Use& use : uses.usesOf(object)) {
        if (use.kind != Use::Kind::Operand || use.user == store.id())
            continue;
        const Value& user = m_function.at(use.user);
        bool mayAddField = user.opcode == Opcode::PutSlotOrAdd || user.opcode == Opcode::PutSlots;
        if (!mayAddField || user.objectOperand() != object)
            continue;
        if (frequencies.frequencyOf(use.block).predicted() * 2 < frequency || !analyses().dominates(store.id(), use))
            continue;
        if (user.opcode == Opcode::PutSlotOrAdd) {
            add(user.fieldAccess(), user.valueOperand());
            continue;
        }
        PayloadListView<PlacedKey> placedKeys = user.placedKeys();
        std::span<const ValueID> stored = user.valuesOperands();
        for (size_t index = 0; index < stored.size() && index < placedKeys.size(); ++index)
            add(placedKeys[index].fieldAccess, stored[index]);
    }
    return bits;
}

bool SlotAccessEmitter::flowNamesConstructorOrdersOf(LayoutSetID receivers) const
{
    return std::ranges::all_of(m_program.at(receivers).completeMembers(), [&](LayoutID member) {
        if (!m_program.at(declaredLayoutOf(m_program, member)).constructor)
            return true;
        return std::ranges::any_of(m_program.at(member).flow.constructorOrders, [](const auto& order) {
            return !order.fields.isEmpty();
        });
    });
}

bool SlotAccessEmitter::isExpectedToCreate(AccessSiteID site) const
{
    return site && !inputs().flow.seesOnlyFinishedObjects(site);
}

bool SlotAccessEmitter::mayCreateListedField(const FieldAccess& access) const
{
    if (!canLeaveSlot(access.slot.representation))
        return false;
    return std::ranges::any_of(m_program.at(access.membersWithField).completeMembers(), [&](LayoutID member) {
        return !m_placement.layout(member).keysAreNeverEnumerated;
    });
}

// The places in the orders of the member where the created fields come next. None where the member lacks one of the fields.
static std::span<const KeyOrderContinuation> continuationsCreating(const Program& program, const Placement& placement, LayoutID member, std::span<const PropertyKeyID> createdKeys)
{
    Vector<FieldIndex, 4> fields;
    for (PropertyKeyID key : createdKeys) {
        std::optional<FieldIndex> field = program.at(member).findField(key);
        if (!field)
            return { };
        fields.append(*field);
    }
    return placement.keyOrders().continuationsCreating(member, fields.span());
}

// Where every member lists the created fields at the same place of an order with the same number, the order word alone says that the step applies.
static std::optional<KeyOrderContinuation> findContinuationOfEveryMember(const Program& program, const Placement& placement, LayoutSetID receivers,
    std::span<const PropertyKeyID> createdKeys, std::optional<uint32_t> orderWordBefore = std::nullopt)
{
    std::optional<KeyOrderContinuation> common;
    for (LayoutID member : program.at(receivers).completeMembers()) {
        // An order is numbered within its shape.
        if (placement.layout(member).keysAreNeverEnumerated || placement.shapes().shapesOf(member).count != 1)
            return std::nullopt;
        std::span<const KeyOrderContinuation> continuations = continuationsCreating(program, placement, member, createdKeys);
        auto found = orderWordBefore ? std::ranges::find(continuations, *orderWordBefore, &KeyOrderContinuation::orderWordBefore) : continuations.begin();
        if (found == continuations.end() || (common && *common != *found))
            return std::nullopt;
        common = *found;
    }
    return common;
}

std::optional<uint32_t> SlotAccessEmitter::orderWordAfterCreating(uint32_t orderWordBefore, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys) const
{
    std::optional<KeyOrderContinuation> common = findContinuationOfEveryMember(m_program, m_placement, receivers, createdKeys, orderWordBefore);
    return common ? std::optional<uint32_t>(common->orderWordAfter) : std::nullopt;
}

static constexpr OpcodeSet storesBySlot { Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::PutSlots };

std::optional<StateOfNewObject> SlotAccessEmitter::stateAtBirth(ValueID id) const
{
    const Value& allocation = m_function.at(id);
    if (allocation.opcode != Opcode::NewObject || !allocation.allocation() || allocation.storage() != AllocationStorage::Heap)
        return std::nullopt;
    // With another prototype the run time may answer with another object.
    if (allocation.prototypeOperand() && mayAllocateOtherLayout(m_program, allocation))
        return std::nullopt;
    const PlacedAllocation& placed = m_placement.allocation(allocation.allocation());
    if (m_placement.freeCellOffset(placed.layout))
        return std::nullopt;
    bool hasFieldInExtension = std::ranges::any_of(placed.initialFields, [&](FieldIndex field) {
        return m_placement.slot(placed.layout, field).isOutOfLine();
    });
    return StateOfNewObject { placed.orderWord, hasFieldInExtension, !allocation.prototypeOperand() };
}

// Every way into the block allocates an object of its own and does nothing else with it.
std::optional<StateOfNewObject> SlotAccessEmitter::stateOfObjectsThatMeet(BlockID block, ValueID parameter)
{
    std::span<const ValueID> parameters = m_function.at(block).parameters.span();
    size_t index = std::ranges::find(parameters, parameter) - parameters.begin();
    std::span<const BlockID> predecessors = analyses().cfg().predecessors(block);
    if (index == parameters.size() || predecessors.empty())
        return std::nullopt;

    std::optional<StateOfNewObject> common;
    for (BlockID predecessor : predecessors) {
        const Block& from = m_function.at(predecessor);
        if (from.terminator.kind != TerminatorKind::Jump || index >= from.terminator.next().arguments.size())
            return std::nullopt;
        ValueID passed = from.terminator.next().arguments[index];
        std::span<const ValueID> values = from.values.span();
        size_t position = values.size();
        while (position && values[position - 1] != passed) {
            if (!leavesObjectsAsTheyAre(values[--position]))
                return std::nullopt;
        }
        std::optional<StateOfNewObject> state = position ? stateAtBirth(passed) : std::nullopt;
        if (!state)
            return std::nullopt;
        if (common) {
            if (common->orderWord != state->orderWord)
                return std::nullopt;
            common->mayHaveExtension |= state->mayHaveExtension;
            common->hasLayoutPrototype &= state->hasLayoutPrototype;
        } else
            common = state;
    }
    return common;
}

bool SlotAccessEmitter::leavesObjectsAsTheyAre(ValueID id)
{
    const Effects& effects = analyses().effectsOf(id).normal;
    return !effects.mayRunOtherCode() && effects.writes.containsOnly(heapsWhoseWritesNobodySees);
}

bool SlotAccessEmitter::applyStoreBySlot(StateOfNewObject& state, const Value& store) const
{
    Vector<PropertyKeyID, 4> createdKeys;
    auto add = [&](FieldAccessID id, ValueID stored, bool mayAdd) {
        if (!id)
            return false;
        const FieldAccess& access = m_placement.fieldAccess(id);
        state.mayHaveExtension |= access.slot.isOutOfLine() && needsStorageFor(access.slot, typeOf(stored));
        if (mayAdd && mayCreateListedField(access))
            createdKeys.append(access.key);
        return true;
    };

    LayoutSetID receivers;
    if (store.opcode == Opcode::PutSlots) {
        PayloadListView<PlacedKey> placedKeys = store.placedKeys();
        std::span<const ValueID> stored = store.valuesOperands();
        if (placedKeys.size() != stored.size())
            return false;
        for (size_t index = 0; index < stored.size(); ++index) {
            if (!add(placedKeys[index].fieldAccess, stored[index], placedKeys[index].mayAdd))
                return false;
        }
        receivers = store.receivers();
    } else {
        if (!add(store.fieldAccess(), store.valueOperand(), store.opcode == Opcode::PutSlotOrAdd))
            return false;
        receivers = m_placement.fieldAccess(store.fieldAccess()).membersWithField;
    }
    if (createdKeys.isEmpty())
        return true;
    std::optional<uint32_t> orderWordAfter = orderWordAfterCreating(state.orderWord, receivers, createdKeys.span());
    if (!orderWordAfter)
        return false;
    state.orderWord = *orderWordAfter;
    return true;
}

// The way back from the user to the allocation has no fork that leads in from elsewhere, and on it stand only tests, values without effects and stores by slot into the object itself.
std::optional<StateOfNewObject> SlotAccessEmitter::stateOfNewObjectBefore(const ValueContext& user, ValueID operand)
{
    AOT_THRESHOLD(unsigned, maxBlocksOnTheWay, 32);
    ValueID object = analyses().identityOf(operand);
    const CFG& cfg = analyses().cfg();

    Vector<ValueID, 4> storesOnTheWay;
    bool hasPassedTestOfPrototype = false;
    auto findStateAtStartOfWay = [&]() -> std::optional<StateOfNewObject> {
        BlockID block = user.block();
        std::span<const ValueID> values = m_function.at(block).values.span();
        size_t position = std::ranges::find(values, user.id()) - values.begin();
        for (unsigned blocks = 0; ; ++blocks) {
            while (position) {
                ValueID id = values[--position];
                if (id == object)
                    return stateAtBirth(id);
                const Value& value = m_function.at(id);
                if (storesBySlot.contains(value.opcode) && analyses().identityOf(value.objectOperand()) == object)
                    storesOnTheWay.append(id);
                else if (!leavesObjectsAsTheyAre(id))
                    return std::nullopt;
            }
            if (std::ranges::contains(m_function.at(block).parameters.span(), object))
                return stateOfObjectsThatMeet(block, object);
            std::span<const BlockID> predecessors = cfg.predecessors(block);
            if (predecessors.size() != 1 || blocks == maxBlocksOnTheWay || m_function.at(block).isHandlerBlock())
                return std::nullopt;
            const Terminator& terminator = m_function.at(predecessors[0]).terminator;
            if (terminator.isConditionalBranch() && terminator.taken().block == block && terminator.notTaken().block != block) {
                const Value& test = m_function.at(terminator.conditionOperand());
                hasPassedTestOfPrototype |= test.opcode == Opcode::HasLayoutPrototype && analyses().identityOf(test.objectOperand()) == object;
            }
            block = predecessors[0];
            values = m_function.at(block).values.span();
            position = values.size();
        }
    };

    // An object that was given another prototype has the mark of it, and may have been through the run time.
    std::optional<StateOfNewObject> state = findStateAtStartOfWay();
    if (!state || (!state->hasLayoutPrototype && !hasPassedTestOfPrototype))
        return std::nullopt;
    state->hasLayoutPrototype = true;
    for (ValueID store : storesOnTheWay | std::views::reverse) {
        if (!applyStoreBySlot(*state, m_function.at(store)))
            return std::nullopt;
    }
    return state;
}

// The comparison is with the whole word: an object that is not extensible, or that has properties outside its slots, is left to the run time.
std::optional<KeyOrderStep> SlotAccessEmitter::testLikeliestKeyOrderStep(CellPointer cell, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys)
{
    struct Candidate {
        uint32_t shapeWord;
        KeyOrderContinuation continuation;
    };
    static constexpr unsigned maxCandidates = 4;
    const ShapeTable& shapes = m_placement.shapes();
    std::span<const LayoutID> members = m_program.at(receivers).completeMembers();
    if (members.size() > 1) {
        if (std::optional<KeyOrderContinuation> common = findContinuationOfEveryMember(m_program, m_placement, receivers, createdKeys))
            return KeyOrderStep { equal(emitter<ShapeTestEmitter>().orderWordOf(cell), static_cast<int32_t>(common->orderWordBefore)), word32(static_cast<int32_t>(common->orderWordAfter)) };
    }

    Vector<Candidate, maxCandidates> candidates;
    for (LayoutID member : members) {
        if (candidates.size() == maxCandidates)
            break;
        if (m_placement.layout(member).keysAreNeverEnumerated)
            continue;
        std::span<const KeyOrderContinuation> continuations = continuationsCreating(m_program, m_placement, member, createdKeys);
        if (!continuations.empty())
            candidates.append({ AOT::shapeWordOf(shapes.generalShapeOf(member)), continuations.front() });
    }
    if (candidates.isEmpty())
        return std::nullopt;

    // An order is numbered within its shape.
    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    bool hasOneShape = members.size() == 1 && shapes.shapesOf(members[0]).count == 1;
    Word32 orderWord = shapeTests.orderWordOf(cell);
    Word32 shapeWord = hasOneShape ? Word32() : shapeTests.shapeWordOf(cell);
    KeyOrderStep step;
    for (const Candidate& candidate : candidates) {
        Condition matches = equal(orderWord, word32(static_cast<int32_t>(candidate.continuation.orderWordBefore)));
        if (!hasOneShape)
            matches = logicalAnd(matches, equal(shapeWord, word32(static_cast<int32_t>(candidate.shapeWord))));
        Word32 orderWordAfter = word32(static_cast<int32_t>(candidate.continuation.orderWordAfter));
        step.orderWordAfter = step.applies ? select(matches, orderWordAfter, step.orderWordAfter) : orderWordAfter;
        step.applies = logicalOr(step.applies, matches);
    }
    return step;
}

std::optional<PlaceOfStoreThatMayAdd> SlotAccessEmitter::placeOfStoreThatMayAdd(ValueID store)
{
    if (m_storesThatMayAddInFront.isEmpty()) {
        m_storesThatMayAddInFront.fill(0, m_function.values.size());
        uint32_t count = 0;
        for (const Block& block : m_function.blocks) {
            for (ValueID id : block.values) {
                if (remembersKeyOrderStep(block, m_function.at(id)))
                    m_storesThatMayAddInFront[id.index()] = count++;
            }
        }
    }
    return m_placement.keyOrders().placeOfStoreThatMayAdd(m_id, m_storesThatMayAddInFront[store.index()]);
}

std::optional<RememberedKeyOrderStep> SlotAccessEmitter::testRememberedKeyOrderStep(const ValueContext& value, CellPointer cell)
{
    if (value.shape() != CodeShape::Fast)
        return std::nullopt;
    std::optional<PlaceOfStoreThatMayAdd> place = placeOfStoreThatMayAdd(value.id());
    if (!place || place->number >= maxPlacesWithKeyOrderStepCache)
        return std::nullopt;
    uint32_t number = place->number;

    static_assert(offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t) && !(offsetOfOrderWord % sizeof(uint64_t)));
    static_assert(!OBJECT_OFFSETOF(KeyOrderStepCache, orderWordBefore) && OBJECT_OFFSETOF(KeyOrderStepCache, shapeWord) == sizeof(uint32_t));
    const B3::AbstractHeap& heap = m_heaps.entire(AbstractHeap::RuntimeState);
    RememberedKeyOrderStep remembered;
    remembered.placePlusOne = number + 1;
    // The shape word of an object never changes, so the two words together change when the order word does.
    remembered.orderAndShapeWordsBefore = Integer(m_out.load64(m_out.address(m_heaps.AOTObject_orderWord, cell.value(), offsetOfOrderWord)));
    std::span<const LayoutID> members = m_program.at(value->opcode == Opcode::PutSlots ? value->receivers() : m_placement.fieldAccess(value->fieldAccess()).membersWithField).completeMembers();
    const ShapeTable& shapes = m_placement.shapes();
    LValue entry = m_instance;
    ptrdiff_t offsetOfEntry = InstanceLayout::offsetOfKeyOrderSteps;
    if (members.size() == 1 && shapes.shapesOf(members[0]).count == 1)
        offsetOfEntry += indexOfKeyOrderStepCache(number, static_cast<uint64_t>(AOT::shapeWordOf(shapes.generalShapeOf(members[0]))) << 32) * sizeof(KeyOrderStepCache);
    else {
        static_assert(sizeof(KeyOrderStepCache) == 1 << 4);
        LValue scaledIndex = m_out.add(m_out.lShr(remembered.orderAndShapeWordsBefore.value(), keyOrderStepCacheShift - 4), static_cast<int64_t>(number) << 4);
        entry = m_out.add(entry, m_out.bitAnd(scaledIndex, m_out.constInt64((numberOfKeyOrderStepCaches - 1) << 4)));
    }
    auto entryField = [&](ptrdiff_t offset) { return m_out.address(heap, entry, offsetOfEntry + offset); };
    Integer rememberedWords(m_out.load64(entryField(OBJECT_OFFSETOF(KeyOrderStepCache, orderWordBefore))));
    // No object has zero for its shape word, so an entry that remembers nothing matches nothing.
    remembered.step.applies = equal(remembered.orderAndShapeWordsBefore, rememberedWords);
    // What an entry holds of an object that comes here is this place's own where no other place meets it.
    if (place->meetsOtherStores) {
        static_assert(std::endian::native == std::endian::little);
        Word32 rememberedTurn(m_out.load8ZeroExt32(entryField(OBJECT_OFFSETOF(KeyOrderStepCache, turnPlusOne))));
        remembered.step.applies = logicalAnd(remembered.step.applies, equal(rememberedTurn, word32(static_cast<int32_t>(number / numberOfKeyOrderStepCaches + 1))));
    }
    remembered.step.orderWordAfter = Word32(m_out.load32(entryField(OBJECT_OFFSETOF(KeyOrderStepCache, orderWordAfter))));
    return remembered;
}

void SlotAccessEmitter::rememberKeyOrderStep(const ValueContext& value, CellPointer cell, const RememberedKeyOrderStep& remembered)
{
    callStub(value, AOT::rememberKeyOrderStep(), { cell.value(), remembered.orderAndShapeWordsBefore.value(), m_out.constInt64(remembered.placePlusOne) });
}

bool SlotAccessEmitter::didCreateField(const ValueContext& value, CellPointer cell, const FieldAccess& access, bool needsStorage, std::optional<uint32_t> placePlusOne)
{
    auto listOneKey = [&](LValue slotKey) {
        if (placePlusOne)
            callStub(value, AOT::didCreateFieldAtPlace(CreatedFieldsAtPlace::One), { cell.value(), slotKey, m_out.constInt64(*placePlusOne) });
        else
            callStub(value, AOT::didCreateField(CreatedFields::One), { cell.value(), slotKey });
        return placePlusOne.has_value();
    };
    const PlacedSlot& slot = access.slot;
    uint32_t slotKey = slotKeyFor(slot.offset, slot.isOutOfLine());
    if (access.findsOffsetByShape) {
        // The key of a slot in the cell is its offset.
        static_assert(slotKeyFor(8, false) == 8);
        bool everyOffsetIsKey = std::ranges::all_of(m_program.at(access.membersWithField).completeMembers(), [&](LayoutID member) {
            return slotKeyFor(m_placement.slots().slot(member, *m_program.at(member).findField(access.key)).offset, false) != noSlotKey;
        });
        if (!everyOffsetIsKey) {
            callVoidOperation(value, RuntimeOperation::DidCreateField, { cell, identifierNumber(access.key) });
            return false;
        }
        LValue lowestKey = m_out.constInt64(slotKey);
        return listOneKey(m_out.add(lowestKey, zeroExtended(distanceBehindLowestOffset(cell, access)).value()));
    }
    if (slotKey == noSlotKey) {
        callVoidOperation(value, RuntimeOperation::DidCreateField, { cell, identifierNumber(access.key) });
        return false;
    }
    // The slot key says where the slot is. What lists the key of a slot in the cell lists any key, and leaves the extension alone.
    bool ensuresStorage = slot.isOutOfLine() && needsStorage;
    if (ensuresStorage) {
        callStub(value, AOT::didCreateField(CreatedFields::OneOutOfLine), { cell.value(), m_out.constInt64(slotKey), m_out.constInt64(static_cast<int64_t>(granuleBitOf(slot.offset))) });
        return false;
    }
    return listOneKey(m_out.constInt64(slotKey));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
