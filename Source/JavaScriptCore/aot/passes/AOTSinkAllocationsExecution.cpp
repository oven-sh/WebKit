/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperands.h"
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t castOperandIndex = 0;

static constexpr bool everyCastHasItsValueFirst()
{
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (interpretedOf(opcode) != Interpretation::Cast)
            continue;
        // What NarrowByField and ReadAgain take beside their value is only looked at.
        uint32_t numberOfOperands = opcode == Opcode::NarrowByField || opcode == Opcode::ReadAgain ? 2 : 1;
        if (numberOfNamedOperandsOf(opcode) != numberOfOperands || hasVariadicOperand(opcode))
            return false;
    }
    return true;
}

static_assert(everyCastHasItsValueFirst());
static_assert(OperandIndex::NarrowByField::value == castOperandIndex && OperandIndex::ReadAgain::object == castOperandIndex);

struct VariantCastOutcomes {
    bool hasUnknown { false };
    bool hasPassing { false };
    bool hasThrowing { false };
    bool allVariantsAreTagged { true };
    bool throwsOnOneConstant { true };
    VirtualConstant throwingConstant { VirtualConstant::Undefined };
    uint32_t passingTagBits { 0 };
    uint32_t throwingTagBits { 0 };

    bool alwaysPasses() const { return !hasUnknown && !hasThrowing; }
    bool canSplit() const { return !hasUnknown && hasPassing && hasThrowing && allVariantsAreTagged && throwsOnOneConstant && !(passingTagBits & throwingTagBits); }
};

static VirtualObjectID findAliasedObject(const SinkingState& state, ValueID value, AliasKind kind)
{
    const AliasEntry* alias = state.findAlias(value);
    return alias && alias->kind == kind ? alias->object : VirtualObjectID();
}

static uint32_t findOperandIndex(const Value& user, ValueID operand)
{
    std::optional<uint32_t> found;
    user.forEachOperand([&](ValueID candidate, uint32_t index) {
        if (!found && candidate == operand)
            found = index;
    });
    RELEASE_ASSERT(found);
    return *found;
}

static VariantCastOutcomes evaluateCastOnVariants(const ShapeContext& context, const ObjectState& state, const Value& cast)
{
    VariantCastOutcomes outcomes;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = context.family.at(variant.shape);
        uint32_t tagMask = variant.tagBit ? 1u << *variant.tagBit : 0;
        outcomes.allVariantsAreTagged &= !!tagMask;
        switch (evaluateCast(context, shape, cast)) {
        case CastOutcome::Unknown:
            outcomes.hasUnknown = true;
            break;
        case CastOutcome::Passes:
            outcomes.hasPassing = true;
            outcomes.passingTagBits |= tagMask;
            break;
        case CastOutcome::Throws:
            // A Check that fails throws nothing: generic code goes on with the value, and there it needs its cell.
            if (cast.opcode == Opcode::Check) {
                outcomes.hasUnknown = true;
                break;
            }
            outcomes.throwsOnOneConstant &= shape.kind == VirtualShapeKind::Constant && (!outcomes.hasThrowing || outcomes.throwingConstant == shape.constant);
            outcomes.hasThrowing = true;
            outcomes.throwingConstant = shape.constant;
            outcomes.throwingTagBits |= tagMask;
            break;
        }
    }
    return outcomes;
}

