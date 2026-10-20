/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveBuiltinsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTSimplifyBuiltinCallsInternal.h"
#include "AOTTypePropagation.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet callsByValue { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct };

std::optional<TailCallRequirement> tailCallRequirementOf(const Function& function, BlockID block, ValueID call)
{
    const Terminator& terminator = function.at(block).terminator;
    if (terminator.kind != TerminatorKind::ReturnCall || terminator.callOperand() != call)
        return std::nullopt;
    return terminator.requirement();
}

// The code that makes or validates a typed result runs in this frame, after the call or in its place.
static bool endsInThisFrame(const Program& program, const Value& call, const BuiltinUsePlan& plan)
{
    if (plan.call.overload && program.builtins().at(plan.call.overload).hasTypedResult())
        return true;
    if (call.opcode != Opcode::Call)
        return false;
    ExpectedResultID expected = call.expectedResult();
    return expected && !program.at(expected).validation.isEmpty();
}

void demoteTailCall(FunctionPassContext& context, IRBuilder& builder, BlockID block)
{
    builder.demoteReturnCall(block);
    context.build.statistics().count(StatisticID::ReturnCallsDemoted);
}

BuiltinResolver::BuiltinResolver(FunctionPassContext& context)
    : m_context(context)
    , m_function(context.function)
    , m_builder(context.analyses.context(), context.function, context.types, &context.analyses)
{
}

BuiltinUseInputs BuiltinResolver::inputs() const
{
    return { m_context.program, m_function, m_context.types, m_context.inputs.flow, m_context.analyses.effectContext(), &m_context.analyses, { } };
}

Vector<BuiltinSite> BuiltinResolver::collectSites()
{
    BuiltinUseInputs inputs = this->inputs();
    Vector<BuiltinSite> sites;
    forEachValueSite(m_context.analyses, m_function, [&](const ValueSite& site) {
        const Value& value = m_function.at(site.value);
        if (!builtinUseOpcodes.contains(value.opcode))
            return;
        if (callsByValue.contains(value.opcode) && m_context.exceeds(LimitID::MaxBuiltinCallArguments, value.argumentsOperands().size(), site.value))
            return;

        BuiltinUsePlan plan = planBuiltinUse(inputs, site.value, site.point());
        std::optional<TailCallRequirement> tailCall = tailCallRequirementOf(m_function, site.block, site.value);
        if (tailCall == TailCallRequirement::Required && !endsInThisFrame(m_context.program, value, plan))
            return;

        // Rule BuiltinUse rewrites a tested use while insertions are queued, when a tail call can no longer be demoted.
        bool needsDemotionBeforeTests = plan && tailCall;
        if (plan.isProven() || needsDemotionBeforeTests)
            sites.append(BuiltinSite { site.point(), site.value, WTF::move(plan) });
    });
    return sites;
}

void BuiltinResolver::demoteTailCalls(std::span<const BuiltinSite> sites)
{
    for (const BuiltinSite& site : sites) {
        if (!tailCallRequirementOf(m_function, site.point.block, site.id))
            continue;
        demoteTailCall(m_context, m_builder, site.point.block);
        ++m_counts.demotedTailCalls;
    }
}

void BuiltinResolver::apply(const BuiltinSite& site)
{
    m_builder.setOrigin(m_function.at(site.id).origin());
    // ScopedLambda hands its arguments to the functor as lvalues.
    auto insertBeforeSite = [&](auto&& value) {
        return m_builder.insertBefore(site.point, WTF::move(value));
    };
    InsertBeforeCall insert = insertBeforeSite;
    applyBuiltinUse({ m_context, site.id, site.point.block, insert }, site.plan);
    count(site.plan);
}

void BuiltinResolver::count(const BuiltinUsePlan& plan)
{
    switch (plan.kind) {
    case BuiltinUseKind::None:
        RELEASE_ASSERT_NOT_REACHED();
    case BuiltinUseKind::Constant:
        ++m_counts.reads;
        return;
    case BuiltinUseKind::NewArray:
        ++m_counts.calls;
        return;
    case BuiltinUseKind::Call:
    case BuiltinUseKind::UnmatchedCall:
    case BuiltinUseKind::LibraryCall:
        break;
    }

    switch (m_context.program.builtins().at(plan.call.overload).mode) {
    case BuiltinCallMode::Get:
    case BuiltinCallMode::Set:
        ++m_counts.reads;
        break;
    case BuiltinCallMode::Call:
    case BuiltinCallMode::Construct:
        ++m_counts.calls;
        break;
    }
    m_counts.conversions += static_cast<uint32_t>(std::ranges::count(plan.call.steps, OperandStepKind::Convert, &OperandStep::kind));
}

PassOutcome BuiltinResolver::run()
{
    for (bool didFoldRead = true; didFoldRead;) {
        Vector<BuiltinSite> sites = collectSites();
        if (sites.isEmpty())
            break;

        demoteTailCalls(sites.span());

        didFoldRead = false;
        Vector<ValueID> changed;
        changed.reserveInitialCapacity(sites.size());
        for (const BuiltinSite& site : sites) {
            if (!site.plan.isProven())
                continue;
            RELEASE_ASSERT(m_function.at(site.point.block).values[site.point.index] == site.id);
            apply(site);
            RELEASE_ASSERT(!m_builder.currentBlock());
            changed.append(site.id);
            didFoldRead |= site.plan.kind == BuiltinUseKind::Constant;
        }
        m_builder.applyInsertions();
        m_context.analyses.didChangeValues();
        narrowValueTypes(TypePropagationInputs { m_context.program, m_context.id, m_function, m_context.analyses, m_context.types, m_context.build }, changed.span());
    }

    if (!m_counts.didChange())
        return PassOutcome::Unchanged;

    Statistics& statistics = m_context.build.statistics();
    statistics.count(StatisticID::BuiltinReadsResolved, m_counts.reads);
    statistics.count(StatisticID::BuiltinCallsResolved, m_counts.calls);
    statistics.count(StatisticID::BuiltinConversionsInserted, m_counts.conversions);
    return PassOutcome::Changed;
}

