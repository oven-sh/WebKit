/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuessedAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTAccessPlan.h"
#include "AOTArrayStorageForms.h"
#include "AOTBarrierEmitter.h"
#include "AOTBuiltinTypes.h"
#include "AOTCellFormat.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTIRProgram.h"
#include "AOTMemoryLocation.h"
#include "AOTRepresentation.h"
#include "AOTResultRules.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStubsArrays.h"
#include "AOTStubsElements.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestEmitter.h"
#include "AOTTypedArrayKinds.h"
#include "IndexingType.h"
#include "JSType.h"
#include "JSTypeInfo.h"
#include "TypedArrayType.h"
#include <optional>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static bool isGuessedFormOf(const ValueContext& access, Opcode genericOperation)
{
    return testsItsOwnGuess(access.value()) && genericOperationOf(access.opcode()) == genericOperation;
}

static bool guessedAccessFindsSlot(const FieldAccess& access)
{
    return access.coverage != FieldAccess::Coverage::NoMember && access.hasLocation && access.slot.hasStorage() && !access.membersWithField.isEmpty();
}

static const PlacedSlot* extensionWordOfGuessedAccess(const FieldAccess& access)
{
    return access.slot.isOutOfLine() ? &access.extensionWord : nullptr;
}

static bool isGuessedAccessToView(Opcode opcode)
{
    return opcode == Opcode::GetTypedArrayElement || opcode == Opcode::PutTypedArrayElement;
}

static bool guessedElementAccessHasTypedSide(const Operation& access)
{
    return !isGuessedAccessToView(access.opcode) || hasInlineElementAccess(access.typedArrayType());
}

static ArrayStorageFormList formsOfGuessedAccess(ElementAccessEmitter& elements, const ValueContext& access)
{
    ArrayStorageFormList forms;
    if (const ArrayStorageForm* guessed = findArrayStorageForm(access->arrayStorage())) {
        forms.append(guessed);
        if (access.opcode() == Opcode::GetArrayElement && guessed->kind == ArrayStorage::Double && access->admitsInt32Storage())
            forms.append(&arrayStorageFormOf(ArrayStorage::Int32));
        return forms;
    }
    if (access.opcode() == Opcode::PutArrayElement)
        return elements.hasOnlyExpectedArmInLine(access) ? elements.formExpectedToHold(access->arrayOperand(), access->valueOperand()) : elements.formsThatMayHold(access->arrayOperand(), access->valueOperand());
    for (const ArrayStorageForm& form : arrayStorageForms)
        forms.append(&form);
    return forms;
}

static void continueGuessedAccessIf(Output& out, Condition condition, BlockEdge otherwise)
{
    LBasicBlock next = out.newBlock();
    out.branch(condition.value(), normalEdge(next), otherwise);
    out.appendToContinuation(next);
}

static Operand ownElementOfGuessedArrayOrElse(ElementAccessEmitter& elements, Output& out, const ArrayStorageForm& form, CellPointer array, Word32 index, LBasicBlock isAbsent)
{
    ArrayElements storage = elements.elementsOf(array);
    LengthTest test = elements.testLength(Integer(out.signExt32To64(index.value())), storage);
    continueGuessedAccessIf(out, test.isInside, unlikelyEdge(isAbsent));
    Operand element = elements.loadRawElement(form, storage, test.indexInside);
    LBasicBlock holdsElement = out.newBlock();
    out.branch(elements.isHole(form, element).value(), unlikelyEdge(isAbsent), normalEdge(holdsElement));
    out.appendToContinuation(holdsElement);
    return element;
}

