/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include <initializer_list>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static bool isAsRead(const FunctionVerifier& verifier)
{
    return verifier.form() == IRForm::AsRead && !verifier.isOptimizing();
}

static bool isFunctionAsRead(const Program& program, FunctionID function)
{
    return function.index() < program.numberOfFunctionsAsRead();
}

static bool isEntrypoint(FunctionRole role)
{
    return role == FunctionRole::GenericEntrypoint || role == FunctionRole::GroupEntrypoint;
}

static bool isGeneratorRamp(const Function& function, CoroutineKind kind)
{
    auto* record = function.creator.tryAs<CreatorRecord::Coroutine>();
    return function.role == FunctionRole::CoroutineRamp && record && record->kind == kind;
}

static bool isLegalInitializerThis(const Function& caller, ValueID object)
{
    Opcode opcode = caller.at(object).opcode;
    if (opcode == Opcode::NewClass || opcode == Opcode::FinishClass)
        return true;
    if (opcode == Opcode::SuperConstruct || opcode == Opcode::SuperConstructList)
        return caller.kind == FunctionKind::DerivedConstructor || caller.kind == FunctionKind::Arrow;
    return opcode == Opcode::NewObject && caller.kind == FunctionKind::BaseConstructor;
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

static bool hasValidationNodes(const Program& program, const Value& call)
{
    ExpectedResultID expectedResult = call.expectedResult();
    return expectedResult && !program.at(expectedResult).validation.isEmpty();
}

// The compiler's helpers and the functions of bun:bundle are in no realm. ResolveBuiltins makes each call of one a direct call of its function of the library.
static bool hasFunctionObject(const BuiltinTable& table, const Value& call)
{
    const Builtin& entry = table.at(table.at(call.overload()).builtin);
    return !entry.flags.contains(BuiltinFlag::IsCompilerHelper) && entry.holder != Builtins::BunBundle;
}

// The run time makes such a closure from the record of the field, over nothing or over the object that has the field.
static bool isInitialValueOfDeferringField(const Program& program, const Function& function, const UseLists& uses, ValueID closure)
{
    std::optional<Use> use = uses.singleUse(closure);
    while (use && use->kind == Use::Kind::Operand && function.at(use->user).opcode == Opcode::Upcast)
        use = uses.singleUse(use->user);
    if (!use || use->kind != Use::Kind::Operand || function.at(use->user).opcode != Opcode::NewObject)
        return false;

    const Value& allocation = function.at(use->user);
    std::optional<uint32_t> index = allocation.variadicOperandIndex(use->operandIndex);
    if (!index || *index >= allocation.fields().size() || allocation.fields()[*index] >= program.at(allocation.layout()).fields.size())
        return false;

    FunctionID closureFunction = function.at(closure).function();
    const LocationFlow& location = flowOf(program, { allocation.layout(), allocation.fields()[*index] }).location;
    ClosureDeferral deferral = program.at(closureFunction).captures.isEmpty() ? ClosureDeferral::CapturesNothing : ClosureDeferral::CapturesObject;
    return location.deferredFunction == closureFunction && location.deferral == deferral;
}

void FunctionVerifier::verifyClosureOperation(BlockID block, ValueID id, const Value& value)
{
    const Function& callee = m_program.at(value.function());
    switch (value.opcode) {
    case Opcode::NewClosure:
        if (isAsRead(*this) && callee.kind == FunctionKind::Initializer)
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
        else if (value.isMadeOnDemand() && !isInitialValueOfDeferringField(m_program, m_function, analyses().useLists(), id))
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

// The types around such an operation name the derived layouts that one version of the callee allocates. A call by value enters the generic version, which allocates others.
static bool restsOnVersionEntered(const Program& program, const Value& call)
{
    if (call.opcode == Opcode::Construct && call.allocatedLayout())
        return true;
    return call.callees() && std::ranges::any_of(program.at(call.callees()).functions, [&](FunctionID callee) {
        return program.at(callee).role == FunctionRole::CallSiteVersion;
    });
}

void FunctionVerifier::verifyCallByValue(BlockID block, ValueID id, const Value& value)
{
    if (m_form == IRForm::AsRead || isOptimizing())
        return;
    if ((value.opcode == Opcode::Call && hasValidationNodes(m_program, value)) || restsOnVersionEntered(m_program, value))
        fail(InternalErrorKind::RequiredPassLeftOperation, block, id);
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
        if (!isLegalInitializerThis(m_function, analyses().sameObjectThroughCasts(thisValue)))
            fail(InternalErrorKind::OperandTypeMismatch, block, id, thisValue.index());
        return;
    }
    case Opcode::CallGroup:
        if (callee.role != FunctionRole::GroupEntrypoint) {
            fail(InternalErrorKind::InvalidCreatorRecord, block, id);
            return;
        }
        if (!callee.numberOfEntryParameters()) {
            fail(InternalErrorKind::ArgumentCountMismatch, block, id, static_cast<uint32_t>(arguments.size()));
            return;
        }
        need(InternalErrorKind::StoredTypeMismatch, block, id, value.closureOperand(), m_program.at(declaredEntryTypeOf(typing(), callee, 0)));
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void FunctionVerifier::verifyArgumentsOperation(BlockID block, ValueID id, const Value& value)
{
    // The return address lies where the code expects it in a frame that is set up at the entry, which a generic convention asks for.
    if (value.opcode == Opcode::ReturnsToResultTest) {
        if (m_function.role != FunctionRole::GenericEntrypoint)
            fail(InternalErrorKind::InvalidCreatorRecord, block, id);
        return;
    }
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
    case Opcode::AwaitBySavingFrame:
    case Opcode::YieldBySavingFrame:
    case Opcode::AsyncGeneratorYield:
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
    if (!builtinCallOpcodes.contains(value.opcode))
        return;

    if (value.opcode == Opcode::CallBuiltin && !isOptimizing() && (hasValidationNodes(m_program, value) || !hasFunctionObject(m_program.builtins(), value))) {
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
