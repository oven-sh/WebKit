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
#include "AOTIdentifierNumbering.h"
#include "AOTPlacement.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTRepresentation.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStaticsEmitter.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestEmitter.h"
#include "AOTTypeTestPlan.h"
#include "AOTValueIdentity.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "VM.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool mayHoldPointer(Operand operand)
{
    return operand.is<CellPointer>() || operand.is<Boxed>();
}

void RuntimeValidationEmitter::checkValue(const OperationContext&, ValueID id, Operand operand)
{
    checkValue(id, operand);
}

void RuntimeValidationEmitter::checkValue(ValueID id, Operand operand)
{
    if (generator().mayBeUnspecified(id) || operand.representation == Representation::Void || operand.representation == Representation::LiteralString)
        return;
    const Type& type = typeOf(id);
    if (type.isNever() || type.opaque)
        return;
    if (!type.environment) {
        checkHasType(id, operand, type, ValidationFailureKind::ValueType);
        return;
    }
    if (operand.is<CellPointer>())
        checkEnvironmentShape(operand.as<CellPointer>(), type.environment, id);
}

void RuntimeValidationEmitter::checkHasType(ValueID id, Operand operand, const Type& type, ValidationFailureKind kind)
{
    bool mayBePointer = mayHoldPointer(operand);
    if (mayBePointer && !(type.primitives & Primitive::Absent))
        failUnless(Condition(m_out.notZero64(operand.value)), kind, { }, id);

    TypeID widest = types().intern(widestTypeOf(operand.representation));
    const TypeTestPlan& plan = typeTestPlanFor(widest, operand.representation, types().intern(testableTypeOf(type)));
    if (plan.verdict == TypeTestVerdict::AlwaysPasses)
        return;
    if (!type.hasObjects()) {
        failUnless(hasType(operand, plan), kind, operand, id);
        return;
    }
    unlessInStack(operand, [&] {
        failUnless(hasType(operand, plan), kind, operand, id);
    });
}

// A cell in a frame, of this function or of a caller, may have no header.
void RuntimeValidationEmitter::unlessInStack(Operand operand, NOESCAPE const Invocable<void()> auto& emitTest)
{
    if (!mayHoldPointer(operand)) {
        emitTest();
        return;
    }
    LBasicBlock isOutsideStack = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(isInStack(Pointer(operand.value)).value(), normalEdge(continuation), normalEdge(isOutsideStack));
    m_out.appendTo(isOutsideStack);
    emitTest();
    m_out.jump(continuation);
    m_out.appendTo(continuation);
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

// The type of the object need not name the layouts of an access: a guard may stand for them, and a guard is no value that could be tested.
// So the object is asked again, at the access, for everything the access relies on: a guard holds only as long as nothing takes away what it found.
void RuntimeValidationEmitter::checkReceiverOfSlotAccess(const OperationContext& context, const Value& access)
{
    static constexpr size_t maxNumberOfComparedRuns = 16;
    struct RunOfShapes {
        uint32_t last;
        uint32_t count;
    };

    if (familyOf(access.opcode) != OperationFamily::SlotAccess)
        return;
    ValueID object = access.objectOperand();
    if (!hasTestableBits(object) || !get(object).is<CellPointer>())
        return;
    const SlotPlan& slots = m_placement.slots();
    bool hasOnlyCompiledObjects = std::ranges::all_of(m_program.at(access.receivers()).members, [&](LayoutID member) {
        return slots.storageOf(member) == LayoutStorage::CompiledObject;
    });
    std::span<const uint32_t> shapes = m_placement.shapes().memberShapes(access.receivers());
    if (!hasOnlyCompiledObjects || shapes.empty())
        return;
    // The shapes ascend, and the layouts of one family have numbers in a row.
    Vector<RunOfShapes, maxNumberOfComparedRuns> runs;
    for (uint32_t shape : shapes) {
        if (!runs.isEmpty() && runs.last().last + 1 == shape) {
            runs.last().last = shape;
            ++runs.last().count;
        } else
            runs.append({ shape, 1 });
    }
    if (runs.size() > maxNumberOfComparedRuns)
        return;

    CellPointer cell = in<CellPointer>(object);
    unlessInStack(cell, [&] {
        Word32 shapeWord = emitter<ShapeTestEmitter>().shapeWordOf(cell, { });
        Condition hasShapeOfReceiver;
        static_assert(AOT::shapeWordOf(2) + 1 == AOT::shapeWordOf(1));
        for (RunOfShapes run : runs)
            hasShapeOfReceiver = logicalOr(hasShapeOfReceiver, below(add(shapeWord, word32(run.last)), word32(run.count)));
        bool hasGuard = !!access.guardOperand();
        failUnless(hasShapeOfReceiver, hasGuard ? ValidationFailureKind::GuardNoLongerHolds : ValidationFailureKind::ReceiverShape, cell, context.valueID());

        ValidationFailureKind kind = hasGuard ? ValidationFailureKind::GuardNoLongerHolds : ValidationFailureKind::OperationPreconditionBroken;
        const FieldAccess& field = m_placement.fieldAccess(access.fieldAccess());
        bool everyReceiverHasSlot = field.coverage == FieldAccess::Coverage::EveryMember || field.coverage == FieldAccess::Coverage::EveryMemberHasSlot;
        if (reliesOf(access.opcode).contains(Proof::SlotFilled) && everyReceiverHasSlot && field.slot.hasStorage() && !field.slot.isOutOfLine())
            failUnless(emitter<SlotEmitter>().isFilled(cell, field.slot, MemoryLocation::objectField(field.key, field.receivers)), kind, cell, context.valueID());
        // A private field of a frozen object is stored like any other.
        if (reliesOf(access.opcode).contains(Proof::NotFrozen) && m_program.at(field.key).kind != PropertyKeyKind::PrivateName)
            failUnless(hasNoBits(emitter<ShapeTestEmitter>().orderWordOf(cell), orderWordIntegrityBits), kind, cell, context.valueID());
    });
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

        unlessInStack(operand, [&] {
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
        });
    }
}