CellPointer GuessedAccessEmitter::receiverOrElse(const ValueContext& value, Boxed object, LayoutSetID receivers, LBasicBlock failure)
{
    Type known = typeOf(value->objectOperand());
    CellPointer cell = unboxCell(object);
    if (known.primitives & ~cellPrimitives) {
        // The empty value has no tag bits, so it passes isCell.
        Condition isPresentCell = isCell(object);
        if (known.primitives & Primitive::Absent)
            isPresentCell = logicalAnd(isPresentCell, isNotEmpty(object));
        branchUnless(isPresentCell, failure);
    }
    branchUnless(emitter<ShapeTestEmitter>().hasShapeIn(cell, receivers, known), failure);
    if (!hasOneInstance() && !holdsOnlyCompiledObjects(types(), m_plan, known))
        branchUnless(emitter<GuardEmitter>().belongsToInstance(cell), failure);
    return cell;
}

CellPointer GuessedAccessEmitter::elementsOwnerOrElse(const ValueContext& value, Boxed object, LBasicBlock failure)
{
    bool isView = isGuessedAccessToView(value.opcode());
    Type known = typeOf(isView ? value->viewOperand() : value->arrayOperand());
    if (known.primitives & ~cellPrimitives) {
        Condition isPresentCell = isCell(object);
        if (known.primitives & Primitive::Absent)
            isPresentCell = logicalAnd(isPresentCell, isNotEmpty(object));
        branchUnless(isPresentCell, failure);
    }
    CellPointer owner = unboxCell(object);
    if (isView) {
        branchUnless(hasCellType(owner, typeForTypedArrayType(value->typedArrayType())), failure);
        return owner;
    }

    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    branchUnless(hasCellType(owner, ArrayType), failure);
    ArrayStorageFormList forms = formsOfGuessedAccess(elements, value);
    if (value.opcode() != Opcode::PutArrayElement) {
        if (forms.size() == 1)
            branchUnless(elements.hasShape(elements.indexingMode(owner, arrayStorageTestMask), *forms[0]), failure);
        return owner;
    }
    if (forms.size() == 1)
        branchUnless(elements.hasWritableShape(elements.indexingMode(owner, IndexingShapeAndWritabilityMask), *forms[0]), failure);
    branchUnless(emitter<TypeTestEmitter>().isPristine(owner, BuiltinClasses::Array, PristineAspects::Storage), failure);
    return owner;
}

Word32 GuessedAccessEmitter::indexOrElse(Boxed position, LBasicBlock failure)
{
    branchUnless(isInt32(position), failure);
    return unboxInt32(position);
}

void GuessedAccessEmitter::generateSlotRead(const ValueContext& value)
{
    if (!isGuessedFormOf(value, Opcode::PropGet) || value.representation() != Representation::JSValue) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    AccessEmitter& accesses = emitter<AccessEmitter>();
    PropertyKeyID key = value->key();
    Boxed object = in<Boxed>(value->objectOperand());
    FieldAccess access = fieldAccessFor(key, value->receivers(), false);
    if (!guessedAccessFindsSlot(access)) {
        define(value, accesses.getById(value, object, key));
        return;
    }

    SlotEmitter& slots = emitter<SlotEmitter>();
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    const PlacedSlot& slot = access.slot;
    const PlacedSlot* extensionWord = extensionWordOfGuessedAccess(access);
    MemoryLocation location = MemoryLocation::objectField(key);
    bool readsEmptySlotAsUndefined = value.opcode() == Opcode::GetSlotOrUndefined || value.opcode() == Opcode::GetDeferredClosureSlotOrUndefined;
    LBasicBlock failure = newBlock();
    Join<Boxed> join(m_out);

    CellPointer cell = receiverOrElse(value, object, types().layoutSets().ofSorted(access.membersWithField.span()), failure);
    CellPointer base = emitter<SlotAccessEmitter>().baseOfSlot(cell, access);
    if (slot.representation == SlotRepresentation::DeferredClosure) {
        NarrowReference word(m_out.load32(slots.addressOf(base, slot, location, extensionWord)));
        branchUnless(above(word, NarrowReference(m_out.constInt32(absentDeferredClosure))), failure);
        join.add(conversions.decode(word, types().at(TypeTable::cellOrMarker), Representation::JSValue).as<Boxed>());
    } else if (value.opcode() == Opcode::GetSlot && value->holdsDeclaredType()) {
        continueGuessedAccessIf(m_out, slots.holdsDeclaredType(base, slot, location, extensionWord), rareEdge(failure));
        Type loaded = types().at(slot.type ? slot.type : widestTypeOf(Typed::representationOf(slot.representation)));
        loaded.primitives &= ~(Primitive::UndefinedOrNull | Primitive::Absent);
        join.add(conversions.box(value, slots.loadSlot(base, slot, location, extensionWord, loaded), loaded));
    } else {
        LBasicBlock slotIsEmpty = readsEmptySlotAsUndefined ? newBlock() : nullptr;
        continueGuessedAccessIf(m_out, slots.holdsValue(base, slot, location, extensionWord), slotIsEmpty ? unlikelyEdge(slotIsEmpty) : rareEdge(failure));
        Operand held = slots.loadSlotThatHoldsValue(base, slot, location, extensionWord);
        join.add(conversions.box(value, held, types().at(widestTypeOf(held.representation))));

        if (slotIsEmpty) {
            m_out.appendTo(slotIsEmpty);
            branchUnless(hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), TypeInfoPerCellBit | JSC::IsPristine), failure);
            if (!value->conditions().isEmpty())
                branchUnless(emitter<GuardEmitter>().areMembersUnchanged(value->conditions()), failure);
            join.add(undefined());
        }
    }

    m_out.appendTo(failure);
    join.add(accesses.getById(value, object, key));
    define(value, join.finish());
}

