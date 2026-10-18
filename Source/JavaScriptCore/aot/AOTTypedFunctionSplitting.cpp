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

class FunctionSplitting {
public:
    FunctionSplitting(const Program& program, FunctionID id, const Function& function, const BitVector& blocksWithCalls, const BitVector& unreachableBlocks)
        : m_program(program)
        , m_id(id)
        , m_function(function)
        , m_numberOfBlocks(function.blocks.size())
        , m_needsFrame(blocksWithCalls)
        , m_isUnreachable(unreachableBlocks)
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

        Vector<unsigned> splitPoints;
        for (unsigned block : reachableFrom(0, m_needsFrame)) {
            for (auto& target : m_function.blocks[block].terminator.targets) {
                if (m_needsFrame.get(target.block.index()) && !splitPoints.contains(target.block.index()))
                    splitPoints.append(target.block.index());
            }
        }
        std::ranges::sort(splitPoints);
        Vector<BitVector> reachedFrom(splitPoints.size());
        Vector<BitVector> cutTargets(splitPoints.size());
        if (splitPoints.size() <= Value::maxContinuations && !usesFrameStorage()) {
            for (unsigned i = 0; i < splitPoints.size(); ++i)
                reachedFrom[i] = reachableFrom(splitPoints[i], { });
            for (unsigned i = 0; i < splitPoints.size(); ++i) {
                for (unsigned k = 0; k < splitPoints.size(); ++k) {
                    if (i != k && m_isUnreachable.get(splitPoints[k]) && reachedFrom[i].get(splitPoints[k]) && !reachedFrom[k].get(splitPoints[i]))
                        cutTargets[i].set(splitPoints[k]);
                }
            }
        }
        size_t copiedSize = 0;
        size_t maxCopiedValues = 3 * sizeOf(isReachable) / 2;
        for (unsigned i = 0; i < splitPoints.size() && copiedSize <= maxCopiedValues; ++i)
            copiedSize += sizeOf(reachableFrom(splitPoints[i], cutTargets[i]));
        if (copiedSize > maxCopiedValues || splitPoints.size() > Value::maxContinuations) {
            unsigned splitPoint = nearestCommonDominator(isReachable);
            while (splitPoint && isInCycleWith(splitPoint, reachableFrom(0, only(splitPoint))))
                splitPoint = m_dominator[splitPoint];
            if (!splitPoint)
                return false;
            splitPoints = { splitPoint };
            reachedFrom = { reachableFrom(splitPoint, { }) };
            cutTargets = { BitVector() };
        }
        BitVector isCut(m_numberOfBlocks);
        for (unsigned splitPoint : splitPoints)
            isCut.set(splitPoint);
        BitVector staysInEntry = reachableFrom(0, isCut);
        bool returns = false;
        for (unsigned block : staysInEntry)
            returns |= m_function.blocks[block].terminator.kind == TerminatorKind::Return;
        if (!returns)
            return false;

        Vector<unsigned> order;
        for (unsigned i = 0; i < splitPoints.size(); ++i)
            order.append(i);
        std::ranges::sort(order, [&](unsigned a, unsigned b) {
            return reachedFrom[a].bitCount() < reachedFrom[b].bitCount();
        });
        Vector<Arguments> liveInto(splitPoints.size());
        result.continuations.grow(splitPoints.size());
        for (unsigned which : order) {
            result.continuations[which] = makeContinuation(splitPoints, which, cutTargets[which], liveInto);
            if (!fitsInRegisters(result.continuations[which])) {
                result.continuations.clear();
                return false;
            }
        }
        result.entry = makeEntry(staysInEntry, splitPoints, liveInto);
        return true;
    }

