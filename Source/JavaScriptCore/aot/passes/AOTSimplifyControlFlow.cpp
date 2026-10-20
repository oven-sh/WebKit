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
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

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
    {
    }

    PassOutcome run()
    {
        Vector<BlockID> reachableBlocks(m_analyses.cfg().reversePostOrder());
        decideBranches(reachableBlocks);
        for (BlockID block : reachableBlocks)
            m_numberOfChanges += simplifyTerminator(block);
        if (m_numberOfChanges)
            m_analyses.didChangeControlFlow();

        bypassHandlers();
        forwardEmptyBlocks();
        m_numberOfChanges += m_builder.emptyUnreachableBlocks();
        mergeChains(m_analyses.cfg());
        if (!m_numberOfChanges)
            return PassOutcome::Unchanged;

        m_analyses.didChangeControlFlow();
        return PassOutcome::Changed;
    }

private:
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
        TypeTestBranches typeTests(m_analyses);
        SlotValueTests slotValueTests(m_analyses, m_builder.types());
        Vector<std::pair<BlockID, uint32_t>> decided;
        for (BlockID block : reachableBlocks) {
            if (m_function.at(block).terminator.kind != TerminatorKind::Branch)
                continue;

            uint32_t target = 0;
            switch (evaluateCondition(m_analyses, typeTests, slotValueTests, block)) {
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
        Terminator& terminator = m_function.at(block).terminator;
        bool changed = false;
        for (;;) {
            const Value& definition = m_function.at(definitionThroughUpcasts(m_function, terminator.conditionOperand()));
            if (definition.opcode != Opcode::LogicalNot)
                break;
            terminator.setConditionOperand(definition.valueOperand());
            std::swap(terminator.taken(), terminator.notTaken());
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

        Terminator& terminator = m_function.at(block).terminator;
        Vector<uint32_t, 8> remainingIndexes;
        for (uint32_t index = 0; index < cases.size(); ++index) {
            if (mayMatch(cases[index]) && terminator.targets[index] != terminator.otherwise())
                remainingIndexes.append(index);
        }
        if (remainingIndexes.isEmpty()) {
            decideTerminator(m_builder, block, static_cast<uint32_t>(cases.size()));
            return true;
        }
        if (remainingIndexes.size() == cases.size())
            return false;

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
        Block& throwingBlock = m_function.at(id);
        Arguments arguments = throwingBlock.handler->arguments;
        m_builder.setOrigin(throwingBlock.terminator.origin());
        throwingBlock.terminator = Terminator { };
        m_builder.moveToEndOf(id);
        m_builder.emitJump(body, WTF::move(arguments));
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
            m_function.at(bodyParameter).setWritten(m_function.at(parameter).written());
            m_function.at(bodyParameter).setPrediction(m_function.at(parameter).prediction());
            substitution.replace(parameter, bodyParameter);
        }

        Block& source = m_function.at(handler);
        Block& destination = m_function.at(body);
        destination.values = std::exchange(source.values, { });
        destination.terminator = std::exchange(source.terminator, Terminator { });
        destination.handler = std::exchange(source.handler, std::nullopt);
        substitution.apply(m_function, &m_analyses);

        m_builder.setOrigin(destination.terminator.origin());
        m_builder.moveToEndOf(handler);
        m_builder.emitJump(body, WTF::move(handlerParameters));
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

    void forwardEmptyBlocks()
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
            return;

        const BlockCycles& cycles = m_analyses.blockCycles();
        forwardedEdges.removeAllMatching([&](const Edge& edge) {
            return cycles.isInCycle(targetOf(m_function, edge).block);
        });

        for (Edge edge : forwardedEdges) {
            const Target& next = m_function.at(targetOf(m_function, edge).block).terminator.next();
            m_builder.redirectEdge(edge, next.block, Arguments(next.arguments));
        }
        m_numberOfChanges += forwardedEdges.size();
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
        return m_analyses.effectsOfTerminator(id).mayThrow() || std::ranges::any_of(m_function.at(id).values, [&](ValueID value) {
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
        Block& block = m_function.at(id);
        Arguments arguments = WTF::move(block.terminator.next().arguments);
        Block successor = std::exchange(m_function.at(*next), Block { });
        for (unsigned index = 0; index < successor.parameters.size(); ++index) {
            ValueID parameter = successor.parameters[index];
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
    Vector<std::pair<BlockID, BlockID>, 2> m_handlerBodies;
    std::optional<ValueSubstitution> m_replacements;
    Vector<ValueID, 8> m_replacedParameters;
    BitVector m_throwingUncoveredBlocks;
    const UseLists* m_useListsBeforeMerging { nullptr };
    size_t m_numberOfChanges { 0 };
};

} // anonymous namespace

PassOutcome simplifyControlFlow(FunctionPassContext& context)
{
    return ControlFlowSimplifier(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