void GuessedAccessEmitter::generateSlotStore(const ValueContext& value)
{
    if (!isGuessedFormOf(value, Opcode::PropSet) || isDeferredClosure(value->valueOperand())) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    AccessEmitter& accesses = emitter<AccessEmitter>();
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    const PropertyKeyID key = value->key();
    ValueID storedValue = value->valueOperand();
    PutMode mode = AccessEmitter::putModeOf(value->mode());
    Boxed object = in<Boxed>(value->objectOperand());
    Operand held = get(storedValue);
    bool comesInFormOfSlot = !held.is<Boxed>();
    auto storeByName = [&] {
        accesses.putById(value, object, key, comesInFormOfSlot ? conversions.box(value, held, typeOf(storedValue)) : held.as<Boxed>(), mode);
    };
    FieldAccess access = fieldAccessFor(key, value->receivers(), true);

    LayoutSetID holders;
    TypeID storedTypeID = typeIDOf(storedValue);
    TypeID tested;
    bool mayPass = guessedAccessFindsSlot(access);
    if (mayPass) {
        holders = types().layoutSets().ofSorted(access.membersWithField.span());
        AccessContext context { m_program, WalkFacts(m_facts), m_plan, types(), m_id };
        if (!storedValueFits(context, storedTypeID, holders, key)) {
            tested = testableStoredTypeOf(context, storedTypeID, storedTypeOf(context, holders, key));
            mayPass = !comesInFormOfSlot && !isNeverType(tested) && storedValueFits(context, types().intersectionOf(storedTypeID, tested), holders, key);
        }
    }
    bool defersClosure = mayPass && access.slot.representation == SlotRepresentation::DeferredClosure;
    if (mayPass && comesInFormOfSlot)
        mayPass = held.representation == (defersClosure ? Representation::Cell32 : Typed::representationOf(access.slot.representation));
    if (!mayPass) {
        storeByName();
        return;
    }

    SlotEmitter& slots = emitter<SlotEmitter>();
    SlotAccessEmitter& slotAccesses = emitter<SlotAccessEmitter>();
    const PlacedSlot& slot = access.slot;
    const PlacedSlot* extensionWord = extensionWordOfGuessedAccess(access);
    MemoryLocation location = MemoryLocation::objectField(key);
    LBasicBlock failure = newBlock();
    LBasicBlock done = newBlock();

    CellPointer cell = receiverOrElse(value, object, holders, failure);
    bool mayCreate = value.opcode() == Opcode::PutSlotOrAdd;
    branchUnless(hasNoBits(emitter<ShapeTestEmitter>().orderWordOf(cell), mayCreate ? orderWordIntegrityBits | orderWordHasFieldOutsideSlot : orderWordIntegrityBits), failure);
    if (mayCreate) {
        branchUnless(hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), JSC::IsPristine), failure);
        if (!value->conditions().isEmpty())
            branchUnless(emitter<GuardEmitter>().areMembersUnchanged(value->conditions()), failure);
    } else
        continueGuessedAccessIf(m_out, slots.holdsValue(slotAccesses.baseOfSlot(cell, access), slot, location, extensionWord), rareEdge(failure));
    if (tested) {
        LBasicBlock fits = newBlock();
        branchOnType(held.as<Boxed>(), typeTestPlanFor(storedTypeID, Representation::JSValue, tested), { normalEdge(fits), rareEdge(failure) });
        m_out.appendTo(fits);
        storedTypeID = types().intersectionOf(storedTypeID, tested);
    }

    Type storedType = types().at(storedTypeID);
    bool needsStorage = needsStorageFor(slot, storedType);
    Operand inFormOfSlot = comesInFormOfSlot ? held : defersClosure ? Operand(conversions.compress(held.as<Boxed>(), storedType)) : conversions.unbox(held.as<Boxed>(), storedType, Typed::representationOf(slot.representation));

    auto finish = [&] {
        m_out.jump(done);
        m_out.appendTo(failure);
        storeByName();
        m_out.jump(done);
        m_out.appendTo(done);
    };

    slotAccesses.markCellIfMarkerReplacesRawValue(cell, access, storedValue);
    bool isWrittenInLine = true;
    if (mayCreate) {
        BlockFrequency frequency = generator().code().at(value.block()).frequency;
        Place coldPlace = frequency == BlockFrequency::Rare ? Place::RareBlock : frequency == BlockFrequency::Unlikely ? Place::UnlikelyBlock : Place::Anywhere;
        StateOfKnowledge slotState = slot.isOutOfLine() ? StateOfKnowledge::FieldInExtension : StateOfKnowledge::Always;
        isWrittenInLine = coldPlace == Place::Anywhere && generator().takesOpenForm(value, slotState);
        if (coldPlace != Place::Anywhere)
            m_out.noteForm(Form::Closed, slotState, coldPlace);
    }
    if (!isWrittenInLine && needsStorage && !defersClosure && slotAccesses.mayCreateListedField(access) && slotAccesses.tryPutSlotOrAddByStub(value, cell, access, inFormOfSlot)) {
        finish();
        return;
    }
    if (mayCreate && slot.isOutOfLine() && needsStorage)
        slots.ensureExtensionHolds(value, cell, access.extensionWord, granuleBitOf(slot.offset), slotAccesses.granuleBitsWithFieldsBehind(access), emitter<BarrierEmitter>().barrierOfExtension(value));

    if (mayCreate && slotAccesses.mayCreateListedField(access)) {
        auto listFieldIfMissing = [&] {
            LBasicBlock creates = newBlock();
            LBasicBlock isListed = newBlock();
            CellPointer base = slotAccesses.baseOfSlot(cell, access);
            Condition isMissing = needsStorage
                ? slots.holdsAbsent(slots.addressOf(base, slot, location, extensionWord), slot.representation)
                : logicalNot(slots.holdsValue(base, slot, location, extensionWord));
            BlockEdge toCreation = slotAccesses.edgeToCreation(creates, value->isExpectedToCreate());
            branch(isMissing, { toCreation, normalEdge(isListed) });
            m_out.appendTo(creates);
            slotAccesses.didCreateField(value, cell, access, toCreation.isRare());
            m_out.jump(isListed);
            m_out.appendTo(isListed);
        };
        slotAccesses.listCreatedFields(cell, holders, singleElementSpan(key), value->isExpectedToCreate(), listFieldIfMissing);
    }

    CellPointer base = slotAccesses.baseOfSlot(cell, access);
    if (!needsStorage)
        slots.storeIfExtensionHolds(base, slot, location, inFormOfSlot, access.extensionWord);
    else {
        if (defersClosure)
            m_out.store32(inFormOfSlot.value, slots.addressOf(base, slot, location, extensionWord));
        else
            slots.storeSlot(base, slot, location, inFormOfSlot, extensionWord, storedType);
        barrierAfterStore(value, cell, storedType);
    }
    finish();
}