private:
    bool mayBeSplit() const
    {
        if (Options::auditAOTTypedAssertions() || m_program.isEntry(m_id) || !m_function.isTyped())
            return false;
        if (m_function.kind != FunctionKind::Plain && m_function.kind != FunctionKind::Arrow && m_function.kind != FunctionKind::Method)
            return false;
        if (m_function.has(FunctionIsGenericEntrypoint | FunctionUsesArgumentBuffer) || m_function.declaredAs || m_function.resumeFunction || m_function.startFunction || m_function.savesFrameOnSuspend || m_program.isResumeFunction(m_function))
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

    bool usesFrameStorage() const
    {
        return !m_function.stackAllocatedEnvironments.isEmpty() || !m_function.mayPointIntoFrame.isEmpty() || m_function.copiesFirstArgumentToHeap || m_function.values.containsIf([](auto& value) {
            return value.isStackAllocatedClosure();
        });
    }

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

    void findCycles()
    {
        constexpr unsigned none = UINT_MAX;
        m_component.fill(none, m_numberOfBlocks);
        Vector<unsigned> index(FillWith { }, m_numberOfBlocks, none);
        Vector<unsigned> low(FillWith { }, m_numberOfBlocks, 0u);
        BitVector isOnStack(m_numberOfBlocks);
        Vector<unsigned> stack;
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
        parameter.copyPredictionFrom(value);
        parameter.immediates = { 0, index };
        return parameter;
    }

    Function makeContinuation(const Vector<unsigned>& splitPoints, unsigned which, const BitVector& cutTargets, Vector<Arguments>& liveInto) const
    {
        unsigned splitPoint = splitPoints[which];
        Arguments& live = liveInto[which];
        Function result;
        result.name = m_function.name;
        result.module = m_function.module;
        result.position = m_function.position;
        result.flags = m_function.flags & (Format::FunctionIsTyped | Format::FunctionIsBuiltin | Format::FunctionIsInBuiltinModule | Format::FunctionIsSloppy);
        result.resultType = m_function.resultType;
        result.resultTypes = m_function.resultTypes;
        result.writes = m_function.writes;
        result.writesShapeWords = m_function.writesShapeWords;
        result.writesOnReturn = m_function.writesOnReturn;
        result.writesShapeWordsOnReturn = m_function.writesShapeWordsOnReturn;
        result.convertsArrayStorage = m_function.convertsArrayStorage;
        result.convertsArrayStorageOnReturn = m_function.convertsArrayStorageOnReturn;
        result.inlineFrames = m_function.inlineFrames;
        result.values = m_function.values;
        result.stackAllocatedEnvironments = m_function.stackAllocatedEnvironments;
        result.mayPointIntoFrame = m_function.mayPointIntoFrame;
        result.copiesFirstArgumentToHeap = m_function.copiesFirstArgumentToHeap;
        result.copiedArgumentCaptureCount = m_function.copiedArgumentCaptureCount;
        result.blocks.grow(m_numberOfBlocks);

        BitVector blocks = reachableFrom(splitPoint, cutTargets);
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
                if (!cutTargets.get(result.blocks[block].terminator.targets[i].block.index()))
                    continue;
                jumpToContinuation(result, block, i, splitPoints, liveInto);
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

        Vector<unsigned> worklist;
        for (unsigned index : isUsed)
            worklist.append(index);
        while (!worklist.isEmpty()) {
            auto& value = m_function.values[worklist.takeLast()];
            if (value.opcode == Opcode::Cast && !isDefined.get(value.child(0).index()) && !isUsed.set(value.child(0).index()))
                worklist.append(value.child(0).index());
        }

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
        Target target { BlockID(splitPoint), { } };
        for (ValueID parameter : m_function.blocks[splitPoint].parameters) {
            result.values.append(parameterLike(m_function.at(parameter), entry.parameters.size()));
            ValueID id(result.values.size() - 1);
            entry.parameters.append(id);
            target.arguments.append(id);
        }
        entry.terminator.kind = TerminatorKind::Jump;
        entry.terminator.targets.append(WTF::move(target));
        return result;
    }

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

    Function makeEntry(const BitVector& staysInEntry, const Vector<unsigned>& splitPoints, const Vector<Arguments>& liveInto) const
    {
        Function result = m_function;
        for (unsigned block = 0; block < m_numberOfBlocks; ++block) {
            if (!staysInEntry.get(block)) {
                result.blocks[block] = Block { };
                continue;
            }
            for (unsigned i = 0; i < result.blocks[block].terminator.targets.size(); ++i) {
                if (splitPoints.contains(result.blocks[block].terminator.targets[i].block.index()))
                    jumpToContinuation(result, block, i, splitPoints, liveInto);
            }
        }
        return result;
    }

    void jumpToContinuation(Function& result, unsigned block, unsigned target, const Vector<unsigned>& splitPoints, const Vector<Arguments>& liveInto) const
    {
        size_t which = splitPoints.find(result.blocks[block].terminator.targets[target].block.index());
        TypeID resultType = m_function.resultType ? m_function.resultType : m_program.types.anything;
        Value jump;
        jump.opcode = Opcode::CallDirect;
        jump.type = resultType;
        jump.rep = repOf(m_program.at(resultType));
        jump.position = m_function.position;
        jump.immediates = { m_id.index(), Value::jumpsToContinuation + static_cast<uint32_t>(which) };
        jump.arguments = liveInto[which];
        Block leaving;
        auto& parameters = m_function.blocks[splitPoints[which]].parameters;
        Arguments passed = result.blocks[block].terminator.targets[target].arguments;
        for (unsigned k = 0; k < passed.size(); ++k) {
            auto& parameter = m_function.at(parameters[k]);
            if (result.at(passed[k]).type != parameter.type) {
                Value upcast;
                upcast.opcode = Opcode::Cast;
                upcast.type = parameter.type;
                upcast.rep = parameter.rep;
                upcast.position = m_function.position;
                upcast.copyPredictionFrom(parameter);
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
    const BitVector& m_isUnreachable;
    Vector<unsigned> m_component;
    BitVector m_isInCycle;
    Vector<unsigned> m_dominator;
};

} // anonymous namespace

bool splitFunction(const Program& program, FunctionID id, const Function& function, const BitVector& blocksWithCalls, const BitVector& unreachableBlocks, SplitFunction& result)
{
    return FunctionSplitting(program, id, function, blocksWithCalls, unreachableBlocks).run(result);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
