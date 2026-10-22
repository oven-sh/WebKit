/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveBuiltinsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPropertyAccessCache.h"
#include "AOTSimplifyBuiltinCallsInternal.h"
#include "AOTTypePropagation.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet callsByValue { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct };

// leaveURLsUnbuilt() looks for a Construct or a CallBuiltin, and some of the steps for a Call beside something else.
static_assert(operationsResolvedToBuiltins.contains(Opcode::Construct) && operationsResolvedToBuiltins.contains(Opcode::CallBuiltin) && operationsResolvedToBuiltins.contains(Opcode::Call));

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

static bool returnsToItsCaller(const Program& program, const BuiltinUsePlan& plan)
{
    if (!plan.isProven())
        return false;
    switch (plan.kind) {
    case BuiltinUseKind::Call:
    case BuiltinUseKind::UnmatchedCall:
        return !forwardsCall(program.builtins().at(plan.call.overload));
    case BuiltinUseKind::None:
    case BuiltinUseKind::Constant:
    case BuiltinUseKind::Undefined:
    case BuiltinUseKind::HostValue:
    case BuiltinUseKind::LibraryCall:
    case BuiltinUseKind::NewArray:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool staysTailCall(const BuiltinUsePlan& plan, std::optional<TailCallRequirement> tailCall)
{
    return tailCall == TailCallRequirement::Required && plan.kind == BuiltinUseKind::LibraryCall && plan.isProven() && !plan.assumedResult;
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
    return { m_context.program, m_function, m_context.types, m_context.inputs.flow, m_context.analyses.effectContext(), &m_context.analyses, { }, !m_context.build.options().obfuscates() };
}

// All that planBuiltinUse() takes from a use that is no tail call, until it asks about the place of the use.
static SiteContent contentOf(const BuiltinUseInputs& inputs, const Value& use)
{
    const Function& function = inputs.function;
    SiteContent content(static_cast<uint32_t>(RefusalKind::BuiltinUse) << 24 | static_cast<uint32_t>(use.opcode) << 8 | use.flags);
    content.add(&inputs.flow);
    content.add(&inputs.effects);
    content.add(use.type);
    switch (use.opcode) {
    case Opcode::PropGet:
        content.add(use.key());
        break;
    case Opcode::PropSet:
        content.add(use.key());
        content.add(static_cast<uint32_t>(use.mode()));
        break;
    case Opcode::Call:
        content.add(use.calleeKey());
        content.add(use.callees());
        content.add(use.expectedResult().index());
        break;
    case Opcode::Construct:
        content.add(use.callees());
        content.add(use.newTargetOperand() && sameObjectThroughCasts(function, use.newTargetOperand()) != sameObjectThroughCasts(function, use.calleeOperand()));
        break;
    case Opcode::SuperConstruct:
        content.add(use.callees());
        break;
    case Opcode::CallBuiltin:
        content.add(use.overload().index());
        content.add(static_cast<uint32_t>(use.builtinCallForm()));
        content.add(use.expectedResult().index());
        break;
    case Opcode::ArraySpeciesCreate:
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    for (ValueID operand : use.operands())
        content.addTypesOf(function, operand);
    return content;
}

Vector<BuiltinSite> BuiltinResolver::collectSites()
{
    BuiltinUseInputs inputs = this->inputs();
    bool asksAboutPlace = false;
    inputs.asksAboutPlace = &asksAboutPlace;
    PropertyAccessCache* cache = m_context.program.propertyAccessCache();
    const bool checksScreen = m_context.build.options().verifiesAfterEveryPass;
    Vector<BuiltinSite> sites;
    for (const ValueSite& site : m_context.sitesIn(SiteList::BuiltinUses)) {
        const Value& value = m_function.at(site.value);
        if (callsByValue.contains(value.opcode) && m_context.exceeds(LimitID::MaxBuiltinCallArguments, value.argumentsOperands().size(), site.value))
            continue;
        std::optional<TailCallRequirement> tailCall = tailCallRequirementOf(m_function, site.block, site.value);
        if (!tailCall && !mayFindProvenPlan(inputs, value)) {
            if (checksScreen) [[unlikely]] {
                inputs.asksOnlyForProvenPlans = true;
                RELEASE_ASSERT(!planBuiltinUse(inputs, site.value, site.point()).isProven());
            }
            continue;
        }
        uint32_t state = RefusedSites::stateOf(m_function, site.value);
        if (m_context.refusedSites.contains(RefusalKind::BuiltinUse, site.value, state))
            continue;

        std::optional<SiteContent> content;
        if (cache && !tailCall) {
            content = contentOf(inputs, value);
            if (!content->holdsOnlyIDsOfProgram())
                content = std::nullopt;
        }
        bool isKnownToBeRefused = content && cache->refusedContents.find(*content);
        if (isKnownToBeRefused && !cache->checksAnswers) [[likely]] {
            m_context.refusedSites.add(RefusalKind::BuiltinUse, m_function, site.value, state);
            continue;
        }

        inputs.asksOnlyForProvenPlans = !tailCall;
        asksAboutPlace = false;
        uint64_t unshareableEventCountBefore = m_context.types.unshareableEventCount();
        BuiltinUsePlan plan = planBuiltinUse(inputs, site.value, site.point());
        RELEASE_ASSERT(!isKnownToBeRefused || !plan.isProven());
        if (content && !isKnownToBeRefused && !plan.isProven() && !asksAboutPlace && m_context.types.unshareableEventCount() == unshareableEventCountBefore)
            cache->refusedContents.add(*content);
        if (tailCall == TailCallRequirement::Required && !endsInThisFrame(m_context.program, value, plan) && !returnsToItsCaller(m_context.program, plan) && !staysTailCall(plan, tailCall))
            continue;

        // Rule BuiltinUse rewrites a tested use while insertions are queued, when a tail call can no longer be demoted.
        bool needsDemotionBeforeTests = plan && tailCall;
        if (plan.isProven() || needsDemotionBeforeTests)
            sites.append(BuiltinSite { site.point(), site.value, WTF::move(plan) });
        else
            m_context.refusedSites.add(RefusalKind::BuiltinUse, m_function, site.value, state);
    }
    return sites;
}

void BuiltinResolver::demoteTailCalls(std::span<const BuiltinSite> sites)
{
    for (const BuiltinSite& site : sites) {
        std::optional<TailCallRequirement> tailCall = tailCallRequirementOf(m_function, site.point.block, site.id);
        if (!tailCall || (staysTailCall(site.plan, tailCall) && !endsInThisFrame(m_context.program, m_function.at(site.id), site.plan)))
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
    case BuiltinUseKind::Undefined:
    case BuiltinUseKind::HostValue:
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
            didFoldRead |= site.plan.kind == BuiltinUseKind::Constant || site.plan.kind == BuiltinUseKind::HostValue;
        }
        m_builder.applyInsertions();
        m_context.analyses.didChangeValues();
        m_context.refusedSites.clear(RefusalKind::BuiltinUse);
        narrowValueTypes(TypePropagationInputs { m_context.program, m_context.id, m_function, m_context.analyses, m_context.types, m_context.build, &m_context.typingMemo }, changed.span());
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
    return object;
}

// Placement plans the cells of a layout from the NewObject operations that name it.
static bool allocateIteratorResultsAsObjects(FunctionPassContext& context)
{
    const Function& function = context.function;
    Vector<ValueID> intrinsics;
    for (const ValueSite& site : context.sitesIn(SiteList::IteratorResults))
        intrinsics.append(site.value);

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

// A library function that implements a built-in takes every call of it. Overloads are listed from the most specific to the most general,
// so the first one says which operand types to expect.
static bool expectOperandTypesOfFirstOverload(FunctionPassContext& context)
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
        if (!operand || !declared || declared->type != TypeTable::anything || declared->flags.contains(PositionFlags::IsInt32Promise))
            continue;
        TypeID accepted = context.types.intern(parameterTypeOfOverload(program, overload, *operand));
        TypeID expected = context.types.intersectionOf(declared->type, accepted);
        if (expected == TypeTable::never || expected == declared->type)
            continue;
        declared->type = expected;
        declared->flags.add(PositionFlags::IsExpected);
        changed.append(entryParameters[index]);
    }
    if (changed.isEmpty())
        return false;

    // Whether a validation has anything left to test depends on what is declared of the parameters, whether or not the type of a value follows.
    context.analyses.didChangeTypes();
    narrowValueTypes(TypePropagationInputs { program, context.id, function, context.analyses, context.types, context.build, &context.typingMemo }, changed.span());
    return true;
}

static PassOutcome runSteps(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    if (leaveURLsUnbuilt(context))
        outcome = PassOutcome::Changed;
    if (expectOperandTypesOfFirstOverload(context))
        outcome = PassOutcome::Changed;

    auto didChangeIf = [&](bool changed) {
        if (changed)
            outcome = PassOutcome::Changed;
    };
    auto hasSitesIn = [&](SiteList list) {
        return !context.sitesIn(list).empty();
    };

    if (hasSitesIn(SiteList::IteratorResults))
        didChangeIf(allocateIteratorResultsAsObjects(context));
    if (hasSitesIn(SiteList::Calls)) {
        Vector<CalleeSplit> splits = planCalleeSplits(context);
        emitCalleeSplits(context, splits.span());
        didChangeIf(!splits.isEmpty());
    }
    if (hasSitesIn(SiteList::CallsWithList)) {
        Vector<ListCallSite> calls = planListCalls(context);
        emitListCalls(context, calls.span());
        didChangeIf(!calls.isEmpty());
    }
    if (hasSitesIn(SiteList::InstanceOfTests)) {
        Vector<BuiltinInstanceOfSite> tests = planBuiltinInstanceOfSites(context);
        rewriteBuiltinInstanceOfSites(context, tests.span());
        didChangeIf(!tests.isEmpty());
    }
    if (context.hasRequiredTailCall() && hasSitesIn(SiteList::Calls)) {
        Vector<CalledOperandTest> tests = planTestsOfCalledOperands(context);
        emitTestsOfCalledOperands(context, tests.span());
        didChangeIf(!tests.isEmpty());
    }
    if (hasSitesIn(SiteList::ArgumentsObjects) && hasSitesIn(SiteList::Calls)) {
        Vector<CallOfOwnArguments> calls = planCallsOfOwnArguments(context);
        passOnOwnArgumentsByCount(context, calls.span());
        didChangeIf(!calls.isEmpty());
    }
    if (context.hasRequiredTailCall() && hasSitesIn(SiteList::Calls)) {
        Vector<ForwardedTailCall> calls = planForwardedTailCalls(context);
        rewriteForwardedTailCalls(context, calls.span());
        didChangeIf(!calls.isEmpty());
    }
    if (!hasSitesIn(SiteList::BuiltinUses))
        return outcome;
    return outcome | BuiltinResolver(context).run();
}

PassOutcome resolveBuiltins(FunctionPassContext& context)
{
    PassOutcome outcome = runSteps(context);
    // Each step tells the analyses of its edits as it makes them. Effects go anyway: FoldConstants asks for them between its own folds, and an answer cached here would change what it folds.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses - FunctionAnalysis::Effects;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
