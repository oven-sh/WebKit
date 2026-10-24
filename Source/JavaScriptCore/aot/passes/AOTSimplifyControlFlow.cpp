/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDecidedBranches.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTKnownValue.h"
#include "AOTParameterReplacement.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class HandlerChoice : uint8_t { CannotMerge, KeepPredecessorHandler, TakeSuccessorHandler };

struct BypassedThrow {
    BlockID block;
    BitVector deadValues;
};

class ControlFlowSimplifier {
public:
    explicit ControlFlowSimplifier(FunctionPassContext& context)
        : m_program(context.program)
        , m_function(context.function)
        , m_analyses(context.analyses)
        , m_builder(context.analyses.context(), context.function, context.types, &context.analyses)
        , m_validates(context.build.validates())
        , m_looksAcrossBlocks(context.mayDecideTestsAcrossBlocks)
    {
    }

    PassOutcome run()
    {
        m_onlyJoinedBlocks = !m_analyses.cfg().hasHandlers();
        Vector<BlockID> reachableBlocks(m_analyses.cfg().reversePostOrder());
        decideBranches(reachableBlocks);
        for (BlockID block : reachableBlocks)
            m_numberOfChanges += simplifyTerminator(block);
        dropFailuresOfPredictionsThatNobodyTests();
        if (m_numberOfChanges)
            m_analyses.didChangeControlFlow();

        bypassHandlers();
        m_onlyJoinedBlocks &= !m_numberOfChanges;
        collapseTestsThatDecideNothing();
        m_numberOfChanges += m_builder.emptyUnreachableBlocks();
        size_t numberOfChangesBeforeMerging = m_numberOfChanges;
        mergeChains(m_analyses.cfg());
        if (!m_numberOfChanges)
            return PassOutcome::Unchanged;

        if (m_numberOfChanges != numberOfChangesBeforeMerging)
            m_analyses.didChangeControlFlow();
        return PassOutcome::Changed;
    }

    bool onlyJoinedBlocks() const { return m_onlyJoinedBlocks; }
    bool replacedParametersByKnownValues() const { return m_replacedParameterByKnownValue; }
    bool mayFindMoreAtOnce() const { return m_mayFindMoreAtOnce; }

private:
    bool leadsTo(BlockID from, BlockID place, unsigned mostSteps) const
    {
        if (from == place)
            return true;
        return mostSteps && std::ranges::any_of(m_function.at(from).terminator.targets, [&](const Target& target) {
            return leadsTo(target.block, place, mostSteps - 1);
        });
    }

    bool isLedToByTestBehind(BlockID block, BlockID place) const
    {
        AOT_THRESHOLD(unsigned, mostBlocks, 32);
        AOT_THRESHOLD(unsigned, mostStepsBehindFailure, 3);
        for (unsigned blocks = 0; blocks < mostBlocks; ++blocks) {
            const Terminator& terminator = m_function.at(block).terminator;
            if (terminator.kind == TerminatorKind::Jump) {
                block = terminator.next().block;
                continue;
            }
            if (terminator.kind == TerminatorKind::SwitchOnShape)
                return leadsTo(terminator.otherwise().block, place, mostStepsBehindFailure);
            if (!terminator.isConditionalBranch())
                return false;
            if (leadsTo(terminator.notTaken().block, place, mostStepsBehindFailure))
                return true;
            if (provesOf(m_function.at(terminator.conditionOperand()).opcode) == Proof::None)
                return false;
            block = terminator.taken().block;
        }
        return false;
    }

    // Without TestingSwitch::FailPredictions nothing leaves where no test is left.
    void dropFailuresOfPredictionsThatNobodyTests()
    {
        if (!m_builder.failsPredictions())
            return;
        for (BlockID id : Vector<BlockID>(m_analyses.cfg().reversePostOrder())) {
            const Terminator& terminator = m_function.at(id).terminator;
            if (terminator.kind != TerminatorKind::Branch || m_function.at(terminator.conditionOperand()).opcode != Opcode::ShouldFailPrediction)
                continue;
            if (isLedToByTestBehind(terminator.notTaken().block, terminator.taken().block))
                continue;
            m_builder.convertBranchToJump(id, 1);
            ++m_numberOfChanges;
        }
    }

