/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPrivateAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTClassToken.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "JSCJSValueInlines.h"
#include "JSCellButterfly.h"

namespace JSC { namespace AOT { namespace Typed {

uint16_t PrivateAccessEmitter::indexInLayoutRecord(const ValueContext& value)
{
    std::optional<uint16_t> index = m_plan.slot(value->layout(), value->field()).indexInLayoutRecord;
    if (!index)
        fail(value, InternalErrorKind::OpcodeWithoutCode);
    return index.value_or(0);
}

bool PrivateAccessEmitter::isEvaluatedOnce(const ValueContext& value)
{
    return m_facts.isEvaluatedAtMostOnce(declaredLayoutOf(m_program, value->layout()));
}

std::optional<FieldAccess> PrivateAccessEmitter::findBoxedSlotInCellOfEveryReceiver(const Type& receiver, PropertyKeyID key, bool isStore)
{
    FieldAccess access = fieldAccessFor(key, receiver.layouts, isStore);
    if (!access.hasLocation || access.coverage != FieldAccess::Coverage::EveryMember || access.findsOffsetByShape || access.mayBeFilledWithoutStorage)
        return std::nullopt;
    const PlacedSlot& slot = access.slot;
    if (slot.storage != SlotStorage::Inline || slot.mayExistWithoutStorage || slot.representation != SlotRepresentation::JSValue || slot.type)
        return std::nullopt;
    for (LayoutID member : access.membersWithField) {
        std::optional<FieldIndex> field = m_plan.slots().findField(m_program, member, key);
        if (!m_plan.layout(member).holdsCompiledObjects() || !field || m_plan.slot(member, *field).type || (isStore && m_plan.slot(member, *field).provenType))
            return std::nullopt;
    }
    return access;
}

TypedPointer PrivateAccessEmitter::addressOfSlot(CellPointer object, const FieldAccess& access)
{
    return emitter<SlotEmitter>().addressOf(object, access.slot, MemoryLocation::objectField(access.key));
}

std::optional<PrivateAccessEmitter::SlotsOfProvenReceiver> PrivateAccessEmitter::findSlotsOfProvenReceiver(const ValueContext& value, bool isStore)
{
    const Type& receiver = typeOf(value->objectOperand());
    if (!receiver.isObject() || !receiver.isJavaScriptValue() || !receiver.layouts || !receiver.hasOnlyLayouts() || receiver.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return std::nullopt;
    const Field& declared = fieldAt(m_program, FieldReference { value->layout(), value->field() });
    if (!declared.key || !declared.form.isEmpty())
        return std::nullopt;

    SlotsOfProvenReceiver slots;
    std::optional<FieldAccess> name = findBoxedSlotInCellOfEveryReceiver(receiver, declared.key, isStore);
    if (!name)
        return std::nullopt;
    slots.name = WTF::move(*name);
    if (hasOneInstance() && isEvaluatedOnce(value))
        return slots;

    PropertyKeyID keyOfBrand;
    for (const Field& field : m_program.at(value->layout()).fields) {
        if (field.flags.contains(FieldFlags::IsBrand) && field.key && m_program.at(field.key).owner == m_program.at(declared.key).owner)
            keyOfBrand = field.key;
    }
    std::optional<FieldAccess> brand = keyOfBrand ? findBoxedSlotInCellOfEveryReceiver(receiver, keyOfBrand, false) : std::nullopt;
    if (!brand)
        return std::nullopt;
    slots.brand = WTF::move(*brand);
    return slots;
}

TypedPointer PrivateAccessEmitter::slotOfNameIfEvaluationMadeObject(const SlotsOfProvenReceiver& slots, CellPointer object, CellPointer token, LBasicBlock asksRunTime)
{
    if (slots.brand.key)
        branchUnless(Condition(m_out.equal(m_out.load64(addressOfSlot(object, slots.brand)), token.value())), asksRunTime);
    return addressOfSlot(object, slots.name);
}

bool PrivateAccessEmitter::findsLocationInLine(const ValueContext& value)
{
    return m_plan.layout(value->layout()).isRecordAlone;
}

TypedPointer PrivateAccessEmitter::locationInKnownStructure(const ValueContext& value, Boxed boxedObject, CellPointer token, LBasicBlock isUnknown)
{
    if (!typeOf(value->objectOperand()).isObject())
        branchUnless(isCell(boxedObject), isUnknown);
    CellPointer object = unboxCell(boxedObject);
    auto wordOfRecord = [&](Pointer records, ClassToken::KnownStructureSlot slot) {
        ptrdiff_t offset = JSCellButterfly::offsetOfData() + (ClassToken::slotOfRecord(indexInLayoutRecord(value), 0) + slot) * sizeof(EncodedJSValue);
        return Word32(m_out.load32(m_out.address(m_heaps.AOTClassToken_records, records.value(), offset)));
    };
    Pointer records = loadField<Pointer>(token, m_heaps.AOTClassToken_knownStructures);
    branchUnless(Condition(m_out.notNull(records.value())), isUnknown);
    Word32 structureID = loadStructureID(object);
    branchUnless(equal(structureID, wordOfRecord(records, ClassToken::StructureNumber)), isUnknown);
    if (!isEvaluatedOnce(value))
        branchUnless(isZero(wordOfRecord(records, ClassToken::LocationOfBrand)), isUnknown);
    Integer location = signExtended(wordOfRecord(records, ClassToken::LocationOfValue));
    Integer butterfly(loadField<Pointer>(object, m_heaps.JSObject_butterfly).value());
    Integer storage = select(lessThan(location, integer(0)), butterfly, Integer(object.value()));
    return TypedPointer(m_heaps.properties.atAnyNumber(), add(storage, location).value());
}

Boxed PrivateAccessEmitter::callPrivateGet(const ValueContext& value, Boxed object, CellPointer token)
{
    StubKey stub = privateGet();
    uint32_t immediate = indexInLayoutRecord(value);
    const PlacedSlot& slot = m_plan.slot(value->layout(), value->field());
    const Field& declaration = fieldAt(m_program, declarationOf(m_program, FieldReference { value->layout(), value->field() }));
    bool isPlainSlotInCell = slot.storage == SlotStorage::Inline && !slot.mayExistWithoutStorage && !declaration.form.contains(FieldForm::IsAccessor);
    if (isPlainSlotInCell && !m_program.at(value->layout()).isDerived() && isEvaluatedOnce(value) && slot.offset < 1u << (32 - bitsOfFieldInPrivateGet)) {
        stub = privateGetFromSlot(slot.representation);
        immediate |= slot.offset << bitsOfFieldInPrivateGet;
    }
    return Boxed(callStub(value, stub, { object.value(), token.value() }, { .immediate = immediate }).first);
}

void PrivateAccessEmitter::generatePrivateGet(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    if (std::optional<SlotsOfProvenReceiver> slots = findSlotsOfProvenReceiver(value, false)) {
        m_out.noteForm(Form::Open, StateOfKnowledge::NameInCell);
        LBasicBlock asksRunTime = newBlock();
        Join<Boxed> join(m_out);
        ScopeFork fork = m_out.forkScope();
        Boxed held(m_out.load64(slotOfNameIfEvaluationMadeObject(*slots, unboxCell(object), token, asksRunTime)));
        branchUnless(Condition(m_out.notNull(held.value())), asksRunTime);
        join.add(held);
        m_out.appendTo(asksRunTime);
        join.add(callPrivateGet(value, object, token));
        define(value, join.finishBehind(fork));
        return;
    }
    if (!findsLocationInLine(value)) {
        define(value, callPrivateGet(value, object, token));
        return;
    }

    LBasicBlock isUnknown = newBlock();
    Join<Boxed> join(m_out);
    ScopeFork fork = m_out.forkScope();
    join.add(Boxed(m_out.load64(locationInKnownStructure(value, object, token, isUnknown))));
    m_out.appendTo(isUnknown);
    join.add(callPrivateGet(value, object, token));
    define(value, join.finishBehind(fork));
}

void PrivateAccessEmitter::storePrivateField(const ValueContext& value, RuntimeOperation operation, bool isOnRarePath)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    callVoidOperation(value, operation, { object, token, word32(indexInLayoutRecord(value)), stored }, { .isOnRarePath = isOnRarePath });
}

void PrivateAccessEmitter::generatePrivateSet(const ValueContext& value)
{
    std::optional<SlotsOfProvenReceiver> slots = findSlotsOfProvenReceiver(value, true);
    if (!slots && !findsLocationInLine(value)) {
        storePrivateField(value, RuntimeOperation::PrivateSet);
        return;
    }

    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    LBasicBlock isUnknown = newBlock();
    LBasicBlock continuation = newBlock();
    ScopeFork fork = m_out.forkScope();
    if (slots) {
        m_out.noteForm(Form::Open, StateOfKnowledge::NameInCell);
        TypedPointer slot = slotOfNameIfEvaluationMadeObject(*slots, unboxCell(object), token, isUnknown);
        branchUnless(Condition(m_out.notNull(m_out.load64(slot))), isUnknown);
        m_out.store64(stored.value(), slot);
    } else
        m_out.store64(stored.value(), locationInKnownStructure(value, object, token, isUnknown));
    barrierAfterStore(value, unboxCell(object), typeOf(value->valueOperand()));
    m_out.jump(continuation);

    m_out.appendTo(isUnknown);
    storePrivateField(value, RuntimeOperation::PrivateSet, true);
    m_out.jump(continuation);

    m_out.appendToJoin(continuation, fork);
}

void PrivateAccessEmitter::generatePrivateDefine(const ValueContext& value)
{
    std::optional<SlotsOfProvenReceiver> slots = findSlotsOfProvenReceiver(value, true);
    if (!slots) {
        storePrivateField(value, RuntimeOperation::PrivateDefine);
        return;
    }

    m_out.noteForm(Form::Open, StateOfKnowledge::NameInCell);
    CellPointer object = unboxCell(in<Boxed>(value->objectOperand()));
    CellPointer token = in<CellPointer>(value->tokenOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    LBasicBlock asksRunTime = newBlock();
    LBasicBlock continuation = newBlock();
    ScopeFork fork = m_out.forkScope();
    bool isBrand = slots->name.key == slots->brand.key;
    TypedPointer slot = isBrand ? addressOfSlot(object, slots->name) : slotOfNameIfEvaluationMadeObject(*slots, object, token, asksRunTime);
    branchUnless(Condition(m_out.isNull(m_out.load64(slot))), asksRunTime);
    SlotAccessEmitter& slotAccesses = emitter<SlotAccessEmitter>();
    if (slotAccesses.mayCreateListedField(slots->name)) {
        slotAccesses.listCreatedFields(object, typeOf(value->objectOperand()).layouts, singleElementSpan(slots->name.key), true, [&] {
            slotAccesses.didCreateField(value, object, slots->name);
        });
    }
    m_out.store64(stored.value(), slot);
    barrierAfterStore(value, object, typeOf(value->valueOperand()));
    m_out.jump(continuation);

    m_out.appendTo(asksRunTime);
    storePrivateField(value, RuntimeOperation::PrivateDefine, true);
    m_out.jump(continuation);

    m_out.appendToJoin(continuation, fork);
}

void PrivateAccessEmitter::generatePrivateHas(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    auto askRunTime = [&] {
        Boxed result = callOperation<Boxed>(value, RuntimeOperation::PrivateHas, { object, token, word32(indexInLayoutRecord(value)) });
        return equal(result, boxedConstant(jsBoolean(true)));
    };
    std::optional<SlotsOfProvenReceiver> slots = findSlotsOfProvenReceiver(value, false);
    if (!slots) {
        defineTest(value, askRunTime());
        return;
    }
    m_out.noteForm(Form::Open, StateOfKnowledge::NameInCell);
    defineTestByBranching(value, [&](BranchTargets targets) {
        CellPointer cell = unboxCell(object);
        if (!slots->brand.key) {
            branch(Condition(m_out.notNull(m_out.load64(addressOfSlot(cell, slots->name)))), targets);
            return;
        }
        LBasicBlock isOfEvaluation = newBlock();
        LBasicBlock isOfNoneOrAnother = newBlock();
        LBasicBlock asksRunTime = newBlock();
        LValue brand = m_out.load64(addressOfSlot(cell, slots->brand));
        m_out.branch(m_out.equal(brand, token.value()), normalEdge(isOfEvaluation), normalEdge(isOfNoneOrAnother));
        m_out.appendTo(isOfEvaluation);
        branch(Condition(m_out.notNull(m_out.load64(addressOfSlot(cell, slots->name)))), targets);
        m_out.appendTo(isOfNoneOrAnother);
        m_out.branch(m_out.isNull(brand), targets.notTaken, rareEdge(asksRunTime));
        m_out.appendTo(asksRunTime);
        branch(askRunTime(), targets);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
