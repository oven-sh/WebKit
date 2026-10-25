/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeValidationEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTable.h"
#include "AOTCellFormat.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardFactDetails.h"
#include "AOTIdentifierNumbering.h"
#include "AOTPlacement.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTSort.h"
#include "AOTStaticsEmitter.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTTypeTestEmitter.h"
#include "AOTTypeTestPlan.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "VM.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool mayHoldPointer(Operand operand)
{
    return operand.is<CellPointer>() || operand.is<Boxed>();
}

// A receiver without the field has other bytes where the slot would be.
static bool everyReceiverHasInlineSlot(const FieldAccess& field)
{
    bool everyReceiverHasSlot = field.coverage == FieldAccess::Coverage::EveryMember || field.coverage == FieldAccess::Coverage::EveryMemberHasSlot;
    return everyReceiverHasSlot && field.slot.hasStorage() && !field.slot.isOutOfLine();
}

void RuntimeValidationEmitter::checkValue(ValueID id, Operand operand)
{
    if (generator().mayBeUnspecified(id) || operand.representation == Representation::Void || operand.representation == Representation::LiteralString)
        return;
    const Type& type = typeOf(id);
    if (type.isNever() || type.opaque)
        return;
    if (!type.environment) {
        // A deleted member leaves its variable empty, and a read may stand in front of the test that the member is unchanged.
        const Value& definition = m_function.at(id);
        bool readsMemberKeptInVariable = definition.opcode == Opcode::GetModuleVariable && m_program.at(definition.variable()).owner;
        bool mayPrecedeItsStore = definition.opcode == Opcode::GetModuleVariable && definition.mayPrecedeItsStore();
        checkHasType(id, operand, readsMemberKeptInVariable || mayPrecedeItsStore ? type.withPrimitives(Primitive::Absent) : type, ValidationFailureKind::ValueType);
        return;
    }
    if (operand.is<CellPointer>())
        checkEnvironmentShape(operand.as<CellPointer>(), type.environment, id);
}

// A cell of a frame may have no header. In a validating build only environments live in frames (staysOnHeapForValidation()), so the header of an object or a closure is there to be read.
void RuntimeValidationEmitter::checkHasType(ValueID id, Operand operand, const Type& type, ValidationFailureKind kind)
{
    bool mustBeThere = mayHoldPointer(operand) && !(type.primitives & Primitive::Absent);
    TypeID widest = types().intern(widestTypeOf(operand.representation));
    const TypeTestPlan& plan = typeTestPlanFor(widest, operand.representation, types().intern(testableTypeOf(type)));
    bool needsTest = plan.verdict != TypeTestVerdict::AlwaysPasses;
    if (!mustBeThere && !needsTest)
        return;

    // One report for both: what was found says which of the two failed.
    LBasicBlock doesNotHold = newBlock();
    LBasicBlock continuation = newBlock();
    if (mustBeThere) {
        LBasicBlock isThere = needsTest ? newBlock() : continuation;
        m_out.branch(m_out.notZero64(operand.value), normalEdge(isThere), rareEdge(doesNotHold));
        if (needsTest)
            m_out.appendTo(isThere);
    }
    if (needsTest)
        m_out.branch(hasType(operand, plan).value(), normalEdge(continuation), rareEdge(doesNotHold));
    m_out.appendTo(doesNotHold);
    reportFailure(kind, operand, id);
    m_out.appendTo(continuation);
}

void RuntimeValidationEmitter::checkIsSpecified(ValueID id)
{
    if (!hasRegister(representationOf(id)) || isDeferredClosure(m_function, id))
        return;
    Operand operand = get(id);
    if (!mayHoldPointer(operand) || typeOf(id).primitives & Primitive::Absent)
        return;
    failUnless(Condition(m_out.notZero64(operand.value)), ValidationFailureKind::UnspecifiedValueInspected, { }, id);
}

bool RuntimeValidationEmitter::hasTestableBits(ValueID id)
{
    if (generator().mayBeUnspecified(id) || isDeferredClosure(m_function, id))
        return false;
    Representation representation = representationOf(id);
    return representation != Representation::Void && representation != Representation::LiteralString;
}

