/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "FPRInfo.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

// Hot / cold function splitting, with no profile: the blocks that the entry reaches without a call stay, and so need no frame. An edge that
// leaves them becomes a jump to a function of its own, which is handed what is live. Nothing runs twice, and control never comes back.
class FunctionSplitting {
public:
    FunctionSplitting(const Program& program, FunctionID id, const Function& function, const BitVector& blocksThatEmitCalls, const BitVector& blocksThatAreNotRun)
        : m_program(program)
        , m_id(id)
        , m_function(function)
        , m_numberOfBlocks(function.blocks.size())
        , m_needsFrame(blocksThatEmitCalls)
        , m_isNotRun(blocksThatAreNotRun)
    {
    }

    bool run(SplitFunction& result)
    {
        if (!mayBeSplit())
            return false;
        BitVector isReachable = reachableFrom(0, { });
        m_needsFrame.filter(isReachable);
        if (m_needsFrame.isEmpty())
            return false;
        findCycles();
        includeCycles(isReachable);
        if (m_needsFrame.get(0))
            return false;

        // One continuation for each block that needs a frame and is entered from one that does not. Each is a copy of all it reaches.
        Vector<unsigned> cuts;
        for (unsigned block : reachableFrom(0, m_needsFrame)) {
            for (auto& target : m_function.blocks[block].terminator.targets) {
                if (m_needsFrame.get(target.block.index()) && !cuts.contains(target.block.index()))
                    cuts.append(target.block.index());
            }
        }
        std::ranges::sort(cuts);
        Vector<BitVector> reachedFrom(cuts.size());
        Vector<BitVector> cutsJumpedTo(cuts.size());
        if (cuts.size() <= Value::maxContinuations && !keepsSomethingInItsFrame()) {
            for (unsigned i = 0; i < cuts.size(); ++i)
                reachedFrom[i] = reachableFrom(cuts[i], { });
            for (unsigned i = 0; i < cuts.size(); ++i) {
                for (unsigned k = 0; k < cuts.size(); ++k) {
                    if (i != k && m_isNotRun.get(cuts[k]) && reachedFrom[i].get(cuts[k]) && !reachedFrom[k].get(cuts[i]))
                        cutsJumpedTo[i].set(cuts[k]);
                }
            }
        }
        size_t sizeOfCopies = 0;
        size_t mostCopied = 3 * sizeOf(isReachable) / 2;
        for (unsigned i = 0; i < cuts.size() && sizeOfCopies <= mostCopied; ++i)
            sizeOfCopies += sizeOf(reachableFrom(cuts[i], cutsJumpedTo[i]));
        // Too much: one continuation, from where all ways to a call go through.
        if (sizeOfCopies > mostCopied || cuts.size() > Value::maxContinuations) {
            unsigned cut = nearestCommonDominator(isReachable);
            while (cut && isInCycleWith(cut, reachableFrom(0, only(cut))))
                cut = m_dominator[cut];
            if (!cut)
                return false;
            cuts = { cut };
            reachedFrom = { reachableFrom(cut, { }) };
            cutsJumpedTo = { BitVector() };
        }
        BitVector isCut(m_numberOfBlocks);
        for (unsigned cut : cuts)
            isCut.set(cut);
        BitVector staysBehind = reachableFrom(0, isCut);
        bool returns = false;
        for (unsigned block : staysBehind)
            returns |= m_function.blocks[block].terminator.kind == TerminatorKind::Return;
        if (!returns)
            return false;

        Vector<unsigned> order;
        for (unsigned i = 0; i < cuts.size(); ++i)
            order.append(i);
        std::ranges::sort(order, [&](unsigned a, unsigned b) {
            return reachedFrom[a].bitCount() < reachedFrom[b].bitCount();
        });
        Vector<Arguments> liveInto(cuts.size());
        result.continuations.grow(cuts.size());
        for (unsigned which : order) {
            result.continuations[which] = makeContinuation(cuts, which, cutsJumpedTo[which], liveInto);
            if (!fitsInRegisters(result.continuations[which])) {
                result.continuations.clear();
                return false;
            }
        }
        result.entry = makeEntry(staysBehind, cuts, liveInto);
        return true;
    }

private:
    bool mayBeSplit() const
    {
        if (Options::auditAOTTypedAssertions() || m_program.isEntry(m_id) || !m_function.isTyped())
            return false;
        if (m_function.kind != FunctionKind::Plain && m_function.kind != FunctionKind::Arrow && m_function.kind != FunctionKind::Method)
            return false;
        if (m_function.has(FunctionIsGenericEntrypoint | FunctionUsesArgumentBuffer) || m_function.declaredAs || m_function.resumeFunction || m_function.startFunction || m_function.savesItsFrame || m_program.isResumeFunction(m_function))
            return false;
        for (auto& block : m_function.blocks) {
            auto& terminator = block.terminator;
            if (terminator.kind == TerminatorKind::Invoke || (terminator.kind == TerminatorKind::Throw && !terminator.targets.isEmpty()))
                return false;
            for (auto& target : terminator.targets) {
                if (!target.block.index() || target.arguments.size() != m_function.at(target.block).parameters.size())
                    return false;
            }
        }
        return true;
    }

