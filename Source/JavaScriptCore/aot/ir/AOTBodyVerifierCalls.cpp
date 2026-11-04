/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBodyVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTSort.h"
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <span>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// InternalErrorKind::InvalidModuleEvaluation reports the number.
enum class EvaluationSiteRule : uint32_t {
    TargetIsFileThatEndsAtOnce = 1,
    ReferrerRequestsTarget,
    BareSiteIsInBodyOfReferrer,
    BareSiteIsInBodyThatNeverWaits,
    BareSiteHasNoHandler,
    BareSiteRunsOnce,
    BareSitesRunInOneOrder,
    EveryRunThatEndsPassesBareSite,
};

} // anonymous namespace

// Terminates: the flow stage has checked that every operand is defined before its user.
static BodyValue stripCastsForCallRules(const BodyIndex& index, ValueID id)
{
    BodyValue value = index.value(id);
    while (inputOperationsThatHandOn.contains(value.opcode()))
        value = index.value(value.operand(0));
    return value;
}

// The front end's link step does not report a record that a file names and no committed file defines. Such a name reads as the null id.
template<typename IDType>
static bool requireRecordForCallRules(BodyVerifier& verifier, const BodyBlock& block, const BodyValue& value, IDType id)
{
    if (id) [[likely]]
        return true;
    verifier.fail(InternalErrorKind::IdOutOfRange, block.id(), value.id(), verifier.id().index());
    return false;
}

// A signature is a type, and an initializer is only ever called directly.
static bool canBeNamedByClosureOperation(const Function& function)
{
    return hasSourceBodyOf(function.role) && function.kind != FunctionKind::Initializer;
}

static bool isLibraryFunctionListedByProgram(const Program& program, FunctionID function)
{
    return program.at(function).role == FunctionRole::Library
        && std::ranges::contains(program.runtimeFunctions, function, &RuntimeFunctionEntry::function);
}

static bool isAllocationOfConstructedClass(const Program& program, const Function& constructor, const BodyIndex& index, ValueID value)
{
    BodyValue allocation = stripCastsForCallRules(index, value);
    if (allocation.opcode() != Opcode::NewObject)
        return false;
    LayoutID layout = allocation.as<Opcode::NewObject>().layout();
    return layout && declaredLayoutOf(program, layout) == constructor.constructedClass;
}

static bool mergesAllocationsOfConstructedClass(const Program& program, const Function& constructor, const BodyIndex& index, ValueID merged)
{
    BlockID join = index.blockOf(merged);
    if (!index.isParameter(merged) || join == entryBlock)
        return false;
    uint32_t place = merged.index() - index.block(join).firstParameter().index();

    bool mergesOnlyAllocations = true;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        index.block(BlockID(number)).forEachOutgoingEdge([&](BodyTarget target, bool) {
            if (target.block() != join)
                return;
            std::span<const ValueID> arguments = target.arguments();
            mergesOnlyAllocations &= place < arguments.size() && isAllocationOfConstructedClass(program, constructor, index, arguments[place]);
        });
    }
    return mergesOnlyAllocations;
}

// An initializer assumes without a test that its `this` is an object.
static bool isLegalThisOfInitializer(const Program& program, const Function& caller, const BodyIndex& index, ValueID thisValue)
{
    BodyValue object = stripCastsForCallRules(index, thisValue);
    Opcode opcode = object.opcode();
    if (opcode == Opcode::NewClass)
        return true;
    if (opcode == Opcode::SuperConstruct || opcode == Opcode::SuperConstructList)
        return caller.kind == FunctionKind::DerivedConstructor || caller.kind == FunctionKind::Arrow;
    if (caller.kind != FunctionKind::BaseConstructor)
        return false;
    return opcode == Opcode::NewObject || mergesAllocationsOfConstructedClass(program, caller, index, object.id());
}

template<Opcode opcode>
static FieldReference privateFieldNamedBy(const BodyValue& value)
{
    auto access = value.as<opcode>();
    return { access.layout(), access.field() };
}