static VirtualConstant constantOfTaggedVariants(const VirtualFamily& family, const ObjectState& state, uint32_t tagBits)
{
    for (const ObjectVariant& variant : state.variants) {
        if (!variant.tagBit || !(tagBits >> *variant.tagBit & 1))
            continue;
        const VirtualShape& shape = family.at(variant.shape);
        RELEASE_ASSERT(shape.kind == VirtualShapeKind::Constant);
        return shape.constant;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static EscapeReasons escapeReasonsOf(const OperandSummary& summary, EffectRule rule)
{
    EscapeReasons reasons;
    if (summary.uses.contains(OperandUse::Retains)) {
        if (rule == EffectRule::Call)
            reasons.add(EscapeReason::RetainedByCallee);
        else if (summary.retainedIn == RetainedIn::UnknownMemory)
            reasons.add(EscapeReason::StoredInUnknownMemory);
        else
            reasons.add(EscapeReason::StoredInEscapingObject);
    }
    if (summary.uses.containsAny({ OperandUse::PassesToUserCode, OperandUse::AppliesTransitively }))
        reasons.add(EscapeReason::PassedToUnknownCode);
    return reasons;
}

static EscapeReasons escapeReasonsOf(const EffectContext& context, const Function& function, const Terminator& terminator, uint32_t operandIndex)
{
    switch (terminator.kind) {
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
        return EscapeReason::Returned;
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
        return EscapeReason::Thrown;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return escapeReasonsOf(summarizeOperand(context, function, terminator, operandIndex), effectRuleOf(terminator.kind));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ObjectInterpreter::ObjectInterpreter(AllocationSinking& sinking, SinkingMode mode, SinkingState& state, ObjectValueMap* values)
    : m_sinking(sinking)
    , m_mode(mode)
    , m_state(state)
    , m_values(values)
{
    RELEASE_ASSERT((mode == SinkingMode::Rewrite) == !!values);
}

VirtualObjectID ObjectInterpreter::objectOf(ValueID value) const
{
    if (VirtualObjectID candidate = m_sinking.candidateDefinedBy(value); candidate && m_state.find(candidate))
        return candidate;
    return findAliasedObject(m_state, value, AliasKind::Object);
}

VirtualObjectID ObjectInterpreter::guardedObjectOf(ValueID value) const
{
    if (VirtualObjectID object = findAliasedObject(m_state, value, AliasKind::Guard))
        return object;
    return objectOf(value);
}

// The guard of a cell is its allocation, which proves nothing about a position.
bool ObjectInterpreter::hasPositionFromAnsweredTest(ValueID access, VirtualObjectID object) const
{
    const Value& value = function().at(access);
    bool reliesOnPosition = (Value::opcodesWithIsInBounds.contains(value.opcode) && value.isInBounds()) || (Value::opcodesWithIsPresent.contains(value.opcode) && value.isPresent());
    return reliesOnPosition && value.guardOperand() && guardedObjectOf(value.guardOperand()) == object;
}

VirtualObjectID ObjectInterpreter::destructuredObjectOf(ValueID value) const
{
    return findAliasedObject(m_state, value, AliasKind::Results);
}

Execution ObjectInterpreter::execute(ValueID id)
{
    Interpretation interpretation = interpretationOf(program(), function().at(id));
    if (m_state.objects.isEmpty() && interpretation != Interpretation::Allocation) {
        keep(id);
        return Execution::Kept;
    }

    if (isRewriting())
        cursor().setOrigin(function().at(id).origin());

    switch (interpretation) {
    case Interpretation::Allocation:
        return executeAllocation(id);
    case Interpretation::Cast:
        return executeCast(id);
    case Interpretation::Read:
        return executeRead(id);
    case Interpretation::Store:
        return executeStore(id);
    case Interpretation::Test:
        return executeTest(id);
    case Interpretation::List:
        return executeListUse(id);
    case Interpretation::Guards:
        return executeGuards(id);
    case Interpretation::None:
        return familyOf(function().at(id).opcode) == OperationFamily::RuntimeValidation ? executeRuntimeValidation(id) : executeOther(id);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Execution ObjectInterpreter::executeAllocation(ValueID id)
{
    VirtualObjectID object = m_sinking.candidateDefinedBy(id);
    if (!object)
        return executeCallOnArguments(id);

    const VirtualObjectDescription& description = m_sinking.descriptionOf(object);
    if (description.staysInPlace)
        return executeBuiltinResult(id, object);

    if (stateOf(object)) {
        m_sinking.abandon(object);
        return executeOther(id);
    }

    VirtualFamily& family = m_sinking.familyOf(object);
    VirtualShapeID shape = family.intern(shapeAtAllocation(m_sinking.objectContext(), family, m_sinking.orders(), m_sinking.at(object).descriptionIndex));

    Vector<std::pair<uint32_t, ValueID>, 8> initialFields;
    forEachInitialField(m_sinking.objectContext(), description, [&](const VirtualField& field, ValueID value) {
        initialFields.append({ *family.findField(field), value });
    });

    ObjectState state = ObjectState::withShape(shape);
    std::optional<uint8_t> tagBitWhenAbsent = m_sinking.at(object).tagBitWhenAbsent;
    RELEASE_ASSERT(tagBitWhenAbsent.has_value() == description.absentResult.has_value());
    if (tagBitWhenAbsent) {
        VirtualShapeID absentShape = family.intern(VirtualShape::ofConstant(*description.absentResult));
        state.variants = { ObjectVariant { *tagBitWhenAbsent, absentShape }, ObjectVariant { static_cast<uint8_t>(*tagBitWhenAbsent + 1), shape } };
    }
    for (auto [field, value] : initialFields) {
        VirtualObjectID held = objectOf(value);
        if (!held)
            continue;
        if (canHold(object, field, held)) {
            state.hold(field, held);
            continue;
        }
        if (!stateOf(held)->isCell())
            build(held, causeOf(id, findOperandIndex(function().at(id), value)));
    }
    const ObjectState& added = m_state.add(object, WTF::move(state));

    if (isRewriting()) {
        ObjectValues& carried = values().ensure(object);
        for (auto [field, value] : initialFields) {
            if (!added.objectHeldIn(field))
                carried.setField(field, resolve(value));
        }
        if (description.kind == VirtualObjectKind::Array)
            carried.setField(*family.findField(VirtualField::part(VirtualFieldKind::Length)), cursor().builder().appendInteger(family.at(shape).length));
        if (tagBitWhenAbsent) {
            RELEASE_ASSERT(function().at(id).opcode == Opcode::SubstringOrUndefined);
            ValueID isPresent = cursor().emit(IRBuilder::make(Opcode::GreaterEq, { function().at(id).startOperand(), cursor().builder().appendInteger(0) }));
            values().ensure(object).tag = emitPresenceTag(isPresent, *tagBitWhenAbsent);
        }
    }

    return Execution::Interpreted;
}

Execution ObjectInterpreter::executeCast(ValueID id)
{
    VirtualObjectID object = objectOf(function().at(id).operand(castOperandIndex));
    if (!object)
        return executeOther(id);

    VariantCastOutcomes outcomes = evaluateCastOnVariants(m_sinking.shapeContext(object), *stateOf(object), function().at(id));
    if (outcomes.canSplit())
        splitThrowingVariants(id, object, outcomes.throwingTagBits);
    else if (!outcomes.alwaysPasses()) {
        requireCellsOfOperands(id);
        keep(id);
        return Execution::Kept;
    }

    // The cast goes, and its proof is about the object, so the cell gets it back when the object is built.
    if (function().at(id).opcode == Opcode::AssumeType)
        stateOf(object)->assume(function().at(id).castType());
    if (!m_sinking.decisions().holdsCells(id)) {
        bind(id, object);
        return Execution::Interpreted;
    }

    assumeTypeOfAlias(object, function().at(id).type);
    if (!stateOf(object)->isCell())
        build(object, causeOf(id, castOperandIndex));
    if (isRewriting())
        replaceRead(id, cellOf(object));
    return Execution::Interpreted;
}

Execution ObjectInterpreter::executeGuards(ValueID id)
{
    VirtualObjectID guarded;
    for (ValueID guard : function().at(id).guardsOperands()) {
        VirtualObjectID object = guardedObjectOf(guard);
        if (!object || (guarded && guarded != object))
            return executeOther(id);
        guarded = object;
    }
    if (!guarded || m_sinking.decisions().holdsCells(id))
        return executeOther(id);

    bind(id, guarded, AliasKind::Guard);
    return Execution::Interpreted;
}

void ObjectInterpreter::splitThrowingVariants(ValueID cast, VirtualObjectID object, uint32_t throwingTagBits)
{
    if (isRewriting()) {
        VirtualConstant constant = constantOfTaggedVariants(m_sinking.familyOf(object), *stateOf(object), throwingTagBits);
        IRBuilder& builder = cursor().builder();

        ValueID test = emitTagTest(object, throwingTagBits);
        BlockID failing = cursor().addCoveredBlock(*this);
        BlockID passing = cursor().addUncoveredBlock();
        builder.function().edit(failing, BlockEdit::Rest).frequency = BlockFrequency::Rare;
        builder.emitBranch(test, Target { failing, { }, { } }, Target { passing, { }, { } });

        cursor().moveTo(failing);
        Value failingCast = function().at(cast);
        failingCast.setExpected({ });
        failingCast.setOperand(castOperandIndex, cursor().emit(makeConstant(constant)));
        cursor().emit(WTF::move(failingCast));
        builder.emitUnreachable();

        cursor().moveTo(passing);
    }

    narrow(object, stateOf(object)->tagBits() & ~throwingTagBits);
    stateOf(object)->normalize();
}

// An object that is not built is what its allocation says, so there is nothing to test until it gets a cell.
Execution ObjectInterpreter::executeRuntimeValidation(ValueID id)
{
    bool testsUnbuiltObject = false;
    bool comparesWithClosure = function().at(id).opcode == Opcode::ValidateLookup;
    function().at(id).forEachOperand([&](ValueID operand, uint32_t index) {
        if (comparesWithClosure && index == OperandIndex::ValidateLookup::expected)
            return;
        VirtualObjectID object = objectOf(operand);
        testsUnbuiltObject |= object && !stateOf(object)->isCell();
    });
    return testsUnbuiltObject ? Execution::Interpreted : executeOther(id);
}

Execution ObjectInterpreter::executeOther(ValueID id)
{
    requireCellsOfOperands(id);
    forgetFieldsWrittenBy(id);
    keep(id);
    return Execution::Kept;
}

void ObjectInterpreter::requireCellsOfOperands(ValueID id)
{
    if (m_state.objects.isEmpty())
        return;

    Opcode opcode = function().at(id).opcode;
    Vector<std::pair<ValueID, uint32_t>, 8> operands;
    function().at(id).forEachOperand([&](ValueID operand, uint32_t index) {
        operands.append({ operand, index });
    });

    for (auto [operand, index] : operands) {
        bool isGuard = operandKindOf(opcode, index) == OperandKind::Guard;
        if (VirtualObjectID object = isGuard ? guardedObjectOf(operand) : objectOf(operand)) {
            if (isGuard && m_mode == SinkingMode::Count)
                m_sinking.at(object).needsGuard = true;
            if (!stateOf(object)->isCell())
                build(object, causeOf(id, index));
            continue;
        }
        if (VirtualObjectID destructured = destructuredObjectOf(operand))
            m_sinking.abandon(destructured);
    }
}

void ObjectInterpreter::forgetFieldsWrittenBy(ValueID id)
{
    if (m_noCellHasValidFields || !m_sinking.inputs().valuesWritingObjects.get(id.index()))
        return;

    for (ObjectEntry& entry : m_state.objects) {
        if (!entry.state.hasCell() || entry.state.fieldsValidInCell.isEmpty())
            continue;
        entry.state.fieldsValidInCell.clearAll();
        stateDidChange(entry.object);
    }
    m_noCellHasValidFields = true;
}

BuildCause describeCause(FunctionAnalyses& analyses, const Use& use)
{
    const Function& function = analyses.function();
    if (use.kind == Use::Kind::TerminatorOperand) {
        const Terminator& terminator = function.at(use.block).terminator;
        return { std::nullopt, escapeReasonsOf(analyses.effectContext(), function, terminator, use.operandIndex) };
    }
    RELEASE_ASSERT(use.kind == Use::Kind::Operand);
    const Value& value = function.at(use.user);
    OperandSummary summary = summarizeOperand(analyses.effectContext(), function, value, use.operandIndex);
    return { value.opcode, escapeReasonsOf(summary, effectRuleOf(value.opcode)) };
}

BuildCause ObjectInterpreter::causeOf(ValueID user, uint32_t operandIndex) const
{
    BuildCause cause;
    if (m_mode == SinkingMode::Count)
        cause = describeCause(m_sinking.analyses(), Use { .kind = Use::Kind::Operand, .user = user, .operandIndex = operandIndex });
    cause.admitsStoresByName = m_sinking.inputs().valuesCallingUnknownCode.get(user.index());
    return cause;
}

BuildCause ObjectInterpreter::causeOf(const Terminator& terminator, uint32_t operandIndex) const
{
    BuildCause cause;
    if (m_mode == SinkingMode::Count)
        cause = { std::nullopt, escapeReasonsOf(m_sinking.analyses().effectContext(), function(), terminator, operandIndex) };
    cause.admitsStoresByName = terminator.kind == TerminatorKind::Return || terminator.kind == TerminatorKind::Throw;
    return cause;
}

void ObjectInterpreter::keep(ValueID id)
{
    if (!isRewriting())
        return;

    if (!m_state.objects.isEmpty()) {
        // Resolving may append values, which moves the values of the function.
        Function& function = cursor().builder().function();
        Value value = function.at(id);
        resolveOperands(value);
        function.edit(id) = WTF::move(value);
    }
    if (m_sinking.inputs().throwingValues.get(id.index()))
        cursor().willKeepThrowingValue(*this);
    cursor().keep(id);
}

void ObjectInterpreter::bind(ValueID id, VirtualObjectID object, AliasKind kind)
{
    if (kind == AliasKind::Object)
        assumeTypeOfAlias(object, function().at(id).type);
    m_state.setAlias(id, object, kind);
}

void ObjectInterpreter::stateDidChange(VirtualObjectID)
{
    if (isRewriting())
        cursor().stateDidChange();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
