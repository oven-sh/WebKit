/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTComparisonChains.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTRuleSupport.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::Switch::subject == OperandIndex::SwitchString::subject);

namespace {

struct SwitchLike {
    ValueID subject;
    ValueID switchedValue;
    ValueID comparison;
    Vector<ChainCase, 1> cases;
};

struct ConstantOperand {
    uint32_t index { 0 };
    ValueID constant;
};

struct ProvenCase {
    uint32_t caseIndex { 0 };
    BlockID stub;
    Target target;
};

} // anonymous namespace

static Edge fallThroughOf(const Function& function, BlockID block)
{
    return Edge { block, static_cast<uint32_t>(function.at(block).terminator.targets.size() - 1) };
}

static bool isRematerializableThroughUpcasts(const Function& function, ValueID value)
{
    return isRematerializable(function.at(definitionThroughUpcasts(function, value)));
}

static bool passesProofsOnlyAbout(const Terminator& branch, uint32_t operandIndex)
{
    return std::ranges::all_of(branch.targets, [&](const Target& target) {
        return std::ranges::all_of(target.proofs, [&](uint32_t proof) {
            return proof == operandIndex;
        });
    });
}

static ValueID incomingSourceOf(const Function& function, Edge incoming, uint32_t parameterIndex)
{
    IncomingValue value = incomingValueOf(function, incoming, parameterIndex);
    switch (value.kind) {
    case IncomingValue::Kind::Argument:
        return value.argument;
    case IncomingValue::Kind::EdgeResult:
        break;
    }
    EdgeResult result = edgeResultAt(function, value.result);
    RELEASE_ASSERT(result.kind == EdgeResultKind::NarrowedOperand);
    return result.subject;
}

static void forEachReplacedParameter(const Function& function, Edge incoming, uint32_t firstParameter, NOESCAPE const Invocable<void(ValueID parameter, ValueID source)> auto& functor)
{
    const CompactList<ValueID>& parameters = function.at(targetOf(function, incoming).block).parameters;
    for (uint32_t index = firstParameter; index < parameters.size(); ++index)
        functor(parameters[index], incomingSourceOf(function, incoming, index));
}

static void appendCases(ComparisonChain& chain, BlockID block, const SwitchLike& description)
{
    chain.cases.appendVector(description.cases);
    chain.fallThrough = Edge { block, static_cast<uint32_t>(description.cases.size()) };
    if (description.comparison)
        chain.comparisons.append(description.comparison);
}