void GuessedAccessEmitter::readElementOfProvenList(const ValueContext& value)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer list = in<CellPointer>(value->arrayOperand());
    bool keyIsRawNumber = representationOf(value->positionOperand()) == Representation::Double;
    bool keyIsBoxed = !keyIsRawNumber && !typeOf(value->positionOperand()).isInteger();
    LBasicBlock outOfLine = newBlock();
    Integer index = keyIsBoxed ? signExtended(indexOrElse(in<Boxed>(value->positionOperand()), outOfLine)) : elements.indexOrElse(value->positionOperand(), true, keyIsRawNumber ? outOfLine : nullptr);
    ArrayElements storage = elements.elementsOf(list);
    Join<Boxed> join(m_out);

    auto indexInsideLength = [&] {
        LengthTest test = elements.testLength(value, index, storage);
        if (!value->isInBounds())
            continueGuessedAccessIf(m_out, test.isInside, rareEdge(outOfLine));
        return test.indexInside;
    };
    auto readInForm = [&](const ArrayStorageForm& form) {
        Operand element = elements.loadRawElement(form, storage, indexInsideLength());
        continueGuessedAccessIf(m_out, logicalNot(elements.isHole(form, element)), rareEdge(outOfLine));
        join.add(elements.boxRawElement(form, element));
    };
    if (!findArrayStorageForm(value->arrayStorage())) {
        LBasicBlock holdsBoxed = newBlock();
        LBasicBlock holdsUnboxed = isDeclaredToHoldNumbersAlone(m_program, types(), typeIDOf(value->arrayOperand())) ? newBlock() : nullptr;
        m_out.branch(elements.holdsBoxedElements(list).value(), normalEdge(holdsBoxed), holdsUnboxed ? normalEdge(holdsUnboxed) : rareEdge(outOfLine));
        if (holdsUnboxed) {
            m_out.appendTo(holdsUnboxed);
            continueGuessedAccessIf(m_out, elements.hasArrayStorage(list, ArrayStorage::Double), rareEdge(outOfLine));
            readInForm(arrayStorageFormOf(ArrayStorage::Double));
        }
        m_out.appendTo(holdsBoxed);
        Boxed element = elements.loadBoxedElement(storage, indexInsideLength());
        continueGuessedAccessIf(m_out, isNotEmpty(element), rareEdge(outOfLine));
        join.add(element);
    } else if (ArrayStorageFormList forms = formsOfGuessedAccess(elements, value); forms.size() == 1) {
        continueGuessedAccessIf(m_out, elements.hasArrayStorage(list, forms[0]->kind), rareEdge(outOfLine));
        readInForm(*forms[0]);
    } else
        elements.forEachFormByShape(elements.indexingMode(list, arrayStorageTestMask), &ElementAccessEmitter::hasShape, forms, outOfLine, readInForm, ElementAccessEmitter::FirstForm::IsLikeTheOthers);

    m_out.appendTo(outOfLine);
    if (keyIsBoxed)
        join.add(emitter<AccessEmitter>().getByVal(value, in<Boxed>(value->arrayOperand()), in<Boxed>(value->positionOperand())));
    else if (keyIsRawNumber)
        join.add(emitter<AccessEmitter>().getByVal(value, in<Boxed>(value->arrayOperand()), emitter<ConversionEmitter>().boxNumberByStub(in<Number>(value->positionOperand()))));
    else
        join.add(Boxed(callStub(value, arrayGetAtIndex(AOT::ArrayRead::OfAnyList), { list.value(), index.value() }).first));
    define(value, join.finish());
}

