/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowApply.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include "PageCount.h"
#include "TypedArrayType.h"
#include <algorithm>
#include <ranges>
#include <utility>
#include <wtf/EnumSet.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t castOperandIndex = 0;

static constexpr bool everyCastHasItsValueFirst()
{
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (interpretedOf(opcode) != Interpretation::Cast)
            continue;
        uint32_t numberOfOperands = opcode == Opcode::NarrowByField ? 2 : 1;
        if (numberOfNamedOperandsOf(opcode) != numberOfOperands || hasVariadicOperand(opcode))
            return false;
    }
    return true;
}

static_assert(everyCastHasItsValueFirst());
static_assert(OperandIndex::NarrowByField::value == castOperandIndex);

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

template<typename Instruction>
static Vector<std::pair<ValueID, uint32_t>, 8> indexedOperandsOf(const Instruction& instruction)
{
    Vector<std::pair<ValueID, uint32_t>, 8> operands;
    instruction.forEachOperand([&](ValueID operand, uint32_t index) {
        operands.append({ operand, index });
    });
    return operands;
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
        uint32_t tagMask = variant.tagMask();
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

bool ObjectInterpreter::reliesOnAnswerThatCellDoesNotKeep(ValueID access, VirtualObjectID object) const
{
    const Value& value = function().at(access);
    if (!OperandPosition::guard(value.opcode) || !value.guardOperand() || guardedObjectOf(value.guardOperand()) != object)
        return false;
    bool reliesOnPosition = (Value::opcodesWithIsInBounds.contains(value.opcode) && value.isInBounds()) || (Value::opcodesWithIsPresent.contains(value.opcode) && value.isPresent());
    return reliesOnPosition || m_sinking.mayBeListOfNumbers(object);
}

VirtualObjectID ObjectInterpreter::destructuredObjectOf(ValueID value) const
{
    return findAliasedObject(m_state, value, AliasKind::Results);
}

void ObjectInterpreter::execute(ValueID id)
{
    Interpretation interpretation = interpretationOf(program(), function().at(id));
    if (m_state.objects.isEmpty() && interpretation != Interpretation::Allocation) {
        keep(id);
        return;
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

void ObjectInterpreter::executeAllocation(ValueID id)
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

    ObjectState state = stateAtAllocation(object, shape);
    std::optional<uint8_t> tagBitWhenAbsent = m_sinking.at(object).tagBitWhenAbsent;
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
}

void ObjectInterpreter::executeCast(ValueID id)
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
        return;
    }

    if (function().at(id).opcode == Opcode::AssumeType)
        stateOf(object)->assume(function().at(id).castType());
    if (!m_sinking.decisions().holdsCells(id)) {
        bind(id, object);
        return;
    }

    assumeTypeOfAlias(object, function().at(id).type);
    if (!stateOf(object)->isCell())
        build(object, causeOf(id, castOperandIndex));
    if (isRewriting())
        replaceRead(id, cellOf(object));
}

void ObjectInterpreter::executeGuards(ValueID id)
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

void ObjectInterpreter::executeRuntimeValidation(ValueID id)
{
    bool testsUnbuiltObject = false;
    bool comparesWithClosure = function().at(id).opcode == Opcode::ValidateLookup;
    function().at(id).forEachOperand([&](ValueID operand, uint32_t index) {
        if (comparesWithClosure && index == OperandIndex::ValidateLookup::expected)
            return;
        VirtualObjectID object = objectOf(operand);
        testsUnbuiltObject |= object && !stateOf(object)->isCell();
    });
    if (!testsUnbuiltObject)
        executeOther(id);
}

void ObjectInterpreter::executeOther(ValueID id)
{
    requireCellsOfOperands(id);
    forgetFieldsWrittenBy(id);
    keep(id);
}

void ObjectInterpreter::executeOnCell(ValueID id, uint32_t operandIndex)
{
    requireCell(function().at(id).operand(operandIndex), causeOf(id, operandIndex));
    return executeOther(id);
}

void ObjectInterpreter::executeAccessOnCell(ValueID access, VirtualObjectID object)
{
    if (reliesOnAnswerThatCellDoesNotKeep(access, object))
        m_sinking.abandon(object);
    return executeOnCell(access, 0);
}

void ObjectInterpreter::didFoldStores(VirtualObjectID object, unsigned numberOfStores)
{
    if (m_mode == SinkingMode::Count)
        m_sinking.at(object).numberOfFoldedStores += numberOfStores;
    stateDidChange(object);
}

ObjectState ObjectInterpreter::stateAtAllocation(VirtualObjectID object, VirtualShapeID shape)
{
    const std::optional<VirtualConstant>& absentResult = m_sinking.descriptionOf(object).absentResult;
    std::optional<uint8_t> tagBitWhenAbsent = m_sinking.at(object).tagBitWhenAbsent;
    RELEASE_ASSERT(tagBitWhenAbsent.has_value() == absentResult.has_value());
    ObjectState state = ObjectState::withShape(shape);
    if (tagBitWhenAbsent) {
        VirtualShapeID absentShape = m_sinking.familyOf(object).intern(VirtualShape::ofConstant(*absentResult));
        state.variants = { ObjectVariant { *tagBitWhenAbsent, absentShape }, ObjectVariant { static_cast<uint8_t>(*tagBitWhenAbsent + 1), shape } };
    }
    return state;
}

// The row of the function says that it only reads or writes the contents of these operands while it runs, so its direct entry takes an address and a count.
std::optional<BytesOperands> findBytesOperands(const Program& program, const Function& function, const Value& call, BuiltinCallForm formOfCall)
{
    if (call.opcode != Opcode::CallBuiltin)
        return std::nullopt;
    // An earlier round may have taken the result apart already.
    BuiltinCallForm form = call.builtinCallForm() == BuiltinCallForm::Whole ? formOfCall : call.builtinCallForm();
    const BuiltinOverload& overload = program.builtins().at(call.overload());
    bool entersDirectEntry = form == BuiltinCallForm::Whole || (form == BuiltinCallForm::Fields && overload.code.fieldsForm == InlineBuiltin::FieldsOfDirectEntry);
    // The bounds come behind the arguments, so none of them is left out.
    if (!overload.code.overloadInRanges || !entersDirectEntry || call.argumentsOperands().size() != overload.parameters.size())
        return std::nullopt;
    // The other overload takes a view on trust, so the types have to say here that each one is a view.
    if (!overloadTakesOperands(program, function, call))
        return std::nullopt;

    BytesOperands bytes { overload.code.overloadInRanges, { } };
    for (std::optional<BuiltinOperand> operand : overload.code.operandsTakenAsBytes) {
        if (operand)
            bytes.operandIndexes.append(operand->isReceiver() ? OperandIndex::CallBuiltin::receiver : OperandIndex::CallBuiltin::arguments + operand->argumentIndex());
    }
    return bytes;
}

// On every way it is a view that exists, or a part of a view that is not made yet.
bool ObjectInterpreter::isRangeOfView(VirtualObjectID object) const
{
    const ObjectState& state = *stateOf(object);
    const VirtualFamily& family = m_sinking.familyOf(object);
    return !state.isCell() && std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        if (shape.kind == VirtualShapeKind::Cell)
            return variant.tagBit.has_value();
        if (shape.kind != VirtualShapeKind::Fields || family.descriptionOf(shape).kind != VirtualObjectKind::ViewRange)
            return false;
        return (state.variants.size() == 1 || variant.tagBit) && isTypedView(typedArrayTypeOf(program().builtins(), program().at(family.descriptionOf(shape).type)));
    });
}

static constexpr int64_t endOfWholeView = static_cast<int64_t>(MAX_ARRAY_BUFFER_SIZE);

void ObjectInterpreter::passRangesOfViews(ValueID call, const BytesOperands& bytes)
{
    IRBuilder& builder = cursor().builder();
    TypeID typeOfBound = builder.types().intern(Type::integer(Range { 0, static_cast<int64_t>(maxSafeInteger()) }));
    Value value = function().at(call);
    Vector<ValueID, 8> arguments(value.argumentsOperands());
    for (uint32_t index : bytes.operandIndexes) {
        ValueID view = value.operand(index);
        ValueID start = builder.appendInteger(0);
        ValueID end = builder.appendInteger(endOfWholeView);
        if (VirtualObjectID object = objectOf(view); object && isRangeOfView(object)) {
            const ObjectState& state = *stateOf(object);
            const VirtualFamily& family = m_sinking.familyOf(object);
            uint32_t tagBitsOfRanges = 0;
            unsigned elementSize = 1;
            for (const ObjectVariant& variant : state.variants) {
                const VirtualShape& shape = family.at(variant.shape);
                if (shape.kind != VirtualShapeKind::Fields)
                    continue;
                tagBitsOfRanges |= variant.tagMask();
                elementSize = JSC::elementSize(typedArrayTypeOf(program().builtins(), program().at(family.descriptionOf(shape).type)));
            }
            auto partAt = [&](uint32_t operandIndex) {
                return valueOfField(object, *family.findField(VirtualField::operand(operandIndex)));
            };
            auto inBytes = [&](ValueID position) {
                return elementSize == 1 ? position : builder.append(Opcode::Mul, { position, builder.appendInteger(elementSize) });
            };
            ValueID base = partAt(OperandIndex::TypedArrayRange::view);
            ValueID startOfRange = partAt(OperandIndex::TypedArrayRange::start);
            ValueID endOfRange = partAt(OperandIndex::TypedArrayRange::end);
            if (state.isVirtual()) {
                view = base;
                start = inBytes(startOfRange);
                end = inBytes(endOfRange);
            } else {
                // The parts have no value on the ways where the view exists, so nothing looks at them before the choice.
                ValueID isRange = emitTagTest(object, tagBitsOfRanges);
                view = cursor().emitSelection(isRange, base, values().find(object)->cell, function().at(view).type);
                start = inBytes(cursor().emitSelection(isRange, startOfRange, start, typeOfBound));
                end = inBytes(cursor().emitSelection(isRange, endOfRange, end, typeOfBound));
            }
            if (index == OperandIndex::CallBuiltin::receiver)
                value.setReceiverOperand(view);
            else
                arguments[index - OperandIndex::CallBuiltin::arguments] = view;
        }
        arguments.append(start);
        arguments.append(end);
    }
    if (!program().builtins().at(bytes.overloadInRanges).receiver)
        value.setReceiverOperand({ });
    value.setOverload(bytes.overloadInRanges);
    value.setArgumentsOperands(arguments.span());
    builder.function().edit(call) = WTF::move(value);
}

void ObjectInterpreter::requireCellsOfOperands(ValueID id, BuiltinCallForm formOfCall)
{
    if (m_state.objects.isEmpty())
        return;

    std::optional<BytesOperands> bytes = findBytesOperands(program(), function(), function().at(id), formOfCall);
    bool passesRange = false;
    Opcode opcode = function().at(id).opcode;
    for (auto [operand, index] : indexedOperandsOf(function().at(id))) {
        bool isGuard = operandKindOf(opcode, index) == OperandKind::Guard;
        if (VirtualObjectID range = bytes && bytes->operandIndexes.contains(index) ? objectOf(operand) : VirtualObjectID(); range && isRangeOfView(range)) {
            requireCellOfHeldObject(range, *m_sinking.familyOf(range).findField(VirtualField::operand(OperandIndex::TypedArrayRange::view)), causeOf(id, index));
            passesRange = true;
            continue;
        }
        if (VirtualObjectID object = isGuard ? guardedObjectOf(operand) : objectOf(operand)) {
            if (isGuard && (reliesOnAnswerThatCellDoesNotKeep(id, object) || m_sinking.mayBeListOfNumbers(object)))
                m_sinking.abandon(object);
            if (isGuard && m_mode == SinkingMode::Count)
                m_sinking.at(object).needsGuard = true;
            if (!stateOf(object)->isCell())
                build(object, causeOf(id, index));
            continue;
        }
        if (VirtualObjectID destructured = destructuredObjectOf(operand))
            m_sinking.abandon(destructured);
    }
    if (passesRange && isRewriting())
        passRangesOfViews(id, *bytes);
}