static void removeRepeatedCases(Vector<ChainCase, 8>& cases)
{
    auto constantOf = [&](uint32_t index) {
        return std::pair { cases[index].number, cases[index].string };
    };
    Vector<uint32_t, 8> order(cases.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    std::ranges::stable_sort(order, { }, constantOf);
    BitVector isRepeated(cases.size());
    for (size_t i = 1; i < order.size(); ++i) {
        if (constantOf(order[i]) == constantOf(order[i - 1]))
            isRepeated.quickSet(order[i]);
    }
    size_t index = 0;
    cases.removeAllMatching([&](const ChainCase&) {
        return isRepeated.quickGet(index++);
    });
}

namespace {

class ChainFinder {
public:
    ChainFinder(FunctionAnalyses& analyses, ChainKind kind, const ChainLimits& limits, const ChainLinkFilter& mayJoinChain)
        : m_analyses(analyses)
        , m_program(analyses.program())
        , m_function(analyses.function())
        , m_kind(kind)
        , m_limits(limits)
        , m_mayJoinChain(mayJoinChain)
    {
    }

    Vector<ComparisonChain> run()
    {
        for (BlockID head : m_analyses.cfg().reversePostOrder()) {
            if (m_links.contains(head))
                continue;
            std::optional<SwitchLike> description = describe(head);
            if (!description)
                continue;

            ComparisonChain chain = growChain(head, *description);
            trimToConvertibleFallThrough(chain);
            if (chain.links.isEmpty())
                continue;
            recordReplacements(chain.fallThrough, numberOfExplicitArguments(m_function, chain.fallThrough));
            removeRepeatedCases(chain.cases);
            if (chain.cases.size() >= m_limits.minCases)
                m_chains.append(WTF::move(chain));
        }
        return WTF::move(m_chains);
    }

private:
    bool isCaseConstant(const Value& constant) const
    {
        switch (m_kind) {
        case ChainKind::Integers:
            return constant.opcode == Opcode::Integer;
        case ChainKind::Numbers:
            return switchCaseOf(constant).has_value();
        case ChainKind::Strings:
            return constant.opcode == Opcode::String;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool canBeSubject(const Type& subject) const
    {
        switch (m_kind) {
        case ChainKind::Integers:
            return subject.isInteger();
        case ChainKind::Numbers:
            return !subject.isInteger();
        case ChainKind::Strings:
            return true;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ChainCase createCase(const Value& constant, Edge edge) const
    {
        ChainCase chainCase { .edge = edge };
        switch (m_kind) {
        case ChainKind::Integers:
            chainCase.number = constant.integer();
            break;
        case ChainKind::Numbers:
            chainCase.number = *switchCaseOf(constant);
            break;
        case ChainKind::Strings:
            chainCase.string = constant.string();
            break;
        }
        return chainCase;
    }

    std::optional<SwitchLike> describe(BlockID block) const
    {
        const Terminator& terminator = m_function.at(block).terminator;
        switch (terminator.kind) {
        case TerminatorKind::Branch:
            return describeBranch(block, terminator);
        case TerminatorKind::Switch:
            if (m_kind == ChainKind::Strings)
                return std::nullopt;
            return describeSwitch(block, terminator);
        case TerminatorKind::SwitchString:
            if (m_kind != ChainKind::Strings)
                return std::nullopt;
            return describeSwitch(block, terminator);
        case TerminatorKind::Jump:
        case TerminatorKind::TestAndBranch:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::Return:
        case TerminatorKind::ReturnCall:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
            return std::nullopt;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    std::optional<ConstantOperand> findConstantOperand(const Value& comparison) const
    {
        for (uint32_t index = 0; index < 2; ++index) {
            std::optional<ValueID> constant = constantThroughCasts(m_program, m_function, comparison.operand(index));
            if (constant && isCaseConstant(m_function.at(*constant)))
                return ConstantOperand { index, *constant };
        }
        return std::nullopt;
    }

    bool isListedWithSingleUse(BlockID block, ValueID comparison) const
    {
        const UseLists& useLists = m_analyses.useLists();
        ValueLocation location = useLists.locationOf(comparison);
        return useLists.useCount(comparison) == 1 && location.placement == ValuePlacement::InBlock && location.block == block;
    }

    std::optional<SwitchLike> describeBranch(BlockID block, const Terminator& branch) const
    {
        ValueID comparison = branch.conditionOperand();
        const Value& strictEquality = m_function.at(comparison);
        if (strictEquality.opcode != Opcode::StrictEq)
            return std::nullopt;
        std::optional<ConstantOperand> constant = findConstantOperand(strictEquality);
        if (!constant)
            return std::nullopt;

        uint32_t comparedIndex = 1 - constant->index;
        ValueID compared = strictEquality.operand(comparedIndex);
        if (constantThroughCasts(m_program, m_function, compared) || !canBeSubject(m_program.typeOf(m_function, compared)))
            return std::nullopt;
        if (!passesProofsOnlyAbout(branch, comparedIndex) || !isListedWithSingleUse(block, comparison) || !m_mayJoinChain(comparison))
            return std::nullopt;
        return SwitchLike { m_analyses.identityOf(compared), compared, comparison, { createCase(m_function.at(constant->constant), Edge { block, 0 }) } };
    }

    std::optional<SwitchLike> describeSwitch(BlockID block, const Terminator& terminator) const
    {
        ValueID switchedValue = terminator.subjectOperand();
        if (!canBeSubject(m_program.typeOf(m_function, switchedValue)))
            return std::nullopt;

        SwitchLike description { m_analyses.identityOf(switchedValue), switchedValue, { }, { } };
        uint32_t numberOfCases = static_cast<uint32_t>(terminator.caseTargets().size());
        description.cases.reserveInitialCapacity(numberOfCases);
        for (uint32_t index = 0; index < numberOfCases; ++index) {
            ChainCase chainCase { .edge = Edge { block, index } };
            if (terminator.kind == TerminatorKind::SwitchString)
                chainCase.string = terminator.strings()[index];
            else
                chainCase.number = terminator.cases()[index];
            description.cases.append(chainCase);
        }
        return description;
    }

    ValueID replacementOf(ValueID value) const
    {
        ValueID replacement = m_replacements.get(value);
        return replacement ? replacement : value;
    }

    void recordReplacements(Edge incoming, uint32_t firstParameter)
    {
        forEachReplacedParameter(m_function, incoming, firstParameter, [&](ValueID parameter, ValueID source) {
            m_replacements.set(parameter, replacementOf(source));
        });
    }

    bool isUsedOnlyBy(ValueID parameter, ValueID comparison) const
    {
        return std::ranges::all_of(m_analyses.useLists().usesOf(parameter), [&](const Use& use) {
            return use.kind == Use::Kind::Operand && use.user == comparison;
        });
    }

    bool canReplaceParameters(Edge incoming, uint32_t firstParameter, ValueID comparison) const
    {
        bool canReplaceAll = true;
        forEachReplacedParameter(m_function, incoming, firstParameter, [&](ValueID parameter, ValueID source) {
            canReplaceAll &= m_function.at(parameter).type == m_function.at(replacementOf(source)).type || isUsedOnlyBy(parameter, comparison);
        });
        return canReplaceAll;
    }

    std::optional<SwitchLike> describeLink(const ComparisonChain& chain, ValueID subject) const
    {
        BlockID next = targetOf(m_function, chain.fallThrough).block;
        const Block& block = m_function.at(next);
        if (m_analyses.cfg().singleIncomingEdge(next) != chain.fallThrough || block.role != BlockRole::Ordinary || block.handler != m_function.at(chain.head).handler)
            return std::nullopt;
        std::optional<SwitchLike> description = describe(next);
        if (!description || description->subject != subject || !canReplaceParameters(chain.fallThrough, 0, description->comparison))
            return std::nullopt;

        bool canMoveValues = std::ranges::all_of(block.values, [&](ValueID value) {
            return value == description->comparison || isRematerializableThroughUpcasts(m_function, value);
        });
        bool leadsBackToHead = block.terminator.targets.containsIf([&](const Target& target) {
            return target.block == chain.head;
        });
        if (!canMoveValues || leadsBackToHead)
            return std::nullopt;
        return description;
    }

    ComparisonChain growChain(BlockID head, const SwitchLike& description)
    {
        ComparisonChain chain;
        chain.head = head;
        chain.subject = replacementOf(description.switchedValue);
        appendCases(chain, head, description);
        while (std::optional<SwitchLike> link = describeLink(chain, description.subject)) {
            if (chain.cases.size() + link->cases.size() > m_limits.maxCases) {
                m_analyses.context().build.gaveUp(m_limits.maxCasesLimit, m_analyses.subject(link->switchedValue));
                break;
            }
            BlockID next = targetOf(m_function, chain.fallThrough).block;
            recordReplacements(chain.fallThrough, 0);
            m_links.add(next);
            chain.links.append(next);
            appendCases(chain, next, *link);
        }
        return chain;
    }

    bool hasConvertibleFallThrough(const ComparisonChain& chain) const
    {
        const Target& target = targetOf(m_function, chain.fallThrough);
        if (target.proofs.isEmpty())
            return true;
        return m_analyses.cfg().singleIncomingEdge(target.block) == chain.fallThrough
            && canReplaceParameters(chain.fallThrough, numberOfExplicitArguments(m_function, chain.fallThrough), ValueID());
    }

    void removeLastLink(ComparisonChain& chain)
    {
        BlockID link = chain.links.takeLast();
        const Block& block = m_function.at(link);
        m_links.remove(link);
        for (ValueID parameter : block.parameters)
            m_replacements.remove(parameter);
        chain.cases.shrink(chain.cases.size() - (block.terminator.targets.size() - 1));
        if (block.terminator.kind == TerminatorKind::Branch)
            chain.comparisons.removeLast();
        chain.fallThrough = fallThroughOf(m_function, chain.links.isEmpty() ? chain.head : chain.links.last());
    }

    void trimToConvertibleFallThrough(ComparisonChain& chain)
    {
        while (!chain.links.isEmpty() && !hasConvertibleFallThrough(chain))
            removeLastLink(chain);
    }

    FunctionAnalyses& m_analyses;
    const Program& m_program;
    const Function& m_function;
    const ChainKind m_kind;
    const ChainLimits m_limits;
    const ChainLinkFilter& m_mayJoinChain;
    IndexSet<BlockID> m_links;
    UncheckedKeyHashMap<ValueID, ValueID> m_replacements;
    Vector<ComparisonChain> m_chains;
};

} // anonymous namespace

Vector<ComparisonChain> findComparisonChains(FunctionAnalyses& analyses, ChainKind kind, const ChainLimits& limits, NOESCAPE const ChainLinkFilter& mayJoinChain)
{
    return ChainFinder(analyses, kind, limits, mayJoinChain).run();
}

static void replaceParameters(IRBuilder& builder, const ComparisonChain& chain, ValueSubstitution& substitution)
{
    Function& function = builder.function();
    auto replace = [&](ValueID parameter, ValueID source) {
        substitution.replace(parameter, source);
    };

    BlockID previous = chain.head;
    for (BlockID link : chain.links) {
        forEachReplacedParameter(function, fallThroughOf(function, previous), 0, replace);
        previous = link;
    }
    RELEASE_ASSERT(fallThroughOf(function, previous) == chain.fallThrough);

    uint32_t firstResult = numberOfExplicitArguments(function, chain.fallThrough);
    forEachReplacedParameter(function, chain.fallThrough, firstResult, replace);

    BlockID target = targetOf(function, chain.fallThrough).block;
    BitVector resultIndexes;
    for (size_t index = firstResult; index < function.at(target).parameters.size(); ++index)
        resultIndexes.set(index);
    builder.removeParameters(target, resultIndexes);
}

static Target takeTarget(Function& function, Edge edge)
{
    return std::exchange(targetOf(function, edge), Target { });
}

static void mergeLinksIntoHead(Function& function, const ComparisonChain& chain)
{
    Block& head = function.at(chain.head);
    if (head.terminator.kind == TerminatorKind::Branch) {
        bool wasListed = head.values.removeFirst(head.terminator.conditionOperand());
        RELEASE_ASSERT(wasListed);
    }
    for (BlockID link : chain.links) {
        Block removed = std::exchange(function.at(link), Block { });
        for (ValueID value : removed.values) {
            if (isRematerializableThroughUpcasts(function, value))
                head.values.append(value);
        }
    }
}

static Vector<ProvenCase> addStubsForProofs(IRBuilder& builder, Vector<Target>& caseTargets)
{
    Vector<ProvenCase> provenCases;
    for (uint32_t caseIndex = 0; caseIndex < caseTargets.size(); ++caseIndex) {
        Target& target = caseTargets[caseIndex];
        if (target.proofs.isEmpty())
            continue;
        BlockID stub = builder.addBlock(builder.function().at(target.block).frequency);
        provenCases.append(ProvenCase { caseIndex, stub, std::exchange(target, Target { stub, { }, { } }) });
    }
    return provenCases;
}

static void passProofsThroughStubs(IRBuilder& builder, BlockID head, Vector<ProvenCase>&& provenCases)
{
    for (ProvenCase& provenCase : provenCases) {
        for (size_t remaining = provenCase.target.proofs.size(); remaining; --remaining)
            provenCase.target.arguments.append(builder.addEdgeResult(Edge { head, provenCase.caseIndex }, OperandIndex::Switch::subject));
    }
    for (ProvenCase& provenCase : provenCases) {
        builder.moveToEndOf(provenCase.stub);
        builder.emitJump(provenCase.target.block, WTF::move(provenCase.target.arguments));
    }
}

template<typename Constant>
static Vector<std::pair<Constant, Target>> pairWithTargets(const ComparisonChain& chain, Constant ChainCase::* constant, Vector<Target>&& targets)
{
    Vector<std::pair<Constant, Target>> cases;
    cases.reserveInitialCapacity(targets.size());
    for (size_t index = 0; index < targets.size(); ++index)
        cases.append({ chain.cases[index].*constant, WTF::move(targets[index]) });
    return cases;
}

void convertChainToSwitch(IRBuilder& builder, FunctionAnalyses& analyses, ChainKind kind, const ComparisonChain& chain)
{
    builder.applyInsertions();
    Function& function = builder.function();

    ValueSubstitution substitution(function);
    replaceParameters(builder, chain, substitution);

    Vector<Target> caseTargets;
    caseTargets.reserveInitialCapacity(chain.cases.size());
    for (const ChainCase& chainCase : chain.cases)
        caseTargets.append(takeTarget(function, chainCase.edge));
    Target otherwise = takeTarget(function, chain.fallThrough);

    mergeLinksIntoHead(function, chain);
    builder.setOrigin(function.at(chain.head).terminator.origin());
    builder.setHandler(function.at(chain.head).handler);
    function.at(chain.head).terminator = Terminator { };
    analyses.didChangeControlFlow();
    builder.moveToEndOf(chain.head);

    Vector<ProvenCase> provenCases = addStubsForProofs(builder, caseTargets);
    switch (kind) {
    case ChainKind::Integers:
    case ChainKind::Numbers:
        builder.emitSwitch(chain.subject, pairWithTargets(chain, &ChainCase::number, WTF::move(caseTargets)), WTF::move(otherwise));
        break;
    case ChainKind::Strings:
        builder.emitSwitchString(chain.subject, pairWithTargets(chain, &ChainCase::string, WTF::move(caseTargets)), WTF::move(otherwise));
        break;
    }
    passProofsThroughStubs(builder, chain.head, WTF::move(provenCases));

    substitution.apply(function, &analyses);
    analyses.didChangeControlFlow();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