static bool isPrivateFieldOfDeclaringClass(const Program& program, FieldReference reference)
{
    const Field& field = fieldAt(program, reference);
    if (!field.flags.contains(FieldFlags::IsPrivate) || !field.key)
        return false;
    const PropertyKey& key = program.at(field.key);
    return key.kind == PropertyKeyKind::PrivateName && key.owner == declarationOf(program, reference).layout;
}

static bool mayBeNamedByBuiltinConstant(const BuiltinTable& table, Opcode opcode, BuiltinID builtin)
{
    const Builtin& entry = table.at(builtin);
    if (isObjectKind(entry.kind))
        return true;
    if (opcode == Opcode::Builtin)
        return entry.kind == BuiltinKind::Constant && table.nameOfSymbol(builtin);
    return entry.kind == BuiltinKind::DataProperty && table.at(table.rootOf(builtin)).scope == BuiltinScope::Module;
}

// An EvalScope names no function. Its flags say where the text is evaluated: in an arrow function they are IsInArrowFunction and the flags of the enclosing function.
static bool mayHoldEvalScope(FunctionKind kind, OptionSet<EvalScopeFlags> flags)
{
    bool isArrow = kind == FunctionKind::Arrow;
    if (flags.contains(EvalScopeFlags::IsInArrowFunction) != isArrow)
        return false;
    if (isArrow)
        return true;
    if (flags.contains(EvalScopeFlags::IsInDerivedConstructor))
        return kind == FunctionKind::DerivedConstructor;
    // A super call in evaluated text calls the instance initializer by value, and an initializer that is called by value is a method.
    if (flags.contains(EvalScopeFlags::IsInFieldInitializer))
        return kind == FunctionKind::Initializer || kind == FunctionKind::Method;
    return true;
}

static bool canBeEvaluatedBySite(const Module& target)
{
    bool isFile = target.kind == ModuleKind::SourceText || target.kind == ModuleKind::CommonJs;
    return isFile && !target.flags.containsAny({ ModuleFlags::IsLibrary, ModuleFlags::HasTopLevelAwait });
}

void BodyVerifier::verifyClosureOperation(const BodyBlock& block, const BodyValue& value)
{
    FunctionID named = value.function();
    if (!requireRecordForCallRules(*this, block, value, named))
        return;
    const Function& callee = program().at(named);
    if (!canBeNamedByClosureOperation(callee)) {
        fail(InternalErrorKind::PayloadOutOfRange, block.id(), value.id());
        return;
    }

    Opcode opcode = value.opcode();
    if (opcode == Opcode::NewClosure) {
        // Operand i fills capture i.
        size_t numberOfCaptures = value.as<Opcode::NewClosure>().capturesOperands().size();
        if (numberOfCaptures != callee.captures.size())
            fail(InternalErrorKind::ArgumentCountMismatch, block.id(), value.id(), static_cast<uint32_t>(numberOfCaptures));
        return;
    }
    if (opcode == Opcode::GetCapture) {
        uint32_t capture = value.as<Opcode::GetCapture>().index();
        if (capture >= callee.captures.size())
            fail(InternalErrorKind::PayloadOutOfRange, block.id(), value.id(), capture);
        return;
    }
    ASSERT(opcode == Opcode::IsClosureOf);
}