void RuntimeValidationEmitter::generateValidateType(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    const Type& type = m_program.at(value->validatedType());
    if (type.isNever()) {
        failUnless(boolean(false), ValidationFailureKind::UnreachableValue, hasTestableBits(source) ? get(source) : Operand(), value.id());
        return;
    }
    if (!hasTestableBits(source) || type.opaque || type.environment)
        return;
    checkHasType(value.id(), get(source), type, ValidationFailureKind::AssumedType);
    checkClassProofs(value.id(), get(source), type);
}

// The shapes ascend, and the layouts of one family have numbers in a row.
Condition RuntimeValidationEmitter::hasShapeAmong(CellPointer cell, LayoutSetID set, std::span<const uint32_t> ascendingShapes)
{
    static constexpr size_t maxNumberOfComparedRuns = 16;
    struct RunOfShapes {
        uint32_t last;
        uint32_t count;
    };
    Vector<RunOfShapes, maxNumberOfComparedRuns> runs;
    for (uint32_t shape : ascendingShapes) {
        if (!runs.isEmpty() && runs.last().last + 1 == shape) {
            runs.last().last = shape;
            ++runs.last().count;
        } else
            runs.append({ shape, 1 });
    }

    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    if (set && runs.size() > maxNumberOfComparedRuns)
        return shapeTests.hasShapeIn(cell, set, Type::object());
    Condition hasShape;
    Word32 shapeWord = shapeTests.shapeWordOf(cell);
    static_assert(AOT::shapeWordOf(2) + 1 == AOT::shapeWordOf(1));
    for (RunOfShapes run : runs)
        hasShape = logicalOr(hasShape, below(add(shapeWord, word32(run.last)), word32(run.count)));
    return hasShape;
}

// A guard may rely on a guard: whether the fields are in the order of the layout is asked of an object that an earlier test found to have the layout.
void RuntimeValidationEmitter::checkShapeBehindGuard(const OperationContext& context, const Value& user)
{
    if (familyOf(user.opcode) == OperationFamily::SlotAccess || !reliesOf(user.opcode).contains(Proof::Shape))
        return;
    ValueID object = guardedOperandOf(user);
    ValueID guard = user.guardOperand();
    if (!object || !guard || !hasTestableBits(object) || !get(object).is<CellPointer>())
        return;
    const GuardFact* fact = analyses().guardFacts(guard).find(Proof::Shape, analyses().identityOf(object));
    if (!fact)
        return;
    const SlotPlan& slots = m_placement.slots();
    Vector<uint32_t, 8> shapes;
    for (LayoutID layout : layoutsProvenBy(m_program, m_function, *fact)) {
        if (slots.storageOf(layout) != LayoutStorage::CompiledObject && m_program.at(layout).flow.allocation != AllocationState::NeverAllocated)
            return;
        ShapeRange range = m_placement.shapes().shapesOf(layout);
        for (uint32_t shape = range.first; shape < range.end(); ++shape)
            shapes.append(shape);
    }
    if (shapes.isEmpty())
        return;
    AOT::sort(shapes);
    CellPointer cell = in<CellPointer>(object);
    failUnless(hasShapeAmong(cell, { }, shapes.span()), ValidationFailureKind::GuardNoLongerHolds, cell, context.valueID());
}

