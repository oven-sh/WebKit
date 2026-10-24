/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTPropertyAccessReceivers.h"
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static bool isFunctionAsRead(const Program& program, FunctionID function)
{
    return function.index() < program.numberOfFunctionsAsRead();
}

static bool isEntrypoint(FunctionRole role)
{
    return role == FunctionRole::GenericEntrypoint || role == FunctionRole::TableEntrypoint || role == FunctionRole::TableLookup;
}

static bool isGeneratorRamp(const Function& function, CoroutineKind kind)
{
    auto* record = function.creator.tryAs<CreatorRecord::Coroutine>();
    return function.role == FunctionRole::CoroutineRamp && record && record->kind == kind;
}

static bool isMergedClassAllocation(const Program& program, const Function& constructor, FunctionAnalyses& analyses, ValueID object)
{
    ValueLocation location = analyses.useLists().locationOf(object);
    if (location.placement != ValuePlacement::Parameter || location.block == entryBlock)
        return false;
    return std::ranges::all_of(analyses.cfg().incomingEdges(location.block), [&](Edge edge) {
        ValueID incoming = valuePassedOn(constructor, edge, location.index);
        if (!incoming)
            return false;
        const Value& allocation = constructor.at(analyses.sameObjectThroughCasts(incoming));
        return allocation.opcode == Opcode::NewObject && declaredLayoutOf(program, allocation.layout()) == constructor.constructedClass;
    });
}

static bool isLegalInitializerThis(const Program& program, const Function& caller, FunctionAnalyses& analyses, ValueID object)
{
    Opcode opcode = caller.at(object).opcode;
    if (opcode == Opcode::NewClass || opcode == Opcode::FinishClass)
        return true;
    if (opcode == Opcode::SuperConstruct || opcode == Opcode::SuperConstructList)
        return caller.kind == FunctionKind::DerivedConstructor || caller.kind == FunctionKind::Arrow;
    if (caller.kind != FunctionKind::BaseConstructor)
        return false;
    return opcode == Opcode::NewObject || isMergedClassAllocation(program, caller, analyses, object);
}

static bool isPrivateField(const Program& program, FieldReference reference)
{
    const Field& field = fieldAt(program, reference);
    if (!field.flags.contains(FieldFlags::IsPrivate) || !field.key)
        return false;
    const PropertyKey& key = program.at(field.key);
    return key.kind == PropertyKeyKind::PrivateName && key.owner == declarationOf(program, reference).layout;
}

static bool isLegalBuiltinConstant(const BuiltinTable& table, const Value& constant)
{
    const Builtin& entry = table.at(constant.builtin());
    if (isObjectKind(entry.kind))
        return true;
    if (constant.opcode == Opcode::Builtin)
        return entry.kind == BuiltinKind::Constant && table.nameOfSymbol(constant.builtin());
    return entry.kind == BuiltinKind::DataProperty
        && table.at(table.rootOf(constant.builtin())).scope == BuiltinScope::Module;
}

static ValueID closureEnvironmentOperand(const Program& program, const Value& allocation)
{
    std::optional<uint32_t> field = program.at(allocation.layout()).flow.closureEnvironmentField;
    if (!field)
        return { };
    std::span<const uint32_t> fields = allocation.fields();
    std::span<const ValueID> values = allocation.valuesOperands();
    size_t index = std::ranges::find(fields, *field) - fields.begin();
    return index < fields.size() && index < values.size() ? values[index] : ValueID();
}

static bool isSoleCapture(const TypingContext& context, const Function& callee, ValueID captured)
{
    return captured && callee.captures.size() == 1 && context.typeOf(captured).isSubsetOf(context.typeOf(callee.captures[0]), context.program);
}

// The object that the store names is what the closure captures, so a closure over an environment comes into its field with an allocation only.
static bool isStoredIntoDeferringFields(const TypingContext& context, const Value& store, uint32_t operandIndex, FunctionID closureFunction)
{
    const Program& program = context.program;
    if ((store.opcode != Opcode::PutSlot && store.opcode != Opcode::PutSlotOrAdd) || operandIndex != OperandIndex::PutSlot::value)
        return false;
    const Function& callee = program.at(closureFunction);
    auto& members = program.at(store.receivers()).members;
    return !members.isEmpty() && std::ranges::all_of(members, [&](LayoutID member) {
        std::optional<uint32_t> field = program.at(member).findField(store.key());
        if (!field)
            return false;
        const LocationFlow& location = flowOf(program, { member, *field }).location;
        if (location.deferredFunction != closureFunction)
            return false;
        if (location.deferral == ClosureDeferral::CapturesNothing)
            return callee.captures.isEmpty();
        return location.deferral == ClosureDeferral::CapturesObject && isSoleCapture(context, callee, store.objectOperand());
    });
}