void BodyVerifier::verifyDirectCall(const BodyBlock& block, const BodyValue& value)
{
    auto call = value.as<Opcode::CallDirect>();
    FunctionID named = call.function();
    if (!requireRecordForCallRules(*this, block, value, named))
        return;
    const Function& callee = program().at(named);
    if (!callee.hasBody() && !isLibraryFunctionListedByProgram(program(), named)) {
        fail(InternalErrorKind::CallToFunctionWithoutCode, block.id(), value.id());
        return;
    }

    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.size() != callee.numberOfEntryParameters()) {
        fail(InternalErrorKind::ArgumentCountMismatch, block.id(), value.id(), static_cast<uint32_t>(arguments.size()));
        return;
    }

    if (callee.kind != FunctionKind::Initializer)
        return;
    std::optional<unsigned> thisParameter = callee.thisParameterIndex();
    if (!thisParameter) {
        fail(InternalErrorKind::OperandTypeMismatch, block.id(), value.id());
        return;
    }
    ValueID thisValue = arguments[*thisParameter];
    if (!isLegalThisOfInitializer(program(), function(), index(), thisValue)) {
        fail(InternalErrorKind::OperandTypeMismatch, block.id(), value.id(), thisValue.index());
        return;
    }

    // `new.target` may select another class, and a parent may return any object. An established position is entered without a test, so of what a constructor has made no more is laid down than that it is an object.
    const DeclaredType* declaredThis = callee.declaredEntryParameter(*thisParameter);
    if (!declaredThis || !declaredThis->isEstablished() || stripCastsForCallRules(index(), thisValue).opcode() == Opcode::NewClass)
        return;
    if (!TypeReader(program()).at(declaredThis->written()).anyObject)
        fail(InternalErrorKind::StoredTypeMismatch, block.id(), value.id(), thisValue.index());
}

void BodyVerifier::verifyArgumentsOperation(const BodyBlock& block, const BodyValue& value)
{
    // A closure reads and writes the enclosing function's parameters through that function's arguments object, so GetMappedArgument and SetMappedArgument may appear in any function.
    Opcode opcode = value.opcode();
    bool isMapped = opcode == Opcode::CreateMappedArguments;
    if (!isMapped && opcode != Opcode::CreateUnmappedArguments)
        return;

    bool takesEveryArgument = function().has(FunctionFlags::HasArgumentCount) && function().hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest });
    if (!takesEveryArgument || (isMapped && !function().has(FunctionFlags::TakesCallee))) {
        fail(InternalErrorKind::FunctionFlagMissing, block.id(), value.id());
        return;
    }

    // The rest list starts after the last positional parameter, so the operation lists every positional parameter.
    size_t numberOfListedParameters = value.variadicOperands().size();
    if (numberOfListedParameters + function().has(FunctionFlags::HasRestParameter) != function().parameters.size())
        fail(InternalErrorKind::ArgumentCountMismatch, block.id(), value.id(), static_cast<uint32_t>(numberOfListedParameters));
}

void BodyVerifier::verifyClassOperation(const BodyBlock& block, const BodyValue& value)
{
    LayoutID named = value.layout();
    if (!requireRecordForCallRules(*this, block, value, named))
        return;
    const Layout& layout = program().at(named);
    bool hasHeritage = layout.has(LayoutFlags::HasHeritage);
    if (value.opcode() != Opcode::NewClass) {
        ASSERT(value.opcode() == Opcode::SuperConstruct || value.opcode() == Opcode::SuperConstructList);
        if (!hasHeritage)
            fail(InternalErrorKind::PayloadOutOfRange, block.id(), value.id());
        return;
    }

    auto definition = value.as<Opcode::NewClass>();
    for (ValueID operand : { definition.heritageOperand(), definition.prototypeParentOperand() }) {
        if (!!operand == hasHeritage)
            continue;
        fail(hasHeritage ? InternalErrorKind::OperandMissing : InternalErrorKind::UnexpectedOperand, block.id(), value.id());
        return;
    }

    ValueID constructor = definition.constructorOperand();
    BodyValue closure = stripCastsForCallRules(index(), constructor);
    if (closure.opcode() != Opcode::NewClosure || closure.as<Opcode::NewClosure>().function() != layout.constructor)
        fail(InternalErrorKind::OperandTypeMismatch, block.id(), value.id(), constructor.index());
}

