/*
 * Copyright (C) 2016 Yusuke Suzuki <utatane.tea@gmail.com>
 * Copyright (C) 2016-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BytecodeGeneratorification.h"

#include "BytecodeDumper.h"
#include "BytecodeGeneratorBaseInlines.h"
#include "BytecodeLivenessAnalysisInlines.h"
#include "BytecodeRewriter.h"
#include "BytecodeStructs.h"
#include "BytecodeUseDef.h"
#include "JSGenerator.h"
#include "Label.h"
#include "StrongInlines.h"
#include "UnlinkedCodeBlockGenerator.h"
#include "UnlinkedMetadataTableInlines.h"
#include <wtf/BitVector.h>

namespace JSC {

class GeneratorFrameAnalysis;

struct YieldData {
    JSInstructionStream::Offset point { 0 };
    VirtualRegister argument { 0 };
    BitVector save; // Locals copied to the generator frame before suspending here.
    BitVector restore; // Locals copied back from the generator frame after resuming here.
    bool found { false }; // The bytecode optimizer may have removed an unreachable op_yield, leaving a hole in the numbering.
};

class BytecodeGeneratorification {
public:
    typedef Vector<YieldData> Yields;

    struct GeneratorFrameData {
        JSInstructionStream::Offset m_point;
        VirtualRegister m_dst;
        VirtualRegister m_scope;
        VirtualRegister m_symbolTable;
        VirtualRegister m_initialValue;
    };

    BytecodeGeneratorification(BytecodeGenerator& bytecodeGenerator, UnlinkedCodeBlockGenerator* codeBlock, JSInstructionStreamWriter& instructions, SymbolTable* generatorFrameSymbolTable, int generatorFrameSymbolTableIndex)
        : m_bytecodeGenerator(bytecodeGenerator)
        , m_codeBlock(codeBlock)
        , m_instructions(instructions)
        , m_graph(m_codeBlock, m_instructions)
        , m_generatorFrameSymbolTable(codeBlock->vm(), generatorFrameSymbolTable)
        , m_generatorFrameSymbolTableIndex(generatorFrameSymbolTableIndex)
    {
        for (const auto& instruction : m_instructions) {
            switch (instruction->opcodeID()) {
            case op_enter: {
                m_enterPoint = instruction.offset();
                break;
            }

            case op_yield: {
                auto bytecode = instruction->as<OpYield>();
                unsigned liveCalleeLocalsIndex = bytecode.m_yieldPoint;
                if (liveCalleeLocalsIndex >= m_yields.size())
                    m_yields.grow(liveCalleeLocalsIndex + 1);
                YieldData& data = m_yields[liveCalleeLocalsIndex];
                data.point = instruction.offset();
                data.argument = bytecode.m_argument;
                data.found = true;
                break;
            }

            case op_create_generator_frame_environment: {
                auto bytecode = instruction->as<OpCreateGeneratorFrameEnvironment>();
                GeneratorFrameData data;
                data.m_point = instruction.offset();
                data.m_dst = bytecode.m_dst;
                data.m_scope = bytecode.m_scope;
                data.m_symbolTable = bytecode.m_symbolTable;
                data.m_initialValue = bytecode.m_initialValue;
                m_generatorFrameData = WTF::move(data);
                break;
            }

            default:
                break;
            }
        }
    }

    struct Storage {
        Identifier identifier;
        unsigned identifierIndex;
        ScopeOffset scopeOffset;
    };

    void run();

#if ASSERT_ENABLED
    void validate(const GeneratorFrameAnalysis&);
#endif

    BytecodeGraph& NODELETE graph() { return m_graph; }

    const Yields& NODELETE yields() const
    {
        return m_yields;
    }

    Yields& NODELETE yields()
    {
        return m_yields;
    }

    JSInstructionStream::Ref NODELETE enterPoint() const
    {
        return m_instructions.at(m_enterPoint);
    }

    std::optional<GeneratorFrameData> NODELETE generatorFrameData() const
    {
        return m_generatorFrameData;
    }

    const JSInstructionStream& NODELETE instructions() const
    {
        return m_instructions;
    }

private:
    Storage storageForGeneratorLocal(VM& vm, unsigned index)
    {
        // We assign a symbol to a register. There is one-on-one corresponding between a register and a symbol.
        // By doing so, we allocate the specific storage to save the given register.
        // This allow us not to save all the live registers even if the registers are not overwritten from the previous resuming time.
        // It means that, the register can be retrieved even if the immediate previous op_save does not save it.

        if (m_storages.size() <= index)
            m_storages.grow(index + 1);
        if (std::optional<Storage> storage = m_storages[index])
            return *storage;

        Identifier identifier = Identifier::from(vm, index);
        unsigned identifierIndex = m_codeBlock->numberOfIdentifiers();
        m_codeBlock->addIdentifier(identifier);
        ScopeOffset scopeOffset = m_generatorFrameSymbolTable->takeNextScopeOffset(NoLockingNecessary);
        m_generatorFrameSymbolTable->add(NoLockingNecessary, identifier.impl(), SymbolTableEntry(VarOffset(scopeOffset)));

        Storage storage = {
            identifier,
            identifierIndex,
            scopeOffset
        };
        m_storages[index] = storage;
        return storage;
    }

    BytecodeGenerator& m_bytecodeGenerator;
    JSInstructionStream::Offset m_enterPoint;
    std::optional<GeneratorFrameData> m_generatorFrameData;
    UnlinkedCodeBlockGenerator* m_codeBlock;
    JSInstructionStreamWriter& m_instructions;
    BytecodeGraph m_graph;
    Vector<std::optional<Storage>> m_storages;
    Yields m_yields;
    Strong<SymbolTable> m_generatorFrameSymbolTable;
    int m_generatorFrameSymbolTableIndex;
};

class GeneratorLivenessAnalysis : public BytecodeLivenessPropagation {
public:
    GeneratorLivenessAnalysis(BytecodeGeneratorification& generatorification)
        : m_generatorification(generatorification)
    {
    }

    void run(UnlinkedCodeBlockGenerator* codeBlock, JSInstructionStreamWriter& instructions)
    {
        // Perform modified liveness analysis to determine which locals are live at the merge points.
        // This produces the conservative results for the question, "which variables should be saved and resumed?".

        runLivenessFixpoint(codeBlock, instructions, m_generatorification.graph());

        for (YieldData& data : m_generatorification.yields()) {
            if (!data.found)
                continue;
            FastBitVector liveness = getLivenessInfoAtInstruction(codeBlock, instructions, m_generatorification.graph(), BytecodeIndex(m_generatorification.instructions().at(data.point).next().offset()));
            liveness.forEachSetBit([&](size_t index) {
                data.save.set(index);
            });
            data.restore = data.save;
        }
    }

private:
    BytecodeGeneratorification& m_generatorification;
};

// Every local that is live across a suspend point has its own slot in the generator frame, so a slot stays right for as long
// as nobody writes the register. Call what runs between entering or resuming the function and the next suspend an activation.
// This decides which copies between registers and slots an activation needs, one of two ways for each local, whichever takes
// fewer instructions:
//
//   Copy where activations begin and end. A suspend point saves the local if the activation may have written it, and a resume
//   point restores it if the activation may read it. A save is such a read. So is a read in an exception handler, for every
//   resume point the handler covers, because a handler cannot tell which activation it is running in.
//
//   Keep it in the frame. A definition is followed by a store if the slot can be read before the next definition, so the slot
//   is right wherever the function can suspend or throw, and a load is right anywhere. Loads go where the local is read and
//   the register may be out of date. This is never done to a loop that can go around without suspending.
class GeneratorFrameAnalysis {
public:
    struct Copy {
        JSInstructionStream::Offset offset;
        unsigned local;
    };

    GeneratorFrameAnalysis(BytecodeGeneratorification& generatorification, UnlinkedCodeBlockGenerator* codeBlock, const JSInstructionStream& instructions)
        : m_generatorification(generatorification)
        , m_codeBlock(codeBlock)
        , m_instructions(instructions)
        , m_numLocals(codeBlock->numCalleeLocals())
        , m_written(m_numLocals)
        , m_cannotKeepInFrame(m_numLocals)
    {
    }

    void run();

    // Both are sorted by offset. A store belongs to the instruction before its offset: jumping to the offset skips it.
    // A load belongs to the instruction at its offset.
    const Vector<Copy>& NODELETE stores() const { return m_stores; }
    const Vector<Copy>& NODELETE loads() const { return m_loads; }

private:
    static constexpr unsigned none = UINT_MAX;

    // Part of a basic block. Blocks are cut after op_enter, op_yield and op_catch, which is where registers stop being right.
    struct Segment {
        unsigned block { none };
        unsigned handler { none }; // The segment holding the op_catch this one unwinds to.
        unsigned yield { none }; // Set if this ends with op_yield.
        unsigned firstDefinition { 0 };
        unsigned endDefinition { 0 };
        JSInstructionStream::Offset firstRead { 0 };
        bool hasRead { false };
        bool isEnter { false };
        bool isCatch { false };
        Vector<unsigned, 2> successors;
        BitVector uses; // Read before being written here.
        BitVector defs;
    };

    // BitVector needs no memory of its own for the number of locals most functions have.
    using Sets = Vector<BitVector>;

    void buildSegments();
    void summarize(unsigned segmentIndex, const JSInstructionStream::Ref&);
    BitVector segmentsThatCanRepeat() const;

    Sets emptySets() const { return Sets(FillWith { }, m_segments.size(), BitVector(m_numLocals)); }

    // Unlike operator=, this does not allocate when the sets are large.
    static void assign(BitVector& target, const BitVector& source)
    {
        target.clearAll();
        target.merge(source);
    }

    static bool mergeAndCheck(BitVector& target, const BitVector& source)
    {
        size_t before = target.bitCount();
        target.merge(source);
        return target.bitCount() != before;
    }

    void unionOfSuccessors(const Sets& in, const Segment& segment, BitVector& result) const
    {
        result.clearAll();
        for (unsigned successor : segment.successors)
            result.merge(in[successor]);
    }

    // Solves in(s) = adjustIn((adjustOut(out(s)) & ~defs(s)) | uses(s)), where out(s) is the union of in() over the successors of s.
    template<typename AdjustOut, typename AdjustIn>
    Sets solveBackward(bool includeUses, const AdjustOut& adjustOut, const AdjustIn& adjustIn) const
    {
        Sets in = emptySets();
        BitVector set(m_numLocals);
        bool changed;
        do {
            changed = false;
            for (unsigned index = m_segments.size(); index--;) {
                const Segment& segment = m_segments[index];
                unionOfSuccessors(in, segment, set);
                adjustOut(segment, set);
                set.exclude(segment.defs);
                if (includeUses)
                    set.merge(segment.uses);
                adjustIn(index, in, set);
                if (in[index] != set) {
                    assign(in[index], set);
                    changed = true;
                }
            }
        } while (changed);
        return in;
    }

    BytecodeGeneratorification& m_generatorification;
    UnlinkedCodeBlockGenerator* m_codeBlock;
    const JSInstructionStream& m_instructions;
    unsigned m_numLocals;
    bool m_frameIsCreated { false };
    Vector<Segment> m_segments;
    Vector<Copy> m_definitions; // Where the store for each definition would go, in bytecode order.
    Vector<std::pair<unsigned, unsigned>> m_checkpointDefinitions; // (segment, local) written by an instruction that can still throw afterwards.
    BitVector m_written; // By anything but op_enter.
    BitVector m_cannotKeepInFrame;
    Vector<Copy> m_stores;
    Vector<Copy> m_loads;
};

void GeneratorFrameAnalysis::summarize(unsigned segmentIndex, const JSInstructionStream::Ref& instruction)
{
    Segment& segment = m_segments[segmentIndex];
    OpcodeID opcodeID = instruction->opcodeID();
    // These have to stay first in their block, so no load can go in front of them. They read nothing, except that everything
    // counts as reading the scope register when there are debugging opcodes.
    bool mustStayFirst = opcodeID == op_loop_hint || opcodeID == op_catch;
    JSInstructionStream::Offset next = instruction.offset() + instruction->size();
    VirtualRegister frame = virtualRegisterForArgumentIncludingThis(static_cast<int32_t>(JSGenerator::Argument::Frame));
    bool nothingCanFollow = isBranch(opcodeID) || isTerminal(opcodeID) || isThrow(opcodeID);
    unsigned firstDefinition = m_definitions.size();
    bool createsFrame = false;

    auto numberOfCheckpoints = instruction->numberOfCheckpoints();
    for (Checkpoint checkpoint = 0; checkpoint < numberOfCheckpoints; ++checkpoint) {
        computeUsesForBytecodeIndex(m_codeBlock, instruction.ptr(), checkpoint, [&](VirtualRegister operand) {
            if (!isValidRegisterForLiveness(operand) || mustStayFirst)
                return;
            if (!segment.hasRead) {
                segment.hasRead = true;
                segment.firstRead = instruction.offset();
            }
            if (!segment.defs.quickGet(operand.toLocal()))
                segment.uses.quickSet(operand.toLocal());
        });
        computeDefsForBytecodeIndex(m_codeBlock, instruction.ptr(), checkpoint, [&](VirtualRegister operand) {
            if (operand == frame)
                createsFrame = true;
            if (!isValidRegisterForLiveness(operand))
                return;
            unsigned local = operand.toLocal();
            segment.defs.quickSet(local);
            // No store is needed for the undefined that op_enter fills the vars with, since a fresh slot holds that too.
            if (opcodeID == op_enter && operand != m_codeBlock->scopeRegister())
                return;
            if (opcodeID != op_enter)
                m_written.quickSet(local);
            if (nothingCanFollow)
                m_cannotKeepInFrame.quickSet(local);
            if (checkpoint + 1 < numberOfCheckpoints)
                m_checkpointDefinitions.append({ segmentIndex, local });
            for (unsigned index = firstDefinition; index < m_definitions.size(); ++index) {
                if (m_definitions[index].local == local)
                    return;
            }
            m_definitions.append({ next, local });
        });
    }

    if (createsFrame && !m_frameIsCreated) {
        m_frameIsCreated = true;
        // There was nowhere to store what has been defined so far. It can be stored now if this is still straight-line code.
        unsigned firstInStraightLine = segmentIndex == 1 && m_segments[0].isEnter ? 0 : segment.firstDefinition;
        for (unsigned index = 0; index < m_definitions.size(); ++index) {
            if (index >= firstInStraightLine)
                m_definitions[index].offset = next;
            else
                m_cannotKeepInFrame.quickSet(m_definitions[index].local);
        }
    }
}

void GeneratorFrameAnalysis::buildSegments()
{
    BytecodeGraph& graph = m_generatorification.graph();
    Vector<unsigned> firstSegmentOfBlock(FillWith { }, graph.size(), none);

    for (auto& block : graph) {
        if (block.isEntryBlock() || block.isExitBlock())
            continue;
        firstSegmentOfBlock[block.index()] = m_segments.size();
        JSInstructionStream::Offset end = block.leaderOffset() + block.totalLength();
        bool isOpen = false;
        for (JSInstructionStream::Offset offset = block.leaderOffset(); offset < end;) {
            auto instruction = m_instructions.at(offset);
            if (!isOpen) {
                isOpen = true;
                Segment segment;
                segment.block = block.index();
                segment.firstDefinition = m_definitions.size();
                segment.uses.ensureSize(m_numLocals);
                segment.defs.ensureSize(m_numLocals);
                m_segments.append(WTF::move(segment));
            }
            summarize(m_segments.size() - 1, instruction);
            offset += instruction->size();

            Segment& segment = m_segments.last();
            segment.endDefinition = m_definitions.size();
            switch (instruction->opcodeID()) {
            case op_enter:
                segment.isEnter = true;
                isOpen = false;
                break;
            case op_catch:
                segment.isCatch = true;
                isOpen = false;
                break;
            case op_yield:
                segment.yield = instruction->as<OpYield>().m_yieldPoint;
                isOpen = false;
                break;
            default:
                break;
            }
        }
    }

    unsigned handlerOfBlock = none;
    for (unsigned index = 0; index < m_segments.size(); ++index) {
        Segment& segment = m_segments[index];
        auto& block = graph[segment.block];
        // The ends of a handler's range are jump targets, so a whole block unwinds to the same place.
        if (firstSegmentOfBlock[segment.block] == index) {
            handlerOfBlock = none;
            if (auto* handler = m_codeBlock->handlerForBytecodeIndex(BytecodeIndex(block.leaderOffset())))
                handlerOfBlock = firstSegmentOfBlock[graph.findBasicBlockWithLeaderOffset(handler->target)->index()];
        }
        segment.handler = handlerOfBlock;

        if (index + 1 < m_segments.size() && m_segments[index + 1].block == segment.block) {
            segment.successors.append(index + 1);
            continue;
        }
        for (unsigned successor : block.successors()) {
            if (firstSegmentOfBlock[successor] != none)
                segment.successors.append(firstSegmentOfBlock[successor]);
        }
    }

    // Resuming runs op_enter again, so what nothing else writes is always in its register.
    for (auto& segment : m_segments)
        segment.uses.filter(m_written);
}

// The segments that can run more than once in an activation: those in a cycle of the graph that is left when the edges that
// suspend are taken out. This is Tarjan's strongly connected components algorithm.
BitVector GeneratorFrameAnalysis::segmentsThatCanRepeat() const
{
    unsigned size = m_segments.size();
    BitVector result(size);
    BitVector isOnStack(size);
    Vector<unsigned> order(FillWith { }, size, none);
    Vector<unsigned> lowLink(FillWith { }, size, 0u);
    Vector<unsigned> stack;
    Vector<std::pair<unsigned, unsigned>> worklist; // A segment, and how many of its edges have been followed.
    unsigned nextOrder = 0;

    for (unsigned root = 0; root < size; ++root) {
        if (order[root] != none)
            continue;
        worklist.append({ root, 0 });
        while (!worklist.isEmpty()) {
            auto [index, edge] = worklist.last();
            const Segment& segment = m_segments[index];
            if (!edge) {
                order[index] = lowLink[index] = nextOrder++;
                stack.append(index);
                isOnStack.quickSet(index);
            }

            unsigned numberOfSuccessors = segment.yield == none ? segment.successors.size() : 0;
            if (edge < numberOfSuccessors + (segment.handler != none)) {
                ++worklist.last().second;
                unsigned target = edge < numberOfSuccessors ? segment.successors[edge] : segment.handler;
                if (target == index)
                    result.quickSet(index);
                if (order[target] == none)
                    worklist.append({ target, 0 });
                else if (isOnStack.quickGet(target))
                    lowLink[index] = std::min(lowLink[index], order[target]);
                continue;
            }

            worklist.removeLast();
            if (!worklist.isEmpty())
                lowLink[worklist.last().first] = std::min(lowLink[worklist.last().first], lowLink[index]);
            if (lowLink[index] != order[index])
                continue;
            bool isCycle = stack.last() != index;
            unsigned member;
            do {
                member = stack.takeLast();
                isOnStack.quickClear(member);
                if (isCycle)
                    result.quickSet(member);
            } while (member != index);
        }
    }
    return result;
}

void GeneratorFrameAnalysis::run()
{
    auto& yields = m_generatorification.yields();
    if (yields.isEmpty())
        return;

    buildSegments();

    BitVector set(m_numLocals);
    auto addHandler = [&](unsigned index, const Sets& in, BitVector& result) {
        if (m_segments[index].handler != none)
            result.merge(in[m_segments[index].handler]);
    };

    Sets live = solveBackward(true, [](const Segment&, BitVector&) { }, addHandler);

    Sets liveAfterYield(yields.size());
    for (auto& segment : m_segments) {
        if (segment.yield == none)
            continue;
        unionOfSuccessors(live, segment, set);
        liveAfterYield[segment.yield] = set;
    }

    // What may the activation have written by the time it gets here?
    {
        Sets written = emptySets();
        // Nothing should be read before it is written, but if something is, then keep saving whatever is in the register.
        assign(written[0], live[0]);
        bool changed;
        do {
            changed = false;
            for (unsigned index = 0; index < m_segments.size(); ++index) {
                const Segment& segment = m_segments[index];
                assign(set, written[index]);
                // A function fills its frame with undefined, which is what op_enter fills the vars with, other than the scope
                // register. A module is given its frame.
                if (!segment.isEnter || !m_frameIsCreated)
                    set.merge(segment.defs);
                else if (m_codeBlock->scopeRegister().isLocal())
                    set.quickSet(m_codeBlock->scopeRegister().toLocal());
                if (segment.handler != none)
                    changed |= mergeAndCheck(written[segment.handler], set);
                if (segment.yield != none) {
                    set.filter(liveAfterYield[segment.yield]);
                    assign(yields[segment.yield].save, set);
                    continue;
                }
                for (unsigned successor : segment.successors)
                    changed |= mergeAndCheck(written[successor], set);
            }
        } while (changed);
    }

    // What may the activation read from here on?
    {
        Sets read = solveBackward(true, [&](const Segment& segment, BitVector& out) {
            if (segment.yield != none)
                assign(out, yields[segment.yield].save);
        }, addHandler);

        for (auto& segment : m_segments) {
            if (segment.yield == none)
                continue;
            unionOfSuccessors(read, segment, set);
            assign(yields[segment.yield].restore, set);
        }
    }

    if (!Options::useGeneratorFrameWriteThrough() || !m_frameIsCreated)
        return;

    bool force = Options::forceGeneratorFrameWriteThrough();
    Vector<unsigned, 64> costOfCopying(FillWith { }, m_numLocals, 0u);
    for (auto& data : yields) {
        if (!data.found)
            continue;
        auto count = [&](size_t local) {
            ++costOfCopying[local];
        };
        data.save.forEachSetBit(count);
        data.restore.forEachSetBit(count);
    }
    // Keeping a local in the frame takes a store and a load at the very least.
    if (!force && std::ranges::none_of(costOfCopying, [](unsigned cost) { return cost > 2; }))
        return;

    for (auto& segment : m_segments) {
        if (segment.handler != none && !m_segments[segment.handler].isCatch)
            return;
    }

    // Which registers may be out of date here, given a load in every segment that reads one that is?
    Sets loads = emptySets();
    {
        BitVector all(m_numLocals);
        for (unsigned local = 0; local < m_numLocals; ++local)
            all.quickSet(local);

        bool changed;
        do {
            changed = false;
            for (unsigned index = 0; index < m_segments.size(); ++index) {
                const Segment& segment = m_segments[index];
                if (segment.yield != none)
                    assign(set, all);
                else {
                    assign(set, segment.isCatch ? all : loads[index]);
                    set.exclude(segment.uses);
                    set.exclude(segment.defs);
                }
                for (unsigned successor : segment.successors)
                    changed |= mergeAndCheck(loads[successor], set);
            }
        } while (changed);
        for (unsigned index = 0; index < m_segments.size(); ++index)
            loads[index].filter(m_segments[index].uses);
    }

    // Whose slot may be read from here on, before the register is written?
    Sets slotIsLive = solveBackward(false, [&](const Segment& segment, BitVector& out) {
        if (segment.yield != none)
            assign(out, liveAfterYield[segment.yield]);
    }, [&](unsigned index, const Sets&, BitVector& result) {
        if (m_segments[index].handler != none)
            result.merge(live[m_segments[index].handler]);
        result.merge(loads[index]);
    });

    m_cannotKeepInFrame.merge(live[0]);
    // The handler would find the register written, but the store that follows the instruction would not have run.
    for (auto [segment, local] : m_checkpointDefinitions) {
        unsigned handler = m_segments[segment].handler;
        if (handler != none && live[handler].quickGet(local))
            m_cannotKeepInFrame.quickSet(local);
    }

    BitVector canRepeat = segmentsThatCanRepeat();
    Vector<unsigned, 64> costOfKeepingInFrame(FillWith { }, m_numLocals, 0u);
    auto add = [&](Vector<Copy>& copies, unsigned segmentIndex, JSInstructionStream::Offset offset, unsigned local) {
        copies.append({ offset, local });
        ++costOfKeepingInFrame[local];
        if (canRepeat.quickGet(segmentIndex) && !force)
            m_cannotKeepInFrame.quickSet(local);
    };

    for (unsigned index = 0; index < m_segments.size(); ++index) {
        const Segment& segment = m_segments[index];
        loads[index].forEachSetBit([&](size_t local) {
            add(m_loads, index, segment.firstRead, local);
        });

        if (segment.firstDefinition == segment.endDefinition)
            continue;
        unsigned firstStore = m_stores.size();
        if (segment.yield != none)
            assign(set, liveAfterYield[segment.yield]);
        else
            unionOfSuccessors(slotIsLive, segment, set);
        for (unsigned definitionIndex = segment.endDefinition; definitionIndex-- > segment.firstDefinition;) {
            auto& definition = m_definitions[definitionIndex];
            bool handlerReads = segment.handler != none && live[segment.handler].quickGet(definition.local);
            if (set.quickGet(definition.local) || handlerReads)
                add(m_stores, index, definition.offset, definition.local);
            set.quickClear(definition.local);
        }
        std::ranges::reverse(m_stores.mutableSpan().subspan(firstStore));
    }

    BitVector keepInFrame(m_numLocals);
    for (unsigned local = 0; local < m_numLocals; ++local) {
        if (!m_cannotKeepInFrame.quickGet(local) && (costOfKeepingInFrame[local] < costOfCopying[local] || force))
            keepInFrame.quickSet(local);
    }

    for (auto& data : yields) {
        if (!data.found)
            continue;
        data.save.exclude(keepInFrame);
        data.restore.exclude(keepInFrame);
    }
    auto isCopied = [&](const Copy& copy) {
        return !keepInFrame.quickGet(copy.local);
    };
    m_stores.removeAllMatching(isCopied);
    m_loads.removeAllMatching(isCopied);
}

#if ASSERT_ENABLED
// Follows the function one instruction at a time, tracking which locals are certainly in their register and which are certainly
// in their slot, to check that GeneratorFrameAnalysis left nothing out. It shares no reasoning with the analysis.
void BytecodeGeneratorification::validate(const GeneratorFrameAnalysis& analysis)
{
    struct State {
        bool merge(const State& other)
        {
            if (!isReached) {
                *this = other;
                return true;
            }
            bool changed = inRegister.setAndCheck(inRegister & other.inRegister);
            changed |= inSlot.setAndCheck(inSlot & other.inSlot);
            return changed;
        }

        bool isReached { false };
        FastBitVector inRegister;
        FastBitVector inSlot;
    };

    unsigned numLocals = m_codeBlock->numCalleeLocals();
    BytecodeLivenessPropagation::runLivenessFixpoint(m_codeBlock, m_instructions, m_graph);

    FastBitVector written(numLocals);
    for (const auto& instruction : m_instructions) {
        if (instruction->opcodeID() == op_enter)
            continue;
        for (Checkpoint checkpoint = 0; checkpoint < instruction->numberOfCheckpoints(); ++checkpoint) {
            computeDefsForBytecodeIndex(m_codeBlock, instruction.ptr(), checkpoint, [&](VirtualRegister operand) {
                if (operand.isLocal())
                    written[operand.toLocal()] = true;
            });
        }
    }

    Vector<State> atHead(m_graph.size());
    bool changed = false;
    bool isChecking = false;

    auto check = [&](bool condition, const char* message, JSInstructionStream::Offset offset, size_t local) {
        if (!isChecking || condition)
            return;
        dataLogLn("Generator frame validation failed at bc#", offset, " for loc", local, ": ", message);
        CodeBlockBytecodeDumper<UnlinkedCodeBlockGenerator>::dumpBlock(m_codeBlock, m_instructions, WTF::dataFile());
        RELEASE_ASSERT_NOT_REACHED();
    };

    auto walk = [&](JSBytecodeBasicBlock& block) {
        State state = atHead[block.index()];
        State* atHandler = nullptr;
        if (auto* handler = m_codeBlock->handlerForBytecodeIndex(BytecodeIndex(block.leaderOffset())))
            atHandler = &atHead[m_graph.findBasicBlockWithLeaderOffset(handler->target)->index()];

        JSInstructionStream::Offset end = block.leaderOffset() + block.totalLength();
        for (JSInstructionStream::Offset offset = block.leaderOffset(); offset < end;) {
            auto instruction = m_instructions.at(offset);
            OpcodeID opcodeID = instruction->opcodeID();
            for (auto& load : std::ranges::equal_range(analysis.loads(), offset, { }, &GeneratorFrameAnalysis::Copy::offset)) {
                check(state.inSlot[load.local], "loaded, but not in its slot", offset, load.local);
                state.inRegister[load.local] = true;
            }

            auto numberOfCheckpoints = instruction->numberOfCheckpoints();
            for (Checkpoint checkpoint = 0; checkpoint < numberOfCheckpoints; ++checkpoint) {
                if (atHandler)
                    changed |= atHandler->merge(state);
                computeUsesForBytecodeIndex(m_codeBlock, instruction.ptr(), checkpoint, [&](VirtualRegister operand) {
                    if (operand.isLocal() && opcodeID != op_loop_hint && opcodeID != op_catch)
                        check(state.inRegister[operand.toLocal()], "read, but not in its register", offset, operand.toLocal());
                });
                computeDefsForBytecodeIndex(m_codeBlock, instruction.ptr(), checkpoint, [&](VirtualRegister operand) {
                    if (!operand.isLocal())
                        return;
                    state.inRegister[operand.toLocal()] = true;
                    state.inSlot[operand.toLocal()] = opcodeID == op_enter && operand != m_codeBlock->scopeRegister() && !isModuleParseMode(m_codeBlock->parseMode());
                });
            }

            if (opcodeID == op_yield) {
                const YieldData& data = m_yields[instruction->as<OpYield>().m_yieldPoint];
                data.save.forEachSetBit([&](size_t local) {
                    check(state.inRegister[local], "saved, but not in its register", offset, local);
                    state.inSlot[local] = true;
                });
                FastBitVector live = BytecodeLivenessPropagation::getLivenessInfoAtInstruction(m_codeBlock, m_instructions, m_graph, BytecodeIndex(instruction.next().offset()));
                (live & written).forEachSetBit([&](size_t local) {
                    check(state.inSlot[local], "live across a suspend point, but not in its slot", offset, local);
                });
                data.restore.forEachSetBit([&](size_t local) {
                    check(state.inSlot[local], "restored, but not in its slot", offset, local);
                });
                state.inRegister = ~written;
                data.restore.forEachSetBit([&](size_t local) {
                    state.inRegister[local] = true;
                });
            }

            offset += instruction->size();
            for (auto& store : std::ranges::equal_range(analysis.stores(), offset, { }, &GeneratorFrameAnalysis::Copy::offset)) {
                check(state.inRegister[store.local], "stored, but not in its register", offset, store.local);
                state.inSlot[store.local] = true;
            }
        }

        for (unsigned successor : block.successors())
            changed |= atHead[successor].merge(state);
    };

    for (auto& block : m_graph) {
        if (block.isEntryBlock() || block.isExitBlock())
            continue;
        State& entry = atHead[block.index()];
        entry.isReached = true;
        // Whatever is read before it is written is no business of ours.
        entry.inRegister = FastBitVector(FillWith { }, numLocals, true);
        entry.inSlot = FastBitVector(numLocals);
        break;
    }

    auto walkAll = [&] {
        for (auto& block : m_graph) {
            if (!block.isEntryBlock() && !block.isExitBlock() && atHead[block.index()].isReached)
                walk(block);
        }
    };
    do {
        changed = false;
        walkAll();
    } while (changed);
    isChecking = true;
    walkAll();
}
#endif // ASSERT_ENABLED

void BytecodeGeneratorification::run()
{
    VM& vm = m_bytecodeGenerator.vm();
    GeneratorFrameAnalysis frameAnalysis(*this, m_codeBlock, m_instructions);
    if (Options::useGeneratorFramePruning()) {
        frameAnalysis.run();
#if ASSERT_ENABLED
        validate(frameAnalysis);
#endif
    } else {
        // We calculate the liveness at each merge point. This gives us the information which registers should be saved and resumed conservatively.
        GeneratorLivenessAnalysis pass(*this);
        pass.run(m_codeBlock, m_instructions);
    }

    BytecodeRewriter rewriter(m_bytecodeGenerator, m_graph, m_codeBlock, m_instructions);

    // Setup the global switch for the generator.
    {
        auto nextToEnterPoint = enterPoint().next();
        unsigned switchTableIndex = m_codeBlock->numberOfUnlinkedSwitchJumpTables();
        VirtualRegister state = virtualRegisterForArgumentIncludingThis(static_cast<int32_t>(JSGenerator::Argument::State));
        auto& jumpTable = m_codeBlock->addUnlinkedSwitchJumpTable();
        jumpTable.m_min = 0;
        jumpTable.m_branchOffsets = FixedVector<int32_t>(m_yields.size() + 1);
        std::ranges::fill(jumpTable.m_branchOffsets, 0);
        jumpTable.add(0, nextToEnterPoint.offset());
        // A yield the bytecode optimizer removed as unreachable can never be resumed at; give its state the default
        // target so the table has no holes (a 0 entry would be rebased into a jump to op_enter).
        for (unsigned i = 0; i < m_yields.size(); ++i)
            jumpTable.add(i + 1, m_yields[i].found ? m_yields[i].point : nextToEnterPoint.offset());
        jumpTable.m_defaultOffset = nextToEnterPoint.offset();

        rewriter.insertFragmentBefore(nextToEnterPoint, [&] (BytecodeRewriter::Fragment& fragment) {
            fragment.appendInstruction<OpSwitchImm>(switchTableIndex, state);
        });
    }

    VirtualRegister scope = virtualRegisterForArgumentIncludingThis(static_cast<int32_t>(JSGenerator::Argument::Frame));

    auto appendStore = [&](BytecodeRewriter::Fragment& fragment, size_t index) {
        VirtualRegister operand = virtualRegisterForLocal(index);
        Storage storage = storageForGeneratorLocal(vm, index);

        fragment.appendInstruction<OpPutToScope>(
            scope, // scope
            storage.identifierIndex, // identifier
            operand, // value
            GetPutInfo(DoNotThrowIfNotFound, ResolvedClosureVar, InitializationMode::NotInitialization, m_bytecodeGenerator.ecmaMode()), // info
            SymbolTableOrScopeDepth::symbolTable(VirtualRegister { m_generatorFrameSymbolTableIndex }), // symbol table constant index
            storage.scopeOffset.offset() // scope offset
        );
    };

    auto appendLoad = [&](BytecodeRewriter::Fragment& fragment, size_t index) {
        VirtualRegister operand = virtualRegisterForLocal(index);
        Storage storage = storageForGeneratorLocal(vm, index);

        fragment.appendInstruction<OpGetFromScope>(
            operand, // dst
            scope, // scope
            storage.identifierIndex, // identifier
            GetPutInfo(DoNotThrowIfNotFound, ResolvedClosureVar, InitializationMode::NotInitialization, m_bytecodeGenerator.ecmaMode()), // info
            0, // local scope depth
            storage.scopeOffset.offset(), // scope offset
            m_bytecodeGenerator.nextValueProfileIndex()
        );
    };

    Vector<const YieldData*> yieldsInOrder;
    for (const YieldData& data : m_yields) {
        if (data.found)
            yieldsInOrder.append(&data);
    }
    std::ranges::sort(yieldsInOrder, { }, &YieldData::point);

    // The rewriter wants its insertions in bytecode order.
    auto stores = frameAnalysis.stores().span();
    auto loads = frameAnalysis.loads().span();
    auto yields = yieldsInOrder.span();
    while (!stores.empty() || !loads.empty() || !yields.empty()) {
        JSInstructionStream::Offset offset = std::numeric_limits<JSInstructionStream::Offset>::max();
        if (!stores.empty())
            offset = std::min(offset, stores.front().offset);
        if (!loads.empty())
            offset = std::min(offset, loads.front().offset);
        if (!yields.empty())
            offset = std::min(offset, yields.front()->point);

        auto instruction = m_instructions.at(offset);
        auto appendStores = [&](BytecodeRewriter::Fragment& fragment) {
            for (; !stores.empty() && stores.front().offset == offset; skip(stores, 1))
                appendStore(fragment, stores.front().local);
        };
        auto appendLoads = [&](BytecodeRewriter::Fragment& fragment) {
            for (; !loads.empty() && loads.front().offset == offset; skip(loads, 1))
                appendLoad(fragment, loads.front().local);
        };

        if (yields.empty() || yields.front()->point != offset) {
            if (!stores.empty() && stores.front().offset == offset)
                rewriter.insertFragmentBefore(instruction, appendStores);
            if (!loads.empty() && loads.front().offset == offset)
                rewriter.insertFragmentAfter(instruction, appendLoads);
            continue;
        }

        const YieldData& data = *yields.front();
        skip(yields, 1);

        // Emit save sequence.
        rewriter.insertFragmentBefore(instruction, [&](BytecodeRewriter::Fragment& fragment) {
            appendStores(fragment);
            appendLoads(fragment);
            data.save.forEachSetBit([&](size_t index) {
                appendStore(fragment, index);
            });

            // Insert op_ret just after save sequence.
            fragment.appendInstruction<OpRet>(data.argument);
        });

        // Emit resume sequence.
        rewriter.replaceBytecodeWithFragment(instruction, [&](BytecodeRewriter::Fragment& fragment) {
            data.restore.forEachSetBit([&](size_t index) {
                appendLoad(fragment, index);
            });
        });
    }

    if (m_generatorFrameData) {
        auto instruction = m_instructions.at(m_generatorFrameData->m_point);
        rewriter.replaceBytecodeWithFragment(instruction, [&] (BytecodeRewriter::Fragment& fragment) {
            if (!m_generatorFrameSymbolTable->scopeSize()) {
                // This will cause us to put jsUndefined() into the generator frame's scope value.
                fragment.appendInstruction<OpMov>(m_generatorFrameData->m_dst, m_generatorFrameData->m_initialValue);
            } else
                fragment.appendInstruction<OpCreateLexicalEnvironment>(m_generatorFrameData->m_dst, m_generatorFrameData->m_scope, m_generatorFrameData->m_symbolTable, m_generatorFrameData->m_initialValue);
        });
    }

    unsigned sizeBefore = m_instructions.size();
    rewriter.execute();

    if (Options::dumpGeneratorFrameStatistics()) [[unlikely]] {
        unsigned saves = 0;
        unsigned restores = 0;
        for (auto* data : yieldsInOrder) {
            saves += data->save.bitCount();
            restores += data->restore.bitCount();
        }
        dataLogLn("GeneratorFrame: suspends ", yieldsInOrder.size(), " locals ", m_codeBlock->numCalleeLocals(), " handlers ", m_codeBlock->numberOfExceptionHandlers(), " saves ", saves, " restores ", restores, " stores ", frameAnalysis.stores().size(), " loads ", frameAnalysis.loads().size(), " bytesBefore ", sizeBefore, " bytesAfter ", m_instructions.size());
    }
}

void performGeneratorification(BytecodeGenerator& bytecodeGenerator, UnlinkedCodeBlockGenerator* codeBlock, JSInstructionStreamWriter& instructions, SymbolTable* generatorFrameSymbolTable, int generatorFrameSymbolTableIndex)
{
    if (Options::dumpBytecodesBeforeGeneratorification()) [[unlikely]] {
        dataLogLn("Bytecodes before generatorification");
        CodeBlockBytecodeDumper<UnlinkedCodeBlockGenerator>::dumpBlock(codeBlock, instructions, WTF::dataFile());
    }

    BytecodeGeneratorification pass(bytecodeGenerator, codeBlock, instructions, generatorFrameSymbolTable, generatorFrameSymbolTableIndex);
    pass.run();

    if (Options::dumpBytecodesBeforeGeneratorification()) [[unlikely]] {
        dataLogLn("Bytecodes after generatorification");
        CodeBlockBytecodeDumper<UnlinkedCodeBlockGenerator>::dumpBlock(codeBlock, instructions, WTF::dataFile());
    }
}

} // namespace JSC