// The type of the object need not name the layouts of an access: a guard may stand for them, and a guard is no value that could be tested.
void RuntimeValidationEmitter::checkReceiverOfSlotAccess(const OperationContext& context, const Value& access)
{
    if (familyOf(access.opcode) != OperationFamily::SlotAccess)
        return;
    ValueID object = access.objectOperand();
    if (!hasTestableBits(object) || !get(object).is<CellPointer>())
        return;
    const SlotPlan& slots = m_placement.slots();
    bool hasOnlyCompiledObjects = std::ranges::all_of(m_program.at(access.receivers()).members, [&](LayoutID member) {
        return slots.storageOf(member) == LayoutStorage::CompiledObject || m_program.at(member).flow.allocation == AllocationState::NeverAllocated;
    });
    std::span<const uint32_t> shapes = m_placement.shapes().memberShapes(access.receivers());
    if (!hasOnlyCompiledObjects || shapes.empty())
        return;

    CellPointer cell = in<CellPointer>(object);
    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    Condition hasShapeOfReceiver = hasShapeAmong(cell, access.receivers(), shapes);
    auto kindOf = [&](ValidationFailureKind withoutGuard) {
        return access.guardOperand() ? ValidationFailureKind::GuardNoLongerHolds : withoutGuard;
    };
    failUnless(hasShapeOfReceiver, kindOf(ValidationFailureKind::ReceiverShape), cell, context.valueID());

    // A read that has no test for the empty marker takes the marker for a value. A read that has one, and a store that creates the field, may find the slot empty.
    const FieldAccess& field = m_placement.fieldAccess(access.fieldAccess());
    if (reliesOf(access.opcode).contains(Proof::SlotFilled) && everyReceiverHasInlineSlot(field))
        failUnless(emitter<SlotEmitter>().isFilled(emitter<SlotAccessEmitter>().baseOfSlot(cell, field), field.slot, MemoryLocation::objectField(field.key, field.receivers)), kindOf(ValidationFailureKind::SlotEmpty), cell, context.valueID());
    // A private field of a frozen object is stored like any other.
    if (reliesOf(access.opcode).contains(Proof::NotFrozen) && m_program.at(field.key).kind != PropertyKeyKind::PrivateName)
        failUnless(hasNoBits(shapeTests.orderWordOf(cell), orderWordIntegrityBits), kindOf(ValidationFailureKind::OperationPreconditionBroken), cell, context.valueID());
}

// The form of a slot with markers is as wide as the form without them, so a read whose type admits no marker loads the bare bits.
void RuntimeValidationEmitter::checkRawNumberSlot(const OperationContext& context, CellPointer cell, const FieldAccess& field)
{
    ValueID id = context.valueID();
    if (!id || !reliesOf(m_function.at(id).opcode).contains(Proof::SlotFilled) || !everyReceiverHasInlineSlot(field))
        return;
    const Type& type = typeOf(id);
    SlotTraits traits = traitsOf(field.slot.representation);
    bool holdsNumbers = traits.valueKind == SlotValueKind::SignedInteger || traits.valueKind == SlotValueKind::Double;
    bool isReadWithoutMarkers = holdsNumbers ? type.isNumber() : traits.valueKind == SlotValueKind::Boolean && type.isBoolean();
    if (!traits.hasUndefined || !isReadWithoutMarkers)
        return;

    SlotEmitter& slots = emitter<SlotEmitter>();
    Operand held = slots.load(slots.addressOf(emitter<SlotAccessEmitter>().baseOfSlot(cell, field), field.slot, MemoryLocation::objectField(field.key, field.receivers)), field.slot.representation);
    Condition holdsValue = held.is<Boxed>() ? isBoolean(held.as<Boxed>()) : logicalNot(emitter<ConversionEmitter>().isMarker(held));
    failUnless(holdsValue, m_function.at(id).guardOperand() ? ValidationFailureKind::GuardNoLongerHolds : ValidationFailureKind::SlotEmpty, cell, id);
}