void BodyVerifier::verifyPrivateAccess(const BodyBlock& block, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    FieldReference reference = opcode == Opcode::PrivateGet ? privateFieldNamedBy<Opcode::PrivateGet>(value)
        : opcode == Opcode::PrivateSet ? privateFieldNamedBy<Opcode::PrivateSet>(value)
        : opcode == Opcode::PrivateDefine ? privateFieldNamedBy<Opcode::PrivateDefine>(value)
        : privateFieldNamedBy<Opcode::PrivateHas>(value);
    if (!requireRecordForCallRules(*this, block, value, reference.layout))
        return;
    if (!isValidField(reference.layout, reference.field) || !isPrivateFieldOfDeclaringClass(program(), reference))
        fail(InternalErrorKind::PayloadOutOfRange, block.id(), value.id(), reference.field);
}

void BodyVerifier::verifySuspension(const BodyBlock& block, const BodyValue& value)
{
    bool isValid = false;
    Opcode opcode = value.opcode();
    if (opcode == Opcode::Await)
        isValid = function().has(FunctionFlags::IsAsync);
    else if (opcode == Opcode::Yield) {
        m_yields.append(value.id());
        isValid = function().has(FunctionFlags::IsGenerator);
    } else {
        ASSERT(opcode == Opcode::InitialYield);
        isValid = function().has(FunctionFlags::IsGenerator) && !m_initialYield;
        m_initialYield = value.id();
    }
    if (!isValid)
        fail(InternalErrorKind::InvalidSuspension, block.id(), value.id());
}

// Code before InitialYield runs when the generator function is called, before there is a generator to yield from.
void BodyVerifier::verifyGeneratorStart()
{
    if (hasFailed() || !function().has(FunctionFlags::IsGenerator))
        return;
    if (!m_initialYield) {
        fail(InternalErrorKind::InvalidSuspension);
        return;
    }

    BlockID blockOfInitialYield = index().blockOf(m_initialYield);
    for (ValueID yield : m_yields) {
        BlockID block = index().blockOf(yield);
        bool followsInitialYield = block == blockOfInitialYield ? m_initialYield.index() < yield.index() : terminatorDominates(blockOfInitialYield, block);
        if (!followsInitialYield) {
            fail(InternalErrorKind::InvalidSuspension, block, yield);
            return;
        }
    }
}

void BodyVerifier::verifyBuiltinConstant(const BodyBlock& block, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    BuiltinID builtin;
    if (opcode == Opcode::ModuleBuiltin) {
        auto constant = value.as<Opcode::ModuleBuiltin>();
        if (!requireRecordForCallRules(*this, block, value, constant.module()))
            return;
        builtin = constant.builtin();
    } else
        builtin = value.as<Opcode::Builtin>().builtin();

    const BuiltinTable& table = program().builtins();
    if (!builtin || !table.contains(builtin)) {
        fail(InternalErrorKind::IdOutOfRange, block.id(), value.id(), id().index());
        return;
    }
    if (!mayBeNamedByBuiltinConstant(table, opcode, builtin))
        fail(InternalErrorKind::InvalidBuiltinCall, block.id(), value.id());
}

void BodyVerifier::verifyDirectEval(const BodyBlock& block, const BodyValue& value)
{
    auto evaluation = value.as<Opcode::DirectEval>();
    EvalScopeID named = evaluation.evalScope();
    if (!requireRecordForCallRules(*this, block, value, named))
        return;
    const EvalScope& scope = program().at(named);
    if (!mayHoldEvalScope(function().kind, scope.flags)) {
        fail(InternalErrorKind::PayloadOutOfRange, block.id(), value.id(), named.index());
        return;
    }
    if (scope.flags.contains(EvalScopeFlags::IsInFunction) && !evaluation.newTargetOperand()) {
        fail(InternalErrorKind::OperandMissing, block.id(), value.id());
        return;
    }

    size_t numberOfOperands = evaluation.environmentsOperands().size();
    for (const EvalBinding& binding : scope.bindings) {
        for (std::optional<uint32_t> operandIndex : { binding.operand, binding.function, binding.setter }) {
            if (operandIndex && *operandIndex >= numberOfOperands) {
                fail(InternalErrorKind::PayloadOutOfRange, block.id(), value.id(), *operandIndex);
                return;
            }
        }
        if (!binding.layout)
            continue;
        if (!binding.operand) {
            fail(InternalErrorKind::PayloadOutOfRange, block.id(), value.id());
            return;
        }
        if (binding.field && !isValidField(binding.layout, *binding.field)) {
            fail(InternalErrorKind::PayloadOutOfRange, block.id(), value.id(), *binding.field);
            return;
        }
    }
}