void ObjectInterpreter::requireCellsOfOperandsBeside(ValueID id, uint32_t operandIndex)
{
    for (auto [operand, index] : indexedOperandsOf(function().at(id))) {
        if (index != operandIndex)
            requireCell(operand, causeOf(id, index));
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

static bool isWithin(const Program& program, TypeID type, TypeID bound)
{
    return type == bound || program.at(type).isSubsetOf(program.at(bound), program);
}

static bool mayBeUndefined(const Type& type)
{
    return type.primitives & Primitive::Undefined;
}

static std::optional<VirtualFieldAccess> lookUpOnAllVariants(const AllocationSinking& sinking, VirtualObjectID object, const ObjectState& state, const Value& access)
{
    const VirtualFamily& family = sinking.familyOf(object);
    std::optional<VirtualFieldAccess> result;
    BitVector visitedDescriptions;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = family.at(variant.shape);
        if (shape.kind == VirtualShapeKind::Constant)
            return std::nullopt;
        if (shape.kind == VirtualShapeKind::Cell || !visitedDescriptions.add(shape.descriptionIndex))
            continue;

        VirtualFieldAccess found = lookUpField(sinking.objectContext(), family.descriptionOf(shape), access);
        if (found.lookup == FieldLookup::Unknown)
            return std::nullopt;
        if (!result) {
            result = found;
            continue;
        }
        if (found.lookup != result->lookup || found.field != result->field || found.dynamicIndex != result->dynamicIndex)
            return std::nullopt;
        if (found.whenAbsent != result->whenAbsent)
            result->whenAbsent = AbsentField::Unknown;
        if (found.whenPresent != result->whenPresent)
            result->whenPresent = PresentField::Unknown;
    }
    return result;
}

static EnumSet<VirtualObjectKind> virtualKindsOf(const VirtualFamily& family, const ObjectState& state)
{
    EnumSet<VirtualObjectKind> kinds;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = family.at(variant.shape);
        if (shape.kind == VirtualShapeKind::Fields)
            kinds.add(family.descriptionOf(shape).kind);
    }
    return kinds;
}

static std::optional<uint32_t> commonArrayLengthOf(const VirtualFamily& family, const ObjectState& state)
{
    std::optional<uint32_t> length;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = family.at(variant.shape);
        if (shape.kind != VirtualShapeKind::Fields || family.descriptionOf(shape).kind != VirtualObjectKind::Array)
            return std::nullopt;
        if (length && *length != shape.length)
            return std::nullopt;
        length = shape.length;
    }
    return length;
}

static bool isStoredIn(const VirtualFamily& family, const VirtualShape& shape, uint32_t field)
{
    if (!hasStored(shape, field))
        return false;
    const VirtualField& virtualField = family.fieldAt(field);
    return virtualField.kind != VirtualFieldKind::Element || virtualField.index < shape.length;
}

static bool isCarriedIn(const VirtualFamily& family, const ObjectState& state, const VirtualShape& shape, uint32_t field)
{
    if (shape.kind == VirtualShapeKind::Cell)
        return state.fieldsValidInCell.get(field);
    return isStoredIn(family, shape, field);
}

static std::optional<uint32_t> tagBitsWhenCarried(const VirtualFamily& family, const ObjectState& state, uint32_t field)
{
    uint32_t carriedBits = 0;
    uint32_t absentBits = 0;
    for (const ObjectVariant& variant : state.variants) {
        if (!variant.tagBit)
            return std::nullopt;
        uint32_t tagMask = variant.tagMask();
        if (isCarriedIn(family, state, family.at(variant.shape), field))
            carriedBits |= tagMask;
        else
            absentBits |= tagMask;
    }
    if (carriedBits & absentBits)
        return std::nullopt;
    return carriedBits;
}

enum class UnprovenVariants : uint8_t { None, Some, All };

static UnprovenVariants unprovenVariantsOf(const VirtualFamily& family, const ObjectState& state, uint32_t namedField)
{
    std::optional<uint32_t> unprovenField = family.findCounterpart(namedField);
    if (!unprovenField)
        return UnprovenVariants::None;
    auto isStoredInSomeVariant = [&](uint32_t field) {
        return std::ranges::any_of(state.variants, [&](const ObjectVariant& variant) {
            return hasStored(family.at(variant.shape), field);
        });
    };
    if (!isStoredInSomeVariant(*unprovenField))
        return UnprovenVariants::None;
    return isStoredInSomeVariant(namedField) ? UnprovenVariants::Some : UnprovenVariants::All;
}

static TypeID typeOfCell(const AllocationSinking& sinking, VirtualObjectID object)
{
    const Candidate& candidate = sinking.at(object);
    return candidate.isJoin ? sinking.function().at(candidate.definition).type : sinking.descriptionOf(object).type;
}

static bool fieldValueFits(const AllocationSinking& sinking, VirtualObjectID object, const ObjectState& state, uint32_t field, TypeID type)
{
    const VirtualFamily& family = sinking.familyOf(object);
    auto fitsIn = [&](const VirtualObjectDescription& description) {
        return isWithin(sinking.program(), typeOfField(sinking.objectContext(), description, family.fieldAt(field)), type);
    };
    return std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        switch (shape.kind) {
        case VirtualShapeKind::Cell:
            return !sinking.at(object).isJoin && fitsIn(sinking.descriptionOf(object));
        case VirtualShapeKind::Constant:
            return false;
        case VirtualShapeKind::Fields:
            return !hasStored(shape, field) || fitsIn(family.descriptionOf(shape));
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static bool isAssumedWithin(const Program& program, const ObjectState& state, TypeID type)
{
    return state.assumedTypes.containsIf([&](TypeID assumedType) {
        return isWithin(program, assumedType, type);
    });
}

static bool fitsElementTypes(const AllocationSinking& sinking, VirtualObjectID array, const ObjectState& state, TypeID type)
{
    const VirtualFamily& family = sinking.familyOf(array);
    return std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        return isWithin(sinking.program(), type, family.descriptionOf(family.at(variant.shape)).elementType);
    });
}

static bool isLateEdge(const VirtualObjectContext& context, const VirtualFamily& family, const VirtualField& field)
{
    if (field.kind != VirtualFieldKind::Named && field.kind != VirtualFieldKind::LayoutField)
        return false;
    return std::ranges::all_of(family.descriptions(), [&](const VirtualObjectDescription& description) {
        return !layoutFieldOf(context.program, description, field) || canStoreLate(context, description, field) != LateStore::Unknown;
    });
}

static void canonicalizeVariants(ObjectState& state)
{
    AOT::sort(state.variants);
    removeRepeatedElements(state.variants);
    state.normalize();
}

static ValueID emitRangeLength(RewriteCursor& cursor, const Program& program, ValueID start, ValueID end)
{
    constexpr int64_t lengthMask = Range::length().maximum;
    static_assert(!(lengthMask & (lengthMask + 1)), "Masking keeps every length and gives the difference the type of a string length, which the result rule of Sub cannot prove.");

    ValueID difference = cursor.emit(IRBuilder::make(Opcode::Sub, { end, start }));
    if (!program.typeOf(cursor.builder().function(), difference).isInt32())
        difference = cursor.emit(IRBuilder::make(Opcode::ToInt32, { difference }));
    ValueID mask = cursor.builder().appendInteger(lengthMask);
    return cursor.emit(IRBuilder::make(Opcode::BitAnd, { difference, mask }));
}

static ValueID emitPositionInBase(RewriteCursor& cursor, const Program& program, ValueID start, ValueID position)
{
    ValueID sum = cursor.emit(IRBuilder::make(Opcode::Add, { start, position }));
    if (!program.typeOf(cursor.builder().function(), sum).isInt32())
        sum = cursor.emit(IRBuilder::make(Opcode::ToInt32, { sum }));
    return sum;
}

static ValueID emitValueOrUndefined(AllocationSinking& sinking, ObjectInterpreter& interpreter, VirtualObjectID object, uint32_t field, TypeID type)
{
    std::optional<uint32_t> carriedBits = tagBitsWhenCarried(sinking.familyOf(object), *interpreter.state().find(object), field);
    RELEASE_ASSERT(carriedBits);
    ValueID carried = interpreter.valueOfField(object, field, type);
    ValueID none = sinking.cursor().builder().appendUndefined();
    return sinking.cursor().emitSelection(interpreter.emitTagTest(object, *carriedBits), carried, none, type);
}

static ValueID emitValueOfEitherField(AllocationSinking& sinking, ObjectInterpreter& interpreter, VirtualObjectID object, uint32_t namedField, uint32_t unprovenField, TypeID type)
{
    std::optional<uint32_t> unprovenBits = tagBitsWhenCarried(sinking.familyOf(object), *interpreter.state().find(object), unprovenField);
    RELEASE_ASSERT(unprovenBits);
    ValueID proven = interpreter.valueOfField(object, namedField);
    ValueID unproven = interpreter.valueOfField(object, unprovenField);
    return sinking.cursor().emitSelection(interpreter.emitTagTest(object, *unprovenBits), unproven, proven, type);
}

void ObjectInterpreter::executeRead(ValueID id)
{
    VirtualObjectID object = objectOf(function().at(id).operand(0));
    if (!object)
        return executeOther(id);

    const ObjectState& state = *stateOf(object);
    std::optional<VirtualFieldAccess> access = lookUpOnAllVariants(m_sinking, object, state, function().at(id));
    if (!access)
        return executeAccessOnCell(id, object);

    const VirtualFamily& family = m_sinking.familyOf(object);
    EnumSet<VirtualObjectKind> kinds = virtualKindsOf(family, state);
    bool hasOneVirtualKind = state.isVirtual() && kinds.hasExactlyOneBitSet();
    if (kinds.containsAny({ VirtualObjectKind::ArgumentList, VirtualObjectKind::Arguments })) {
        if (!hasOneVirtualKind)
            return executeAccessOnCell(id, object);
        return kinds.contains(VirtualObjectKind::ArgumentList) ? readArgumentList(id, object, *access) : readArguments(id, object, *access);
    }
    if (access->lookup == FieldLookup::DynamicElement && kinds.contains(VirtualObjectKind::StringRange)) {
        Vector<VirtualObjectID, 2> ranges;
        for (VirtualObjectID range = object; range && !stateOf(range)->isCell(); range = stateOf(range)->objectHeldIn(partOf(range, VirtualFieldKind::StringBase))) {
            EnumSet<VirtualObjectKind> kindsOfRange = virtualKindsOf(m_sinking.familyOf(range), *stateOf(range));
            if (!stateOf(range)->isVirtual() || !kindsOfRange.hasExactlyOneBitSet() || !kindsOfRange.contains(VirtualObjectKind::StringRange))
                return executeAccessOnCell(id, object);
            ranges.append(range);
        }
        if (ranges.isEmpty())
            return executeAccessOnCell(id, object);
        if (isRewriting()) {
            ValueID position = access->dynamicIndex;
            for (VirtualObjectID range : ranges)
                position = emitPositionInBase(cursor(), program(), valueOfField(range, partOf(range, VirtualFieldKind::RangeStart)), position);
            replaceRead(id, cursor().emit(IRBuilder::make(Opcode::GetStringCodeUnit, { valueOfField(ranges.last(), partOf(ranges.last(), VirtualFieldKind::StringBase)), position })));
        }
        return;
    }
    if (access->lookup == FieldLookup::DynamicElement)
        return executeDynamicElementRead(id, object, access->dynamicIndex);

    const Type& type = program().typeOf(function(), id);
    bool readsLength = access->lookup == FieldLookup::Field && access->field.kind == VirtualFieldKind::Length;
    std::optional<uint32_t> arrayLength = readsLength ? commonArrayLengthOf(family, state) : std::nullopt;
    if (arrayLength) {
        if (!Type::integer(Range::one(*arrayLength)).isSubsetOf(type, program()))
            return executeAccessOnCell(id, object);
        if (isRewriting())
            replaceRead(id, cursor().builder().appendInteger(*arrayLength));
        return;
    }
    if (readsLength && kinds.contains(VirtualObjectKind::StringRange)) {
        if (!hasOneVirtualKind || !Type::integer(Range::length()).isSubsetOf(type, program()))
            return executeAccessOnCell(id, object);
        if (isRewriting()) {
            ValueID start = valueOfField(object, *family.findField(VirtualField::part(VirtualFieldKind::RangeStart)));
            ValueID end = valueOfField(object, *family.findField(VirtualField::part(VirtualFieldKind::RangeEnd)));
            replaceRead(id, emitRangeLength(cursor(), program(), start, end));
        }
        return;
    }

    std::optional<uint32_t> field = access->lookup == FieldLookup::Field ? family.findField(access->field) : std::nullopt;
    switch (field ? unprovenVariantsOf(family, state, *field) : UnprovenVariants::None) {
    case UnprovenVariants::None:
        break;
    case UnprovenVariants::Some:
        if (!canReadEitherField(id, object, *access, *field))
            return executeAccessOnCell(id, object);
        if (isRewriting())
            replaceRead(id, emitValueOfEitherField(m_sinking, *this, object, *field, *family.findCounterpart(*field), function().at(id).type));
        return;
    case UnprovenVariants::All:
        if (function().at(id).opcode != Opcode::PropGet)
            return executeAccessOnCell(id, object);
        field = family.findCounterpart(*field);
        break;
    }
    FieldRead read = classifyRead(object, *access, field);
    return fitsRead(id, object, field, read) ? finishRead(id, object, field, read) : executeAccessOnCell(id, object);
}