static Value makeIteratorResultObject(const Program& program, const Value& intrinsic)
{
    LayoutID layoutID = program.layoutOf(BuiltinLayouts::IteratorResult);
    const Layout& layout = program.at(layoutID);
    std::optional<uint32_t> valueField = layout.findField(program.keyOf(BuiltinNames::value));
    std::optional<uint32_t> doneField = layout.findField(program.keyOf(BuiltinNames::done));
    RELEASE_ASSERT(valueField && doneField);

    Value object = IRBuilder::make(Opcode::NewObject);
    object.setLayout(layoutID);
    object.setFields(std::array { *valueField, *doneField });
    object.setValuesOperands(std::array { intrinsic.valueOperand(), intrinsic.doneOperand() });
    object.setAllocationSite(intrinsic.allocationSite());
    object.setKeepsShape(intrinsic.keepsShape());
    return object;
}

// Flow proves the layout of these objects, and placement plans the cells of a layout from the NewObject operations that name it.
static bool allocateIteratorResultsAsObjects(FunctionPassContext& context)
{
    const Function& function = context.function;
    Vector<ValueID> intrinsics;
    forEachValueSite(context.analyses, function, [&](const ValueSite& site) {
        if (function.at(site.value).opcode == Opcode::CreateIteratorResult)
            intrinsics.append(site.value);
    });

    for (ValueID intrinsic : intrinsics)
        replaceKeepingType(context, intrinsic, makeIteratorResultObject(context.program, function.at(intrinsic)));
    if (!intrinsics.isEmpty())
        context.analyses.didChangeValues();
    return !intrinsics.isEmpty();
}

static BuiltinOverloadID findFirstOverloadImplementedBy(const BuiltinTable& table, RuntimeFunctionID function)
{
    for (BuiltinID builtin : table.at(function).implements) {
        for (BuiltinOverloadID id : table.overloadIDsOf(builtin)) {
            if (table.at(id).libraryFunction() == function)
                return id;
        }
    }
    return { };
}

static std::optional<BuiltinOperand> builtinOperandOf(const Function& function, unsigned entryParameterIndex)
{
    switch (function.entryParameterKind(entryParameterIndex)) {
    case EntryParameter::This:
        return BuiltinOperand::receiver();
    case EntryParameter::Parameter:
        return BuiltinOperand::argument(entryParameterIndex - function.firstParameterIndex());
    case EntryParameter::Capture:
    case EntryParameter::Callee:
    case EntryParameter::NewTarget:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The function of the library behind an entry takes every call of it. The overloads of an entry go from the particular to whatever is left,
// so what the first one accepts is what the operands are expected to be.
static bool predictOperandsOfImplementedEntry(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, function.enclosingRuntimeFunction);
    if (!entry || entry->function != context.id)
        return false;
    BuiltinOverloadID overload = findFirstOverloadImplementedBy(program.builtins(), entry->id);
    if (!overload)
        return false;

    Vector<ValueID, 4> changed;
    const CompactList<ValueID>& entryParameters = function.at(entryBlock).parameters;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        std::optional<BuiltinOperand> operand = builtinOperandOf(function, index);
        DeclaredType* declared = function.declaredEntryParameter(index);
        if (!operand || !declared || declared->written || declared->predicted)
            continue;
        TypeID accepted = context.types.intern(parameterTypeOfOverload(program, overload, *operand));
        TypeID predicted = context.types.intersectionOf(declared->type, accepted);
        if (predicted == TypeTable::never || predicted == declared->type)
            continue;
        declared->predicted = predicted;
        changed.append(entryParameters[index]);
    }
    if (changed.isEmpty())
        return false;

    narrowValueTypes(TypePropagationInputs { program, context.id, function, context.analyses, context.types, context.build }, changed.span());
    return true;
}

PassOutcome resolveBuiltins(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    auto didChangeIf = [&](bool changed) {
        if (changed)
            outcome = PassOutcome::Changed;
    };

    didChangeIf(predictOperandsOfImplementedEntry(context));
    didChangeIf(allocateIteratorResultsAsObjects(context));
    didChangeIf(expandStaticRequires(context));
    didChangeIf(predictReceiversFromSeenMethods(context));
    {
        Vector<CalleeSplit> splits = planCalleeSplits(context);
        emitCalleeSplits(context, splits.span());
        didChangeIf(!splits.isEmpty());
    }
    {
        Vector<ListCallSite> calls = planListCalls(context);
        emitListCalls(context, calls.span());
        didChangeIf(!calls.isEmpty());
    }
    {
        Vector<BuiltinInstanceOfSite> tests = planBuiltinInstanceOfSites(context);
        rewriteBuiltinInstanceOfSites(context, tests.span());
        didChangeIf(!tests.isEmpty());
    }
    {
        Vector<CalledOperandTest> tests = planTestsOfCalledOperands(context);
        emitTestsOfCalledOperands(context, tests.span());
        didChangeIf(!tests.isEmpty());
    }
    {
        Vector<ForwardedTailCall> calls = planForwardedTailCalls(context);
        rewriteForwardedTailCalls(context, calls.span());
        didChangeIf(!calls.isEmpty());
    }
    return outcome | BuiltinResolver(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