    bool keepsSomethingInItsFrame() const
    {
        return !m_function.environmentsInFrame.isEmpty() || !m_function.mayPointIntoFrame.isEmpty() || m_function.copiesFirstArgumentOutOfFrame || m_function.values.containsIf([](auto& value) {
            return value.isClosureInFrame();
        });
    }

    // Without entering a block of `barrier`.
    BitVector reachableFrom(unsigned start, const BitVector& barrier) const
    {
        BitVector result(m_numberOfBlocks);
        result.set(start);
        Vector<unsigned, 16> worklist { start };
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                unsigned block = target.block.index();
                if (!barrier.get(block) && !result.set(block))
                    worklist.append(block);
            }
        }
        return result;
    }

    size_t sizeOf(const BitVector& blocks) const
    {
        size_t size = 0;
        for (unsigned block : blocks)
            size += m_function.blocks[block].values.size() + 1;
        return size;
    }

    BitVector only(unsigned block) const
    {
        BitVector result(m_numberOfBlocks);
        result.set(block);
        return result;
    }

    // Strongly connected components of what the entry reaches, iteratively. A component is named after one of its blocks.
    void findCycles()
    {
        constexpr unsigned none = UINT_MAX;
        m_component.fill(none, m_numberOfBlocks);
        Vector<unsigned> index(FillWith { }, m_numberOfBlocks, none);
        Vector<unsigned> low(FillWith { }, m_numberOfBlocks, 0u);
        BitVector isOnStack(m_numberOfBlocks);
        Vector<unsigned> stack;
        // A block, and the next of its successors to look at.
        Vector<std::pair<unsigned, unsigned>> work { { 0u, 0u } };
        unsigned next = 0;
        while (!work.isEmpty()) {
            auto [block, successor] = work.last();
            if (!successor) {
                index[block] = low[block] = next++;
                stack.append(block);
                isOnStack.set(block);
            }
            auto& targets = m_function.blocks[block].terminator.targets;
            if (successor < targets.size()) {
                ++work.last().second;
                unsigned target = targets[successor].block.index();
                if (target == block)
                    m_isInCycle.set(block);
                if (index[target] == none)
                    work.append({ target, 0u });
                else if (isOnStack.get(target))
                    low[block] = std::min(low[block], index[target]);
                continue;
            }
            work.removeLast();
            if (!work.isEmpty())
                low[work.last().first] = std::min(low[work.last().first], low[block]);
            if (low[block] != index[block])
                continue;
            bool isCycle = stack.last() != block;
            for (;;) {
                unsigned member = stack.takeLast();
                isOnStack.clear(member);
                m_component[member] = block;
                if (isCycle)
                    m_isInCycle.set(member);
                if (member == block)
                    break;
            }
        }
    }

    // A loop with a call in it needs a frame as a whole. So a continuation is entered from outside every cycle it is part of, and what is
    // live into it is defined before it, once.
    void includeCycles(const BitVector& isReachable)
    {
        BitVector componentNeedsFrame(m_numberOfBlocks);
        for (unsigned block : m_needsFrame) {
            if (m_isInCycle.get(block))
                componentNeedsFrame.set(m_component[block]);
        }
        for (unsigned block : isReachable) {
            if (m_isInCycle.get(block) && componentNeedsFrame.get(m_component[block]))
                m_needsFrame.set(block);
        }
    }

    bool isInCycleWith(unsigned block, const BitVector& others) const
    {
        if (!m_isInCycle.get(block))
            return false;
        for (unsigned other : others) {
            if (m_component[other] == m_component[block])
                return true;
        }
        return false;
    }

    // Of the blocks that need a frame. 0: the entry.
    unsigned nearestCommonDominator(const BitVector& isReachable)
    {
        Vector<unsigned> postorder;
        {
            BitVector isSeen(m_numberOfBlocks);
            isSeen.set(0);
            Vector<std::pair<unsigned, unsigned>> stack { { 0u, 0u } };
            while (!stack.isEmpty()) {
                auto& [block, next] = stack.last();
                auto& targets = m_function.blocks[block].terminator.targets;
                if (next == targets.size()) {
                    postorder.append(block);
                    stack.removeLast();
                    continue;
                }
                unsigned target = targets[next++].block.index();
                if (!isSeen.set(target))
                    stack.append({ target, 0u });
            }
        }
        constexpr unsigned none = UINT_MAX;
        Vector<unsigned> number(FillWith { }, m_numberOfBlocks, none);
        for (unsigned i = 0; i < postorder.size(); ++i)
            number[postorder[i]] = i;
        Vector<Vector<unsigned, 2>> predecessors(m_numberOfBlocks);
        for (unsigned block : isReachable) {
            for (auto& target : m_function.blocks[block].terminator.targets)
                predecessors[target.block.index()].append(block);
        }
        auto& dominator = m_dominator;
        dominator.fill(none, m_numberOfBlocks);
        dominator[0] = 0;
        auto intersect = [&](unsigned a, unsigned b) {
            while (a != b) {
                while (number[a] < number[b])
                    a = dominator[a];
                while (number[b] < number[a])
                    b = dominator[b];
            }
            return a;
        };
        for (bool didChange = !tryFindImmediateDominators(postorder.size(), [&](unsigned i) { return postorder[postorder.size() - 1 - i]; }, predecessors, dominator); didChange;) {
            didChange = false;
            for (unsigned i = postorder.size() - 1; i--;) {
                unsigned block = postorder[i];
                unsigned found = none;
                for (unsigned predecessor : predecessors[block]) {
                    if (dominator[predecessor] != none)
                        found = found == none ? predecessor : intersect(found, predecessor);
                }
                if (std::exchange(dominator[block], found) != found)
                    didChange = true;
            }
        }
        unsigned result = none;
        for (unsigned block : m_needsFrame)
            result = result == none ? block : intersect(result, block);
        return result;
    }

    static bool isConstant(Opcode opcode)
    {
        switch (opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::BigInt:
        case Opcode::FunctionReference:
        case Opcode::BuiltinFunction:
            return true;
        default:
            return false;
        }
    }

    static Value parameterLike(const Value& value, unsigned index)
    {
        Value parameter;
        parameter.opcode = Opcode::Parameter;
        parameter.rep = value.rep;
        parameter.position = value.position;
        parameter.inlineFrame = value.inlineFrame;
        parameter.type = value.type;
        parameter.takePredictionOf(value);
        parameter.immediates = { 0, index };
        return parameter;
    }

    // Values and blocks keep their numbers. `live`: what it takes, before what the block itself takes.
    Function makeContinuation(const Vector<unsigned>& cuts, unsigned which, const BitVector& cutsJumpedTo, Vector<Arguments>& liveInto) const
    {
        unsigned cut = cuts[which];
        Arguments& live = liveInto[which];
        Function result;
        result.name = m_function.name;
        result.module = m_function.module;
        result.position = m_function.position;
        result.flags = m_function.flags & (Format::FunctionIsTyped | Format::FunctionIsBuiltin | Format::FunctionIsOfBuiltinModule | Format::FunctionIsSloppy);
        result.resultType = m_function.resultType;
        result.resultTypes = m_function.resultTypes;
        result.writes = m_function.writes;
        result.writesShapeWords = m_function.writesShapeWords;
        result.writesIfItReturns = m_function.writesIfItReturns;
        result.writesShapeWordsIfItReturns = m_function.writesShapeWordsIfItReturns;
        result.convertsArrayStorage = m_function.convertsArrayStorage;
        result.convertsArrayStorageIfItReturns = m_function.convertsArrayStorageIfItReturns;
        result.inlineFrames = m_function.inlineFrames;
        result.values = m_function.values;
        result.environmentsInFrame = m_function.environmentsInFrame;
        result.mayPointIntoFrame = m_function.mayPointIntoFrame;
        result.copiesFirstArgumentOutOfFrame = m_function.copiesFirstArgumentOutOfFrame;
        result.numberOfCapturesOfCopiedArgument = m_function.numberOfCapturesOfCopiedArgument;
        result.blocks.grow(m_numberOfBlocks);

        BitVector blocks = reachableFrom(cut, cutsJumpedTo);
        BitVector isDefined(m_function.values.size());
        for (unsigned block : blocks) {
            result.blocks[block] = m_function.blocks[block];
            for (ValueID parameter : m_function.blocks[block].parameters)
                isDefined.set(parameter.index());
            for (ValueID value : m_function.blocks[block].values)
                isDefined.set(value.index());
        }
        for (unsigned block : BitVector(blocks)) {
            for (unsigned i = 0; i < result.blocks[block].terminator.targets.size(); ++i) {
                if (!cutsJumpedTo.get(result.blocks[block].terminator.targets[i].block.index()))
                    continue;
                jumpToContinuation(result, block, i, cuts, liveInto);
                blocks.set(result.blocks.size() - 1);
                for (ValueID value : result.blocks.last().values)
                    isDefined.set(value.index());
            }
        }
        BitVector isUsed(m_function.values.size());
        auto use = [&](ValueID& value) {
            if (!isDefined.get(value.index()))
                isUsed.set(value.index());
        };
        for (unsigned block : blocks) {
            for (ValueID value : result.blocks[block].values)
                result.at(value).forEachChild(use);
            result.blocks[block].terminator.forEachChild(use);
        }

        // Whoever looks through a cast may ask for what is under it.
        Vector<unsigned> worklist;
        for (unsigned index : isUsed)
            worklist.append(index);
        while (!worklist.isEmpty()) {
            auto& value = m_function.values[worklist.takeLast()];
            if (value.opcode == Opcode::Cast && !isDefined.get(value.child(0).index()) && !isUsed.set(value.child(0).index()))
                worklist.append(value.child(0).index());
        }

        // What is passed keeps its operation, though no block has it and nothing lowers it: the lowering goes by how a value came about
        // (keyedAccessOf(), isLocalAllocation(), the operand of a cast) as it does in the whole function, and so stays within what was summed up for that.
        auto& entry = result.blocks[0];
        for (unsigned index : isUsed) {
            ValueID id(index);
            if (isConstant(result.at(id).opcode)) {
                entry.values.append(id);
                continue;
            }
            entry.parameters.append(id);
            live.append(id);
        }
        Target target { BlockID(cut), { } };
        for (ValueID parameter : m_function.blocks[cut].parameters) {
            result.values.append(parameterLike(m_function.at(parameter), entry.parameters.size()));
            ValueID id(result.values.size() - 1);
            entry.parameters.append(id);
            target.arguments.append(id);
        }
        entry.terminator.kind = TerminatorKind::Jump;
        entry.terminator.targets.append(WTF::move(target));
        return result;
    }

    // As Lowering::lowerEntryParameters() counts them: by the representation of the type, which need not be how the value is kept.
    bool fitsInRegisters(const Function& function) const
    {
        unsigned numberOfGPRs = 0;
        unsigned numberOfFPRs = 0;
        for (ValueID parameter : function.blocks[0].parameters) {
            if (function.at(parameter).rep == Rep::Void)
                return false;
            Rep rep = repOf(m_program.at(function.at(parameter).type));
            ++(rep == Rep::Double || rep == Rep::OptionalDouble ? numberOfFPRs : numberOfGPRs);
        }
        return numberOfGPRs <= numberOfDirectArgumentGPRs && numberOfFPRs <= FPRInfo::numberOfArgumentRegisters;
    }

    Function makeEntry(const BitVector& staysBehind, const Vector<unsigned>& cuts, const Vector<Arguments>& liveInto) const
    {
        Function result = m_function;
        for (unsigned block = 0; block < m_numberOfBlocks; ++block) {
            if (!staysBehind.get(block)) {
                result.blocks[block] = Block { };
                continue;
            }
            for (unsigned i = 0; i < result.blocks[block].terminator.targets.size(); ++i) {
                if (cuts.contains(result.blocks[block].terminator.targets[i].block.index()))
                    jumpToContinuation(result, block, i, cuts, liveInto);
            }
        }
        return result;
    }

    void jumpToContinuation(Function& result, unsigned block, unsigned target, const Vector<unsigned>& cuts, const Vector<Arguments>& liveInto) const
    {
        size_t which = cuts.find(result.blocks[block].terminator.targets[target].block.index());
        TypeID resultType = m_function.resultType ? m_function.resultType : m_program.types.anything;
        Value jump;
        jump.opcode = Opcode::CallDirect;
        jump.type = resultType;
        jump.rep = repOf(m_program.at(resultType));
        jump.position = m_function.position;
        jump.immediates = { m_id.index(), Value::jumpsToContinuation + static_cast<uint32_t>(which) };
        jump.arguments = liveInto[which];
        Block leaving;
        // As the parameter of the block is kept: see Lowering::edgeTo().
        auto& parameters = m_function.blocks[cuts[which]].parameters;
        Arguments passed = result.blocks[block].terminator.targets[target].arguments;
        for (unsigned k = 0; k < passed.size(); ++k) {
            auto& parameter = m_function.at(parameters[k]);
            if (result.at(passed[k]).type != parameter.type) {
                Value upcast;
                upcast.opcode = Opcode::Cast;
                upcast.type = parameter.type;
                upcast.rep = parameter.rep;
                upcast.position = m_function.position;
                upcast.takePredictionOf(parameter);
                upcast.children[0] = passed[k];
                upcast.immediates[1] = static_cast<uint32_t>(CastReason::Upcast);
                result.values.append(WTF::move(upcast));
                passed[k] = ValueID(result.values.size() - 1);
                leaving.values.append(passed[k]);
            }
            jump.arguments.append(passed[k]);
        }
        result.values.append(WTF::move(jump));
        ValueID id(result.values.size() - 1);
        leaving.values.append(id);
        leaving.terminator.kind = TerminatorKind::Return;
        leaving.terminator.value = id;
        result.blocks.append(WTF::move(leaving));
        result.blocks[block].terminator.targets[target] = Target { BlockID(result.blocks.size() - 1), { } };
    }

    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    unsigned m_numberOfBlocks;
    BitVector m_needsFrame;
    const BitVector& m_isNotRun;
    Vector<unsigned> m_component;
    BitVector m_isInCycle;
    Vector<unsigned> m_dominator;
};

} // anonymous namespace

bool splitFunction(const Program& program, FunctionID id, const Function& function, const BitVector& blocksThatEmitCalls, const BitVector& blocksThatAreNotRun, SplitFunction& result)
{
    return FunctionSplitting(program, id, function, blocksThatEmitCalls, blocksThatAreNotRun).run(result);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