void GuessedAccessEmitter::readNumberOfProvenList(const ValueContext& value)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer list = in<CellPointer>(value->arrayOperand());
    Representation representation = value.representation();
    Type result = typeOf(value.id());
    LBasicBlock outOfLine = newBlock();
    Integer index = elements.indexOrElse(value->positionOperand(), true, nullptr);
    ArrayElements storage = elements.elementsOf(list);
    OperandJoin join(m_out, representation);
    bool isWhole = types().at(types().withoutPrimitives(typeIDOf(value.id()), Primitive::Undefined)).isNumberWithoutFraction();
    ArrayStorageFormList forms { &arrayStorageFormOf(isWhole ? ArrayStorage::Int32 : ArrayStorage::Double), &arrayStorageFormOf(isWhole ? ArrayStorage::Double : ArrayStorage::Int32) };
    elements.forEachFormByShape(elements.indexingMode(list, arrayStorageTestMask), &ElementAccessEmitter::hasShape, forms, outOfLine, [&](const ArrayStorageForm& form) {
        LengthTest test = elements.testLength(value, index, storage);
        if (!value->isInBounds())
            continueGuessedAccessIf(m_out, test.isInside, rareEdge(outOfLine));
        Operand element = elements.loadRawElement(form, storage, test.indexInside);
        continueGuessedAccessIf(m_out, logicalNot(elements.isHole(form, element)), rareEdge(outOfLine));
        join.add(elements.convertRawElement(form, element, representation, result));
    }, ElementAccessEmitter::FirstForm::IsLikeTheOthers);

    m_out.appendTo(outOfLine);
    Boxed answer(callStub(value, testsForItself(value) ? arrayGetAtIndex(AOT::ArrayRead::OfAnyList) : arrayGetAtIndex(), { list.value(), index.value() }).first);
    join.add(emitter<ConversionEmitter>().unbox(answer, result, representation));
    define(value, join.finish());
}

