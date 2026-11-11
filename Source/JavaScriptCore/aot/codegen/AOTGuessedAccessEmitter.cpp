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
#include "AOTCompileUnit.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTIRProgram.h"
#include "AOTMemoryLocation.h"
#include "AOTOperatorForms.h"
#include "AOTRepresentation.h"
#include "AOTResultRules.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStaleAnswers.h"
#include "AOTStubsArrays.h"
#include "AOTStubsElements.h"
#include "AOTThrowEmitter.h"
#include "AOTTruthinessEmitter.h"
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
    return access.coverage != FieldAccess::Coverage::NoMember && access.hasLocation && access.slot.hasStorage() && !access.slot.holdsAccessor && !access.membersWithField.isEmpty();
}

static const PlacedSlot* extensionWordOfGuessedAccess(const FieldAccess& access)
{
    return access.slot.isOutOfLine() ? &access.extensionWord : nullptr;
}

static bool isGuessedAccessToView(Opcode opcode)
{
    return opcode == Opcode::GetTypedArrayElement || opcode == Opcode::PutTypedArrayElement;
}

static bool storesInViewOfSeveralKinds(const Operation& access)
{
    return access.opcode == Opcode::PutTypedArrayElement && access.typedArrayType() == NotTypedArray;
}

static bool guessedElementAccessHasTypedSide(const Operation& access)
{
    return !isGuessedAccessToView(access.opcode) || hasInlineElementAccess(access.typedArrayType()) || storesInViewOfSeveralKinds(access);
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

CellPointer GuessedAccessEmitter::receiverOrElse(const ValueContext& value, Boxed object, LayoutSetID receivers, LBasicBlock failure, const FieldAccess* readBehindTest, std::optional<CellPointer>* baseOfSlot)
{
    Type known = typeOf(value->objectOperand());
    CellPointer cell = unboxCell(object);
    MarkedObjects marked = value.opcode() == Opcode::GetSlot && value->isFilledWhereShapeFits() ? MarkedObjects::AreRefused : MarkedObjects::MayBeRefused;
    if ((value.opcode() == Opcode::GetSlot || value.opcode() == Opcode::GetSlotOrUndefined) && value->receiverMayBeInBirth())
        marked = MarkedObjects::Pass;
    if (known.primitives & ~cellPrimitives) {
        // The empty value has no tag bits, so it passes isCell.
        Condition isPresentCell = isCell(object);
        if (known.primitives & Primitive::Absent)
            isPresentCell = logicalAnd(isPresentCell, isNotEmpty(object));
        branchUnless(isPresentCell, failure);
    }
    bool isHeldAgainstEveryShape = marked == MarkedObjects::AreRefused || !known.hasOnlyLayouts() || known.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) || !holdsOnlyCompiledObjects(types(), m_plan, known);
    if (readBehindTest && isHeldAgainstEveryShape)
        *baseOfSlot = emitter<SlotAccessEmitter>().baseOfSlotWhereOneTableTellsShapeAndDistance(cell, *readBehindTest, receivers, marked != MarkedObjects::Pass, failure);
    if (!baseOfSlot || !*baseOfSlot)
        branchUnless(emitter<ShapeTestEmitter>().hasShapeIn(cell, receivers, known, marked), failure);
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
        if (!storesInViewOfSeveralKinds(value.value()))
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

    bool asksWhatSlotsHold = sourcesOfProofsCheckedUnder(generator().build().options().testing) & bitOfSourceOfProofs(ProofSource::Field);
    std::optional<CodeGenerator::WaysOfTruth> ways = asksWhatSlotsHold || slot.representation == SlotRepresentation::DeferredClosure ? std::nullopt : generator().waysOfTruthOf(value);
    TypeID heldBySlots;
    if (ways || asksWhatSlotsHold) {
        AccessContext context { m_program, WalkFacts(m_facts), m_plan, types(), m_id };
        heldBySlots = typeHeldBySlotsOf(context, access.membersWithField.span(), key);
    }
    TypeID askedOfWhatSlotsHold = heldBySlots;
    if (asksWhatSlotsHold && heldBySlots) {
        bool namesOrdinaryObjects = std::ranges::any_of(membersOf(types(), types().at(heldBySlots)), [&](LayoutID member) {
            return !m_plan.layout(member).holdsCompiledObjects();
        });
        bool namesWhatIsOutOfReach = m_facts.testMentionsWhatIsOutOfReach(types(), heldBySlots, scopedLambda<bool(FunctionID)>([&](FunctionID named) {
            return m_facts.linked().mayBeMentionedBy(generator().unit(), named);
        }));
        if (namesOrdinaryObjects || namesWhatIsOutOfReach)
            askedOfWhatSlotsHold = types().unionOf(types().withoutObjects(heldBySlots), types().at(heldBySlots).hasOnlyClosures() ? TypeTable::function : TypeTable::object);
    }
    Type held;
    if (ways) {
        if (heldBySlots) {
            held = types().at(heldBySlots);
            held.primitives &= ~Primitive::Absent;
        }
        bool tellsTruth = heldBySlots && held.isJavaScriptValue()
            && (ways->areOfObjectAndNothing ? !(held.primitives & ~Primitive::UndefinedOrNull) : chooseConversionForm(m_program, types(), Opcode::ToBoolean, types().intern(held)) != OperatorForm::Generic);
        if (!tellsTruth)
            ways = std::nullopt;
    }
    std::optional<Join<Boxed>> join;
    if (!ways)
        join.emplace(m_out);
    bool asksObjectAgain = accesses.verifiesFastPathAnswers(key) && m_program.at(key).kind != PropertyKeyKind::PrivateName;
    auto askedAgainOfObject = [&](Boxed answered) {
        if (asksObjectAgain)
            callVoidOperation(value, RuntimeOperation::StopAtStaleAnswer, { object, answered, identifierNumber(key), word32(static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Value, StubThatAnswers::TypedReadByName))) });
        return answered;
    };
    auto foundInSlot = [&](Boxed found) {
        if (asksWhatSlotsHold && heldBySlots)
            emitter<ThrowEmitter>().requireOrCrash(value, hasType(found, typeTestPlanFor(TypeTable::anythingOrAbsent, Representation::JSValue, askedOfWhatSlotsHold)), found, ProofSource::Field);
        return askedAgainOfObject(found);
    };

    LayoutSetID holders = membersWithFieldAmong(access, value->receivers());
    std::optional<CellPointer> baseByOneTable;
    CellPointer cell = receiverOrElse(value, object, holders, failure, &access, &baseByOneTable);
    if (baseByOneTable || emitter<ShapeTestEmitter>().lastTestIsHeldAgainstEveryShape())
        access.layoutsThatTestInCodeLetThrough = holders;
    bool letsMarkedObjectsPass = (value.opcode() == Opcode::GetSlot || value.opcode() == Opcode::GetSlotOrUndefined) && value->receiverMayBeInBirth();
    bool receiverBearsNoMark = baseByOneTable ? !letsMarkedObjectsPass : emitter<ShapeTestEmitter>().lastTestRejectsMarks();
    CellPointer base = baseByOneTable ? *baseByOneTable : emitter<SlotAccessEmitter>().baseOfSlot(cell, access, holders);
    if (slot.representation == SlotRepresentation::DeferredClosure) {
        NarrowReference word(m_out.load32(slots.addressOf(base, slot, location, extensionWord)));
        branchUnless(above(word, NarrowReference(m_out.constInt32(absentDeferredClosure))), failure);
        join->add(conversions.decode(word, types().at(TypeTable::cellOrMarker), Representation::JSValue).as<Boxed>());
    } else if (value.opcode() == Opcode::GetSlot && value->holdsDeclaredType()) {
        if (!value->isFilledWhereShapeFits() || !receiverBearsNoMark)
            continueGuessedAccessIf(m_out, slots.holdsDeclaredType(base, slot, location, extensionWord), rareEdge(failure));
        else if (asksWhatSlotsHold)
            emitter<ThrowEmitter>().requireOrCrash(value, slots.holdsDeclaredType(base, slot, location, extensionWord), std::nullopt, ProofSource::Field);
        Type loaded = types().at(slot.type ? slot.type : widestTypeOf(Typed::representationOf(slot.representation)));
        loaded.primitives &= ~(Primitive::UndefinedOrNull | Primitive::Absent);
        if (ways) {
            held.primitives &= ~Primitive::UndefinedOrNull;
            branch(emitter<TruthinessEmitter>().isTruthy(value, slots.loadSlot(base, slot, location, extensionWord, loaded), held), ways->targets);
        } else
            join->add(foundInSlot(conversions.box(value, slots.loadSlot(base, slot, location, extensionWord, loaded), loaded)));
    } else {
        LBasicBlock slotIsEmpty = readsEmptySlotAsUndefined ? newBlock() : nullptr;
        bool asksWhetherSlotIsFilled = value.opcode() != Opcode::GetSlot || !value->isFilledWhereShapeFits() || !receiverBearsNoMark;
        LBasicBlock slotIsEmptyWhereItIsToldFilled = !asksWhetherSlotIsFilled && asksWhatSlotsHold ? newBlock() : nullptr;
        Operand loaded = asksWhetherSlotIsFilled
            ? slots.loadSlotOrLeaveIfEmpty(base, slot, location, extensionWord, slotIsEmpty ? unlikelyEdge(slotIsEmpty) : rareEdge(failure))
            : slotIsEmptyWhereItIsToldFilled ? slots.loadSlotOrLeaveIfEmpty(base, slot, location, extensionWord, rareEdge(slotIsEmptyWhereItIsToldFilled))
            : slots.loadSlotThatHoldsValue(base, slot, location, extensionWord);
        if (ways)
            branch(emitter<TruthinessEmitter>().isTruthy(value, loaded, held), ways->targets);
        else
            join->add(foundInSlot(conversions.box(value, loaded, types().at(widestTypeOf(loaded.representation)))));

        if (slotIsEmptyWhereItIsToldFilled) {
            m_out.appendTo(slotIsEmptyWhereItIsToldFilled);
            emitter<ThrowEmitter>().crashOnFalseProof(value, std::nullopt, ProofSource::Field);
        }
        if (slotIsEmpty) {
            m_out.appendTo(slotIsEmpty);
            branchUnless(hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), TypeInfoPerCellBit | JSC::IsPristine), failure);
            if (!value->conditions().isEmpty())
                branchUnless(emitter<GuardEmitter>().areMembersUnchanged(value->conditions()), failure);
            if (ways)
                m_out.jump(ways->targets.notTaken);
            else
                join->add(askedAgainOfObject(undefined()));
        }
    }

    m_out.appendTo(failure);
    if (ways) {
        define(value, accesses.getById(value, object, key));
        return;
    }
    join->add(accesses.getById(value, object, key));
    define(value, join->finish());
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
        holders = membersWithFieldAmong(access, value->receivers());
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
    if (emitter<ShapeTestEmitter>().lastTestIsHeldAgainstEveryShape())
        access.layoutsThatTestInCodeLetThrough = holders;
    bool mayCreate = value.opcode() == Opcode::PutSlotOrAdd;
    bool isWrittenInLine = true;
    if (mayCreate) {
        BlockFrequency frequency = generator().code().at(value.block()).frequency;
        Place coldPlace = frequency == BlockFrequency::Rare ? Place::RareBlock : frequency == BlockFrequency::Unlikely ? Place::UnlikelyBlock : Place::Anywhere;
        StateOfKnowledge slotState = slot.isOutOfLine() ? StateOfKnowledge::FieldInExtension : StateOfKnowledge::Always;
        isWrittenInLine = coldPlace == Place::Anywhere && generator().takesOpenForm(value, slotState);
        if (coldPlace != Place::Anywhere)
            m_out.noteForm(Form::Closed, slotState, coldPlace);
    }
    bool overwritesFilledSlot = mayCreate && isWrittenInLine && !value->isExpectedToCreate() && !slot.isOutOfLine() && slotAccesses.mayCreateListedField(access);
    auto checkCreationConditions = [&] {
        branchUnless(hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), JSC::IsPristine), failure);
        if (!value->conditions().isEmpty())
            branchUnless(emitter<GuardEmitter>().areMembersUnchanged(value->conditions()), failure);
    };
    uint32_t bitsBlockingStore = value->isDefinition() ? orderWordIntegrityBits : orderWordIsFrozen;
    branchUnless(hasNoBits(emitter<ShapeTestEmitter>().orderWordOf(cell), mayCreate && !overwritesFilledSlot ? orderWordIntegrityBits | orderWordHasFieldOutsideSlot : bitsBlockingStore), failure);
    if (mayCreate && !overwritesFilledSlot)
        checkCreationConditions();
    if (!mayCreate)
        continueGuessedAccessIf(m_out, slots.holdsValue(slotAccesses.baseOfSlot(cell, access, holders), slot, location, extensionWord), rareEdge(failure));
    if (tested) {
        LBasicBlock fits = newBlock();
        branchOnType(held.as<Boxed>(), typeTestPlanFor(TypeTable::anythingOrAbsent, Representation::JSValue, tested), { normalEdge(fits), rareEdge(failure) });
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

    if (overwritesFilledSlot) {
        LBasicBlock creates = newBlock();
        LBasicBlock overwrites = newBlock();
        LBasicBlock stores = newBlock();
        CellPointer base = slotAccesses.baseOfSlot(cell, access, holders);
        branch(slots.holdsAbsent(slots.addressOf(base, slot, location, extensionWord), slot.representation), { rareEdge(creates), normalEdge(overwrites) });

        m_out.appendTo(creates);
        branchUnless(hasNoBits(emitter<ShapeTestEmitter>().orderWordOf(cell), orderWordIntegrityBits | orderWordHasFieldOutsideSlot), failure);
        checkCreationConditions();
        slotAccesses.askObjectWhatStandsInWayOfStore(value, cell);
        slotAccesses.didCreateField(value, cell, access, true);
        m_out.jump(stores);

        m_out.appendTo(overwrites);
        slotAccesses.askObjectWhatStandsInWayOfStore(value, cell);
        m_out.jump(stores);

        m_out.appendTo(stores);
        slotAccesses.markCellIfMarkerReplacesRawValue(cell, access, storedValue);
        slots.storeSlot(slotAccesses.baseOfSlot(cell, access, holders), slot, location, inFormOfSlot, extensionWord, storedType);
        barrierAfterStore(value, cell, storedType);
        finish();
        return;
    }
    slotAccesses.askObjectWhatStandsInWayOfStore(value, cell);
    slotAccesses.markCellIfMarkerReplacesRawValue(cell, access, storedValue);
    if (!isWrittenInLine && needsStorage && !defersClosure && slotAccesses.mayCreateListedField(access) && slotAccesses.tryPutSlotOrAddByStub(value, cell, access, inFormOfSlot)) {
        finish();
        return;
    }
    if (mayCreate && slot.isOutOfLine() && needsStorage)
        slots.ensureExtensionHolds(value, cell, access.extensionWord, granuleBitOf(slot.offset), slotAccesses.granuleBitsWithLaterStores(value, access), emitter<BarrierEmitter>().barrierOfExtension(value));

    if (mayCreate && slotAccesses.mayCreateListedField(access)) {
        auto listFieldIfMissing = [&] {
            LBasicBlock creates = newBlock();
            LBasicBlock isListed = newBlock();
            CellPointer base = slotAccesses.baseOfSlot(cell, access, holders);
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

    CellPointer base = slotAccesses.baseOfSlot(cell, access, holders);
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

bool GuessedAccessEmitter::unitReckonsWithElement(const ValueContext& read)
{
    static constexpr OpcodeSet reckoners = numberProducingOperators | allSums | allRelations | allBitwiseOperators;
    if (!m_reckonersAreKnown) {
        m_reckonersAreKnown = true;
        const Code& code = generator().code();
        for (uint32_t block = 0; block < code.numberOfBlocks(); ++block) {
            for (ValueID id : code.valuesOf(BlockID(block))) {
                const Operation& user = code.at(id);
                if (user.opcode == Opcode::PutArrayElement) {
                    m_isReckonedWith.set(user.valueOperand().index());
                    continue;
                }
                bool reckons = reckoners.contains(user.opcode) || (user.opcode == Opcode::Unbox && user.representation != Representation::Cell)
                    || (user.opcode == Opcode::TypeTest && types().at(user.testedType()).hasOnlyPrimitives(Primitive::Number));
                if (!reckons)
                    continue;
                user.forEachOperand([&](ValueID operand, uint32_t) {
                    m_isReckonedWith.set(operand.index());
                });
            }
        }
    }
    return m_isReckonedWith.get(read.id().index());
}

static bool isTestedAfterJump(const Code& code, const ValueContext& read)
{
    unsigned operationsLeft = 4;
    ValueID carried = read.id();
    BlockID block = read.block();
    std::span<const ValueID> operations = read.operationsBehind();
    for (unsigned stepsLeft = 4; stepsLeft--;) {
        for (ValueID id : operations) {
            const Operation& operation = code.at(id);
            if (operation.opcode == Opcode::CheckProof || operation.opcode == Opcode::LoopHint)
                continue;
            if (operation.opcode == Opcode::ToBoolean)
                return operation.operand(0) == carried;
            if (!operationsLeft--)
                return false;
        }
        const Terminator& terminator = code.at(block).terminator;
        if (terminator.kind != TerminatorKind::Jump)
            return false;
        const Edge& edge = code.at(terminator.next());
        std::span<const ValueID> arguments = edge.arguments();
        std::span<const ValueID> parameters = code.at(edge.to).parameters();
        size_t position = std::ranges::find(arguments, carried) - arguments.begin();
        if (position < std::min(arguments.size(), parameters.size()))
            carried = parameters[position];
        block = edge.to;
        operations = code.valuesOf(block);
    }
    return false;
}

bool GuessedAccessEmitter::isTestedForPresence(const ValueContext& read)
{
    const Code& code = generator().code();
    for (ValueID id : read.operationsBehind()) {
        const Operation& operation = code.at(id);
        if (operation.opcode == Opcode::CheckProof)
            continue;
        if (operation.opcode == Opcode::ToBoolean)
            return operation.operand(0) == read.id();
        if (operation.opcode != Opcode::TypeTest || operation.operand(0) != read.id())
            break;
        TypeID tested = operation.testedType();
        return types().at(tested).hasOnlyPrimitives(Primitive::UndefinedOrNull) || tested == types().withoutNullish(typeIDOf(read.id()));
    }
    return code.at(read.block()).frequency != BlockFrequency::Rare && isTestedAfterJump(code, read);
}

void GuessedAccessEmitter::readElementOfProvenList(const ValueContext& value)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer list = in<CellPointer>(value->arrayOperand());
    bool keyIsOptionalNumber = representationOf(value->positionOperand()) == Representation::OptionalDouble;
    bool keyIsRawNumber = keyIsOptionalNumber || representationOf(value->positionOperand()) == Representation::Double;
    bool keyIsOptionalInteger = representationOf(value->positionOperand()) == Representation::OptionalInt64;
    bool keyIsBoxed = !keyIsRawNumber && !keyIsOptionalInteger && !typeOf(value->positionOperand()).isInteger();
    LBasicBlock outOfLine = newBlock();
    Integer index = keyIsOptionalInteger ? Integer(in<OptionalInteger>(value->positionOperand()).value()) : keyIsBoxed ? signExtended(indexOrElse(in<Boxed>(value->positionOperand()), outOfLine)) : elements.indexOrElse(value->positionOperand(), !keyIsRawNumber || typeOf(value->positionOperand()).isNumberWithoutFraction(), keyIsRawNumber ? outOfLine : nullptr, ElementAccessEmitter::NaNIndexMode::ProduceOutOfBoundsIndex);
    ArrayElements storage = elements.elementsOf(list);
    Join<Boxed> join(m_out);
    const Type& read = typeOf(value.id());
    bool isOfTable = read.hasObjects() && read.primitives & Primitive::Null && !(read.primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent)) && m_code.at(value.block()).frequency != BlockFrequency::Rare;
    bool answersAbsence = read.primitives & Primitive::Undefined && generator().runsRepeatedly(value.block()) && (isOfTable || isTestedForPresence(value));
    LBasicBlock isAbsent = answersAbsence ? newBlock() : nullptr;
    BlockEdge whereAbsent = answersAbsence ? normalEdge(isAbsent) : rareEdge(outOfLine);

    auto indexInsideLength = [&] {
        LengthTest test = elements.testLength(value, index, storage);
        if (!value->isInBounds())
            continueGuessedAccessIf(m_out, test.isInside, whereAbsent);
        return test.indexInside;
    };
    auto readInForm = [&](const ArrayStorageForm& form) {
        Operand element = elements.loadRawElement(form, storage, indexInsideLength());
        if (!value->listHasNoHoles())
            continueGuessedAccessIf(m_out, logicalNot(elements.isHole(form, element)), whereAbsent);
        else
            elements.checkElementPresenceProof(value, logicalNot(elements.isHole(form, element)));
        join.add(elements.boxRawElement(form, element));
    };
    if (!findArrayStorageForm(value->arrayStorage())) {
        LBasicBlock holdsBoxed = newBlock();
        bool isDeclaredToHoldNumbers = isDeclaredToHoldNumbersAlone(m_program, types(), typeIDOf(value->arrayOperand()));
        bool isReckonedWithInLoop = !isDeclaredToHoldNumbers && generator().runsRepeatedly(value.block()) && (typeOf(value.id()).primitives & Primitive::Number) && unitReckonsWithElement(value);
        LBasicBlock holdsUnboxed = isDeclaredToHoldNumbers || isReckonedWithInLoop ? newBlock() : nullptr;
        if (isReckonedWithInLoop)
            build().statistics().count(StatisticID::BoxedReadsWithDoublePath);
        m_out.branch(elements.holdsBoxedElements(list).value(), normalEdge(holdsBoxed), isDeclaredToHoldNumbers ? normalEdge(holdsUnboxed) : rareEdge(holdsUnboxed ? holdsUnboxed : outOfLine));
        if (holdsUnboxed) {
            m_out.appendTo(holdsUnboxed);
            continueGuessedAccessIf(m_out, elements.hasArrayStorage(list, ArrayStorage::Double), rareEdge(outOfLine));
            readInForm(arrayStorageFormOf(ArrayStorage::Double));
        }
        m_out.appendTo(holdsBoxed);
        Boxed element = elements.loadBoxedElement(storage, indexInsideLength());
        if (!value->listHasNoHoles())
            continueGuessedAccessIf(m_out, isNotEmpty(element), whereAbsent);
        else
            elements.checkElementPresenceProof(value, isNotEmpty(element));
        join.add(element);
    } else if (ArrayStorageFormList forms = formsOfGuessedAccess(elements, value); forms.size() == 1) {
        continueGuessedAccessIf(m_out, elements.hasArrayStorage(list, forms[0]->kind), rareEdge(outOfLine));
        readInForm(*forms[0]);
    } else
        elements.forEachFormByShape(elements.indexingMode(list, arrayStorageTestMask), &ElementAccessEmitter::hasShape, forms, outOfLine, readInForm, ElementAccessEmitter::FirstForm::IsLikeTheOthers);

    if (answersAbsence) {
        m_out.appendTo(isAbsent);
        Type key = typeOf(value->positionOperand());
        if (!key.isInteger() || !key.integers.isSubsetOf(Range { 0, MAX_ARRAY_INDEX }))
            branchUnless(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), outOfLine);
        branchUnless(elements.isPristineArray(list, { PristineAspects::Storage, PristineAspects::Names }), outOfLine);
        join.add(undefined());
    }

    m_out.appendTo(outOfLine);
    if (keyIsOptionalInteger)
        join.add(emitter<AccessEmitter>().getByVal(value, in<Boxed>(value->arrayOperand()), emitter<ConversionEmitter>().boxNumberByStub(in<OptionalInteger>(value->positionOperand()))));
    else if (keyIsBoxed)
        join.add(emitter<AccessEmitter>().getByVal(value, in<Boxed>(value->arrayOperand()), in<Boxed>(value->positionOperand())));
    else if (keyIsOptionalNumber)
        join.add(emitter<AccessEmitter>().getByVal(value, in<Boxed>(value->arrayOperand()), emitter<ConversionEmitter>().boxNumberByStub(in<OptionalNumber>(value->positionOperand()))));
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
    bool keyIsOptionalNumber = representationOf(value->positionOperand()) == Representation::OptionalDouble;
    bool keyIsRawNumber = keyIsOptionalNumber || representationOf(value->positionOperand()) == Representation::Double;
    Integer index = elements.indexOrElse(value->positionOperand(), !keyIsRawNumber || typeOf(value->positionOperand()).isNumberWithoutFraction(), keyIsRawNumber ? outOfLine : nullptr, ElementAccessEmitter::NaNIndexMode::ProduceOutOfBoundsIndex);
    ArrayElements storage = elements.elementsOf(list);
    OperandJoin join(m_out, representation);
    auto readInForm = [&](const ArrayStorageForm& form) {
        LengthTest test = elements.testLength(value, index, storage);
        if (!value->isInBounds())
            continueGuessedAccessIf(m_out, test.isInside, rareEdge(outOfLine));
        Operand element = elements.loadRawElement(form, storage, test.indexInside);
        if (!value->listHasNoHoles())
            continueGuessedAccessIf(m_out, logicalNot(elements.isHole(form, element)), rareEdge(outOfLine));
        else
            elements.checkElementPresenceProof(value, logicalNot(elements.isHole(form, element)));
        join.add(elements.convertRawElement(value, form, element, representation, typeIDOf(value.id())));
    };
    bool takesAnyNumber = types().intersectionOf(typeIDOf(value.id()), TypeTable::number) == TypeID(TypeTable::number);
    auto readDouble = [&] {
        continueGuessedAccessIf(m_out, elements.hasArrayStorage(list, ArrayStorage::Double), rareEdge(outOfLine));
        readInForm(arrayStorageFormOf(ArrayStorage::Double));
    };
    auto readBoxed = [&] {
        readInForm(arrayStorageFormOf(ArrayStorage::Contiguous));
    };
    if (m_code.at(value.block()).frequency == BlockFrequency::Rare)
        m_out.jump(outOfLine);
    else if (!takesAnyNumber || m_code.isConstant(value->positionOperand())) {
        LBasicBlock holdsBoxed = newBlock();
        LBasicBlock holdsOther = newBlock();
        bool mayHoldFractions = result.primitives & Primitive::FractionalNumber;
        m_out.branch(elements.holdsBoxedElements(list).value(), normalEdge(holdsBoxed), mayHoldFractions ? normalEdge(holdsOther) : unlikelyEdge(holdsOther));
        m_out.appendTo(holdsOther);
        readDouble();
        m_out.appendTo(holdsBoxed);
        readBoxed();
    } else {
        LBasicBlock holdsDoubles = newBlock();
        LBasicBlock holdsOther = newBlock();
        m_out.branch(elements.hasArrayStorage(list, ArrayStorage::Double).value(), normalEdge(holdsDoubles), normalEdge(holdsOther));
        m_out.appendTo(holdsOther);
        continueGuessedAccessIf(m_out, elements.holdsBoxedElements(list), rareEdge(outOfLine));
        readBoxed();
        m_out.appendTo(holdsDoubles);
        readInForm(arrayStorageFormOf(ArrayStorage::Double));
    }

    m_out.appendTo(outOfLine);
    elements.stopUnlessListIsAsMadeWhereNothingIsFound(value, list);
    bool mayHaveNumberNames = value->mayHaveNumberNames();
    AOT::ArrayRead read = mayHaveNumberNames ? AOT::ArrayRead::OfPlainList : testsForItself(value) ? AOT::ArrayRead::OfAnyList : AOT::ArrayRead::OfListAsMade;
    Boxed answer = keyIsOptionalNumber
        ? emitter<AccessEmitter>().getByVal(value, in<Boxed>(value->arrayOperand()), emitter<ConversionEmitter>().boxNumberByStub(in<OptionalNumber>(value->positionOperand())))
        : keyIsRawNumber && mayHaveNumberNames ? Boxed(callStub(value, arrayGetAtNumber(read), { list.value(), in<Number>(value->positionOperand()).value() }).first)
        : keyIsRawNumber
        ? emitter<AccessEmitter>().getByVal(value, in<Boxed>(value->arrayOperand()), emitter<ConversionEmitter>().boxNumberByStub(in<Number>(value->positionOperand())))
        : Boxed(callStub(value, arrayGetAtIndex(read), { list.value(), index.value() }).first);
    elements.stopUnlessElementIsOf(value, answer, typeIDOf(value.id()));
    join.add(emitter<ConversionEmitter>().unbox(answer, result, representation));
    define(value, join.finish());
}