static bool isValueOfDeferringField(const TypingContext& context, const UseLists& uses, ValueID closure)
{
    const Program& program = context.program;
    const Function& function = context.function;
    std::optional<Use> use = uses.singleUse(closure);
    while (use && use->kind == Use::Kind::Operand && function.at(use->user).opcode == Opcode::Upcast)
        use = uses.singleUse(use->user);
    if (!use || use->kind != Use::Kind::Operand)
        return false;
    if (function.at(use->user).opcode != Opcode::NewObject)
        return isStoredIntoDeferringFields(context, function.at(use->user), use->operandIndex, function.at(closure).function());

    const Value& allocation = function.at(use->user);
    std::optional<uint32_t> index = allocation.variadicOperandIndex(use->operandIndex);
    if (!index || *index >= allocation.fields().size() || allocation.fields()[*index] >= program.at(allocation.layout()).fields.size())
        return false;

    FunctionID closureFunction = function.at(closure).function();
    const Function& callee = program.at(closureFunction);
    const LocationFlow& location = flowOf(program, { allocation.layout(), allocation.fields()[*index] }).location;
    if (location.deferredFunction != closureFunction)
        return false;
    switch (location.deferral) {
    case ClosureDeferral::Unknown:
    case ClosureDeferral::FlattenedObject:
        return false;
    case ClosureDeferral::CapturesNothing:
        return callee.captures.isEmpty();
    case ClosureDeferral::CapturesObject:
        return isSoleCapture(context, callee, use->user);
    case ClosureDeferral::CapturesEnvironment:
        return isSoleCapture(context, callee, closureEnvironmentOperand(program, allocation));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FunctionVerifier::verifyClosureOperation(BlockID block, ValueID id, const Value& value)
{
    const Function& callee = m_program.at(value.function());
    switch (value.opcode) {
    case Opcode::NewClosure:
        if (isAsRead(*this) && callee.kind == FunctionKind::Initializer)
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
        else if (value.isMadeOnDemand() && !isValueOfDeferringField(typing(), analyses().useLists(), id))
            fail(InternalErrorKind::ClosureOnDemandWithoutField, block, id);
        return;
    case Opcode::GetCapture: {
        if (value.index() >= callee.captures.size()) {
            fail(InternalErrorKind::PayloadOutOfRange, block, id, value.index());
            return;
        }
        ValueID closure = value.closureOperand();
        if (typeOf(closure).singleFunction() != value.function())
            fail(InternalErrorKind::OperandTypeMismatch, block, id, closure.index());
        return;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void FunctionVerifier::verifyCallByValue(BlockID block, ValueID id, const Value& value)
{
    if (m_form == IRForm::AsRead || isOptimizing())
        return;
    if (isCallThatRequiredPassRemoves(m_program, value))
        fail(InternalErrorKind::RequiredPassLeftOperation, block, id);
}

static bool asksOnlyForRowsOf(const Program& program, const Type& receiver, LayoutSetID rows)
{
    ReceiverCandidates candidates = candidatesOf(program, receiver, nullptr, { });
    if (candidates.proof != ReceiverProof::Type || candidates.hasOtherParts || candidates.mayBeNullish)
        return false;
    const auto& members = program.at(rows).members;
    return std::ranges::all_of(candidates.proven, [&](LayoutID layout) {
        return std::ranges::binary_search(members, layout);
    });
}

void FunctionVerifier::verifyDirectCall(BlockID block, ValueID id, const Value& value)
{
    const Function& callee = m_program.at(value.function());
    if (!callee.hasBody() && !isFunctionAsRead(m_program, value.function())) {
        fail(InternalErrorKind::InvalidCreatorRecord, block, id);
        return;
    }

    std::span<const ValueID> arguments = value.argumentsOperands();
    switch (value.opcode) {
    case Opcode::CallDirect: {
        if (isEntrypoint(callee.role)) {
            fail(InternalErrorKind::InvalidCreatorRecord, block, id);
            return;
        }
        if (!isAsRead(*this) || callee.kind != FunctionKind::Initializer)
            return;
        std::optional<unsigned> thisParameter = callee.thisParameterIndex();
        if (!thisParameter) {
            fail(InternalErrorKind::OperandTypeMismatch, block, id);
            return;
        }
        ValueID thisValue = arguments[*thisParameter];
        if (!isLegalInitializerThis(m_program, m_function, analyses(), analyses().sameObjectThroughCasts(thisValue)))
            fail(InternalErrorKind::OperandTypeMismatch, block, id, thisValue.index());
        return;
    }
    case Opcode::CallTable: {
        const auto* lookup = callee.creator.tryAs<CreatorRecord::TableLookup>();
        if (callee.role != FunctionRole::TableLookup || !lookup || lookup->key != value.key()) {
            fail(InternalErrorKind::InvalidCreatorRecord, block, id);
            return;
        }
        // A pair of a layout and a selector that does not exist may lie over the entry of another pair.
        if (arguments.empty() || !asksOnlyForRowsOf(m_program, typeOf(arguments[0]), value.receivers()))
            fail(InternalErrorKind::OperandTypeMismatch, block, id);
        return;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void FunctionVerifier::verifyArgumentsOperation(BlockID block, ValueID id, const Value&)
{
    auto* entrypoint = m_function.creator.tryAs<CreatorRecord::Entrypoint>();
    if (!entrypoint || entrypoint->arguments != ArgumentPassing::Buffer)
        fail(InternalErrorKind::InvalidCreatorRecord, block, id);
}

void FunctionVerifier::verifyClassOperation(BlockID block, ValueID id, const Value& value)
{
    const Layout& layout = m_program.at(value.layout());
    bool hasHeritage = layout.has(LayoutFlags::HasHeritage);
    switch (value.opcode) {
    case Opcode::SuperConstruct:
    case Opcode::SuperConstructList:
        if (!hasHeritage)
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    case Opcode::NewClass:
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    for (ValueID operand : { value.heritageOperand(), value.prototypeParentOperand() }) {
        if (!!operand == hasHeritage)
            continue;
        fail(hasHeritage ? InternalErrorKind::OperandMissing : InternalErrorKind::UnexpectedOperand, block, id);
        return;
    }

    if (!isAsRead(*this))
        return;
    ValueID constructor = value.constructorOperand();
    const Value& closure = m_function.at(analyses().definitionThroughUpcasts(constructor));
    if (closure.opcode != Opcode::NewClosure || closure.function() != layout.constructor)
        fail(InternalErrorKind::OperandTypeMismatch, block, id, constructor.index());
}

void FunctionVerifier::verifyPrivateAccess(BlockID block, ValueID id, const Value& value)
{
    if (!isPrivateField(m_program, { value.layout(), value.field() }))
        fail(InternalErrorKind::PayloadOutOfRange, block, id, value.field());
}

void FunctionVerifier::verifySuspension(BlockID block, uint32_t, ValueID id, const Value& value)
{
    bool isValid = true;
    switch (value.opcode) {
    case Opcode::Await:
        isValid = m_function.isUnsplitCoroutine();
        break;
    case Opcode::Yield:
        m_yields.append(id);
        isValid = m_function.isUnsplitCoroutine();
        break;
    case Opcode::InitialYield:
        isValid = m_function.isUnsplitCoroutine() && !m_initialYield;
        m_initialYield = id;
        break;
    case Opcode::StepAsyncGenerator:
        isValid = m_function.isUnsplitCoroutine() || m_function.role == FunctionRole::CoroutineBody;
        break;
    case Opcode::AwaitBySavingFrame:
    case Opcode::AwaitMayContinue:
    case Opcode::YieldBySavingFrame:
    case Opcode::AsyncGeneratorYield:
    case Opcode::TakeGeneratorPrototype:
        isValid = m_function.role == FunctionRole::CoroutineBody;
        break;
    case Opcode::AwaitCallBySavingFrame: {
        auto* callee = m_program.at(value.function()).creator.tryAs<CreatorRecord::Coroutine>();
        isValid = m_function.role == FunctionRole::CoroutineBody && callee && callee->entryWithoutPromise == value.function();
        break;
    }
    case Opcode::ReturnAsyncResult: {
        std::optional<Use> use = analyses().useLists().singleUse(id);
        isValid = use && use->kind == Use::Kind::TerminatorOperand && use->block == block && m_function.at(block).terminator.kind == TerminatorKind::Return;
        break;
    }
    case Opcode::NewGenerator:
        isValid = isGeneratorRamp(m_program.at(value.function()), CoroutineKind::Generator);
        break;
    case Opcode::NewAsyncGenerator:
        isValid = isGeneratorRamp(m_program.at(value.function()), CoroutineKind::AsyncGenerator);
        break;
    default:
        break;
    }
    if (!isValid)
        fail(InternalErrorKind::InvalidSuspension, block, id);
}

void FunctionVerifier::verifyGeneratorStart()
{
    if (!hasSourceBodyOf(m_function.role) || !m_function.has(FunctionFlags::IsGenerator))
        return;
    if (!m_initialYield && isAsRead(*this)) {
        fail(InternalErrorKind::InvalidSuspension);
        return;
    }
    for (ValueID yield : m_yields) {
        ValueLocation location = analyses().useLists().locationOf(yield);
        if (!m_initialYield || !analyses().dominates(m_initialYield, ProgramPoint { location.block, location.index })) {
            fail(InternalErrorKind::InvalidSuspension, location.block, yield);
            return;
        }
    }
}

void FunctionVerifier::verifyBuiltinOperation(BlockID block, ValueID id, const Value& value)
{
    if (value.opcode == Opcode::Builtin || value.opcode == Opcode::ModuleBuiltin) {
        if (!isLegalBuiltinConstant(m_program.builtins(), value))
            fail(InternalErrorKind::InvalidBuiltinCall, block, id);
        return;
    }
    if (value.opcode == Opcode::NativeBinding) {
        // The host evaluates a binding by its number. What native code hands over by itself has none: no text names it.
        const Builtin& entry = m_program.builtins().at(value.builtin());
        if (entry.kind != BuiltinKind::NativeBinding || !entry.isNamedByText)
            fail(InternalErrorKind::InvalidBuiltinCall, block, id);
        return;
    }
    if (!builtinCallOpcodes.contains(value.opcode))
        return;

    if (!isOptimizing() && isCallThatRequiredPassRemoves(m_program, value)) {
        fail(InternalErrorKind::RequiredPassLeftOperation, block, id);
        return;
    }
    if (!overloadTakesOperands(typing(), value))
        fail(InternalErrorKind::InvalidBuiltinCall, block, id);
}

void FunctionVerifier::verifyDirectEval(BlockID block, ValueID id, const Value& value)
{
    std::span<const ValueID> operands = value.environmentsOperands();
    for (const EvalBinding& binding : m_program.at(value.evalScope()).bindings) {
        for (std::optional<uint32_t> operandIndex : { binding.operand, binding.function, binding.setter }) {
            if (operandIndex && *operandIndex >= operands.size()) {
                fail(InternalErrorKind::PayloadOutOfRange, block, id, *operandIndex);
                return;
            }
        }
        if (!binding.layout)
            continue;
        if (!binding.operand) {
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
            return;
        }

        const Layout& layout = m_program.at(binding.layout);
        ValueID holder = operands[*binding.operand];
        LayoutID expectedEnvironment = layout.isEnvironment() ? binding.layout : LayoutID();
        if (typeOf(holder).environment != expectedEnvironment) {
            fail(InternalErrorKind::InvalidEnvironmentUse, block, id, holder.index());
            return;
        }
        if (binding.field && *binding.field >= layout.fields.size()) {
            fail(InternalErrorKind::PayloadOutOfRange, block, id, *binding.field);
            return;
        }
    }
}

void FunctionVerifier::verifyResolvedCall(BlockID block, ValueID id, const Value& value)
{
    const ValueIdentities& identities = analyses().identities();
    ValueID target = value.targetOperand();
    const Value& resolution = m_function.at(identities.identityOf(target));
    bool isResolvedForCall = resolution.opcode == Opcode::ResolveCallTarget
        && identities.areSameValue(resolution.calleeOperand(), value.calleeOperand())
        && resolution.argumentCount() == value.argumentsOperands().size();
    if (!isResolvedForCall)
        fail(InternalErrorKind::ResolvedTargetMismatch, block, id, target.index());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