void RuntimeValidationEmitter::checkClassProofs(ValueID id, CellPointer cell, const ClassPart& part)
{
    // Every array starts with the marks that the test reads. An object of another class may be pristine without them.
    OptionSet<PristineAspects> aspects = part.provenPristine - aspectsWithoutTest;
    if (!aspects.isEmpty() && part.builtinClass == BuiltinClasses::Array && pristineTestOf(m_builtins, part.builtinClass) == PristineTest::MayPass) {
        Condition hasMarks = emitter<TypeTestEmitter>().isPristine(cell, part.builtinClass, aspects);
        if (aspects.contains(PristineAspects::Names))
            failUnless(hasMarks, ValidationFailureKind::ArrayNotPristine, cell, id);
        else
            askOperationUnless(hasMarks, ValidationFailureKind::ArrayElementsNotOrdinary, cell, id);
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

// One number may be held as an integer and as a double.
void RuntimeValidationEmitter::generateValidateSameValue(const ValueContext& value)
{
    ValueID kept = value->keptOperand();
    ValueID replacement = value->replacementOperand();
    if (!hasTestableBits(kept) || !hasTestableBits(replacement))
        return;

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
    failUnless(Condition(m_out.doubleEqual(keptNumber.value(), replacingNumber.value())), kind, keptValue, value.id());
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// A complete set of callees says which functions the call may run. A callee that is no function makes the call throw.
void RuntimeValidationEmitter::generateValidateCallee(const ValueContext& value)
{
    ValueID callee = value->calleeOperand();
    if (!hasTestableBits(callee))
        return;

    Operand operand = get(callee);
    TypeID widest = types().intern(widestTypeOf(operand.representation));
    const TypeTestPlan& isFunction = typeTestPlanFor(widest, operand.representation, types().intern(Type::function()));
    const TypeTestPlan& isListed = typeTestPlanFor(widest, operand.representation, types().intern(testableTypeOf(m_program.at(value->validatedType()))));
    if (isFunction.verdict == TypeTestVerdict::NeverPasses || isListed.verdict == TypeTestVerdict::AlwaysPasses)
        return;
    unlessInStack(operand, [&] {
        failUnless(logicalOr(logicalNot(hasType(operand, isFunction)), hasType(operand, isListed)), ValidationFailureKind::UnlistedCallee, operand, value.id());
    });
}

Type RuntimeValidationEmitter::testableTypeOf(const Type& type)
{
    bool shapesArePermanent = layoutTestKindFor(m_program, membersOf(m_program, type)) == LayoutTestKind::Type;
    Type testable = type;
    if (shapesArePermanent)
        testable.layouts = { };
    testable = testable.withoutProofs();
    if (shapesArePermanent)
        testable = testable.unionWith(Type::forLayoutSet(type.layouts), types());

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
    return maySuspend(m_function.at(id).opcode) && analyses().effectsOf(id).normal.maySuspend;
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