// No type test asks for what flow proved about the instances of a class: what they hold, how they store it, and that they are pristine.
void RuntimeValidationEmitter::checkClassProofs(ValueID id, Operand operand, const Type& type)
{
    if (!mayHoldPointer(operand))
        return;
    TypeID widest = types().intern(widestTypeOf(operand.representation));

    // A built-in object that the type lists beside the class may be an instance of the class, and nothing is proven about it: Array.prototype is an array.
    const TypeTestPlan* isListedBuiltin = nullptr;
    if (!type.builtins.isEmpty()) {
        Type listedBuiltins;
        for (BuiltinID builtin : type.builtins) {
            if (!m_placement.builtinCell(builtin))
                return;
            listedBuiltins = listedBuiltins.unionWith(Type::forBuiltin(builtin), types());
        }
        isListedBuiltin = &typeTestPlanFor(widest, operand.representation, types().intern(listedBuiltins));
        if (isListedBuiltin->verdict == TypeTestVerdict::AlwaysPasses)
            return;
        if (isListedBuiltin->verdict == TypeTestVerdict::NeverPasses)
            isListedBuiltin = nullptr;
    }

    for (const ClassPart& part : type.classes) {
        bool hasProofs = part.contentsAreProven || !part.provenPristine.isEmpty();
        if (!hasProofs)
            continue;
        const TypeTestPlan& isInstance = typeTestPlanFor(widest, operand.representation, types().intern(Type::builtinClass(part.builtinClass).withoutClassProofs()));
        if (isInstance.verdict != TypeTestVerdict::NeedsTest || !m_program.at(isInstance.to).classes.size())
            continue;

        LBasicBlock instance = newBlock();
        LBasicBlock continuation = newBlock();
        Condition isProvenInstance = hasType(operand, isInstance);
        if (isListedBuiltin)
            isProvenInstance = logicalAnd(isProvenInstance, logicalNot(hasType(operand, *isListedBuiltin)));
        m_out.branch(isProvenInstance.value(), normalEdge(instance), normalEdge(continuation));
        m_out.appendTo(instance);
        checkClassProofs(id, operand.is<CellPointer>() ? operand.as<CellPointer>() : unboxCell(operand.as<Boxed>()), part);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
    }
}

void RuntimeValidationEmitter::checkClassProofs(ValueID id, CellPointer cell, const ClassPart& part)
{
    // Every array starts with the marks that the test reads. An object of another class may be pristine without them.
    OptionSet<PristineAspects> aspects = part.provenPristine - aspectsWithoutTest;
    if (!aspects.isEmpty() && part.builtinClass == BuiltinClasses::Array && pristineTestOf(m_builtins, part.builtinClass) == PristineTest::MayPass) {
        TypeTestEmitter& tests = emitter<TypeTestEmitter>();
        if (aspects.contains(PristineAspects::Names))
            askOperationUnless(tests.isPristine(cell, part.builtinClass, PristineAspects::Names), ValidationFailureKind::ArrayNotPristine, cell, id);
        OptionSet<PristineAspects> otherAspects = aspects - PristineAspects::Names;
        if (!otherAspects.isEmpty())
            askOperationUnless(tests.isPristine(cell, part.builtinClass, otherAspects), ValidationFailureKind::ArrayElementsNotOrdinary, cell, id);
    }
    // Beside contents that are not proven the storage is an expectation, and every access tests it.
    if (!part.contentsAreProven)
        return;
    if (hasArrayStorageForm(part.arrayStorage))
        failUnless(emitter<ElementAccessEmitter>().hasArrayStorage(cell, part.arrayStorage), ValidationFailureKind::ArrayStorageKind, cell, id);
    for (ContentsSlot slot : allMembersOf<ContentsSlot>) {
        TypeID contents = part.contentsAt(m_builtins, slot);
        if (contents == TypeTable::anything || !m_builtins.findSlot(part.builtinClass, slot))
            continue;
        if (!m_placement.typesNeededAtRunTime().get(contents.index()))
            continue;
        Integer holds = callWithoutSite<Integer>(RuntimeOperation::ContentsHaveType, { cell, word32(static_cast<int32_t>(slot)), word32(static_cast<int32_t>(contents.index())) });
        failUnless(isNotZero(holds), ValidationFailureKind::ContainerContents, cell, id);
    }
}

void RuntimeValidationEmitter::generateValidateFirstRun(const ValueContext& value)
{
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    failUnless(emitter<ConversionEmitter>().isAbsent(statics.loadVariable(value->variable())), ValidationFailureKind::SecondRun, { }, value.id());
    statics.storeVariable(value->variable(), boxedConstant(jsBoolean(true)), Type::fromPrimitives(Primitive::Boolean));
}