bool GuessedAccessEmitter::tryReadNumberOfProvenList(const ValueContext& value)
{
    if (value.opcode() != Opcode::GetArrayElement || (value.representation() != Representation::Double && value.representation() != Representation::OptionalDouble) || findArrayStorageForm(value->arrayStorage()))
        return false;
    if (!numberOfProvenListIsReadUnboxed(types(), typeIDOf(value->arrayOperand()), typeIDOf(value->positionOperand()), representationOf(value->positionOperand())))
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
        Boxed position = in<Boxed>(value->positionOperand());
        LValue constantIndex = emitter<AccessEmitter>().tryUnboxConstantIndex(position);
        if (constantIndex && isKnownIndex(constantIndex->asInt64())) {
            define(value, Boxed(callStub(value, getByValAtKnownIndex(static_cast<unsigned>(constantIndex->asInt64())), { in<Boxed>(value->arrayOperand()).value() }).first));
            return;
        }
        StubKey stub = constantIndex ? getByValAtConstantIndex() : getByValKeepingRegisters();
        define(value, Boxed(callStub(value, stub, { in<Boxed>(value->arrayOperand()).value(), constantIndex ? constantIndex : position.value() }).first));
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
    bool keyIsOptionalNumber = representationOf(value->positionOperand()) == Representation::OptionalDouble;
    bool keyIsRawNumber = keyIsOptionalNumber || representationOf(value->positionOperand()) == Representation::Double;
    Integer index = keyIsRawNumber ? elements.indexOrElse(value->positionOperand(), typeOf(value->positionOperand()).isNumberWithoutFraction(), outOfLine, ElementAccessEmitter::NaNIndexMode::ProduceOutOfBoundsIndex) : signExtended(indexOrElse(in<Boxed>(value->positionOperand()), outOfLine));
    ArrayElements storage = elements.elementsOf(list);
    build().statistics().count(StatisticID::UntypedKeyStoresAppendingInline);
    ArrayStorageFormList forms = formsOfGuessedAccess(elements, value);
    elements.forEachFormByShape(elements.indexingMode(list, IndexingShapeAndWritabilityMask), &ElementAccessEmitter::hasWritableShape, forms, outOfLine, [&](const ArrayStorageForm& form) {
        std::optional<Operand> rawElement = elements.rawElementToStore(form, stored, outOfLine);
        if (!rawElement) {
            m_out.jump(outOfLine);
            return;
        }
        LengthTest test = elements.testLength(index, storage);
        LBasicBlock inBounds = newBlock();
        LBasicBlock pastEnd = newBlock();
        m_out.branch(test.isInside.value(), normalEdge(inBounds), normalEdge(pastEnd));
        m_out.appendTo(pastEnd);
        branchUnless(below(test.indexInside, elements.vectorLength(storage)), outOfLine);
        elements.setPublicLength(storage, add(test.indexInside, integer(1)));
        m_out.jump(inBounds);
        m_out.appendTo(inBounds);
        elements.stopWhereHoleWouldBeStored(value, form, *rawElement);
        elements.storeRawElement(form, storage, test.indexInside, *rawElement);
        if (form.holdsCells && typeOf(stored).hasCells()) {
            Boxed storedElement = rawElement->as<Boxed>();
            emitter<BarrierEmitter>().elementBarrierAfterStore(value, list, singleElementSpan(storedElement));
        }
        m_out.jump(continuation);
    }, ElementAccessEmitter::FirstForm::IsLikeTheOthers, elements.int32ThatArmOfValuesStoresInEitherKind(stored, forms));

    m_out.appendTo(outOfLine);
    if (keyIsRawNumber && !keyIsOptionalNumber && value->mode() == ECMAMode::Strict) {
        build().statistics().count(StatisticID::ArrayStoreKeysPassedUnboxed);
        const Type& storedType = typeOf(stored);
        bool mayConvertToDouble = storedType.isNumberLike() && !storedType.isInt32() && storedType.primitives & Primitive::FractionalNumber;
        callStub(value, arrayPutAtNumber(mayConvertToDouble ? AOT::ArrayStoredValue::Number : AOT::ArrayStoredValue::Value), { list.value(), in<Boxed>(stored).value(), in<Number>(value->positionOperand()).value() });
    } else {
        Boxed key = keyIsOptionalNumber ? emitter<ConversionEmitter>().boxNumberByStub(in<OptionalNumber>(value->positionOperand())) : keyIsRawNumber ? emitter<ConversionEmitter>().boxNumberByStub(in<Number>(value->positionOperand())) : in<Boxed>(value->positionOperand());
        emitter<AccessEmitter>().putByVal(value, in<Boxed>(value->arrayOperand()), key, in<Boxed>(stored), AccessEmitter::putModeOf(value->mode()));
    }
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
    if (storesInViewOfSeveralKinds(value.value())) {
        branchUnless(isNumber(stored), failure);
        Number element = numberToDouble(stored);
        TypedArrayElements storage = elements.typedArrayElementsOf(owner, failure);
        Integer indexInView = signExtended(index);
        branchUnless(below(indexInView, storage.length), failure);
        elements.forEachKindOfView(value, owner, scopedLambda<void(TypedArrayType)>([&](TypedArrayType kind) {
            elements.storeTypedArrayElement(storage, indexInView, kind, element);
            m_out.jump(done);
        }));
    } else if (storesInView) {
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
                LBasicBlock int32Case = newBlock();
                LBasicBlock doubleCase = newBlock();
                Join<Word32> join(m_out);
                ScopeFork fork = m_out.forkScope();
                m_out.branch(isInt32(stored).value(), normalEdge(int32Case), unlikelyEdge(doubleCase));
                m_out.appendToArm(int32Case, fork);
                join.add(unboxInt32(stored));
                m_out.appendToArm(doubleCase, fork);
                join.add(emitter<ConversionEmitter>().toInt32(unboxDouble(stored)));
                element = join.finishBehind(fork);
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
            elements.stopWhereHoleWouldBeStored(value, form, *element);
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

Condition GuessedAccessEmitter::hasElementOfList(const ValueContext& value)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    ValueID position = value->positionOperand();
    CellPointer list = in<CellPointer>(value->arrayOperand());
    Integer index = elements.indexOrElse(position, true, nullptr, ElementAccessEmitter::NaNIndexMode::ProduceOutOfBoundsIndex);
    ArrayElements storage = elements.elementsOf(list);
    LBasicBlock holdsBoxed = newBlock();
    LBasicBlock holdsUnboxed = newBlock();
    LBasicBlock isAbsent = newBlock();
    LBasicBlock outOfLine = newBlock();
    Join<Condition> join(m_out);
    auto testInForm = [&](const ArrayStorageForm& form) {
        LengthTest test = elements.testLength(value, index, storage);
        continueGuessedAccessIf(m_out, test.isInside, unlikelyEdge(isAbsent));
        continueGuessedAccessIf(m_out, logicalNot(elements.isHole(form, elements.loadRawElement(form, storage, test.indexInside))), unlikelyEdge(isAbsent));
        join.add(boolean(true));
    };
    m_out.branch(elements.holdsBoxedElements(list).value(), normalEdge(holdsBoxed), normalEdge(holdsUnboxed));
    m_out.appendTo(holdsUnboxed);
    continueGuessedAccessIf(m_out, elements.hasArrayStorage(list, ArrayStorage::Double), rareEdge(outOfLine));
    testInForm(arrayStorageFormOf(ArrayStorage::Double));
    m_out.appendTo(holdsBoxed);
    testInForm(arrayStorageFormOf(ArrayStorage::Contiguous));

    m_out.appendTo(isAbsent);
    branchUnless(emitter<TypeTestEmitter>().isPristine(list, BuiltinClasses::Array, PristineAspects::Storage), outOfLine);
    branchUnless(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), outOfLine);
    join.add(boolean(false));

    m_out.appendTo(outOfLine);
    join.add(emitter<AccessEmitter>().hasByVal(value, boxCell(list), emitter<ConversionEmitter>().box(value, get(position), typeOf(position))));
    return join.finish();
}

void GuessedAccessEmitter::generateElementTest(const ValueContext& value)
{
    bool givesCondition = value.isBranchCondition() || value.representation() == Representation::Boolean;
    if (!isGuessedFormOf(value, Opcode::ElemHas) || (!givesCondition && value.representation() != Representation::JSValue)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    if (readTakesIndexOfProvenList(types(), typeIDOf(value->arrayOperand()), typeIDOf(value->positionOperand()))) {
        Condition hasElement = hasElementOfList(value);
        if (givesCondition)
            defineTest(value, hasElement);
        else
            define(value, boxBoolean(hasElement));
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