    static bool mayStrictlyEqual(const Type& subject, int64_t integer)
    {
        bool mayBeNegativeZero = subject.primitives & Primitive::WholeNumber;
        if (!integer && mayBeNegativeZero)
            return true;
        return (subject.primitives & Primitive::Integer) && subject.integers.contains(integer);
    }

    static bool mayStrictlyEqual(const Type& subject, StringID string)
    {
        return (subject.primitives & Primitive::String) || subject.stringLiterals.contains(string);
    }

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }

    bool canDecide(Edge edge)
    {
        if (!canFollowWithoutTest(m_function, edge))
            return false;
        if (!hasEdgeResults(m_function, edge))
            return true;
        return argumentsFitParametersAfterCasts(m_program, m_function, m_builder.types(), Target { targetOf(m_function, edge).block, argumentsWithoutTest(m_function, edge), { } });
    }

    void decideBranches(const Vector<BlockID>& reachableBlocks)
    {
        std::optional<TypeTestBranches> typeTests;
        std::optional<SlotValueTests> slotValueTests;
        if (m_looksAcrossBlocks) {
            typeTests.emplace(m_analyses);
            slotValueTests.emplace(m_analyses, m_builder.types(), typeTests->blocksThatBranchOnTypeTest());
        }
        Vector<std::pair<BlockID, uint32_t>> decided;
        auto noteWaysGivenUp = makeScopeExit([&] {
            m_mayFindMoreAtOnce |= typeTests && typeTests->gaveUpOnLongWays();
        });
        for (BlockID block : reachableBlocks) {
            if (m_function.at(block).terminator.kind != TerminatorKind::Branch)
                continue;

            uint32_t target = 0;
            switch (m_looksAcrossBlocks ? evaluateCondition(m_analyses, *typeTests, *slotValueTests, block) : evaluateConditionByDefinitions(m_analyses, block)) {
            case KnownBoolean::Unknown:
                continue;
            case KnownBoolean::True:
                target = 0;
                break;
            case KnownBoolean::False:
                target = 1;
                break;
            }
            if (canDecide(Edge { block, target }))
                decided.append({ block, target });
        }

        if (decided.isEmpty())
            return;
        for (auto [block, target] : decided)
            decideTerminator(m_builder, block, target);
        for (auto [block, target] : decided)
            castArgumentsToParameters(m_builder, block, m_validates);
        m_builder.applyInsertions();
        m_numberOfChanges += decided.size();
    }

    bool simplifyTerminator(BlockID block)
    {
        switch (m_function.at(block).terminator.kind) {
        case TerminatorKind::Branch:
            return simplifyBranch(block);
        case TerminatorKind::Switch:
            return simplifySwitch(block);
        case TerminatorKind::SwitchString:
            return simplifySwitchString(block);
        case TerminatorKind::Jump:
        case TerminatorKind::Return:
        case TerminatorKind::ReturnCall:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool simplifyBranch(BlockID block)
    {
        const Terminator& terminator = m_function.at(block).terminator;
        bool changed = false;
        for (;;) {
            const Value& definition = m_function.at(definitionThroughUpcasts(m_function, terminator.conditionOperand()));
            if (definition.opcode != Opcode::LogicalNot)
                break;
            Terminator& negated = m_function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
            negated.setConditionOperand(definition.valueOperand());
            std::swap(negated.taken(), negated.notTaken());
            changed = true;
        }
        if (!terminator.taken().proofs.isEmpty() || terminator.taken() != terminator.notTaken())
            return changed;
        decideTerminator(m_builder, block, 0);
        return true;
    }

    bool simplifySwitch(BlockID block)
    {
        const Terminator& terminator = m_function.at(block).terminator;
        ValueID subject = terminator.subjectOperand();
        PayloadListView<int64_t> cases = terminator.cases();
        std::optional<uint32_t> decidedTarget = decidedTargetOf(terminator, knownValueOf(m_program, m_function, subject));

        const Type& subjectType = typeOf(subject);
        return simplifyCases<int64_t>(block, cases, decidedTarget, [&](int64_t integer) {
            return mayStrictlyEqual(subjectType, integer) && !isRuledOutByDefinition(m_program, m_function, subject, KnownValue::integer(integer));
        }, [](Terminator& edited, std::span<const int64_t> remainingCases) {
            edited.setCases(remainingCases);
        });
    }

    bool simplifySwitchString(BlockID block)
    {
        const Terminator& terminator = m_function.at(block).terminator;
        ValueID subject = terminator.subjectOperand();
        std::span<const StringID> strings = terminator.strings();
        std::optional<uint32_t> decidedTarget = decidedTargetOf(terminator, knownValueOf(m_program, m_function, subject));

        const Type& subjectType = typeOf(subject);
        return simplifyCases<StringID>(block, strings, decidedTarget, [&](StringID string) {
            return mayStrictlyEqual(subjectType, string);
        }, [](Terminator& edited, std::span<const StringID> remainingStrings) {
            edited.setStrings(remainingStrings);
        });
    }

    template<typename Case>
    bool simplifyCases(BlockID block, const auto& cases, std::optional<uint32_t> decidedTarget, NOESCAPE const Invocable<bool(Case)> auto& mayMatch, NOESCAPE const Invocable<void(Terminator&, std::span<const Case>)> auto& setCases)
    {
        if (decidedTarget && canDecide(Edge { block, *decidedTarget })) {
            decideTerminator(m_builder, block, *decidedTarget);
            return true;
        }

        Vector<uint32_t, 8> remainingIndexes;
        for (uint32_t index = 0; index < cases.size(); ++index) {
            const Terminator& terminator = m_function.at(block).terminator;
            if (mayMatch(cases[index]) && terminator.targets[index] != terminator.otherwise())
                remainingIndexes.append(index);
        }
        if (remainingIndexes.isEmpty()) {
            decideTerminator(m_builder, block, static_cast<uint32_t>(cases.size()));
            return true;
        }
        if (remainingIndexes.size() == cases.size())
            return false;

        Terminator& terminator = m_function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
        Vector<Case, 8> remainingCases;
        Vector<Target, 2> remainingTargets;
        for (uint32_t index : remainingIndexes) {
            remainingCases.append(cases[index]);
            remainingTargets.append(WTF::move(terminator.targets[index]));
        }
        remainingTargets.append(WTF::move(terminator.otherwise()));
        setCases(terminator, remainingCases.span());
        terminator.targets = WTF::move(remainingTargets);
        return true;
    }

    void bypassHandlers()
    {
        if (!m_analyses.cfg().hasHandlers())
            return;

        Vector<BypassedThrow> bypassedThrows;
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            if (throwsIntoIgnoringHandler(block))
                bypassedThrows.append({ block, findDeadValuesBeforeThrow(block) });
        }
        for (const BypassedThrow& bypassed : bypassedThrows) {
            if (bypassHandler(bypassed.block) && !bypassed.deadValues.isEmpty())
                m_builder.eraseValues(bypassed.block, bypassed.deadValues);
        }
        for (size_t index = 0; index < m_handlerBodies.size(); ++index)
            bypassHandler(m_handlerBodies[index].second);
    }

    bool throwsIntoIgnoringHandler(BlockID id) const
    {
        const Block& block = m_function.at(id);
        TerminatorKind kind = block.terminator.kind;
        if ((kind != TerminatorKind::Throw && kind != TerminatorKind::Rethrow) || !block.handler)
            return false;
        BlockID handler = block.handler->block;
        const Block& handlerBlock = m_function.at(handler);
        return handler != id && handlerBlock.role == BlockRole::CatchHandler && !indexAfterCaughtValues(m_function, handlerBlock);
    }

    // Whether such an operation completes or throws, the code of the handler runs next with the same arguments, and it sees neither a result nor an exception.
    BitVector findDeadValuesBeforeThrow(BlockID id)
    {
        const Block& block = m_function.at(id);
        const UseLists& useLists = m_analyses.useLists();
        BitVector indexes(block.values.size());
        for (size_t index = block.values.size(); index--;) {
            ValueID value = block.values[index];
            Effects effects = m_analyses.effectsOf(value).onAnyEdge();
            effects.mayThrow = false;
            // A user in the block comes later, so it is among the operations found.
            bool isUsedElsewhere = std::ranges::any_of(useLists.usesOf(value), [&](const Use& use) {
                switch (use.kind) {
                case Use::Kind::Operand:
                case Use::Kind::TerminatorOperand:
                    return use.block != id;
                case Use::Kind::EdgeArgument:
                case Use::Kind::HandlerArgument:
                case Use::Kind::BlockSubject:
                    return true;
                }
                RELEASE_ASSERT_NOT_REACHED();
            });
            if (isUsedElsewhere || !effects.isRemovableWhenUnused())
                break;
            indexes.quickSet(index);
        }
        return indexes;
    }

    bool bypassHandler(BlockID id)
    {
        if (!throwsIntoIgnoringHandler(id))
            return false;

        BlockID body = bodyOf(m_function.at(id).handler->block);
        Block& throwingBlock = m_function.edit(id, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments });
        Arguments arguments = throwingBlock.handler->arguments;
        m_builder.setOrigin(throwingBlock.terminator.origin());
        throwingBlock.terminator = Terminator { };
        m_builder.moveToEndOf(id);
        m_builder.emitJump(body, WTF::move(arguments));
        // The builder reports the jump, not the terminator in whose place it stands.
        m_analyses.didChangeControlFlow();
        ++m_numberOfChanges;
        return true;
    }

    BlockID bodyOf(BlockID handler)
    {
        for (auto [knownHandler, body] : m_handlerBodies) {
            if (knownHandler == handler)
                return body;
        }
        BlockID body = moveBodyOutOf(handler);
        m_handlerBodies.append({ handler, body });
        return body;
    }

    BlockID moveBodyOutOf(BlockID handler)
    {
        BlockID body = m_builder.addBlock(m_function.at(handler).frequency);
        Arguments handlerParameters = m_function.at(handler).parameters;
        ValueSubstitution substitution(m_function);
        for (ValueID parameter : handlerParameters) {
            m_builder.setOrigin(m_function.at(parameter).origin());
            ValueID bodyParameter = m_builder.addParameter(body, m_function.at(parameter).type);
            m_function.edit(bodyParameter, ValueEdit::Type).setExpected(m_function.at(parameter).expected());
            substitution.replace(parameter, bodyParameter);
        }

        Block& source = m_function.edit(handler, { BlockEdit::Values, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments });
        Block& destination = m_function.edit(body, { BlockEdit::Values, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments });
        destination.values = std::exchange(source.values, { });
        destination.terminator = std::exchange(source.terminator, Terminator { });
        destination.handler = std::exchange(source.handler, std::nullopt);
        substitution.apply(m_function, &m_analyses);

        m_builder.setOrigin(destination.terminator.origin());
        m_builder.moveToEndOf(handler);
        m_builder.emitJump(body, WTF::move(handlerParameters));
        // The builder reports the jump, not what has moved into the body.
        m_analyses.didChangeControlFlow();
        return body;
    }

    bool canForwardThrough(BlockID id) const
    {
        const Block& block = m_function.at(id);
        if (!block.parameters.isEmpty() || !block.values.isEmpty() || block.isHandlerBlock() || block.terminator.kind != TerminatorKind::Jump)
            return false;
        BlockID target = block.terminator.next().block;
        BlockFrequency targetFrequency = m_function.at(target).frequency;
        return target != id && rarerOf(block.frequency, targetFrequency) == targetFrequency;
    }

    void collapseTestsThatDecideNothing()
    {
        Vector<BlockID, 8> sources = forwardEmptyBlocks();
        while (!sources.isEmpty()) {
            Vector<ValueID, 8> testedValues = simplifyTerminatorsOf(sources.span());
            if (testedValues.isEmpty())
                return;
            sources = eraseUnusedValues(WTF::move(testedValues));
            sources.appendVector(forwardEmptyBlocks());
        }
    }

    static ValueID testedValueOf(const Terminator& terminator)
    {
        switch (terminator.kind) {
        case TerminatorKind::Branch:
            return terminator.conditionOperand();
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
            return terminator.subjectOperand();
        case TerminatorKind::Jump:
        case TerminatorKind::Return:
        case TerminatorKind::ReturnCall:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            return { };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Vector<ValueID, 8> simplifyTerminatorsOf(std::span<const BlockID> blocks)
    {
        Vector<ValueID, 8> testedValues;
        bool changed = false;
        for (BlockID block : blocks) {
            ValueID tested = testedValueOf(m_function.at(block).terminator);
            if (!tested || !simplifyTerminator(block))
                continue;
            changed = true;
            m_onlyJoinedBlocks = false;
            ++m_numberOfChanges;
            if (m_function.at(block).terminator.kind == TerminatorKind::Jump)
                testedValues.append(tested);
        }
        if (changed)
            m_analyses.didChangeControlFlow();
        return testedValues;
    }

    Vector<BlockID, 8> eraseUnusedValues(Vector<ValueID, 8>&& worklist)
    {
        const CFG& cfg = m_analyses.cfg();
        const UseLists& useLists = m_analyses.useLists();
        HashMap<ValueID, unsigned> numberOfErasedUses;
        Vector<ValueLocation, 8> erased;
        auto eraseUseOf = [&](ValueID used) {
            ++numberOfErasedUses.add(used, 0).iterator->value;
            worklist.append(used);
        };
        while (!worklist.isEmpty()) {
            ValueID id = worklist.takeLast();
            ValueLocation location = useLists.locationOf(id);
            if (useLists.useCount(id) != numberOfErasedUses.get(id) || erased.contains(location))
                continue;
            switch (location.placement) {
            case ValuePlacement::Detached:
                continue;
            case ValuePlacement::Parameter:
                if (location.block == cfg.root())
                    continue;
                for (Edge edge : cfg.incomingEdges(location.block)) {
                    IncomingValue incoming = incomingValueOf(m_function, edge, location.index);
                    if (incoming.kind == IncomingValue::Kind::Argument)
                        eraseUseOf(incoming.argument);
                }
                break;
            case ValuePlacement::InBlock:
                if (!m_analyses.isRemovableWhenUnused(id))
                    continue;
                m_function.at(id).forEachOperand([&](ValueID operand, uint32_t) {
                    eraseUseOf(operand);
                });
                break;
            }
            erased.append(location);
        }
        if (erased.isEmpty())
            return { };

        Vector<BlockID, 8> sources;
        for (const ValueLocation& location : erased) {
            if (location.placement != ValuePlacement::Parameter)
                continue;
            for (Edge edge : cfg.incomingEdges(location.block)) {
                if (!edge.isHandlerEdge() && !sources.contains(edge.from))
                    sources.append(edge.from);
            }
        }

        AOT::sort(erased, { }, [](const ValueLocation& location) {
            return std::tuple(location.block, location.placement, location.index);
        });
        for (size_t first = 0; first < erased.size();) {
            size_t end = first;
            BitVector indexes;
            while (end < erased.size() && erased[end].block == erased[first].block && erased[end].placement == erased[first].placement)
                indexes.set(erased[end++].index);
            if (erased[first].placement == ValuePlacement::Parameter)
                m_builder.removeParameters(erased[first].block, indexes);
            else
                m_builder.eraseValues(erased[first].block, indexes);
            first = end;
        }
        m_numberOfChanges += erased.size();
        return sources;
    }

    Vector<BlockID, 8> forwardEmptyBlocks()
    {
        Vector<Edge, 8> forwardedEdges;
        for (BlockID from : m_analyses.cfg().reversePostOrder()) {
            const Terminator& terminator = m_function.at(from).terminator;
            if (terminator.kind == TerminatorKind::Jump)
                continue;
            for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
                if (canForwardThrough(terminator.targets[targetIndex].block))
                    forwardedEdges.append(Edge { from, targetIndex });
            }
        }
        if (forwardedEdges.isEmpty())
            return { };

        auto isInCycle = [&](BlockID block) {
            return m_analyses.isInCycle(block);
        };
        forwardedEdges.removeAllMatching([&](const Edge& edge) {
            return isInCycle(targetOf(m_function, edge).block);
        });

        Vector<BlockID, 8> lastEmptyBlocks(forwardedEdges.size(), [&](size_t index) {
            BlockID last = targetOf(m_function, forwardedEdges[index]).block;
            for (;;) {
                BlockID next = m_function.at(last).terminator.next().block;
                if (!canForwardThrough(next) || isInCycle(next))
                    return last;
                last = next;
            }
        });

        Vector<BlockID, 8> sources;
        for (size_t index = 0; index < forwardedEdges.size(); ++index) {
            Edge edge = forwardedEdges[index];
            const Target& next = m_function.at(lastEmptyBlocks[index]).terminator.next();
            m_builder.redirectEdge(edge, next.block, Arguments(next.arguments));
            if (sources.isEmpty() || sources.last() != edge.from)
                sources.append(edge.from);
        }
        m_numberOfChanges += forwardedEdges.size();
        return sources;
    }

    std::optional<BlockID> findMergeableSuccessor(const CFG& cfg, BlockID id) const
    {
        const Terminator& terminator = m_function.at(id).terminator;
        if (terminator.kind != TerminatorKind::Jump)
            return std::nullopt;
        BlockID next = terminator.next().block;
        if (next == id || next == cfg.root() || cfg.incomingEdges(next).size() != 1 || m_function.at(next).isHandlerBlock())
            return std::nullopt;
        return next;
    }

    bool mayThrowIn(BlockID id)
    {
        return m_analyses.effectTraitsOfTerminator(id).normal.mayThrow() || std::ranges::any_of(m_function.at(id).values, [&](ValueID value) {
            return m_analyses.mayThrow(value);
        });
    }

    void collectHandlerChoiceInputs(const CFG& cfg)
    {
        if (!cfg.hasHandlers())
            return;

        for (BlockID id : cfg.reversePostOrder()) {
            std::optional<BlockID> next = findMergeableSuccessor(cfg, id);
            if (!next)
                continue;
            for (BlockID member : { id, *next }) {
                if (!m_function.at(member).handler && !m_throwingUncoveredBlocks.get(member.index()) && mayThrowIn(member))
                    m_throwingUncoveredBlocks.set(member.index());
            }
            if (!m_function.at(id).handler && m_function.at(*next).handler)
                m_useListsBeforeMerging = &m_analyses.useLists();
        }
    }

    ValueID resolve(ValueID id) const { return m_replacements ? m_replacements->resolve(id) : id; }

    bool haveSameHandler(const Block& block, const Block& successor) const
    {
        if (!block.handler || !successor.handler)
            return !block.handler && !successor.handler;
        if (block.handler->block != successor.handler->block)
            return false;
        return std::ranges::equal(block.handler->arguments, successor.handler->arguments, [&](ValueID argument, ValueID successorArgument) {
            return resolve(argument) == resolve(successorArgument);
        });
    }

    bool hasUsesReachableFromHandler(const CFG& cfg, BlockID id, BlockID next) const
    {
        RELEASE_ASSERT(m_useListsBeforeMerging);
        const Block& block = m_function.at(id);
        const Block& successor = m_function.at(next);
        BitVector barrier(cfg.numNodes());
        barrier.set(id.index());
        BitVector reachableBlocks = cfg.blocksReachableFrom(successor.handler->block, barrier);

        auto isUsedOnHandlerPath = [&](ValueID definition) {
            return std::ranges::any_of(m_useListsBeforeMerging->usesOf(definition), [&](const Use& use) {
                return reachableBlocks.get(use.block.index()) || (use.kind == Use::Kind::HandlerArgument && use.block == next);
            });
        };
        auto isReplacedByValueOfBlock = [&](ValueID parameter) {
            return block.values.contains(resolve(parameter));
        };
        return std::ranges::any_of(block.values, isUsedOnHandlerPath)
            || std::ranges::any_of(successor.parameters, isUsedOnHandlerPath)
            || std::ranges::any_of(m_replacedParameters, [&](ValueID parameter) {
                return isUsedOnHandlerPath(parameter) && isReplacedByValueOfBlock(parameter);
            });
    }

    HandlerChoice chooseHandler(const CFG& cfg, BlockID id, BlockID next) const
    {
        const Block& block = m_function.at(id);
        const Block& successor = m_function.at(next);
        if (haveSameHandler(block, successor))
            return HandlerChoice::KeepPredecessorHandler;

        if (!successor.handler) {
            bool canBeCovered = !m_throwingUncoveredBlocks.get(next.index()) && successor.terminator.kind != TerminatorKind::ReturnCall;
            return canBeCovered ? HandlerChoice::KeepPredecessorHandler : HandlerChoice::CannotMerge;
        }

        if (block.handler || m_throwingUncoveredBlocks.get(id.index()) || hasUsesReachableFromHandler(cfg, id, next))
            return HandlerChoice::CannotMerge;
        return HandlerChoice::TakeSuccessorHandler;
    }

    bool mergeSuccessorInto(const CFG& cfg, BlockID id)
    {
        std::optional<BlockID> next = findMergeableSuccessor(cfg, id);
        if (!next)
            return false;
        HandlerChoice choice = chooseHandler(cfg, id, *next);
        if (choice == HandlerChoice::CannotMerge)
            return false;

        if (!m_replacements)
            m_replacements.emplace(m_function);
        Block& block = m_function.edit(id);
        Arguments arguments = WTF::move(block.terminator.next().arguments);
        Block successor = std::exchange(m_function.edit(*next), Block { });
        m_onlyJoinedBlocks &= successor.parameters.isEmpty();
        for (unsigned index = 0; index < successor.parameters.size(); ++index) {
            ValueID parameter = successor.parameters[index];
            m_replacedParameterByKnownValue = m_replacedParameterByKnownValue || m_function.at(arguments[index]).type != m_function.at(parameter).type || knownValueOf(m_program, m_function, arguments[index]).isKnown();
            if (canReplaceParameter(m_function, arguments[index], parameter)) {
                m_replacements->replace(parameter, arguments[index]);
                m_replacedParameters.append(parameter);
                continue;
            }
            becomeUpcast(m_function, parameter, arguments[index]);
            block.values.append(parameter);
        }
        block.values.appendVector(successor.values);
        block.terminator = WTF::move(successor.terminator);
        if (choice == HandlerChoice::TakeSuccessorHandler)
            block.handler = WTF::move(successor.handler);
        m_mayFindMoreAtOnce |= rarerOf(block.frequency, successor.frequency) != block.frequency || (block.values.isEmpty() && block.parameters.isEmpty());
        block.frequency = rarerOf(block.frequency, successor.frequency);
        if (m_throwingUncoveredBlocks.get(next->index()))
            m_throwingUncoveredBlocks.set(id.index());
        ++m_numberOfChanges;
        return true;
    }

    void mergeChains(const CFG& cfg)
    {
        collectHandlerChoiceInputs(cfg);
        for (BlockID id : cfg.reversePostOrder()) {
            m_replacedParameters.shrink(0);
            while (mergeSuccessorInto(cfg, id)) { }
        }
        if (m_replacements)
            m_replacements->apply(m_function, &m_analyses);
    }

    const Program& m_program;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    IRBuilder m_builder;
    bool m_validates;
    bool m_looksAcrossBlocks;
    Vector<std::pair<BlockID, BlockID>, 2> m_handlerBodies;
    std::optional<ValueSubstitution> m_replacements;
    Vector<ValueID, 8> m_replacedParameters;
    BitVector m_throwingUncoveredBlocks;
    const UseLists* m_useListsBeforeMerging { nullptr };
    size_t m_numberOfChanges { 0 };
    bool m_onlyJoinedBlocks { false };
    bool m_replacedParameterByKnownValue { false };
    bool m_mayFindMoreAtOnce { false };
};

} // anonymous namespace

PassOutcome simplifyControlFlow(FunctionPassContext& context)
{
    ControlFlowSimplifier simplifier(context);
    PassOutcome outcome = simplifier.run();
    context.lookedOnlyNearby = !context.mayDecideTestsAcrossBlocks;
    if (outcome == PassOutcome::Unchanged)
        return outcome;
    context.keptAnalyses = allAnalyses;
    if (!simplifier.replacedParametersByKnownValues())
        context.passesThatCanWait.add(PassID::FoldConstants);
    if (!simplifier.onlyJoinedBlocks())
        return outcome;
    context.passesLeftIdle = { PassID::RemoveTrivialParameters, PassID::PropagateBranchConditions, PassID::EliminateDeadCode };
    if (!context.foldingAskedForUses)
        context.passesLeftIdle.add(PassID::FoldConstants);
    context.reachedFixpoint = !simplifier.mayFindMoreAtOnce();
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
