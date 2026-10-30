/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveBuiltinsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCoroutines.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardAvailability.h"
#include "AOTInductionVariables.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTPropertyAccessCache.h"
#include "AOTSimplifyBuiltinCallsInternal.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet resolvedCallsByValue { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct };

static_assert(operationsResolvedToBuiltins.contains(Opcode::Construct) && operationsResolvedToBuiltins.contains(Opcode::CallBuiltin) && operationsResolvedToBuiltins.contains(Opcode::Call));

std::optional<TailCallRequirement> tailCallRequirementOf(const Function& function, BlockID block, ValueID call)
{
    const Terminator& terminator = function.at(block).terminator;
    if (terminator.kind != TerminatorKind::ReturnCall || terminator.callOperand() != call)
        return std::nullopt;
    return terminator.requirement();
}

static bool endsInThisFrame(const Program& program, const BuiltinUsePlan& plan)
{
    return plan.call.overload && program.builtins().at(plan.call.overload).hasTypedResult();
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
        if (resolvedCallsByValue.contains(value.opcode) && m_context.exceeds(LimitID::MaxBuiltinCallArguments, value.argumentsOperands().size(), site.value))
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

        inputs.asksOnlyForProvenPlans = tailCall != TailCallRequirement::Allowed;
        asksAboutPlace = false;
        uint64_t unshareableEventCountBefore = m_context.types.unshareableEventCount();
        BuiltinUsePlan plan = planBuiltinUse(inputs, site.value, site.point());
        RELEASE_ASSERT(!isKnownToBeRefused || !plan.isProven());
        if (content && !isKnownToBeRefused && !plan.isProven() && !asksAboutPlace && m_context.types.unshareableEventCount() == unshareableEventCountBefore)
            cache->refusedContents.add(*content);
        if (tailCall == TailCallRequirement::Required && !endsInThisFrame(m_context.program, plan) && !returnsToItsCaller(m_context.program, plan) && !staysTailCall(plan, tailCall))
            continue;

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
        if (!tailCall || (staysTailCall(site.plan, tailCall) && !endsInThisFrame(m_context.program, site.plan)))
            continue;
        demoteTailCall(m_context, m_builder, site.point.block);
        ++m_counts.demotedTailCalls;
    }
}

void BuiltinResolver::apply(const BuiltinSite& site)
{
    m_builder.setOrigin(m_function.at(site.id).origin());
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

static bool allocateIteratorResultsAsObjects(FunctionPassContext& context)
{
    const Function& function = context.function;
    Vector<ValueID> intrinsics;
    for (const ValueSite& site : context.sitesIn(SiteList::IteratorResults))
        intrinsics.append(site.value);

    for (ValueID id : intrinsics) {
        const Value& intrinsic = function.at(id);
        replaceKeepingType(context, id, makeIteratorResultObject(context.program, intrinsic.valueOperand(), intrinsic.doneOperand(), intrinsic.allocationSite()));
    }
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
        if (!operand || !declared || declared->written() != TypeTable::anything || declared->has(PositionFlags::IsInt32Promise))
            continue;
        TypeID accepted = context.types.intern(parameterTypeOfOverload(program, overload, *operand));
        TypeID expected = context.types.intersectionOf(TypeTable::anything, accepted);
        if (expected == TypeTable::never || expected == TypeTable::anything)
            continue;
        declared->expect(ExpectedType(expected));
        changed.append(entryParameters[index]);
    }
    if (changed.isEmpty())
        return false;

    context.analyses.didChangeTypes();
    narrowValueTypes(TypePropagationInputs { program, context.id, function, context.analyses, context.types, context.build, &context.typingMemo }, changed.span());
    return true;
}