void BodyVerifier::verifyModuleEvaluation(const BodyBlock& block, const BodyValue& value)
{
    auto failRule = [&](EvaluationSiteRule rule) {
        fail(InternalErrorKind::InvalidModuleEvaluation, block.id(), value.id(), static_cast<uint32_t>(rule));
    };

    ModuleID target = value.module();
    ModuleID referrer = value.referrer();
    if (!requireRecordForCallRules(*this, block, value, target) || !requireRecordForCallRules(*this, block, value, referrer))
        return;
    if (!canBeEvaluatedBySite(program().at(target)))
        return failRule(EvaluationSiteRule::TargetIsFileThatEndsAtOnce);

    bool isBare = value.opcode() == Opcode::EvaluateModule && value.as<Opcode::EvaluateModule>().isBare();
    m_moduleEvaluations.append(ModuleEvaluationInBody { referrer, target, block.id(), value.id(), isBare });
    if (!isBare)
        return;
    if (program().at(referrer).body != id())
        return failRule(EvaluationSiteRule::BareSiteIsInBodyOfReferrer);
    if (function().has(FunctionFlags::IsAsync))
        return failRule(EvaluationSiteRule::BareSiteIsInBodyThatNeverWaits);
    if (block.hasHandler())
        return failRule(EvaluationSiteRule::BareSiteHasNoHandler);
    if (index().loopOf(block.id()))
        failRule(EvaluationSiteRule::BareSiteRunsOnce);
}

// The sites were gathered in stream order, in which a block comes after every block that dominates it.
// The block of a bare site has no handler, so control that leaves it for another block has passed the site.
void BodyVerifier::verifyBareEvaluations()
{
    if (hasFailed())
        return;

    const ModuleEvaluationInBody* last = nullptr;
    for (const ModuleEvaluationInBody& site : m_moduleEvaluations) {
        if (!site.isBare)
            continue;
        if (last && !index().dominates(last->block, site.block))
            return fail(InternalErrorKind::InvalidModuleEvaluation, site.block, site.value, static_cast<uint32_t>(EvaluationSiteRule::BareSitesRunInOneOrder));
        last = &site;
    }
    if (!last)
        return;

    for (uint32_t number = 0; number < index().numberOfBlocks(); ++number) {
        BlockID block(number);
        TerminatorKind kind = index().terminator(block).kind();
        bool returns = kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall;
        if (returns && !index().dominates(last->block, block))
            return fail(InternalErrorKind::InvalidModuleEvaluation, last->block, last->value, static_cast<uint32_t>(EvaluationSiteRule::EveryRunThatEndsPassesBareSite));
    }
}

void BodyVerifier::verifyRequestsOfEvaluations()
{
    if (hasFailed() || m_moduleEvaluations.isEmpty())
        return;

    AOT::sort(m_moduleEvaluations, { }, [](const ModuleEvaluationInBody& site) {
        return std::tuple { site.referrer, site.target, site.value };
    });
    ModuleID referrer;
    Vector<ModuleID> requiredModules;
    for (const ModuleEvaluationInBody& site : m_moduleEvaluations) {
        if (site.referrer != referrer) {
            referrer = site.referrer;
            requiredModules.shrink(0);
            for (const ModuleRequest& request : program().at(referrer).requests) {
                if (request.kind == ModuleRequestKind::Require)
                    requiredModules.append(request.target);
            }
            AOT::sort(requiredModules);
        }
        if (!std::ranges::binary_search(requiredModules, site.target))
            return fail(InternalErrorKind::InvalidModuleEvaluation, site.block, site.value, static_cast<uint32_t>(EvaluationSiteRule::ReferrerRequestsTarget));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