bool GuessedAccessEmitter::tryReadNumberOfProvenList(const ValueContext& value)
{
    if (value.opcode() != Opcode::GetArrayElement || (value.representation() != Representation::Double && value.representation() != Representation::OptionalDouble) || findArrayStorageForm(value->arrayStorage()))
        return false;
    if (!readTakesIndexOfProvenList(types(), typeIDOf(value->arrayOperand()), typeIDOf(value->positionOperand())))
        return false;
    readNumberOfProvenList(value);
    return true;
}

void GuessedAccessEmitter::generateElementRead(const ValueContext& value)
{
    if (tryReadNumberOfProvenList(value))
        return;
    if (!isGuessedFormOf(value, Opcode::ElemGet) || value.representation() != Representation::JSValue) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    if (value.opcode() == Opcode::GetArrayElement && isProvenToBeList(types(), typeIDOf(value->arrayOperand()))) {
        readElementOfProvenList(value);
        return;
    }
    if (value.opcode() == Opcode::GetArrayElement) {
        define(value, Boxed(callStub(value, getByValKeepingRegisters(), { in<Boxed>(value->arrayOperand()).value(), in<Boxed>(value->positionOperand()).value() }).first));
        return;
    }

    AccessEmitter& accesses = emitter<AccessEmitter>();
    Boxed object = in<Boxed>(value->viewOperand());
    Boxed position = in<Boxed>(value->positionOperand());
    if (!guessedElementAccessHasTypedSide(value.value())) {
        define(value, accesses.getByVal(value, object, position));
        return;
    }

    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    LBasicBlock failure = newBlock();
    Join<Boxed> join(m_out);

    CellPointer owner = elementsOwnerOrElse(value, object, failure);
    Word32 index = indexOrElse(position, failure);
    TypedArrayType type = value->typedArrayType();
    TypedArrayElements storage = elements.typedArrayElementsOf(owner, failure);
    Integer indexInView = signExtended(index);
    branchUnless(below(indexInView, storage.length), failure);
    Operand element = elements.loadTypedArrayElement(storage, indexInView, type);
    join.add(conversions.box(value, element, types().at(elementTypeOf(type))));

    m_out.appendTo(failure);
    join.add(accesses.getByVal(value, object, position));
    define(value, join.finish());
}

