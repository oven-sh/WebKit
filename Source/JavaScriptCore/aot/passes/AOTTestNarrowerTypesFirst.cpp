/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

// A value is tested for what flow expects of it (undefined|number), and what has passed is tested at once for what its use needs (number).
struct TestChain {
    BlockID wider;
    BlockID narrower;
    BlockFrequency frequencyOfRest;
};

class NarrowerTestsFirst {
public:
    explicit NarrowerTestsFirst(FunctionPassContext& context)
        : m_context(context)
        , m_function(context.function)
        , m_analyses(context.analyses)
    {
    }

    PassOutcome run()
    {
        collectChains();
        if (m_chains.isEmpty())
            return PassOutcome::Unchanged;

        IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
        for (const TestChain& chain : m_chains)
            rewrite(builder, chain);
        // The builder reports the blocks and the edges, not the type that a test now tests for.
        m_analyses.didChangeControlFlow();
        m_context.keptAnalyses = allAnalyses;
        return PassOutcome::Changed;
    }

private:
    const Value* testThatEnds(const Block& block) const
    {
        if (block.terminator.kind != TerminatorKind::Branch || block.values.isEmpty() || block.terminator.conditionOperand() != block.values.last())
            return nullptr;
        const Value& test = m_function.at(block.values.last());
        if (test.opcode != Opcode::TypeTest || m_analyses.useLists().useCount(block.values.last()) != 1)
            return nullptr;
        return &test;
    }

    static bool passesOnlyWhatWasProven(const Target& target)
    {
        return target.arguments.isEmpty() && target.proofs.size() == 1 && !target.proofs[0];
    }

    void collectChains()
    {
        const CFG& cfg = m_analyses.cfg();
        BitVector isTaken(m_function.blocks.size());
        for (BlockID id : cfg.reversePostOrder()) {
            const Block& block = m_function.at(id);
            if (isTaken.quickGet(id.index()) || block.frequency == BlockFrequency::Rare || block.handler)
                continue;
            const Value* wider = testThatEnds(block);
            if (!wider || !passesOnlyWhatWasProven(block.terminator.targets[0]))
                continue;

            BlockID next = block.terminator.targets[0].block;
            const Block& nextBlock = m_function.at(next);
            if (next == id || nextBlock.handler || nextBlock.isHandlerBlock() || nextBlock.values.size() != 1 || nextBlock.parameters.size() != 1 || !cfg.singleIncomingEdge(next))
                continue;
            const Value* narrower = testThatEnds(nextBlock);
            ValueID proven = nextBlock.parameters[0];
            if (!narrower || narrower->valueOperand() != proven || m_analyses.useLists().useCount(proven) != 1)
                continue;
            if (nextBlock.terminator.targets[0].block == next || nextBlock.terminator.targets[0].block == id)
                continue;
            const Type& widerType = m_context.types.at(wider->testedType());
            const Type& narrowerType = m_context.types.at(narrower->testedType());
            if (wider->testedType() == narrower->testedType() || !narrowerType.isSubsetOf(widerType, m_context.program))
                continue;
            // `links[id] ??= ..`, `session().project`: the second test asks whether there is nothing. Mostly there is something, which would fail it, then pass the first test on the failing side and be asked again.
            if (narrowerType.isUndefinedOrNull() || m_function.at(nextBlock.terminator.targets[0].block).frequency > m_function.at(nextBlock.terminator.targets[1].block).frequency)
                continue;

            isTaken.quickSet(id.index());
            isTaken.quickSet(next.index());
            // Where the first test admits nothing beside the second but undefined and null, flow expects `undefined|number` and the use needs a number: the rest is unlikely.
            bool restIsNothing = widerType.withoutNullish().isSubsetOf(narrowerType, m_context.program);
            m_chains.append({ id, next, restIsNothing ? BlockFrequency::Unlikely : block.frequency });
        }
    }

    void rewrite(IRBuilder& builder, const TestChain& chain)
    {
        ValueID widerID = m_function.at(chain.wider).values.last();
        ValueID tested = m_function.at(widerID).valueOperand();
        TypeID widerType = m_function.at(widerID).testedType();
        TypeID narrowerType = m_function.at(m_function.at(chain.narrower).values.last()).testedType();
        Target taken = m_function.at(chain.wider).terminator.targets[0];
        Target notTaken = m_function.at(chain.wider).terminator.targets[1];
        Target provenNarrower = m_function.at(chain.narrower).terminator.targets[0];

        builder.setOrigin(m_function.at(widerID).origin());
        BlockID failed = builder.addBlock(chain.frequencyOfRest);
        builder.moveToEndOf(failed);
        ValueID widerTest = builder.appendTypeTest(tested, widerType);
        builder.emitBranch(widerTest, WTF::move(taken), WTF::move(notTaken));

        m_function.edit(widerID, ValueEdit::Definition).setTestedType(narrowerType);
        builder.redirectEdge(Edge { chain.wider, 0 }, WTF::move(provenNarrower));
        builder.redirectEdge(Edge { chain.wider, 1 }, Target { failed, { }, { } });
    }

    FunctionPassContext& m_context;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    Vector<TestChain> m_chains;
};

} // anonymous namespace

PassOutcome testNarrowerTypesFirst(FunctionPassContext& context)
{
    return NarrowerTestsFirst(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