void ObjectInterpreter::finishRead(ValueID id, VirtualObjectID object, std::optional<uint32_t> field, FieldRead read)
{
    switch (read) {
    case FieldRead::Unknown:
        break;
    case FieldRead::HeldObject:
        bind(id, stateOf(object)->objectHeldIn(*field));
        return;
    case FieldRead::Value:
        if (isRewriting())
            replaceRead(id, valueOfField(object, *field, function().at(id).type));
        return;
    case FieldRead::ValueOrUndefined:
        if (isRewriting())
            replaceRead(id, emitValueOrUndefined(m_sinking, *this, object, *field, function().at(id).type));
        return;
    case FieldRead::Undefined:
        if (isRewriting())
            replaceRead(id, emitUndefined());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ObjectInterpreter::canReadEitherField(ValueID read, VirtualObjectID object, const VirtualFieldAccess& access, uint32_t namedField) const
{
    const ObjectState& state = *stateOf(object);
    const VirtualFamily& family = m_sinking.familyOf(object);
    uint32_t unprovenField = *family.findCounterpart(namedField);
    if (function().at(read).opcode != Opcode::PropGet || access.whenPresent != PresentField::IsOwnData || function().at(read).type != TypeTable::anything)
        return false;
    if (state.objectHeldIn(namedField) || state.objectHeldIn(unprovenField) || !tagBitsWhenCarried(family, state, unprovenField))
        return false;
    return std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        return hasStored(shape, namedField) || hasStored(shape, unprovenField);
    });
}

FieldRead ObjectInterpreter::classifyRead(VirtualObjectID object, const VirtualFieldAccess& access, std::optional<uint32_t> field) const
{
    const ObjectState& state = *stateOf(object);
    switch (access.lookup) {
    case FieldLookup::Unknown:
    case FieldLookup::DynamicElement:
        return FieldRead::Unknown;
    case FieldLookup::AbsentEverywhere:
        return state.hasCell() ? FieldRead::Unknown : FieldRead::Undefined;
    case FieldLookup::Field:
        break;
    }

    const VirtualFamily& family = m_sinking.familyOf(object);
    bool readsOwnData = access.whenPresent == PresentField::IsOwnData;
    bool findsNothing = access.whenAbsent == AbsentField::FindsNothing;
    bool hasUnknown = false;
    bool hasValue = false;
    bool hasUndefined = false;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = family.at(variant.shape);
        switch (shape.kind) {
        case VirtualShapeKind::Constant:
            hasUnknown = true;
            break;
        case VirtualShapeKind::Cell:
            if (readsOwnData && field && state.fieldsValidInCell.get(*field))
                hasValue = true;
            else
                hasUnknown = true;
            break;
        case VirtualShapeKind::Fields:
            if (field && isStoredIn(family, shape, *field)) {
                hasValue |= readsOwnData;
                hasUnknown |= !readsOwnData;
            } else {
                hasUndefined |= findsNothing;
                hasUnknown |= !findsNothing;
            }
            break;
        }
    }

    if (hasUnknown)
        return FieldRead::Unknown;
    if (!hasValue)
        return FieldRead::Undefined;
    bool isHeld = !!state.objectHeldIn(*field);
    if (!hasUndefined)
        return isHeld ? FieldRead::HeldObject : FieldRead::Value;
    if (isHeld || !tagBitsWhenCarried(family, state, *field))
        return FieldRead::Unknown;
    return FieldRead::ValueOrUndefined;
}

