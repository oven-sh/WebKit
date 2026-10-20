/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTClosureTestEmitter.h"
#include "AOTCompiledStructure.h"
#include "AOTConstantEmitter.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTForInEmitter.h"
#include "AOTModuleWords.h"
#include "AOTPlacement.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStaticsEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestEmitter.h"
#include "JSGlobalObject.h"
#include "MarkedBlock.h"
#include "Structure.h"
#include "Watchpoint.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(sizeof(OptionSet<CompiledStructure::Flag>) == sizeof(uint8_t));
static_assert(sizeof(RequireEdgeState) == sizeof(uint8_t));
static_assert(sizeof(WatchpointState) == sizeof(uint8_t));
static_assert(sizeof(InstanceHeader::shapeEpoch) == sizeof(int64_t));
static_assert(offsetOfFirstField <= MarkedBlock::atomSize);

GuardEmitter::GuardEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Condition GuardEmitter::isPresentCell(Boxed value, const Type& type)
{
    // The empty value has no tag bits, so it passes isCell.
    if (type.primitives & Primitive::Absent)
        return logicalAnd(isCell(value), isNotEmpty(value));
    return isCell(value);
}

CellPointer GuardEmitter::untestedCell(const OperationContext& context, ValueID id)
{
    switch (representationOf(id)) {
    case Representation::Cell:
        return in<CellPointer>(id);
    case Representation::JSValue:
        return unboxCell(in<Boxed>(id));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    fail(context, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(Representation::Cell).as<CellPointer>();
}

CellPointer GuardEmitter::cellOrElse(const OperationContext& context, ValueID id, B3::FrequentedBlock isNotCell)
{
    const Type& type = typeOf(id);
    if (representationOf(id) == Representation::JSValue && (type.primitives & ~cellPrimitives)) {
        LBasicBlock isCellBlock = newBlock();
        m_out.branch(isPresentCell(in<Boxed>(id), type).value(), normalEdge(isCellBlock), isNotCell);
        m_out.appendTo(isCellBlock);
    }
    return untestedCell(context, id);
}

void GuardEmitter::generateShapeTest(const ValueContext& value)
{
    ValueID object = value->objectOperand();
    defineTestByBranching(value, [&](BranchTargets targets) {
        CellPointer cell = cellOrElse(value, object, targets.notTaken);
        emitter<ShapeTestEmitter>().branchOnShapeIn(cell, value->layouts(), typeOf(object), targets);
    });
}

void GuardEmitter::generateIsSlotFilled(const ValueContext& value)
{
    CellPointer object = in<CellPointer>(value->objectOperand());
    const FieldAccess& access = m_placement.fieldAccess(value->fieldAccess());
    auto isFilled = [&] {
        return emitter<SlotEmitter>().isFilled(object, access.slot, MemoryLocation::objectField(access.key, access.receivers), &access.extensionWord);
    };

    switch (access.coverage) {
    case FieldAccess::Coverage::NoMember:
        defineTest(value, boolean(false));
        return;
    case FieldAccess::Coverage::SomeMembers:
        defineTestByBranching(value, [&](BranchTargets targets) {
            LBasicBlock hasField = newBlock();
            emitter<ShapeTestEmitter>().branchOnShapeIn(object, access.membersWithField, Type::forLayoutSet(access.receivers), { normalEdge(hasField), targets.notTaken });
            m_out.appendTo(hasField);
            branch(isFilled(), targets);
        });
        return;
    case FieldAccess::Coverage::EveryMemberHasSlot:
    case FieldAccess::Coverage::EveryMember:
        defineTest(value, isFilled());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void GuardEmitter::generateHasNoExtraProperties(const ValueContext& value)
{
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(in<CellPointer>(value->objectOperand()));
    defineTest(value, hasNoBits(orderWord, orderWordHasExtraProperties));
}

void GuardEmitter::generateIsNotFrozen(const ValueContext& value)
{
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(in<CellPointer>(value->objectOperand()));
    defineTest(value, hasNoBits(orderWord, orderWordIntegrityBits));
}

void GuardEmitter::generateHasLayoutPrototype(const ValueContext& value)
{
    Pointer structure = loadStructure(in<CellPointer>(value->objectOperand()));
    Word32 flags = loadByte(structure, m_heaps.AOTCompiledStructure_flags);
    defineTest(value, hasNoBits(flags, static_cast<uint32_t>(CompiledStructure::Flag::HasOtherPrototype)));
}

void GuardEmitter::generateBelongsToInstance(const ValueContext& value)
{
    defineTest(value, belongsToInstance(in<CellPointer>(value->objectOperand())));
}

void GuardEmitter::generateIsPristine(const ValueContext& value)
{
    ValueID subject = value->valueOperand();
    Operand operand = get(subject);
    BuiltinClassID builtinClass = value->builtinClass();
    const TypeTestPlan& instanceTest = typeTestPlanFor(m_function.at(subject).type, operand.representation, types().intern(Type::builtinClass(builtinClass)));
    auto isPristine = [&] {
        return emitter<TypeTestEmitter>().isPristine(untestedCell(value, subject), builtinClass, value->aspects());
    };

    switch (instanceTest.verdict) {
    case TypeTestVerdict::NeverPasses:
        defineTest(value, boolean(false));
        return;
    case TypeTestVerdict::AlwaysPasses:
        defineTest(value, isPristine());
        return;
    case TypeTestVerdict::NeedsTest:
        defineTestByBranching(value, [&](BranchTargets targets) {
            LBasicBlock isInstance = newBlock();
            branchOnType(operand, instanceTest, { normalEdge(isInstance), targets.notTaken });
            m_out.appendTo(isInstance);
            branch(isPristine(), targets);
        });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void GuardEmitter::generateIsNotThenable(const ValueContext& value)
{
    ValueID operand = value->valueOperand();
    const Type& type = typeOf(operand);
    if (!protocolMayFindMember(m_program, type, Protocol::Thenable)) {
        defineTest(value, boolean(true));
        return;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        branchOnIsNotThenable(in<Boxed>(operand), type, targets);
    });
}

void GuardEmitter::generateIsMemberUnchanged(const ValueContext& value)
{
    defineTest(value, isGuardBitSet(value->guardBit()));
}

void GuardEmitter::generateIsPrototypeLinkUnchanged(const ValueContext& value)
{
    defineTest(value, isGuardBitSet(value->guardBit()));
}

void GuardEmitter::generateIsClassEvaluatedOnce(const ValueContext& value)
{
    defineTest(value, isGuardBitSet(value->guardBit()));
}

void GuardEmitter::generateIsRequireEdgeReady(const ValueContext& value)
{
    std::optional<uint32_t> edge = value->edge();
    if (!edge) {
        defineTest(value, boolean(false));
        return;
    }
    Pointer requireEdges = loadImmutablePointer(AOT_INSTANCE_OFFSET(modules.requireEdges));
    Word32 state(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTInstance_mutableFields, requireEdges.value(), *edge)));
    defineTest(value, equal(state, word32(static_cast<int32_t>(RequireEdgeState::Ready))));
}

void GuardEmitter::generateHasKeyOrder(const ValueContext& value)
{
    std::optional<OrderWord> expected = value->orderWord();
    if (!expected) {
        defineTest(value, boolean(false));
        return;
    }
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(in<CellPointer>(value->objectOperand()));
    defineTest(value, equal(bitAnd(orderWord, word32(orderAndCountMask)), word32(expected->value())));
}

void GuardEmitter::generateIsShapeEpochCurrent(const ValueContext& value)
{
    defineTest(value, equal(in<Integer>(value->epochOperand()), loadField<Integer>(instance(), m_heaps.AOTInstance_shapeEpoch)));
}

void GuardEmitter::generateIsClosureOf(const ValueContext& value)
{
    FunctionID function = value->function();
    defineTestByBranching(value, [&](BranchTargets targets) {
        CellPointer cell = cellOrElse(value, value->valueOperand(), targets.notTaken);
        emitter<ClosureTestEmitter>().branchOnIsClosureOf(cell, function, targets);
    });
}

void GuardEmitter::generateIsEnumeratorCurrent(const ValueContext& value)
{
    ForInEmitter& forIn = emitter<ForInEmitter>();
    CellPointer enumerator = in<CellPointer>(value->enumeratorOperand());
    ForInEmitter::EnumeratorFields fields = forIn.loadFields(enumerator);
    Integer nameIndex = forIn.nameIndexOf(fields, integerOf(value->positionOperand()));
    Condition isInStructurePart = forIn.isInStructurePart(fields, nameIndex);
    defineTestByBranching(value, [&](BranchTargets targets) {
        CellPointer object = cellOrElse(value, value->objectOperand(), targets.notTaken);
        branch(logicalAnd(isInStructurePart, forIn.hasCachedKeys(enumerator, fields, object)), targets);
    });
}

Condition GuardEmitter::isGuardBitSet(GuardBitID id)
{
    if (!id)
        return boolean(false);
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    return hasBits(Word32(m_out.load8ZeroExt32(statics.guardByteOf(id))), statics.guardMaskOf(id));
}

Condition GuardEmitter::belongsToInstance(CellPointer cell)
{
    return hasInstanceRealm(loadStructure(cell));
}

Condition GuardEmitter::isNotThenable(const OperationContext&, Boxed value, const Type& type)
{
    if (!protocolMayFindMember(m_program, type, Protocol::Thenable))
        return boolean(true);
    return asBoolean([&](BranchTargets targets) {
        branchOnIsNotThenable(value, type, targets);
    });
}

Condition GuardEmitter::hasInstanceRealm(Pointer structure)
{
    return equal(loadField<Pointer>(structure, m_heaps.Structure_realm), loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject)));
}

void GuardEmitter::branchOnIsNotThenable(Boxed value, const Type& type, BranchTargets targets)
{
    if (type.primitives & ~cellPrimitives) {
        LBasicBlock isCellBlock = newBlock();
        branch(isPresentCell(value, type), { normalEdge(isCellBlock), targets.taken });
        m_out.appendTo(isCellBlock);
    }
    CellPointer cell = unboxCell(value);
    if (type.primitives & cellPrimitives) {
        LBasicBlock isObjectBlock = newBlock();
        branch(isObjectCell(cell), { normalEdge(isObjectBlock), targets.taken });
        m_out.appendTo(isObjectBlock);
    }

    Pointer structure = loadStructure(cell);
    Word32 thenableState = bitAnd(loadField<Word32>(structure, m_heaps.Structure_bitField), word32(Structure::s_definitelyNonThenableStateBits));
    Condition isCachedAsNonThenable = equal(thenableState, word32(static_cast<int32_t>(Structure::DefinitelyNonThenableState::NonThenable) << Structure::s_definitelyNonThenableStateShift));

    // The Structure of a compiled object does not show its extra properties, and "then" may be one.
    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    Condition isNotCompiledObject = hasNoBits(shapeTests.shapeWordOf(cell, { }), compiledObjectShapeBit);
    Condition structureShowsEveryProperty = logicalOr(isNotCompiledObject, hasNoBits(shapeTests.orderWordOf(cell), orderWordHasExtraProperties));

    LBasicBlock cachedStateApplies = newBlock();
    branch(logicalAnd(logicalAnd(isCachedAsNonThenable, hasInstanceRealm(structure)), structureShowsEveryProperty), { normalEdge(cachedStateApplies), targets.notTaken });

    m_out.appendTo(cachedStateApplies);
    Pointer realm = loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject));
    branchOnIsWatched(m_out.address(realm.value(), m_heaps.InlineWatchpointSet_data, OBJECT_OFFSETOF(JSGlobalObject, m_promiseThenWatchpointSet)), targets);
}

void GuardEmitter::branchOnIsWatched(TypedPointer inlineWatchpointSet, BranchTargets targets)
{
    LBasicBlock isNotThinWatched = newBlock();
    LBasicBlock isInflated = newBlock();
    Pointer data(m_out.loadPtr(inlineWatchpointSet));
    branch(equal(data, pointerConstant(InlineWatchpointSet::encodeState(IsWatched))), { targets.taken, rareEdge(isNotThinWatched) });

    m_out.appendTo(isNotThinWatched);
    branch(hasNoBits(data, InlineWatchpointSet::IsThinFlag), { normalEdge(isInflated), targets.notTaken });

    m_out.appendTo(isInflated);
    branch(equal(loadByte(data, m_heaps.WatchpointSet_state), word32(IsWatched)), targets);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