void RuntimeValidationEmitter::generateShouldFailPrediction(const ValueContext& value)
{
    TypedPointer state = instanceField(m_heaps.counters, AOT_INSTANCE_OFFSET(runtime.exceptions.predictionFailureState));
    LValue drawn = m_out.load32(state);
    drawn = m_out.bitXor(drawn, m_out.shl(drawn, m_out.constInt32(13)));
    drawn = m_out.bitXor(drawn, m_out.lShr(drawn, m_out.constInt32(17)));
    drawn = m_out.bitXor(drawn, m_out.shl(drawn, m_out.constInt32(5)));
    m_out.store32(drawn, state);
    uint32_t period = build().recordedOptions().predictionFailurePeriod;
    RELEASE_ASSERT(period);
    build().statistics().count(StatisticID::PlacesWherePredictionsFailUnasked);
    defineTest(value, Condition(m_out.belowOrEqual(drawn, m_out.constInt32(static_cast<int32_t>(UINT32_MAX / period)))));
}

// One number may be held as an integer and as a double, and NaN with any sign and payload.
void RuntimeValidationEmitter::generateValidateSameValue(const ValueContext& value)
{
    ValueID kept = value->keptOperand();
    ValueID replacement = value->replacementOperand();
    if (!hasTestableBits(kept) || !hasTestableBits(replacement))
        return;
    // A builder makes a new string each time it is asked for one: the same characters in another object.
    auto isMadeByBuilder = [&](ValueID id) {
        return m_function.at(definitionThroughUpcasts(m_function, id)).opcode == Opcode::StringBuilderToString;
    };
    if (isMadeByBuilder(kept) || isMadeByBuilder(replacement))
        return;
    // A view of a buffer that can be resized has a Structure of its own, which start-up does not mark: the test says no of a view that is pristine.
    const Value& keptOperation = m_function.at(kept);
    if (keptOperation.opcode == Opcode::IsPristine && m_builtins.at(keptOperation.builtinClass()).typedArrayType != NotTypedArray)
        return;
    // The test for storage that a store may write says no while another thread holds the lock of the cell. Code relies on its yes alone.
    if (keptOperation.opcode == Opcode::HasArrayStorage && keptOperation.isWritable() && hasArrayStorageForm(keptOperation.arrayStorage()) && get(replacement).is<Condition>()) {
        ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
        CellPointer array = in<CellPointer>(keptOperation.arrayOperand());
        Condition hasForm = elements.hasWritableShapeIgnoringLock(elements.indexingMode(array, IndexingShapeAndWritabilityMask), arrayStorageFormOf(keptOperation.arrayStorage()));
        failUnless(logicalOr(logicalNot(in<Condition>(replacement)), hasForm), ValidationFailureKind::GuardNoLongerHolds, array, value.id());
        return;
    }

    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Boxed keptValue = conversions.box(value, get(kept), typeOf(kept));
    Boxed replacingValue = conversions.box(value, get(replacement), typeOf(replacement));
    const TypeTestPlan& isNumber = typeTestPlanFor(TypeTable::anything, Representation::JSValue, TypeTable::number);
    ValidationFailureKind kind = provesOf(m_function.at(kept).opcode) == Proof::None ? ValidationFailureKind::ReadsDiffer : ValidationFailureKind::GuardNoLongerHolds;

    LBasicBlock differ = newBlock();
    LBasicBlock areNumbers = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(m_out.equal(keptValue.value(), replacingValue.value()), normalEdge(continuation), rareEdge(differ));

    m_out.appendTo(differ);
    failUnless(logicalAnd(hasType(keptValue, isNumber), hasType(replacingValue, isNumber)), kind, keptValue, value.id());
    m_out.jump(areNumbers);

    m_out.appendTo(areNumbers);
    Number keptNumber = conversions.unbox(keptValue, m_program.at(TypeTable::number), Representation::Double).as<Number>();
    Number replacingNumber = conversions.unbox(replacingValue, m_program.at(TypeTable::number), Representation::Double).as<Number>();
    Condition areBothNaN = logicalNot(logicalOr(equal(keptNumber, keptNumber), equal(replacingNumber, replacingNumber)));
    failUnless(logicalOr(equal(keptNumber, replacingNumber), areBothNaN), kind, keptValue, value.id());
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// The listed callees that a test can tell apart one by one.
static Type testableCallees(const Type& listed, const BuiltinTable& builtins, const Placement& placement, TypeArena& types)
{
    Type callees;
    callees.functions = listed.functions;
    for (BuiltinID builtin : listed.builtins) {
        if (isTestableBuiltin(builtins, builtin) && placement.builtinCell(builtin))
            callees.builtins.add(builtin);
    }
    for (const ClassPart& part : listed.classes) {
        if (builtins.at(part.builtinClass).instanceTest != BuiltinInstanceTest::None)
            callees = callees.unionWith(Type::builtinClass(part.builtinClass), types);
    }
    return callees;
}

// No built-in function is a proxy. A built-in that is a new object for each module may be a bound function: `require` is one.
static Type kindsRejectedDespiteWidening(const Type& listed, const BuiltinTable& builtins, TypeArena& types)
{
    Type kinds = Type::builtinClass(BuiltinClasses::Proxy);
    bool everyBuiltinIsOneObject = std::ranges::all_of(listed.builtins, [&](BuiltinID builtin) {
        return isTestableBuiltin(builtins, builtin);
    });
    if (everyBuiltinIsOneObject)
        kinds = kinds.unionWith(Type::builtinClass(BuiltinClasses::BoundFunction), types);
    return kinds;
}

// A complete set of callees says which functions the call may run.
void RuntimeValidationEmitter::generateValidateCallee(const ValueContext& value)
{
    ValueID callee = value->calleeOperand();
    if (!hasTestableBits(callee))
        return;

    Operand operand = get(callee);
    TypeID widest = types().intern(widestTypeOf(operand.representation));
    auto planFor = [&](const Type& type) -> const TypeTestPlan& {
        return typeTestPlanFor(widest, operand.representation, types().intern(type));
    };
    const Type& listed = m_program.at(value->validatedType());
    if (listed.anyFunction || hasMembersWithoutLayout(m_program, listed))
        return;
    Type testable = testableTypeOf(listed);
    bool isWidened = (testable.anyFunction && !listed.anyFunction) || (testable.anyObject && !listed.anyObject);

    const TypeTestPlan& isFunction = planFor(Type::function());
    const TypeTestPlan& isListed = planFor(testable);
    if (isFunction.verdict == TypeTestVerdict::NeverPasses || (isListed.verdict == TypeTestVerdict::AlwaysPasses && !isWidened))
        return;
    Condition mayBeCalled = hasType(operand, isListed);
    if (isWidened) {
        Condition isTestableCallee = hasType(operand, planFor(testableCallees(listed, m_builtins, m_placement, types())));
        Condition isRejectedKind = hasType(operand, planFor(kindsRejectedDespiteWidening(listed, m_builtins, types())));
        mayBeCalled = logicalOr(isTestableCallee, logicalAnd(mayBeCalled, logicalNot(isRejectedKind)));
    }
    failUnless(logicalOr(logicalNot(hasType(operand, isFunction)), mayBeCalled), ValidationFailureKind::UnlistedCallee, operand, value.id());
}

// A method call that became a direct call reads no method. The operation looks the name up and runs no code of the program: an accessor, a proxy and a miss fail like another function.
void RuntimeValidationEmitter::generateValidateLookup(const ValueContext& value)
{
    ValueID object = value->objectOperand();
    ValueID expected = value->expectedOperand();
    if (!hasTestableBits(object) || !hasTestableBits(expected))
        return;

    Operand receiver = get(object);
    Operand method = get(expected);
    auto objectTestFor = [&](Operand operand) -> const TypeTestPlan& {
        return typeTestPlanFor(types().intern(widestTypeOf(operand.representation)), operand.representation, TypeTable::object);
    };
    auto cellOf = [&](Operand operand) {
        if (operand.is<NarrowReference>())
            return decode(operand.as<NarrowReference>());
        return operand.is<CellPointer>() ? operand.as<CellPointer>() : unboxCell(operand.as<Boxed>());
    };

    // A receiver that is no object makes the call throw, or finds the method on the prototype of its wrapper.
    const TypeTestPlan& receiverIsObject = objectTestFor(receiver);
    const TypeTestPlan& methodIsObject = objectTestFor(method);
    if (receiverIsObject.verdict == TypeTestVerdict::NeverPasses || methodIsObject.verdict == TypeTestVerdict::NeverPasses)
        return;
    LBasicBlock areObjects = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(logicalAnd(hasType(receiver, receiverIsObject), hasType(method, methodIsObject)).value(), normalEdge(areObjects), normalEdge(continuation));
    m_out.appendTo(areObjects);
    Integer findsMethod = callWithoutSite<Integer>(RuntimeOperation::ValidateLookup, { cellOf(receiver), identifierNumber(value->key()), cellOf(method) });
    failUnless(isNotZero(findsMethod), ValidationFailureKind::LookupDiffers, receiver, value.id());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

Type RuntimeValidationEmitter::testableTypeOf(const Type& type)
{
    Type testable = type.withoutClassProofs().withUntestableBuiltinsWidened();

    // Functions, objects of the host and ordinary objects were converted to such a family, and nothing lists them.
    if (hasMembersWithoutLayout(m_program, type))
        testable = testable.unionWith(Type::object(), types());

    // Placement numbers the cells and keys that shipped code names, and validation adds none.
    bool everyBuiltinHasCell = std::ranges::all_of(testable.builtins, [&](BuiltinID builtin) {
        return m_placement.builtinCell(builtin).has_value();
    });
    if (!everyBuiltinHasCell)
        testable = testable.unionWith(Type::object(), types());

    bool everySymbolHasNumber = std::ranges::all_of(testable.symbolLiterals, [&](PropertyKeyID symbol) {
        return build().identifierNumbering().numberOf(symbol).has_value();
    });
    if (!everySymbolHasNumber)
        testable = testable.withPrimitives(Primitive::Symbol);
    return testable;
}

void RuntimeValidationEmitter::checkEnvironmentShape(CellPointer cell, LayoutID layout, ValueID id)
{
    uint32_t shape = m_placement.shapes().generalShapeOf(layout);
    if (m_program.at(layout).kind == LayoutKind::Closure || !shape)
        return;
    static_assert(offsetOfEnvironmentShapeLowByte == JSCell::indexingTypeAndMiscOffset() && offsetOfEnvironmentShapeHighByte == JSCell::typeInfoFlagsOffset());
    static_assert(environmentShapesPerPageShift == 16);
    Word32 lowByte = loadByte(cell, m_heaps.JSCell_indexingTypeAndMisc);
    Word32 highByte = loadByte(cell, m_heaps.JSCell_typeInfoFlags);
    Condition hasShape = equal(bitOr(lowByte, shiftLeft(highByte, word32(8))), word32(shape & 0xffff));
    if (inputs().frameAllocation.mayBeInFrame(layout))
        hasShape = logicalOr(isInStack(asPointer(cell)), hasShape);
    failUnless(hasShape, ValidationFailureKind::ValueType, { }, id);
}

void RuntimeValidationEmitter::checkIsNotReleased(const OperationContext& context, CellPointer environment, LayoutID layout)
{
    std::optional<uint32_t> freeCellOffset = m_placement.freeCellOffset(layout);
    if (!freeCellOffset)
        return;
    CellPointer freeCell(m_out.load64(emitter<StaticsEmitter>().inStatics(m_heaps.collectorState, *freeCellOffset)));
    failUnless(notEqual(freeCell, environment), ValidationFailureKind::ValueType, { }, context.valueID());
}

void RuntimeValidationEmitter::checkIsNotFrameCell(const OperationContext& context, Operand operand, std::optional<Operand> retainer)
{
    if (!mayHoldPointer(operand))
        return;
    Condition escapes = isInStack(Pointer(operand.value));
    if (retainer && mayHoldPointer(*retainer))
        escapes = logicalAnd(escapes, logicalNot(isInStack(Pointer(retainer->value))));
    failUnless(logicalNot(escapes), ValidationFailureKind::FrameCellEscaped, { }, context.valueID());
}

bool RuntimeValidationEmitter::isSuspensionPoint(ValueID id)
{
    return maySuspend(m_function.at(id).opcode) && analyses().maySuspend(id);
}

// Whether a path from the allocation to the point on which the allocation does not run again has a suspension.
bool RuntimeValidationEmitter::maySuspendBetween(ValueID allocation, ProgramPoint point)
{
    const CFG& cfg = analyses().cfg();
    ValueLocation start = analyses().useLists().locationOf(allocation);
    BitVector barrier(m_function.blocks.size());
    barrier.set(start.block.index());
    for (unsigned blockIndex : cfg.blocksReachableFrom(start.block, barrier)) {
        BlockID block(blockIndex);
        const BlockValues& values = m_function.at(block).values;
        for (uint32_t index = block == start.block ? start.index + 1 : 0; index < values.size(); ++index) {
            if (!isSuspensionPoint(values[index]))
                continue;
            bool reachesPoint = block == point.block
                ? index < point.index || cfg.isInCycleAvoiding(block, start.block)
                : point.block != start.block && cfg.blocksReachableFrom(block, barrier).get(point.block.index());
            if (reachesPoint)
                return true;
        }
    }
    return false;
}

// A frame that is saved at a suspension comes back at another address, so the address of a cell is good up to the next suspension only. The cell is dead there.
void RuntimeValidationEmitter::poisonFrameCells(ProgramPoint point)
{
    for (ValueID allocation : m_framePointers.allocations()) {
        if (isDeferredClosure(m_function, allocation) || !analyses().dominates(allocation, point) || maySuspendBetween(allocation, point))
            continue;
        const Value& value = m_function.at(allocation);
        CellPointer cell = in<CellPointer>(allocation);
        if (arrayAllocationsWithStorage.contains(value.opcode)) {
            poisonFrameArray(cell);
            continue;
        }
        if (value.opcode != Opcode::NewClosure) {
            RELEASE_ASSERT(value.opcode == Opcode::NewObject);
            LayoutID layout = value.layout();
            poisonFields(cell, layout, m_program.at(layout).isEnvironment() ? AbstractHeap::EnvironmentFields : AbstractHeap::ObjectFields);
            continue;
        }
        switch (value.closureForm()) {
        case ClosureForm::Full:
            storeField(cell, m_heaps.AOTClosure_entry, integer(-1));
            continue;
        case ClosureForm::Slim:
            storeField(cell, m_heaps.AOTSlimClosure_word, integer(-1));
            continue;
        case ClosureForm::Deferred:
        case ClosureForm::Singleton:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// A rest array of many arguments is on the heap, and so is every array of a realm whose arrays are not pristine.
void RuntimeValidationEmitter::poisonFrameArray(CellPointer array)
{
    LBasicBlock isInFrame = newBlock();
    LBasicBlock continuation = newBlock();
    LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.collectorState, array.value(), JSCell::cellStateOffset()));
    m_out.branch(m_out.equal(state, m_out.constInt32(frameCellState)), normalEdge(isInFrame), normalEdge(continuation));
    m_out.appendTo(isInFrame);
    m_out.storePtr(m_out.constIntPtr(-1), array.value(), m_heaps.JSObject_butterfly);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void RuntimeValidationEmitter::poisonEnvironment(CellPointer environment, LayoutID layout)
{
    poisonFields(environment, layout, AbstractHeap::EnvironmentFields);
}

// The fields become absent: checkHasType reports that at the next read of one that is always filled.
void RuntimeValidationEmitter::poisonFields(CellPointer cell, LayoutID layout, AbstractHeap heap)
{
    const PlannedLayout& planned = m_placement.layout(layout);
    SlotEmitter& slots = emitter<SlotEmitter>();
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (slot.storage != SlotStorage::Inline || !traitsOf(slot.representation).hasAbsent)
            continue;
        slots.storeAbsent(slots.addressOf(cell, slot, MemoryLocation(heap)), slot.representation);
    }
}

Condition RuntimeValidationEmitter::isInStack(Pointer pointer)
{
    Pointer top(m_out.loadPtr(m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), vm().value(), VM::topEntryFrameOffset())));
    return logicalAnd(aboveOrEqual(pointer, stackPointer()), below(pointer, top));
}

Pointer RuntimeValidationEmitter::stackPointer()
{
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Int64);
    emitter<RuntimeCallEmitter>().applyEffects(patchpoint, Effects::none(), ArgumentBufferAccess::None);
    patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        jit.move(MacroAssembler::stackPointerRegister, params[0].gpr());
    });
    return Pointer(patchpoint);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