bool ObjectInterpreter::fitsRead(ValueID read, VirtualObjectID object, std::optional<uint32_t> field, FieldRead kind) const
{
    TypeID type = function().at(read).type;
    const ObjectState& state = *stateOf(object);
    switch (kind) {
    case FieldRead::Unknown:
        return false;
    case FieldRead::Undefined:
        return mayBeUndefined(program().at(type));
    case FieldRead::ValueOrUndefined:
        return mayBeUndefined(program().at(type)) && fieldValueFits(m_sinking, object, state, *field, type);
    case FieldRead::Value:
        return fieldValueFits(m_sinking, object, state, *field, type);
    case FieldRead::HeldObject:
        return fitsType(state.objectHeldIn(*field), type);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ObjectInterpreter::fitsType(VirtualObjectID object, TypeID type) const
{
    const ObjectState* state = stateOf(object);
    RELEASE_ASSERT(state);
    return isAssumedWithin(program(), *state, type) || fitsTypeWithoutCasts(object, type);
}

bool ObjectInterpreter::fitsTypeWithoutCasts(VirtualObjectID object, TypeID type) const
{
    const ObjectState* state = stateOf(object);
    RELEASE_ASSERT(state);
    const VirtualFamily& family = m_sinking.familyOf(object);
    return std::ranges::all_of(state->variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        switch (shape.kind) {
        case VirtualShapeKind::Cell:
            return isWithin(program(), typeOfCell(m_sinking, object), type);
        case VirtualShapeKind::Constant:
            return isWithin(program(), typeOf(shape.constant), type);
        case VirtualShapeKind::Fields: {
            const VirtualObjectDescription& description = family.descriptionOf(shape);
            if (!description.absentResult)
                return isWithin(program(), description.type, type);
            return program().at(description.type).withoutNullish().isSubsetOf(program().at(type), program());
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

void ObjectInterpreter::assumeTypeOfAlias(VirtualObjectID object, TypeID type)
{
    if (!isWithin(program(), typeOfCell(m_sinking, object), type))
        stateOf(object)->assume(type);
}

void ObjectInterpreter::executeDynamicElementRead(ValueID read, VirtualObjectID object, ValueID index)
{
    TypeID type = function().at(read).type;
    auto takesEveryElement = [&] {
        if (!hasListedElements(object) || !mayBeUndefined(program().at(type)))
            return false;
        const VirtualShape& shape = shapeOf(object, stateOf(object)->variants[0]);
        return isWithin(program(), m_sinking.familyOf(object).descriptionOf(shape).elementType, type);
    };
    if (!takesEveryElement())
        return executeOnCell(read, 0);

    requireCellsOfListedElements(object, causeOf(read, 0));
    if (!isRewriting())
        return;

    ValueID none = emitUndefined();
    Vector<ValueID, 8> elements = listedElementsOf(object);
    if (elements.isEmpty()) {
        replaceRead(read, none);
        return;
    }

    IRBuilder& builder = cursor().builder();
    BlockID continuation = cursor().addUncoveredBlock();
    ValueID result = builder.addParameter(continuation, type);
    Vector<std::pair<int64_t, Target>> cases;
    cases.reserveInitialCapacity(elements.size());
    for (size_t position = 0; position < elements.size(); ++position)
        cases.append({ static_cast<int64_t>(position), Target { continuation, { elements[position] }, { } } });
    builder.emitSwitch(resolve(index), WTF::move(cases), Target { continuation, { none }, { } });
    cursor().moveTo(continuation);
    replaceRead(read, result);
}

void ObjectInterpreter::executeStore(ValueID id)
{
    Opcode opcode = function().at(id).opcode;
    VirtualObjectID object = objectOf(function().at(id).operand(0));
    if (!object)
        return executeOther(id);

    if (!stateOf(object)->isVirtual())
        return executeAccessOnCell(id, object);

    ValueID stored = storesWithoutValue.contains(opcode) ? ValueID() : function().at(id).valueOperand();
    if (opcode == Opcode::AppendElement || opcode == Opcode::AppendHole) {
        std::optional<uint32_t> length = commonArrayLengthOf(m_sinking.familyOf(object), *stateOf(object));
        if (!length || exceedsLimit(LimitID::MaxVirtualArrayLength, static_cast<uint64_t>(*length) + 1, object))
            return executeAccessOnCell(id, object);
        if (stored && !fitsElementTypes(m_sinking, object, *stateOf(object), function().at(stored).type))
            return executeAccessOnCell(id, object);
        if (!fieldFor(object, VirtualField::element(*length)))
            return executeAccessOnCell(id, object);
        if (m_state.isDestructured(object)) {
            m_sinking.abandon(object);
            return executeAccessOnCell(id, object);
        }
        appendElement(object, stored);
        return didFoldStores(object, 1);
    }

    std::optional<VirtualFieldAccess> access = lookUpOnAllVariants(m_sinking, object, *stateOf(object), function().at(id));
    if (!access || access->lookup != FieldLookup::Field)
        return executeAccessOnCell(id, object);
    EnumSet<VirtualObjectKind> kinds = virtualKindsOf(m_sinking.familyOf(object), *stateOf(object));
    if (kinds.contains(VirtualObjectKind::Arguments))
        return kinds.hasExactlyOneBitSet() ? storeArgument(id, object, *access) : executeAccessOnCell(id, object);

    std::optional<uint32_t> field = fieldFor(object, access->field);
    if (!field)
        return executeAccessOnCell(id, object);
    if (opcode == Opcode::PropDelete) {
        if (m_state.isDestructured(object)) {
            m_sinking.abandon(object);
            return executeAccessOnCell(id, object);
        }
        deleteField(object, *field);
        if (isRewriting())
            replaceRead(id, cursor().builder().appendBoolean(true));
        return didFoldStores(object, 1);
    }
    if (!canStore(object, *access, *field, stored)) {
        if (opcode != Opcode::PropSet || !canStoreUnprovenValue(object, *access, *field))
            return executeAccessOnCell(id, object);
        field = fieldFor(object, VirtualField::unprovenNamed(access->field.key));
        if (!field)
            return executeAccessOnCell(id, object);
    }
    if (m_state.isDestructured(object)) {
        m_sinking.abandon(object);
        return executeAccessOnCell(id, object);
    }

    if (VirtualObjectID held = objectOf(stored); held && !canHold(object, *field, held)) {
        requireCell(stored, BuildCause { .reasons = EscapeReason::StoredInEscapingObject });
        if (!stateOf(object)->isVirtual())
            return executeAccessOnCell(id, object);
    }
    storeField(object, *field, stored);
    return didFoldStores(object, 1);
}

bool ObjectInterpreter::exceedsLimit(LimitID limit, uint64_t amount, VirtualObjectID object)
{
    if (m_mode == SinkingMode::Count)
        return m_sinking.exceeds(limit, amount, m_sinking.at(object).definition);
    return amount > m_sinking.limit(limit);
}

std::optional<uint32_t> ObjectInterpreter::fieldFor(VirtualObjectID object, const VirtualField& field)
{
    VirtualFamily& family = m_sinking.familyOf(object);
    if (std::optional<uint32_t> existing = family.findField(field))
        return existing;
    if (exceedsLimit(LimitID::MaxVirtualObjectFields, static_cast<uint64_t>(family.numberOfFields()) + 1, object))
        return std::nullopt;
    return family.ensureField(field);
}

bool ObjectInterpreter::canStore(VirtualObjectID object, const VirtualFieldAccess& access, uint32_t field, ValueID stored) const
{
    constexpr EnumSet<VirtualObjectKind> writableKinds { VirtualObjectKind::Object, VirtualObjectKind::Environment, VirtualObjectKind::Array, VirtualObjectKind::Arguments };
    const VirtualFamily& family = m_sinking.familyOf(object);
    const VirtualField& virtualField = family.fieldAt(field);
    TypeID storedType = function().at(stored).type;
    return std::ranges::all_of(stateOf(object)->variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        if (shape.kind != VirtualShapeKind::Fields)
            return false;
        const VirtualObjectDescription& description = family.descriptionOf(shape);
        if (!writableKinds.contains(description.kind))
            return false;
        if (description.kind == VirtualObjectKind::Array && virtualField.kind == VirtualFieldKind::Element && virtualField.index >= shape.length)
            return false;
        bool isPresent = hasStored(shape, field) || hasStoredCounterpart(family, shape, field);
        bool runsNoCode = isPresent ? access.whenPresent == PresentField::IsOwnData : access.whenAbsent == AbsentField::FindsNothing;
        return runsNoCode && isWithin(program(), storedType, typeOfField(m_sinking.objectContext(), description, virtualField));
    });
}

bool ObjectInterpreter::canStoreUnprovenValue(VirtualObjectID object, const VirtualFieldAccess& access, uint32_t namedField) const
{
    const VirtualFamily& family = m_sinking.familyOf(object);
    if (family.refusesUnprovenValues() || m_sinking.storesByNameWaitIn(m_state.block))
        return false;
    return std::ranges::all_of(stateOf(object)->variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        if (shape.kind != VirtualShapeKind::Fields || !canHoldUnprovenValue(program(), family.descriptionOf(shape), access.field))
            return false;
        bool isPresent = hasStored(shape, namedField) || hasStoredCounterpart(family, shape, namedField);
        return isPresent ? access.whenPresent == PresentField::IsOwnData : access.whenAbsent == AbsentField::FindsNothing;
    });
}

bool ObjectInterpreter::canHold(VirtualObjectID holder, uint32_t field, VirtualObjectID held) const
{
    if (m_sinking.decisions().holdsCells(holder, field) || m_sinking.familyOf(holder).fieldAt(field).kind == VirtualFieldKind::UnprovenNamed)
        return false;

    const VirtualFamily& heldFamily = m_sinking.familyOf(held);
    bool canBuildHeldObject = std::ranges::all_of(stateOf(held)->variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = heldFamily.at(variant.shape);
        return shape.kind != VirtualShapeKind::Fields || heldFamily.descriptionOf(shape).canBeBuilt;
    });
    if (!canBuildHeldObject)
        return false;

    const VirtualFamily& family = m_sinking.familyOf(holder);
    if (isLateEdge(m_sinking.objectContext(), family, family.fieldAt(field)))
        return true;
    return !m_state.canReach(held, holder);
}

void ObjectInterpreter::storeField(VirtualObjectID object, uint32_t field, ValueID stored)
{
    VirtualObjectID held = objectOf(stored);
    if (held && !canHold(object, field, held)) {
        requireCell(stored, BuildCause { .reasons = EscapeReason::StoredInEscapingObject });
        held = { };
        if (!stateOf(object)->isVirtual()) {
            m_sinking.abandon(object);
            return;
        }
    }

    ObjectState& state = *stateOf(object);
    VirtualFamily& family = m_sinking.familyOf(object);
    if (held)
        state.hold(field, held);
    else
        state.release(field);
    if (std::optional<uint32_t> counterpart = family.findCounterpart(field))
        state.release(*counterpart);
    if (isRewriting())
        values().ensure(object).setField(field, held ? ValueID() : resolve(stored));

    for (ObjectVariant& variant : state.variants) {
        RELEASE_ASSERT(family.at(variant.shape).kind == VirtualShapeKind::Fields);
        VirtualShape next = shapeAfterStore(family, m_sinking.orders(), family.at(variant.shape), field);
        variant.shape = family.intern(WTF::move(next));
    }
    canonicalizeVariants(state);
}

void ObjectInterpreter::deleteField(VirtualObjectID object, uint32_t namedField)
{
    ObjectState& state = *stateOf(object);
    VirtualFamily& family = m_sinking.familyOf(object);
    state.release(namedField);
    if (std::optional<uint32_t> counterpart = family.findCounterpart(namedField))
        state.release(*counterpart);
    for (ObjectVariant& variant : state.variants)
        variant.shape = family.intern(shapeAfterDelete(family, m_sinking.orders(), family.at(variant.shape), namedField));
    canonicalizeVariants(state);
}

void ObjectInterpreter::appendElement(VirtualObjectID object, ValueID stored)
{
    VirtualFamily& family = m_sinking.familyOf(object);
    ObjectState& state = *stateOf(object);
    std::optional<uint32_t> length = commonArrayLengthOf(family, state);
    if (!length) {
        m_sinking.abandon(object);
        return;
    }

    uint32_t appendedField = 0;
    for (ObjectVariant& variant : state.variants) {
        VirtualShape shape = family.at(variant.shape);
        variant.shape = family.intern(shapeAfterAppend(family, shape, !stored, appendedField));
    }
    canonicalizeVariants(state);

    if (isRewriting()) {
        ValueID newLength = cursor().builder().appendInteger(static_cast<int64_t>(*length) + 1);
        values().ensure(object).setField(family.ensureField(VirtualField::part(VirtualFieldKind::Length)), newLength);
    }
    if (stored)
        storeField(object, appendedField, stored);
}

void ObjectInterpreter::replaceRead(ValueID read, ValueID replacement)
{
    if (!isRewriting())
        return;
    m_sinking.tables().replacedReads.append({ read, replacement });
    cursor().keep(read);
}

ValueID ObjectInterpreter::valueOfField(VirtualObjectID object, uint32_t field)
{
    RELEASE_ASSERT(isRewriting());
    const ObjectValues* objectValues = values().find(object);
    RELEASE_ASSERT(objectValues);
    if (ValueID value = objectValues->field(field))
        return value;

    VirtualObjectID held = stateOf(object)->objectHeldIn(field);
    RELEASE_ASSERT(held);
    return cellOf(held);
}

ValueID ObjectInterpreter::valueOfField(VirtualObjectID object, uint32_t field, TypeID typeInVariants)
{
    ValueID value = valueOfField(object, field);
    if (isWithin(program(), function().at(value).type, typeInVariants))
        return value;
    return cursor().emit(makeAssumeType(value, typeInVariants));
}

ValueID ObjectInterpreter::emitUndefined()
{
    return cursor().emit(Value(Opcode::Undefined));
}

static constexpr uint32_t takenTargetOfTest = 0;

static bool hasLastingAnswer(const Value& test)
{
    return provesForever(provesOf(test.opcode));
}

static bool hasUntaggedVariant(const ObjectState& state)
{
    return state.variants.containsIf([](const ObjectVariant& variant) {
        return !variant.tagBit;
    });
}

static std::optional<TestEntry> evaluateAtOperand(const AllocationSinking& sinking, const ObjectState& state, TestEntry entry, uint32_t operandIndex)
{
    const Value& test = sinking.function().at(entry.test);
    ShapeContext context = sinking.shapeContext(entry.object);
    uint32_t tagBitsWhenTrue = 0;
    uint32_t tagBitsWhenFalse = 0;
    unsigned numberOfTrueAnswers = 0;
    for (const ObjectVariant& variant : state.variants) {
        switch (evaluateTest(context, context.family.at(variant.shape), test, operandIndex)) {
        case KnownBoolean::Unknown:
            return std::nullopt;
        case KnownBoolean::True:
            ++numberOfTrueAnswers;
            tagBitsWhenTrue |= variant.tagMask();
            break;
        case KnownBoolean::False:
            tagBitsWhenFalse |= variant.tagMask();
            break;
        }
    }

    if (!numberOfTrueAnswers || numberOfTrueAnswers == state.variants.size()) {
        entry.result = !!numberOfTrueAnswers;
        return entry;
    }
    if (hasUntaggedVariant(state) || (tagBitsWhenTrue & tagBitsWhenFalse))
        return std::nullopt;
    entry.tagBitsWhenTrue = tagBitsWhenTrue;
    return entry;
}

static std::optional<uint32_t> tagBitsOnEdge(const SinkingState& state, const TestEntry& answer, uint32_t targetIndex)
{
    const ObjectState* objectState = state.find(answer.object);
    if (!objectState || hasUntaggedVariant(*objectState))
        return std::nullopt;
    uint32_t tagBits = objectState->tagBits();
    return targetIndex == takenTargetOfTest ? tagBits & answer.tagBitsWhenTrue : tagBits & ~answer.tagBitsWhenTrue;
}

static bool mayTakeShapeCase(const ShapeContext& context, const VirtualShape& shape, const Terminator& switchOnShape, uint32_t targetIndex)
{
    std::optional<uint32_t> shapeCase = findShapeCase(context, shape, switchOnShape);
    return !shapeCase || *shapeCase == targetIndex;
}

static bool hasOperands(const Value& value)
{
    bool hasOperand = false;
    value.forEachOperand([&](ValueID, uint32_t) {
        hasOperand = true;
    });
    return hasOperand;
}

static bool canBeBuiltByCaller(const Function& function, const VirtualObjectDescription& description)
{
    const Value& allocation = function.at(description.allocation);
    if ((description.kind == VirtualObjectKind::Opaque || description.kind == VirtualObjectKind::ViewRange) && hasOperands(allocation))
        return false;
    return description.canBeBuilt && !description.staysInPlace && boundToOf(allocation.opcode) == BoundTo::Nothing;
}

void ObjectInterpreter::executeTest(ValueID id)
{
    if (testsLeftToRun.contains(function().at(id).opcode))
        return executeOther(id);

    std::optional<TestEntry> entry = evaluateTestOn(id);
    if (!entry) {
        noteListsThatMayNotFit(id);
        return executeOther(id);
    }

    m_state.setTest(*entry);
    if (isRewriting())
        replaceRead(id, entry->result ? cursor().builder().appendBoolean(*entry->result) : emitTagTest(entry->object, entry->tagBitsWhenTrue));
}

void ObjectInterpreter::noteListsThatMayNotFit(ValueID test)
{
    const Value& value = function().at(test);
    VirtualObjectID object = value.opcode == Opcode::HasArrayStorage ? objectOf(value.arrayOperand()) : VirtualObjectID();
    if (!object)
        return;
    for (const ObjectVariant& variant : stateOf(object)->variants) {
        const VirtualShape& shape = shapeOf(object, variant);
        if (shape.kind != VirtualShapeKind::Fields)
            continue;
        const VirtualObjectDescription& description = m_sinking.familyOf(object).descriptionOf(shape);
        if (classifyListOfNumbers(program(), description) == ListOfNumbers::MayNotFit)
            m_sinking.didTestListThatMayNotFit(description.allocation);
    }
}

std::optional<TestEntry> ObjectInterpreter::evaluateTestOn(ValueID test) const
{
    const Value& value = function().at(test);
    if (value.opcode == Opcode::StrictEq && objectOf(value.leftOperand()) && objectOf(value.rightOperand()))
        return evaluateComparison(test);

    VirtualObjectID object;
    std::optional<uint32_t> objectOperand;
    std::optional<uint32_t> guardOperand;
    bool hasOneObject = true;
    value.forEachOperand([&](ValueID operand, uint32_t index) {
        bool isGuard = value.operandKind(index) == OperandKind::Guard;
        VirtualObjectID tracked = isGuard ? guardedObjectOf(operand) : objectOf(operand);
        if (!tracked)
            return;
        std::optional<uint32_t>& position = isGuard ? guardOperand : objectOperand;
        if (position || (object && object != tracked))
            hasOneObject = false;
        position = index;
        object = tracked;
    });
    if (!object || !hasOneObject)
        return std::nullopt;

    uint32_t operandIndex = objectOperand ? *objectOperand : *guardOperand;
    return evaluateAtOperand(m_sinking, *stateOf(object), TestEntry { test, object, std::nullopt }, operandIndex);
}

std::optional<TestEntry> ObjectInterpreter::evaluateComparison(ValueID test) const
{
    const Value& value = function().at(test);
    VirtualObjectID left = objectOf(value.leftOperand());
    VirtualObjectID right = objectOf(value.rightOperand());
    const ObjectState& leftState = *stateOf(left);
    const ObjectState& rightState = *stateOf(right);
    bool areSameObject = left == right;

    if (!areSameObject) {
        if (std::optional<TestEntry> entry = evaluateAtOperand(m_sinking, leftState, TestEntry { test, left, std::nullopt }, OperandIndex::StrictEq::left))
            return entry;
        if (std::optional<TestEntry> entry = evaluateAtOperand(m_sinking, rightState, TestEntry { test, right, std::nullopt }, OperandIndex::StrictEq::right))
            return entry;
        if (m_sinking.at(left).family != m_sinking.at(right).family)
            return std::nullopt;
    }

    ShapeContext context = m_sinking.shapeContext(left);
    std::optional<bool> result;
    for (const ObjectVariant& leftVariant : leftState.variants) {
        for (const ObjectVariant& rightVariant : rightState.variants) {
            if (areSameObject && leftVariant != rightVariant)
                continue;
            KnownBoolean answer = evaluateTestOfTwoShapes(context, shapeOf(left, leftVariant), shapeOf(right, rightVariant), areSameObject, value);
            if (answer == KnownBoolean::Unknown)
                return std::nullopt;
            bool isTrue = answer == KnownBoolean::True;
            if (result && *result != isTrue)
                return std::nullopt;
            result = isTrue;
        }
    }
    return TestEntry { test, left, result };
}

ValueID ObjectInterpreter::emitTagTest(VirtualObjectID object, uint32_t tagBits)
{
    ValueID tag = values().find(object)->tag;
    RELEASE_ASSERT(tag);
    IRBuilder& builder = cursor().builder();
    ValueID mask = builder.appendInteger(tagBits);
    ValueID masked = builder.append(Opcode::BitAnd, { tag, mask });
    return builder.append(Opcode::ToBoolean, { masked });
}

void ObjectInterpreter::buildBeforeBranch(const Terminator& terminator)
{
    unsigned numberOfLiveTargets = 0;
    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex)
        numberOfLiveTargets += !isDeadEdge(terminator, targetIndex);
    if (numberOfLiveTargets < 2)
        return;

    Vector<VirtualObjectID, 4> objects;
    for (const ObjectEntry& entry : m_state.objects) {
        if (!entry.state.isCell() && m_sinking.at(entry.object).isBuiltBeforeBranches)
            objects.append(entry.object);
    }
    for (VirtualObjectID object : objects)
        build(object, BuildCause { .limit = LimitID::MaxMaterializations });
}

void ObjectInterpreter::executeTerminator(const Terminator& terminator)
{
    buildBeforeBranch(terminator);
    switch (terminator.kind) {
    case TerminatorKind::Return:
        executeReturn(terminator);
        return;
    case TerminatorKind::Branch:
        requireCell(terminator.conditionOperand(), causeOf(terminator, OperandIndex::Branch::condition));
        return;
    case TerminatorKind::Jump:
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::Unreachable:
        return;
    case TerminatorKind::SwitchOnShape: {
        ValueID subject = terminator.subjectOperand();
        VirtualObjectID object = objectOf(subject);
        if (!object)
            return;
        ShapeContext context = m_sinking.shapeContext(object);
        bool hasUnknownCase = stateOf(object)->variants.containsIf([&](const ObjectVariant& variant) {
            return !findShapeCase(context, shapeOf(object, variant), terminator);
        });
        if (hasUnknownCase)
            requireCell(subject, causeOf(terminator, OperandIndex::SwitchOnShape::subject));
        return;
    }
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::TryCallInline:
        for (auto [operand, index] : indexedOperandsOf(terminator)) {
            if (objectOf(operand))
                requireCell(operand, causeOf(terminator, index));
        }
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ObjectInterpreter::executeRelevantValue(const BlockValues& values, uint32_t entry)
{
    ValueID id = values[entry & ~AllocationSinking::onlyWritesOrThrows];
    if (!(entry & AllocationSinking::onlyWritesOrThrows)) {
        execute(id);
        return;
    }
    if (m_sinking.checksShortcuts()) [[unlikely]] {
        SinkingState state = m_state;
        ObjectInterpreter(m_sinking, m_mode, state).execute(id);
        forgetFieldsWrittenBy(id);
        RELEASE_ASSERT(m_state == state);
        return;
    }
    forgetFieldsWrittenBy(id);
}

void ObjectInterpreter::executeTerminatorOf(BlockID block)
{
    const Terminator& terminator = function().at(block).terminator;
    if (m_sinking.mentionsName(terminator, block)) {
        executeTerminator(terminator);
        return;
    }
    if (!m_sinking.checksShortcuts()) [[likely]]
        return;
    SinkingState before = m_state;
    executeTerminator(terminator);
    RELEASE_ASSERT(m_state == before);
}

void ObjectInterpreter::checkThatOtherValuesChangeNothing(BlockID block)
{
    const BlockValues& values = function().at(block).values;
    std::span<const uint32_t> relevantValues = m_sinking.relevantValuesOf(block);
    SinkingState before = m_state;
    for (uint32_t index = 0; index < values.size(); ++index) {
        if (!relevantValues.empty() && (relevantValues.front() & ~AllocationSinking::onlyWritesOrThrows) == index) {
            relevantValues = relevantValues.subspan(1);
            continue;
        }
        execute(values[index]);
        RELEASE_ASSERT(m_state == before);
    }
}

void ObjectInterpreter::executeReturn(const Terminator& terminator)
{
    ValueID value = terminator.valueOperand();
    if (!value)
        return;

    BuildCause cause = causeOf(terminator, OperandIndex::Return::value);
    if (m_sinking.policy().returnedObjects == ReturnedObjects::AreBuilt) {
        requireCell(value, cause);
        return;
    }

    VirtualObjectID object = objectOf(value);
    if (object) {
        const VirtualFamily& family = m_sinking.familyOf(object);
        bool canStayVirtual = std::ranges::all_of(stateOf(object)->variants, [&](const ObjectVariant& variant) {
            const VirtualShape& shape = family.at(variant.shape);
            return shape.kind != VirtualShapeKind::Fields || canBeBuiltByCaller(function(), family.descriptionOf(shape));
        });
        if (canStayVirtual) {
            Vector<VirtualObjectID, 4> objectsInFields;
            for (const HeldObject& entry : stateOf(object)->heldObjects)
                objectsInFields.append(entry.object);
            for (VirtualObjectID objectInField : objectsInFields)
                build(objectInField, cause);
        } else
            requireCell(value, cause);
    }

    if (m_mode != SinkingMode::Count)
        return;
    if (object && !stateOf(object)->isCell())
        m_sinking.at(object).diesVirtual = true;
    m_sinking.addVirtualReturn(VirtualReturn { m_state.block, value, object, object ? *stateOf(object) : ObjectState { } });
}

std::optional<TestEntry> ObjectInterpreter::answerAtBranch(ValueID condition) const
{
    const TestEntry* recorded = m_state.findTest(condition);
    if (!recorded)
        return std::nullopt;
    if (recorded->result)
        return *recorded;
    if (hasLastingAnswer(function().at(condition)))
        return evaluateTestOn(condition);
    if (!recorded->tagBitsWhenTrue)
        return std::nullopt;
    return *recorded;
}

bool ObjectInterpreter::isDeadEdge(const Terminator& terminator, uint32_t targetIndex) const
{
    switch (terminator.kind) {
    case TerminatorKind::Branch: {
        std::optional<TestEntry> answer = answerAtBranch(terminator.conditionOperand());
        if (!answer)
            return false;
        if (answer->result)
            return (targetIndex == takenTargetOfTest) != *answer->result;
        std::optional<uint32_t> tagBits = tagBitsOnEdge(m_state, *answer, targetIndex);
        return tagBits && !*tagBits;
    }
    case TerminatorKind::SwitchOnShape: {
        VirtualObjectID object = objectOf(terminator.subjectOperand());
        if (!object || !stateOf(object)->isVirtual())
            return false;
        ShapeContext context = m_sinking.shapeContext(object);
        return !stateOf(object)->variants.containsIf([&](const ObjectVariant& variant) {
            return mayTakeShapeCase(context, shapeOf(object, variant), terminator, targetIndex);
        });
    }
    case TerminatorKind::Jump:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ObjectInterpreter::hasInterpretedTest(const Terminator& terminator) const
{
    switch (terminator.kind) {
    case TerminatorKind::Branch:
        return !!m_state.findTest(terminator.conditionOperand());
    case TerminatorKind::SwitchOnShape: {
        VirtualObjectID object = objectOf(terminator.subjectOperand());
        return object && stateOf(object)->isVirtual();
    }
    case TerminatorKind::Jump:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ObjectInterpreter::narrowOnEdge(const Terminator& terminator, uint32_t targetIndex)
{
    switch (terminator.kind) {
    case TerminatorKind::Branch: {
        std::optional<TestEntry> answer = answerAtBranch(terminator.conditionOperand());
        if (!answer || answer->result)
            break;
        if (std::optional<uint32_t> tagBits = tagBitsOnEdge(m_state, *answer, targetIndex))
            narrow(answer->object, *tagBits);
        break;
    }
    case TerminatorKind::SwitchOnShape: {
        VirtualObjectID object = objectOf(terminator.subjectOperand());
        if (!object || !stateOf(object)->isVirtual())
            break;
        if (std::optional<uint32_t> tagBits = tagBitsOfShapeCase(terminator, object, targetIndex))
            narrow(object, *tagBits);
        break;
    }
    case TerminatorKind::Jump:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        break;
    }
    forgetAnswersOfTests();
}

void ObjectInterpreter::forgetAnswersOfTests()
{
    for (TestEntry& entry : m_state.tests)
        entry = TestEntry { entry.test, hasLastingAnswer(function().at(entry.test)) ? VirtualObjectID() : entry.object, std::nullopt };
}

void ObjectInterpreter::narrow(VirtualObjectID object, uint32_t keptTagBits)
{
    ObjectState& state = *stateOf(object);
    state.variants.removeAllMatching([&](const ObjectVariant& variant) {
        return variant.tagBit && !(keptTagBits & variant.tagMask());
    });
    RELEASE_ASSERT(!state.variants.isEmpty());
    state.lacksFieldsOnSomeWay |= std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        return shapeOf(object, variant).kind == VirtualShapeKind::Constant;
    });

    state.heldObjects.removeAllMatching([&](const HeldObject& held) {
        return !state.variants.containsIf([&](const ObjectVariant& variant) {
            return hasStored(shapeOf(object, variant), held.field);
        });
    });
}

std::optional<uint32_t> ObjectInterpreter::tagBitsOfShapeCase(const Terminator& terminator, VirtualObjectID object, uint32_t targetIndex) const
{
    const ObjectState& state = *stateOf(object);
    if (hasUntaggedVariant(state))
        return std::nullopt;

    ShapeContext context = m_sinking.shapeContext(object);
    uint32_t tagBits = 0;
    for (const ObjectVariant& variant : state.variants) {
        if (mayTakeShapeCase(context, shapeOf(object, variant), terminator, targetIndex))
            tagBits |= variant.tagMask();
    }
    return tagBits;
}

static_assert(OperandIndex::AppendSpread::array == OperandIndex::AppendElements::array);
static_assert(OperandIndex::AppendSpread::iterable == OperandIndex::AppendElements::source);

void convertListCall(Value& call, std::span<const ValueID> elements)
{
    Value listCall = call;
    switch (listCall.opcode) {
    case Opcode::CallList:
        call.become(Opcode::Call);
        call.setCalleeOperand(listCall.calleeOperand());
        call.setThisValueOperand(listCall.thisValueOperand());
        call.setCalleeKey(listCall.calleeKey());
        call.setCalleeName(listCall.calleeName());
        call.setExpectedResult(listCall.expectedResult());
        call.setIsDemotedTailCall(listCall.isDemotedTailCall());
        break;
    case Opcode::ConstructList:
        call.become(Opcode::Construct);
        call.setCalleeOperand(listCall.calleeOperand());
        call.setNewTargetOperand(listCall.newTargetOperand());
        call.setCalleeName(listCall.calleeName());
        break;
    case Opcode::SuperConstructList:
        call.become(Opcode::SuperConstruct);
        call.setLayout(listCall.layout());
        call.setParentOperand(listCall.parentOperand());
        call.setNewTargetOperand(listCall.newTargetOperand());
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    call.setAccessSite(listCall.accessSite());
    call.setCallees(listCall.callees());
    call.setArgumentsOperands(elements);
}

void ObjectInterpreter::executeListUse(ValueID id)
{
    switch (function().at(id).opcode) {
    case Opcode::AppendSpread:
    case Opcode::AppendElements:
        return executeAppendElements(id);
    case Opcode::CopyDataProperties:
        return executeCopyDataProperties(id);
    case Opcode::CallList:
    case Opcode::ConstructList:
    case Opcode::SuperConstructList:
        return executeCallWithList(id);
    case Opcode::DestructureIterable:
        return executeDestructuring(id);
    case Opcode::ResultAt:
        return executeResultRead(id);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

const VirtualObjectDescription* ObjectInterpreter::singleDescriptionOf(VirtualObjectID object) const
{
    const ObjectState* state = stateOf(object);
    if (!state || state->variants.size() != 1)
        return nullptr;
    const VirtualShape& shape = shapeOf(object, state->variants[0]);
    if (shape.kind != VirtualShapeKind::Fields)
        return nullptr;
    return &m_sinking.familyOf(object).descriptionOf(shape);
}

bool ObjectInterpreter::hasListedElements(VirtualObjectID object) const
{
    const VirtualObjectDescription* description = singleDescriptionOf(object);
    return description && description->kind == VirtualObjectKind::Array;
}

std::optional<uint32_t> ObjectInterpreter::findListedElement(VirtualObjectID object, uint32_t position) const
{
    const VirtualShape& shape = singleShapeOf(object);
    if (position >= shape.length)
        return std::nullopt;
    std::optional<uint32_t> field = m_sinking.familyOf(object).findField(VirtualField::element(position));
    if (!field || !hasStored(shape, *field))
        return std::nullopt;
    return field;
}

Vector<ValueID, 8> ObjectInterpreter::listedElementsOf(VirtualObjectID object)
{
    RELEASE_ASSERT(isRewriting() && hasListedElements(object));
    uint32_t length = singleShapeOf(object).length;
    Vector<ValueID, 8> elements;
    elements.reserveInitialCapacity(length);
    ValueID valueOfHoles;
    for (uint32_t position = 0; position < length; ++position) {
        if (std::optional<uint32_t> field = findListedElement(object, position)) {
            elements.append(valueOfField(object, *field, singleDescriptionOf(object)->elementType));
            continue;
        }
        if (!valueOfHoles)
            valueOfHoles = emitUndefined();
        elements.append(valueOfHoles);
    }
    return elements;
}

void ObjectInterpreter::requireCellOfHeldObject(VirtualObjectID holder, uint32_t field, const BuildCause& cause)
{
    VirtualObjectID held = stateOf(holder)->objectHeldIn(field);
    if (!held)
        return;

    build(held, cause);
    ObjectState& state = *stateOf(holder);
    if (state.isCell())
        return;

    state.release(field);
    if (isRewriting()) {
        ValueID cell = cellOf(held);
        values().ensure(holder).setField(field, cell);
    }
    stateDidChange(holder);
}

void ObjectInterpreter::requireCellsOfListedElements(VirtualObjectID object, const BuildCause& cause)
{
    Vector<HeldObject, 1> heldObjects = stateOf(object)->heldObjects;
    for (const HeldObject& held : heldObjects)
        requireCellOfHeldObject(object, held.field, cause);
    if (!hasListedElements(object))
        m_sinking.abandon(object);
}

bool ObjectInterpreter::canAppendListedElements(VirtualObjectID target, VirtualObjectID source)
{
    const ObjectState* state = stateOf(target);
    if (!state)
        return false;

    uint32_t length = singleShapeOf(source).length;
    uint32_t numberOfHoles = 0;
    for (uint32_t position = 0; position < length; ++position)
        numberOfHoles += !findListedElement(source, position);

    uint32_t targetLength = shapeOf(target, state->variants[0]).length;
    const Type& listedType = program().at(singleDescriptionOf(source)->elementType);
    const Type& typeOfHoles = program().at(TypeTable::undefined);
    bool admitsElements = std::ranges::all_of(state->variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = shapeOf(target, variant);
        if (shape.kind != VirtualShapeKind::Fields || shape.length != targetLength)
            return false;
        const VirtualObjectDescription& description = m_sinking.familyOf(target).descriptionOf(shape);
        if (description.kind != VirtualObjectKind::Array)
            return false;
        const Type& elementType = program().at(description.elementType);
        if (numberOfHoles < length && !listedType.isSubsetOf(elementType, program()))
            return false;
        return !numberOfHoles || typeOfHoles.isSubsetOf(elementType, program());
    });
    if (!admitsElements)
        return false;

    if (exceedsLimit(LimitID::MaxVirtualArrayLength, static_cast<uint64_t>(targetLength) + length, target))
        return false;

    for (uint32_t position = 0; position < length; ++position) {
        if (!fieldFor(target, VirtualField::element(targetLength + position)))
            return false;
    }
    return true;
}

void ObjectInterpreter::executeAppendElements(ValueID id)
{
    constexpr uint32_t arrayIndex = OperandIndex::AppendElements::array;
    constexpr uint32_t sourceIndex = OperandIndex::AppendElements::source;

    ValueID array = function().at(id).operand(arrayIndex);
    VirtualObjectID target = objectOf(array);
    VirtualObjectID source = objectOf(function().at(id).operand(sourceIndex));
    if (source == target || !hasListedElements(source))
        return executeOther(id);

    requireCellsOfListedElements(source, causeOf(id, sourceIndex));
    bool targetIsVirtual = hasListedElements(source) && canAppendListedElements(target, source);
    if (targetIsVirtual && m_state.isDestructured(target)) {
        m_sinking.abandon(target);
        targetIsVirtual = false;
    }
    if (!targetIsVirtual)
        requireCell(array, causeOf(id, arrayIndex));
    if (!hasListedElements(source))
        return executeOther(id);

    uint32_t length = singleShapeOf(source).length;
    Vector<ValueID, 8> elements;
    if (isRewriting()) {
        cursor().setOrigin(function().at(id).origin());
        elements = listedElementsOf(source);
    }

    if (targetIsVirtual) {
        for (uint32_t position = 0; position < length; ++position)
            appendElement(target, isRewriting() ? elements[position] : id);
        return didFoldStores(target, 1);
    }

    forgetFieldsWrittenBy(id);
    if (isRewriting() && length) {
        cursor().willKeepThrowingValue(*this);
        ValueID cell = resolve(array);
        for (ValueID element : elements)
            cursor().emit(IRBuilder::make(Opcode::AppendElement, { cell, element }));
    }
}

std::optional<Vector<CopiedField, 8>> ObjectInterpreter::collectCopiedFields(ValueID copy, VirtualObjectID source) const
{
    const Value& value = function().at(copy);
    if (!value.excludedOperands().empty())
        return std::nullopt;

    const VirtualObjectDescription* description = singleDescriptionOf(source);
    if (!description || !description->hasKeyOrder)
        return std::nullopt;
    if (description->kind != VirtualObjectKind::Object && description->kind != VirtualObjectKind::BuiltinFields)
        return std::nullopt;

    const VirtualFamily& family = m_sinking.familyOf(source);
    std::span<const PropertyKeyID> excludedKeys = value.excludedKeys();
    Vector<CopiedField, 8> fields;
    if (hasUnprovenField(family, singleShapeOf(source)))
        return std::nullopt;
    for (uint32_t field : m_sinking.orders().fieldsOf(singleShapeOf(source).order)) {
        const VirtualField& property = family.fieldAt(field);
        if (property.kind != VirtualFieldKind::Named)
            return std::nullopt;
        if (std::ranges::contains(excludedKeys, property.key))
            continue;
        if (!isEnumerable(program(), *description, property))
            return std::nullopt;
        fields.append(CopiedField { property.key, field, 0 });
    }
    return fields;
}

bool ObjectInterpreter::tryResolveTargetFields(VirtualObjectID target, const VirtualObjectDescription& source, std::span<CopiedField> fields)
{
    const ObjectState* state = stateOf(target);
    if (!state)
        return false;

    Vector<const VirtualObjectDescription*, 2> descriptions;
    for (const ObjectVariant& variant : state->variants) {
        const VirtualShape& shape = shapeOf(target, variant);
        if (shape.kind != VirtualShapeKind::Fields)
            return false;
        const VirtualObjectDescription& description = m_sinking.familyOf(target).descriptionOf(shape);
        if (description.kind != VirtualObjectKind::Object)
            return false;
        descriptions.append(&description);
    }

    const VirtualObjectContext& context = m_sinking.objectContext();
    for (CopiedField& field : fields) {
        VirtualField property = VirtualField::named(field.key);
        const Type& copiedType = program().at(typeOfField(context, source, property));
        bool isAdmitted = std::ranges::all_of(descriptions, [&](const VirtualObjectDescription* description) {
            return layoutFieldOf(program(), *description, property)
                && isEnumerable(program(), *description, property)
                && copiedType.isSubsetOf(program().at(typeOfField(context, *description, property)), program());
        });
        if (!isAdmitted)
            return false;
        std::optional<uint32_t> targetField = fieldFor(target, property);
        if (!targetField)
            return false;
        field.targetField = *targetField;
    }
    return true;
}

void ObjectInterpreter::executeCopyDataProperties(ValueID id)
{
    ValueID targetValue = function().at(id).targetOperand();
    VirtualObjectID target = objectOf(targetValue);
    VirtualObjectID source = objectOf(function().at(id).sourceOperand());
    if (source == target)
        return executeOther(id);
    std::optional<Vector<CopiedField, 8>> fields = collectCopiedFields(id, source);
    if (!fields)
        return executeOther(id);

    bool targetIsVirtual = tryResolveTargetFields(target, *singleDescriptionOf(source), fields->mutableSpan());
    BuildCause cause = causeOf(id, OperandIndex::CopyDataProperties::source);
    for (const CopiedField& field : *fields)
        requireCellOfHeldObject(source, field.sourceField, cause);
    targetIsVirtual = targetIsVirtual && stateOf(target)->isVirtual();
    if (!targetIsVirtual)
        requireCell(targetValue, causeOf(id, OperandIndex::CopyDataProperties::target));
    if (!singleDescriptionOf(source))
        return executeOther(id);

    if (targetIsVirtual) {
        for (const CopiedField& field : *fields)
            storeField(target, field.targetField, isRewriting() ? valueOfField(source, field.sourceField) : id);
        return didFoldStores(target, static_cast<unsigned>(fields->size()));
    }

    forgetFieldsWrittenBy(id);
    if (isRewriting() && !fields->isEmpty()) {
        cursor().setOrigin(function().at(id).origin());
        cursor().willKeepThrowingValue(*this);
        ValueID cell = resolve(targetValue);
        for (const CopiedField& field : *fields) {
            Value define = IRBuilder::make(Opcode::PropDefine, { cell, valueOfField(source, field.sourceField) });
            define.setKey(field.key);
            cursor().emit(WTF::move(define));
        }
    }
}

void ObjectInterpreter::executeCallWithList(ValueID id)
{
    constexpr uint32_t listIndex = OperandIndex::CallList::list;

    VirtualObjectID list = objectOf(function().at(id).operand(listIndex));
    const VirtualObjectDescription* description = singleDescriptionOf(list);
    if (description && description->kind == VirtualObjectKind::ArgumentList)
        return forwardArgumentList(id, list);
    if (!hasListedElements(list))
        return executeOther(id);

    requireCellsOfListedElements(list, causeOf(id, listIndex));
    requireCellsOfOperandsBeside(id, listIndex);
    if (!hasListedElements(list))
        return executeOther(id);

    if (isRewriting()) {
        cursor().setOrigin(function().at(id).origin());
        Vector<ValueID, 8> elements = listedElementsOf(list);
        convertListCall(cursor().builder().function().edit(id), elements.span());
    }
    forgetFieldsWrittenBy(id);
    keep(id);
}

void ObjectInterpreter::executeDestructuring(ValueID id)
{
    VirtualObjectID object = objectOf(function().at(id).iterableOperand());
    const VirtualObjectDescription* description = singleDescriptionOf(object);
    if (description && description->kind == VirtualObjectKind::ArgumentList)
        return destructureArgumentList(id, object);
    bool isWithinPrefix = description && description->kind == VirtualObjectKind::BuiltinPrefix && function().at(id).count() <= singleShapeOf(object).length;
    if (!hasListedElements(object) && !isWithinPrefix)
        return executeOther(id);

    bind(id, object, AliasKind::Results);
}

void ObjectInterpreter::abandonDestructuredRead(ValueID read, VirtualObjectID object)
{
    m_sinking.abandon(object);
    if (isRewriting())
        replaceRead(read, emitUndefined());
}

void ObjectInterpreter::executeResultRead(ValueID id)
{
    VirtualObjectID object = destructuredObjectOf(function().at(id).resultsOperand());
    if (!object)
        return executeOther(id);

    uint32_t index = function().at(id).index();
    const VirtualObjectDescription* description = singleDescriptionOf(object);
    if (description && description->kind == VirtualObjectKind::ArgumentList) {
        if (!program().at(TypeTable::anything).isSubsetOf(program().typeOf(function(), id), program()))
            return abandonDestructuredRead(id, object);
        if (isRewriting()) {
            Value argument(Opcode::GetArgument);
            argument.setArgumentIndex(description->firstArgument + index);
            replaceRead(id, cursor().emit(WTF::move(argument)));
        }
        return;
    }
    bool isPrefix = description && description->kind == VirtualObjectKind::BuiltinPrefix;
    if (!hasListedElements(object) && !isPrefix)
        return abandonDestructuredRead(id, object);

    std::optional<uint32_t> field = findListedElement(object, index);
    if (isPrefix && !field)
        return abandonDestructuredRead(id, object);
    VirtualObjectID held = field ? stateOf(object)->objectHeldIn(*field) : VirtualObjectID();
    FieldRead read = !field ? FieldRead::Undefined : held ? FieldRead::HeldObject : FieldRead::Value;
    return fitsRead(id, object, field, read) ? finishRead(id, object, field, read) : abandonDestructuredRead(id, object);
}

struct MemberVisit {
    VirtualObjectID member;
    uint32_t numberOfVisitedHeldObjects { 0 };
};

enum class PropertyInitialization : uint8_t { Listed, ListedWithPlaceholder, ListedWithFiller, StoredLate };

struct PlannedProperty {
    uint32_t field { 0 };
    VirtualObjectID awaitedObject;
    PropertyInitialization initialization { PropertyInitialization::Listed };
};

static bool isStoredInAnyVariant(const VirtualFamily& family, const ObjectState& state, uint32_t field)
{
    return std::ranges::any_of(state.variants, [&](const ObjectVariant& variant) {
        return hasStored(family.at(variant.shape), field);
    });
}

static bool isStoredInEveryVariant(const VirtualFamily& family, const ObjectState& state, uint32_t field)
{
    return std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        return hasStored(family.at(variant.shape), field);
    });
}

static bool canBuildEveryVariant(const VirtualFamily& family, const ObjectState& state)
{
    return std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        return shape.kind != VirtualShapeKind::Fields || family.descriptionOf(shape).canBeBuilt;
    });
}

static bool hasVariantWithUnprovenField(const VirtualFamily& family, const ObjectState& state)
{
    return std::ranges::any_of(state.variants, [&](const ObjectVariant& variant) {
        return hasUnprovenField(family, family.at(variant.shape));
    });
}

static bool isHardEdge(const AllocationSinking& sinking, VirtualObjectID holder, const ObjectState& state, uint32_t field)
{
    if (state.needsTag() || sinking.at(holder).isJoin)
        return true;
    const VirtualFamily& family = sinking.familyOf(holder);
    const VirtualObjectDescription& description = family.descriptionOf(family.at(state.variants[0].shape));
    return canStoreLate(sinking.objectContext(), description, family.fieldAt(field)) == LateStore::Unknown;
}

static Vector<PlannedProperty, 8> planProperties(const AllocationSinking& sinking, const SinkingState& state, VirtualObjectID object, const FreshResultVariant& variant)
{
    const VirtualFamily& family = sinking.familyOf(object);
    const ObjectState& objectState = *state.find(object);

    Vector<PlannedProperty, 8> properties;
    bool allocationHasEnded = false;
    for (const FreshResultField& property : variant.properties) {
        PlannedProperty planned;
        planned.field = *family.findField(property.field);
        if (property.field.kind == VirtualFieldKind::UnprovenNamed)
            planned.initialization = PropertyInitialization::ListedWithFiller;
        VirtualObjectID held = objectState.objectHeldIn(planned.field);
        if (held && !state.find(held)->isCell()) {
            planned.awaitedObject = held;
            switch (canStoreLate(sinking.objectContext(), variant.description, property.field)) {
            case LateStore::NeedsNoPlaceholder:
                planned.initialization = PropertyInitialization::StoredLate;
                break;
            case LateStore::NeedsUndefinedPlaceholder:
                planned.initialization = PropertyInitialization::ListedWithPlaceholder;
                break;
            case LateStore::CreatesProperty:
                allocationHasEnded = true;
                break;
            case LateStore::Unknown:
                break;
            }
        }
        if (allocationHasEnded)
            planned.initialization = PropertyInitialization::StoredLate;
        properties.append(planned);
    }
    return properties;
}

static unsigned countRepeatedStores(const AllocationSinking& sinking, const SinkingState& state, VirtualObjectID object)
{
    const ObjectState& objectState = *state.find(object);
    const VirtualFamily& family = sinking.familyOf(object);
    const VirtualShape& shape = family.at(objectState.variants[0].shape);
    if (objectState.needsTag() || shape.kind != VirtualShapeKind::Fields)
        return 0;

    FreshResultVariant variant = variantOfShape(sinking, object, shape);
    BitVector fieldsListedByAllocation;
    forEachInitialField(sinking.objectContext(), variant.description, [&](const VirtualField& initialField, ValueID) {
        fieldsListedByAllocation.set(*family.findField(initialField));
    });

    unsigned count = 0;
    for (const PlannedProperty& property : planProperties(sinking, state, object, variant)) {
        switch (property.initialization) {
        case PropertyInitialization::Listed:
            break;
        case PropertyInitialization::ListedWithPlaceholder:
            count += fieldsListedByAllocation.get(property.field);
            break;
        case PropertyInitialization::ListedWithFiller:
        case PropertyInitialization::StoredLate:
            ++count;
            break;
        }
    }
    return count;
}

static bool isBorrowingCall(const BuildCause& cause)
{
    return cause.opcode && effectRuleOf(*cause.opcode) == EffectRule::Call && cause.reasons.isEmpty() && cause.limit == LimitID::None;
}

void ObjectInterpreter::requireCell(ValueID value, const BuildCause& cause)
{
    if (VirtualObjectID object = objectOf(value))
        build(object, cause);
}

void ObjectInterpreter::build(VirtualObjectID root, const BuildCause& cause)
{
    if (stateOf(root)->isCell())
        return;

    Vector<VirtualObjectID, 4> members = collectVirtualMembers(root);
    for (VirtualObjectID member : members) {
        const ObjectState& state = *stateOf(member);
        if (!canBuildEveryVariant(m_sinking.familyOf(member), state))
            m_sinking.abandon(member);
        if (!cause.admitsStoresByName && hasVariantWithUnprovenField(m_sinking.familyOf(member), state))
            m_sinking.refuseUnprovenValues(member);
        if (m_sinking.at(member).isJoin && !fitsType(member, function().at(m_sinking.at(member).definition).type))
            m_sinking.abandon(member);
        if (m_mode == SinkingMode::Count && state.hasCell())
            m_sinking.didBuildAfterMerge(member, state.cellJoinedAt);
    }

    if (!isRewriting()) {
        buildMembers(members.span(), cause);
        return;
    }

    Origin userOrigin = cursor().builder().origin();
    buildMembers(members.span(), cause);
    cursor().setOrigin(userOrigin);
}

Vector<VirtualObjectID, 4> ObjectInterpreter::collectVirtualMembers(VirtualObjectID root) const
{
    Vector<VirtualObjectID, 4> members;
    Vector<VirtualObjectID, 4> worklist { root };
    BitVector visited;
    while (!worklist.isEmpty()) {
        VirtualObjectID member = worklist.takeLast();
        const ObjectState* state = stateOf(member);
        RELEASE_ASSERT(state);
        if (state->isCell() || !visited.add(member.index()))
            continue;

        members.append(member);
        const VirtualFamily& family = m_sinking.familyOf(member);
        for (const HeldObject& held : state->heldObjects | std::views::reverse) {
            if (isStoredInAnyVariant(family, *state, held.field))
                worklist.append(held.object);
        }
    }
    return members;
}

bool ObjectInterpreter::isBuiltInArmsOf(VirtualObjectID holder, const HeldObject& held) const
{
    const ObjectState& state = *stateOf(holder);
    return stateOf(held.object)->isMissingOnSomeWay && state.needsTag() && !hasBothAllocationOutcomes(holder)
        && !isStoredInEveryVariant(m_sinking.familyOf(holder), state, held.field);
}

bool ObjectInterpreter::isReachedOnlyThrough(VirtualObjectID holder, VirtualObjectID held) const
{
    Vector<VirtualObjectID, 4> members = collectVirtualMembers(held);
    if (members.contains(holder))
        return false;
    for (VirtualObjectID member : members) {
        if (!stateOf(member)->isMissingOnSomeWay)
            return false;
    }
    for (const ObjectEntry& entry : m_state.objects) {
        if (members.contains(entry.object))
            continue;
        for (const HeldObject& other : entry.state.heldObjects) {
            if (!members.contains(other.object))
                continue;
            if (entry.object != holder || other.object != held || !isBuiltInArmsOf(holder, other))
                return false;
        }
    }
    return !m_state.aliases.containsIf([&](const AliasEntry& alias) {
        return members.contains(alias.object);
    }) && !m_state.tests.containsIf([&](const TestEntry& test) {
        return members.contains(test.object);
    });
}

void ObjectInterpreter::buildMembers(std::span<const VirtualObjectID> members, const BuildCause& cause)
{
    Vector<LateFieldStore, 2> late;
    Vector<MemberVisit, 4> visits;
    BitVector membersBeingVisited;
    BitVector membersBuiltInArms;
    for (VirtualObjectID member : members) {
        const ObjectState& state = *stateOf(member);
        for (const HeldObject& held : state.heldObjects) {
            if (stateOf(held.object)->isCell() || !isBuiltInArmsOf(member, held))
                continue;
            if (isStoredInAnyVariant(m_sinking.familyOf(member), state, held.field) && !isReachedOnlyThrough(member, held.object)) {
                RELEASE_ASSERT(!isRewriting());
                m_sinking.abandon(held.object);
            }
            if (!isRewriting())
                continue;
            for (VirtualObjectID memberOfArms : collectVirtualMembers(held.object))
                membersBuiltInArms.set(memberOfArms.index());
        }
    }

    auto beginVisit = [&](VirtualObjectID member) {
        if (stateOf(member)->isCell() || membersBuiltInArms.get(member.index()))
            return;
        membersBeingVisited.set(member.index());
        visits.append(MemberVisit { member, 0 });
    };

    auto emitCell = [&](VirtualObjectID member) {
        bool isAsAllocated = hasBothAllocationOutcomes(member);
        if (stateOf(member)->needsTag() && !isAsAllocated) {
            buildByTag(member, late, cause);
            return;
        }
        ValueID cell = isAsAllocated
            ? emitAllocation(member, VirtualShape(shapeOf(member, stateOf(member)->variants[1])), late, AbsentResult::IsPossible)
            : emitValueOfVariant(member, stateOf(member)->variants[0], late);
        ObjectValues& carried = values().ensure(member);
        carried.cell = cell;
        if (m_sinking.at(member).needsGuard && !carried.guard)
            carried.guard = cursor().emit(Value(Opcode::AllGuards));
    };

    auto finishVisit = [&](VirtualObjectID member) {
        visits.removeLast();
        membersBeingVisited.clear(member.index());
        if (m_mode == SinkingMode::Count)
            m_sinking.at(member).numberOfRepeatedStores += countRepeatedStores(m_sinking, m_state, member);
        if (isRewriting())
            emitCell(member);
        didBuild(member, cause);
        if (isRewriting() && m_sinking.at(member).isJoin)
            values().ensure(member).cell = cellOf(member);
    };

    for (VirtualObjectID first : members | std::views::reverse) {
        beginVisit(first);
        while (!visits.isEmpty()) {
            VirtualObjectID member = visits.last().member;
            const ObjectState& state = *stateOf(member);
            uint32_t position = visits.last().numberOfVisitedHeldObjects;
            if (position == state.heldObjects.size()) {
                finishVisit(member);
                continue;
            }

            ++visits.last().numberOfVisitedHeldObjects;
            HeldObject held = state.heldObjects[position];
            if (stateOf(held.object)->isCell() || !isStoredInAnyVariant(m_sinking.familyOf(member), state, held.field))
                continue;
            if (isBuiltInArmsOf(member, held) && !isReachedOnlyThrough(member, held.object)) {
                RELEASE_ASSERT(!isRewriting());
                m_sinking.abandon(held.object);
            }
            if (!isHardEdge(m_sinking, member, state, held.field) && m_state.canReach(held.object, member)) {
                if (m_mode == SinkingMode::Count)
                    m_sinking.at(held.object).holdersThatStoreItLate.appendIfNotContains(member);
                continue;
            }
            if (membersBeingVisited.get(held.object.index())) {
                RELEASE_ASSERT(!isRewriting());
                m_sinking.abandon(member);
                continue;
            }
            beginVisit(held.object);
        }
    }

    emitLateStores(late.span());
}

ValueID ObjectInterpreter::emitValueOfVariant(VirtualObjectID object, const ObjectVariant& variant, Vector<LateFieldStore, 2>& late)
{
    const VirtualShape& shape = shapeOf(object, variant);
    switch (shape.kind) {
    case VirtualShapeKind::Cell: {
        ValueID cell = values().find(object)->cell;
        RELEASE_ASSERT(cell);
        return cell;
    }
    case VirtualShapeKind::Constant:
        return cursor().emit(makeConstant(shape.constant));
    case VirtualShapeKind::Fields:
        return emitAllocation(object, VirtualShape(shape), late);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ObjectInterpreter::hasBothAllocationOutcomes(VirtualObjectID object) const
{
    std::optional<uint8_t> tagBitWhenAbsent = m_sinking.at(object).tagBitWhenAbsent;
    if (!tagBitWhenAbsent || m_sinking.descriptionOf(object).staysInPlace || stateOf(object)->lacksFieldsOnSomeWay)
        return false;
    const auto& variants = stateOf(object)->variants;
    return variants.size() == 2 && variants[0].tagBit == *tagBitWhenAbsent && variants[1].tagBit == *tagBitWhenAbsent + 1;
}

ValueID ObjectInterpreter::emitAllocation(VirtualObjectID object, const VirtualShape& shape, Vector<LateFieldStore, 2>& late, AbsentResult absentResult)
{
    const VirtualFamily& family = m_sinking.familyOf(object);
    FreshResultVariant variant = variantOfShape(m_sinking, object, shape);
    cursor().setOrigin(variant.prototype.origin());

    ValueID placeholder;
    Vector<FieldValue, 8> properties;
    Vector<FieldValue, 2> storedByName;
    for (const PlannedProperty& property : planProperties(m_sinking, m_state, object, variant)) {
        const VirtualField& field = family.fieldAt(property.field);
        if (property.initialization == PropertyInitialization::Listed) {
            properties.append(FieldValue { field, valueOfField(object, property.field, typeOfField(m_sinking.objectContext(), variant.description, field)) });
            continue;
        }
        if (property.initialization == PropertyInitialization::ListedWithFiller) {
            properties.append(FieldValue { field, cursor().emit(makeFiller(program(), variant.description, field)) });
            storedByName.append(FieldValue { field, valueOfField(object, property.field) });
            continue;
        }

        ValueID knownValue = property.awaitedObject ? ValueID() : valueOfField(object, property.field);
        late.append(LateFieldStore { object, property.field, property.awaitedObject, knownValue });
        if (property.initialization == PropertyInitialization::StoredLate)
            continue;
        if (!placeholder)
            placeholder = emitUndefined();
        properties.append(FieldValue { field, placeholder });
    }
    Vector<FieldValue, 4> parts;
    for (const FreshResultField& part : variant.parts)
        parts.append(FieldValue { part.field, valueOfField(object, *family.findField(part.field)) });

    Value allocation = makeAllocation(program(), AllocationRequest { variant.description, variant.prototype, properties.span(), parts.span(), variant.length, absentResult == AbsentResult::IsPossible });
    if (allocation.opcode == Opcode::NewObject) {
        bool mayListPlaceholders = program().at(allocation.layout()).kind != LayoutKind::Record;
        allocation.setListsOnlyStoredValues(mayListPlaceholders && !placeholder && storedByName.isEmpty() && hasEveryFieldOfBirthStored(m_sinking.shapeContext(object), shape));
    }
    ValueID cell = cursor().emit(WTF::move(allocation));
    for (const FieldValue& stored : storedByName)
        cursor().emit(makeStoreByName(stored, cell));
    if (m_sinking.at(object).needsGuard) {
        Value guard(Opcode::AllGuards);
        guard.setGuardsOperands(singleElementSpan(cell));
        ValueID cellGuard = cursor().emit(WTF::move(guard));
        values().ensure(object).guard = cellGuard;
    }
    return cell;
}

void ObjectInterpreter::buildByTag(VirtualObjectID object, Vector<LateFieldStore, 2>& late, const BuildCause& cause)
{
    struct MemberOfArms {
        VirtualObjectID member;
        ObjectState state;
        ObjectValues values;
    };

    IRBuilder& builder = cursor().builder();
    const ObjectState& state = *stateOf(object);
    Vector<HeldObject, 2> heldObjectsOfArms;
    Vector<MemberOfArms, 2> membersOfArms;
    for (const HeldObject& held : state.heldObjects) {
        if (stateOf(held.object)->isCell() || !isBuiltInArmsOf(object, held))
            continue;
        heldObjectsOfArms.append(held);
        for (VirtualObjectID member : collectVirtualMembers(held.object)) {
            if (membersOfArms.containsIf([&](const MemberOfArms& known) { return known.member == member; }))
                continue;
            const ObjectValues* carried = values().find(member);
            membersOfArms.append(MemberOfArms { member, *stateOf(member), carried ? *carried : ObjectValues { } });
        }
    }
    bool needsGuard = m_sinking.at(object).needsGuard;
    size_t numberOfLateStores = late.size();
    ValueID carriedTag = values().find(object)->tag;
    ValueID carriedCell = values().find(object)->cell;
    ValueID carriedGuard = values().find(object)->guard;
    RELEASE_ASSERT(carriedTag && (carriedCell || !state.hasCell()));

    ShapeContext context = m_sinking.shapeContext(object);
    TypeID type = TypeTable::never;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = shapeOf(object, variant);
        type = builder.types().unionOf(type, shape.kind == VirtualShapeKind::Cell ? function().at(carriedCell).type : typeOfShape(context, shape));
    }

    BlockID continuation = cursor().addUncoveredBlock();
    ValueID cellParameter = builder.addParameter(continuation, type);
    ValueID guardParameter = needsGuard ? builder.addParameter(continuation, TypeTable::guard) : ValueID();

    Vector<BlockID, 4> arms;
    Vector<std::pair<int64_t, Target>> cases;
    for (const ObjectVariant& variant : state.variants) {
        RELEASE_ASSERT(variant.tagBit);
        arms.append(cursor().addUncoveredBlock());
        cases.append({ int64_t(1) << *variant.tagBit, Target { arms.last(), { }, { } } });
    }
    Target otherwise = cases.takeLast().second;
    builder.emitSwitch(carriedTag, WTF::move(cases), WTF::move(otherwise));

    for (size_t index = 0; index < arms.size(); ++index) {
        const ObjectVariant& variant = state.variants[index];
        cursor().moveTo(arms[index]);
        for (const MemberOfArms& member : membersOfArms) {
            *stateOf(member.member) = member.state;
            values().ensure(member.member) = member.values;
        }
        for (const HeldObject& held : heldObjectsOfArms) {
            if (hasStored(shapeOf(object, variant), held.field) && !stateOf(held.object)->isCell())
                buildMembers(collectVirtualMembers(held.object).span(), cause);
        }
        Arguments arguments { emitValueOfVariant(object, variant, late) };
        if (needsGuard) {
            ValueID guard;
            switch (shapeOf(object, variant).kind) {
            case VirtualShapeKind::Cell:
                guard = carriedGuard;
                break;
            case VirtualShapeKind::Constant:
                break;
            case VirtualShapeKind::Fields:
                guard = values().find(object)->guard;
                break;
            }
            arguments.append(guard ? guard : cursor().emit(Value(Opcode::AllGuards)));
        }
        builder.emitJump(continuation, WTF::move(arguments));
    }
    RELEASE_ASSERT(late.size() == numberOfLateStores);
    for (const MemberOfArms& member : membersOfArms) {
        m_state.remove(member.member);
        values().remove(member.member);
    }

    cursor().moveTo(continuation);
    ObjectValues& carried = values().ensure(object);
    carried.cell = cellParameter;
    carried.guard = guardParameter;
}

void ObjectInterpreter::emitLateStores(std::span<const LateFieldStore> stores)
{
    for (const LateFieldStore& store : stores) {
        const VirtualObjectDescription& description = m_sinking.descriptionOf(store.holder);
        ValueID holder = values().find(store.holder)->cell;
        ValueID stored = store.held ? cellOf(store.held) : store.value;
        RELEASE_ASSERT(holder && stored);

        const VirtualField& field = m_sinking.familyOf(store.holder).fieldAt(store.field);
        cursor().setOrigin(function().at(description.allocation).origin());
        if (field.kind == VirtualFieldKind::UnprovenNamed) {
            cursor().emit(makeStoreByName(FieldValue { field, stored }, holder));
            continue;
        }
        Value value = makeFieldStore(program(), description, FieldValue { field, stored }, holder, holder);
        if (Value::opcodesWithReceivers.contains(value.opcode))
            setExactReceivers(value, description.layout, cursor().builder().types());
        cursor().setOrigin(function().at(description.allocation).origin());
        cursor().emit(WTF::move(value));
    }
}

void ObjectInterpreter::didBuild(VirtualObjectID object, const BuildCause& cause)
{
    ObjectState& state = *stateOf(object);
    if (m_mode == SinkingMode::Count) {
        const BlockFrequencies& frequencies = m_sinking.analyses().blockFrequencies();
        BlockFrequency frequency = frequencies.classOf(m_state.block);
        if (m_enteredEdge.edge)
            frequency = rarerOf(frequencies.classOf(m_enteredEdge.edge->edge.from), frequencies.classOf(m_enteredEdge.edge->edge));
        bool isBorrowed = isBorrowingCall(cause);
        auto countBuilds = [&](VirtualObjectID built, unsigned numberOfBuilds) {
            Candidate& candidate = m_sinking.at(built);
            candidate.numberOfBuilds += numberOfBuilds;
            if (isRarerThan(frequency, frequencies.classOf(candidate.block)))
                candidate.numberOfRarerBuilds += numberOfBuilds;
            else if (isBorrowed)
                candidate.numberOfBorrowingBuilds += numberOfBuilds;
            if (!candidate.firstCause)
                candidate.firstCause = cause;
        };
        unsigned numberOfAllocations = 0;
        for (const ObjectVariant& variant : state.variants) {
            const VirtualShape& shape = shapeOf(object, variant);
            if (shape.kind != VirtualShapeKind::Fields)
                continue;
            ++numberOfAllocations;
            VirtualObjectID allocation = m_sinking.candidateDefinedBy(m_sinking.familyOf(object).descriptionOf(shape).allocation);
            if (allocation && allocation != object)
                countBuilds(allocation, 1);
        }
        countBuilds(object, numberOfAllocations);
    }

    state.becomeCell();
    stateDidChange(object);
    if (!isRewriting())
        return;

    ObjectValues& carried = values().ensure(object);
    carried.tag = { };
    carried.fields.clear();
}

ValueID ObjectInterpreter::resolve(ValueID value)
{
    VirtualObjectID object = objectOf(value);
    if (!object)
        return value;

    ValueID cell = cellOf(object);
    if (m_enteredEdge.edge && m_enteredEdge.edge->edge.isHandlerEdge())
        return cell;
    TypeID type = function().at(value).type;
    if (!type || function().at(cell).type == type || program().typeOf(function(), cell).isSubsetOf(program().at(type), program()))
        return cell;
    return cursor().emit(makeAssumeType(cell, type));
}

ValueID ObjectInterpreter::cellOf(VirtualObjectID object)
{
    const ObjectState& state = *stateOf(object);
    RELEASE_ASSERT(state.isCell());
    ValueID cell = values().find(object)->cell;
    RELEASE_ASSERT(cell);

    // A handler edge carries no value of the block it leaves, so it has no place for a cast.
    if (m_enteredEdge.edge && m_enteredEdge.edge->edge.isHandlerEdge())
        return cell;

    for (TypeID assumedType : state.assumedTypes) {
        const Type& type = program().typeOf(function(), cell);
        if (type.isSubsetOf(program().at(assumedType), program()))
            continue;
        if (!type.castTo(program().at(assumedType), cursor().builder().types()).isSubsetOf(type, program()))
            continue;
        if (m_sinking.context().build.validates() && function().at(cell).opcode != Opcode::Parameter)
            cursor().emit(makeValidateType(cell, assumedType));
        cell = cursor().emit(makeAssumeType(cell, assumedType));
    }
    return cell;
}

ValueID ObjectInterpreter::resolveGuard(ValueID guard)
{
    VirtualObjectID object = guardedObjectOf(guard);
    if (!object)
        return guard;

    RELEASE_ASSERT(stateOf(object)->isCell());
    ValueID cellGuard = values().find(object)->guard;
    RELEASE_ASSERT(cellGuard);
    return cellGuard;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