void GuessedAccessEmitter::storeInProvenListUnderAnyKey(const ValueContext& value)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer list = in<CellPointer>(value->arrayOperand());
    ValueID stored = value->valueOperand();
    LBasicBlock outOfLine = newBlock();
    LBasicBlock continuation = newBlock();
    Integer index = signExtended(indexOrElse(in<Boxed>(value->positionOperand()), outOfLine));
    ArrayElements storage = elements.elementsOf(list);
    elements.forEachFormByShape(elements.indexingMode(list, IndexingShapeAndWritabilityMask), &ElementAccessEmitter::hasWritableShape, formsOfGuessedAccess(elements, value), outOfLine, [&](const ArrayStorageForm& form) {
        std::optional<Operand> rawElement = elements.rawElementToStore(form, stored, outOfLine);
        if (!rawElement) {
            m_out.jump(outOfLine);
            return;
        }
        LengthTest test = elements.testLength(index, storage);
        branchUnless(test.isInside, outOfLine);
        elements.storeRawElement(form, storage, test.indexInside, *rawElement);
        if (form.holdsCells && typeOf(stored).hasCells()) {
            Boxed storedElement = rawElement->as<Boxed>();
            emitter<BarrierEmitter>().elementBarrierAfterStore(value, list, singleElementSpan(storedElement));
        }
        m_out.jump(continuation);
    }, ElementAccessEmitter::FirstForm::IsLikeTheOthers);

    m_out.appendTo(outOfLine);
    emitter<AccessEmitter>().putByVal(value, in<Boxed>(value->arrayOperand()), in<Boxed>(value->positionOperand()), in<Boxed>(stored), AccessEmitter::putModeOf(value->mode()));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void GuessedAccessEmitter::generateElementStore(const ValueContext& value)
{
    if (!isGuessedFormOf(value, Opcode::ElemSet)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    if (value.opcode() == Opcode::PutArrayElement && storeTakesIndexOfProvenList(types(), typeIDOf(value->arrayOperand()), typeIDOf(value->positionOperand()))) {
        ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
        elements.storeInProvenList(value, formsOfGuessedAccess(elements, value));
        return;
    }
    if (value.opcode() == Opcode::PutArrayElement && isProvenToBeList(types(), typeIDOf(value->arrayOperand()))) {
        storeInProvenListUnderAnyKey(value);
        return;
    }

    AccessEmitter& accesses = emitter<AccessEmitter>();
    bool storesInView = isGuessedAccessToView(value.opcode());
    ValueID storedValue = value->valueOperand();
    PutMode mode = AccessEmitter::putModeOf(value->mode());
    Boxed object = in<Boxed>(storesInView ? value->viewOperand() : value->arrayOperand());
    Boxed position = in<Boxed>(value->positionOperand());
    Boxed stored = in<Boxed>(storedValue);
    if (!guessedElementAccessHasTypedSide(value.value())) {
        accesses.putByVal(value, object, position, stored, mode);
        return;
    }

    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    LBasicBlock failure = newBlock();
    LBasicBlock done = newBlock();

    CellPointer owner = elementsOwnerOrElse(value, object, failure);
    Word32 index = indexOrElse(position, failure);
    if (storesInView) {
        TypedArrayType type = value->typedArrayType();
        Operand element;
        if (isClamped(type)) {
            branchUnless(isInt32(stored), failure);
            element = unboxInt32(stored);
        } else {
            branchUnless(isNumber(stored), failure);
            if (isFloat(type))
                element = numberToDouble(stored);
            else {
                element = selectByBranch<Word32>(isInt32(stored), [&] {
                    return unboxInt32(stored);
                }, [&] {
                    return emitter<ConversionEmitter>().toInt32(unboxDouble(stored));
                });
            }
        }
        TypedArrayElements storage = elements.typedArrayElementsOf(owner, failure);
        Integer indexInView = signExtended(index);
        branchUnless(below(indexInView, storage.length), failure);
        elements.storeTypedArrayElement(storage, indexInView, type, element);
        m_out.jump(done);
    } else {
        auto storeInForm = [&](const ArrayStorageForm& form) {
            std::optional<Operand> element = elements.rawElementToStore(form, storedValue, failure);
            if (!element) {
                m_out.jump(failure);
                return;
            }
            ArrayElements storage = elements.elementsOf(owner);
            LengthTest test = elements.testLength(signExtended(index), storage);
            branchUnless(test.isInside, failure);
            elements.storeRawElement(form, storage, test.indexInside, *element);
            if (form.holdsCells && typeOf(storedValue).hasCells()) {
                Boxed storedElement = element->as<Boxed>();
                emitter<BarrierEmitter>().elementBarrierAfterStore(value, owner, singleElementSpan(storedElement));
            }
            m_out.jump(done);
        };
        ArrayStorageFormList forms = formsOfGuessedAccess(elements, value);
        if (forms.size() == 1)
            storeInForm(*forms[0]);
        else
            elements.forEachFormByShape(elements.indexingMode(owner, IndexingShapeAndWritabilityMask), &ElementAccessEmitter::hasWritableShape, forms, failure, storeInForm, ElementAccessEmitter::FirstForm::IsLikeTheOthers);
    }

    m_out.appendTo(failure);
    accesses.putByVal(value, object, position, stored, mode);
    m_out.jump(done);

    m_out.appendTo(done);
}

void GuessedAccessEmitter::generateElementTest(const ValueContext& value)
{
    bool givesCondition = value.isBranchCondition() || value.representation() == Representation::Boolean;
    if (!isGuessedFormOf(value, Opcode::ElemHas) || (!givesCondition && value.representation() != Representation::JSValue)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    AccessEmitter& accesses = emitter<AccessEmitter>();
    Boxed object = in<Boxed>(value->arrayOperand());
    Boxed position = in<Boxed>(value->positionOperand());
    Condition hasElement;
    if (!guessedElementAccessHasTypedSide(value.value()))
        hasElement = accesses.hasByVal(value, object, position);
    else {
        ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
        LBasicBlock failure = newBlock();
        LBasicBlock elementIsAbsent = newBlock();
        Join<Condition> join(m_out);

        CellPointer array = elementsOwnerOrElse(value, object, failure);
        Word32 index = indexOrElse(position, failure);
        auto testInForm = [&](const ArrayStorageForm& form) {
            ownElementOfGuessedArrayOrElse(elements, m_out, form, array, index, elementIsAbsent);
            join.add(boolean(true));
        };
        ArrayStorageFormList forms = formsOfGuessedAccess(elements, value);
        if (forms.size() == 1)
            testInForm(*forms[0]);
        else
            elements.forEachFormByShape(elements.indexingMode(array, arrayStorageTestMask), &ElementAccessEmitter::hasShape, forms, failure, testInForm, ElementAccessEmitter::FirstForm::IsLikeTheOthers);

        m_out.appendTo(elementIsAbsent);
        Condition prototypesHaveNoElements = emitter<TypeTestEmitter>().isPristine(array, BuiltinClasses::Array, PristineAspects::Storage);
        branchUnless(logicalAnd(prototypesHaveNoElements, greaterThanOrEqual(index, 0)), failure);
        join.add(boolean(false));

        m_out.appendTo(failure);
        join.add(accesses.hasByVal(value, object, position));
        hasElement = join.finish();
    }

    if (givesCondition)
        defineTest(value, hasElement);
    else
        define(value, boxBoolean(hasElement));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