// `new Array(n)` returns only if the number n is a whole number from 0 to 2^32 - 1, or -0, and the list is then that long. No comparison tells -0 from 0,
// so behind the call a comparison with n asks for the length of the list. That is an integer, and so is a counter that stays below it.
static bool compareWithLengthsOfNewLists(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    if (!analyses.opcodesOfValues().contains(Opcode::CallBuiltin) || !analyses.opcodesOfValues().intersects(relationalOperators))
        return false;

    struct NewList {
        ValueID list;
        ValueID number;
        ProgramPoint behind;
        Vector<Use, 4> comparisons { };
    };
    Vector<NewList, 2> lists;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& call = function.at(values[index]);
            if (call.opcode != Opcode::CallBuiltin || call.argumentsOperands().empty() || inlineImplementationOf(program, call) != InlineBuiltin::ArrayWithLength)
                continue;
            // Anything but a number would be the only element.
            ValueID number = call.argumentsOperands()[0];
            if (!program.typeOf(function, number).isNumber() || program.typeOf(function, number).isInteger())
                continue;
            // What flow knows of the lists made here stands in the cast behind the call.
            ValueID list = values[index];
            uint32_t last = index;
            while (last + 1 < values.size() && function.at(values[last + 1]).opcode == Opcode::AssumeType && function.at(values[last + 1]).operand(0) == list)
                list = values[++last];
            if (proofsNeededFromGuard(program, function, IRBuilder::make(Opcode::GetArrayLength, { list, ValueID() })).isEmpty())
                lists.append({ list, number, { block, last + 1 } });
        }
    }
    if (lists.isEmpty())
        return false;

    bool hasComparison = false;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& comparison = function.at(id);
            if (!relationalOperators.contains(comparison.opcode))
                continue;
            for (uint32_t operandIndex = 0; operandIndex < comparison.numberOfOperands(); ++operandIndex) {
                ValueID compared = analyses.identityOf(comparison.operand(operandIndex));
                for (NewList& made : lists) {
                    if (compared != analyses.identityOf(made.number) || !analyses.dominates(made.list, analyses.useLists().pointOf(id)))
                        continue;
                    made.comparisons.append(Use { Use::Kind::Operand, block, id, operandIndex });
                    hasComparison = true;
                    break;
                }
            }
        }
    }
    if (!hasComparison)
        return false;

    // What an edge says of n, it would now say of the length. Whoever reads it there wants n, which may be -0.
    ValueSubstitution namesOnEdges(function);
    for (const NewList& made : lists) {
        for (const Use& use : made.comparisons) {
            for (const Use& branch : analyses.useLists().usesOf(use.user)) {
                if (branch.kind != Use::Kind::TerminatorOperand || function.at(branch.block).terminator.kind != TerminatorKind::Branch)
                    continue;
                for (uint32_t targetIndex = 0; targetIndex < 2; ++targetIndex) {
                    const Target& target = targetOf(function, Edge { branch.block, targetIndex });
                    size_t result = target.proofs.find(use.operandIndex);
                    if (result != notFound)
                        namesOnEdges.replace(function.at(target.block).parameters[firstEdgeResultParameter(function, branch.block, targetIndex) + result], function.at(use.user).operand(use.operandIndex));
                }
            }
        }
    }
    if (!namesOnEdges.isEmpty())
        namesOnEdges.apply(function, &analyses);

    IRBuilder builder(analyses.context(), function, context.types, &analyses);
    Vector<ValueID> changed;
    for (const NewList& made : lists) {
        if (made.comparisons.isEmpty())
            continue;
        builder.setOrigin(function.at(made.list).origin());
        ValueID length = builder.insertBefore(made.behind, IRBuilder::make(Opcode::GetArrayLength, { made.list, ValueID() }));
        for (const Use& use : made.comparisons) {
            replaceUse(function, use, length);
            changed.append(use.user);
        }
    }
    builder.applyInsertions();
    analyses.didChangeValues();
    narrowValueTypes(TypePropagationInputs { program, context.id, function, analyses, context.types, context.build, &context.typingMemo }, changed.span());
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
    // The list is tested once the callee has passed its test.
    for (unsigned round = 0; round < 2 && context.hasRequiredTailCall() && hasSitesIn(SiteList::Calls); ++round) {
        Vector<CalledOperandTest> tests = planTestsOfCalledOperands(context);
        if (tests.isEmpty())
            break;
        emitTestsOfCalledOperands(context, tests.span());
        didChangeIf(true);
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
    if (hasSitesIn(SiteList::BuiltinUses))
        outcome |= BuiltinResolver(context).run();
    didChangeIf(compareWithLengthsOfNewLists(context));
    return outcome;
}

PassOutcome resolveBuiltins(FunctionPassContext& context)
{
    PassOutcome outcome = runSteps(context);
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses - FunctionAnalysis::Effects;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
