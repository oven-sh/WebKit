/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationsObjects.h"
#include "B3BasicBlockInlines.h"
#include "B3Dominators.h"
#include "B3InsertionSetInlines.h"
#include "B3NaturalLoops.h"
#include "B3StackmapGenerationParams.h"
#include "B3VariableValue.h"
#include "JSCInlines.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

// Values of which nothing is known go through the stubs of code compiled from bytecode. A cache is a slot in a block the function gets once it has missed often enough; until then it uses one all functions share, whose slots stay empty.

// The block as of now: the function may get its own while it runs. One that was found earlier stays valid.
LValue Lowering::loadICData()
{
    PatchpointValue* data = m_out.patchpoint(Int64);
    data->effects = Effects::none();
    // Any operation may give the function slots of its own.
    m_heaps.decoratePatchpointRead(&m_heaps.AOTTyped_collectorState, data);
    if (!m_context.slotBlockFetchGenerator) {
        m_context.slotBlockFetchGenerator = createSharedTask<StackmapGeneratorFunction>([indexReferences = &m_context.indexReferences](CCallHelpers& jit, const StackmapGenerationParams& params) {
            indexReferences->load16(jit, instanceGPR, params[0].gpr(), Instance::offsetOfStates(), sizeof(uint16_t));
            jit.loadPtr(CCallHelpers::BaseIndex(instanceGPR, params[0].gpr(), CCallHelpers::TimesEight), params[0].gpr());
        });
    }
    data->setGenerator(m_context.slotBlockFetchGenerator);
    ++m_context.numberOfSlotBlockFetches;
    return data;
}

class CFGCutAtCycles {
public:
    using Node = BasicBlock*;
    using Set = IndexSet<BasicBlock*>;
    template<typename T> using Map = IndexMap<BasicBlock*, T>;
    using List = Vector<BasicBlock*, 4>;

    CFGCutAtCycles(Procedure& proc, bool isReducible, const IndexMap<BasicBlock*, B3::Value*>& fetchAt)
        : m_proc(proc)
        , m_loops(isReducible ? &proc.naturalLoops() : nullptr)
        , m_loopCutBelowHeader(proc.size(), nullptr)
        , m_component(proc.size(), 0u)
        , m_isOnCycle(proc.size(), false)
        , m_isCut(proc.size(), false)
    {
        if (isReducible)
            cutNaturalLoops(fetchAt);
        else
            cutComponents();
    }

    bool isOnCycle(Node node) const { return m_loops ? !!m_loops->innerMostLoopOf(node) : m_isOnCycle[node]; }
    bool isLeftOut(Node from, Node to) const
    {
        if (m_loops)
            return m_loopCutBelowHeader[to] && !m_loops->belongsTo(from, *m_loopCutBelowHeader[to]);
        return m_isOnCycle[to] && !m_isCut[to] && m_component[from] != m_component[to];
    }

    Node root() { return m_proc[0]; }
    template<typename T> Map<T> newMap() { return IndexMap<BasicBlock*, T>(m_proc.size()); }
    List successors(Node node)
    {
        List result;
        for (BasicBlock* successor : node->successorBlocks()) {
            if (!isLeftOut(node, successor))
                result.append(successor);
        }
        if (node == root())
            result.appendVector(m_cuts);
        return result;
    }
    List predecessors(Node node)
    {
        List result;
        for (BasicBlock* predecessor : node->predecessors()) {
            if (!isLeftOut(predecessor, node))
                result.append(predecessor);
        }
        if (m_isCut[node])
            result.append(root());
        return result;
    }
    unsigned index(Node node) const { return node->index(); }
    Node node(unsigned index) const { return m_proc[index]; }
    unsigned numNodes() const { return m_proc.size(); }
    PointerDump<BasicBlock> dump(Node node) const { return pointerDump(node); }
    void dump(PrintStream& out) const { m_proc.dump(out); }

private:
    void cutAt(BasicBlock* block)
    {
        if (m_isCut[block])
            return;
        m_isCut[block] = true;
        m_cuts.append(block);
    }

    void cutNaturalLoops(const IndexMap<BasicBlock*, B3::Value*>& fetchAt)
    {
        auto& dominators = m_proc.dominators();
        for (unsigned i = m_loops->numLoops(); i--;) {
            auto& loop = m_loops->loop(i);
            BasicBlock* header = loop.header();
            BasicBlock* aboveLatches = nullptr;
            for (BasicBlock* predecessor : header->predecessors()) {
                if (!m_loops->belongsTo(predecessor, loop))
                    continue;
                if (!aboveLatches)
                    aboveLatches = predecessor;
                while (!dominators.dominates(aboveLatches, predecessor))
                    aboveLatches = dominators.idom(aboveLatches);
            }
            BasicBlock* cut = header;
            for (BasicBlock* block = aboveLatches; block != header; block = dominators.idom(block)) {
                if (block->frequency() < cut->frequency() || (cut != header && block->frequency() == cut->frequency()))
                    cut = block;
            }
            bool hasFetch = false;
            bool hasFetchAheadOfCut = false;
            for (unsigned j = loop.size(); j--;) {
                if (!fetchAt[loop.at(j)])
                    continue;
                hasFetch = true;
                hasFetchAheadOfCut |= !dominators.dominates(cut, loop.at(j));
            }
            if (!hasFetch)
                continue;
            if (!hasFetchAheadOfCut)
                cut = header;
            cutAt(cut);
            if (cut != header)
                m_loopCutBelowHeader[header] = &loop;
        }
    }

    void cutComponents()
    {
        struct Frame {
            BasicBlock* block;
            unsigned nextSuccessor;
        };
        IndexMap<BasicBlock*, unsigned> number(m_proc.size(), 0u);
        IndexMap<BasicBlock*, unsigned> lowest(m_proc.size(), 0u);
        IndexMap<BasicBlock*, bool> isBeingVisited(m_proc.size(), false);
        Vector<BasicBlock*, 16> openComponents;
        Vector<Frame, 16> frames;
        unsigned nextNumber = 0;
        auto enter = [&](BasicBlock* block) {
            number[block] = lowest[block] = ++nextNumber;
            isBeingVisited[block] = true;
            openComponents.append(block);
            frames.append(Frame { block, 0 });
        };
        enter(m_proc[0]);
        while (!frames.isEmpty()) {
            BasicBlock* block = frames.last().block;
            if (frames.last().nextSuccessor < block->numSuccessors()) {
                BasicBlock* successor = block->successorBlock(frames.last().nextSuccessor++);
                if (!number[successor]) {
                    enter(successor);
                    continue;
                }
                if (!m_component[successor])
                    lowest[block] = std::min(lowest[block], number[successor]);
                if (isBeingVisited[successor])
                    cutAt(successor);
                if (successor == block)
                    m_isOnCycle[block] = true;
                continue;
            }
            frames.removeLast();
            isBeingVisited[block] = false;
            if (!frames.isEmpty())
                lowest[frames.last().block] = std::min(lowest[frames.last().block], lowest[block]);
            if (lowest[block] != number[block])
                continue;
            bool isAlone = openComponents.last() == block;
            for (;;) {
                BasicBlock* member = openComponents.takeLast();
                m_component[member] = number[block];
                if (!isAlone)
                    m_isOnCycle[member] = true;
                if (member == block)
                    break;
            }
        }
    }

    Procedure& m_proc;
    const NaturalLoops* m_loops;
    List m_cuts;
    IndexMap<BasicBlock*, const NaturalLoop*> m_loopCutBelowHeader;
    IndexMap<BasicBlock*, unsigned> m_component;
    IndexMap<BasicBlock*, bool> m_isOnCycle;
    IndexMap<BasicBlock*, bool> m_isCut;
};

static void placeSlotBlockFetchesOnCycles(Procedure& proc, const StackmapGenerator* generator, IndexMap<BasicBlock*, B3::Value*>& fetchAt, bool isReducible)
{
    CFGCutAtCycles graph(proc, isReducible, fetchAt);
    B3::Value* someFetch = nullptr;
    IndexMap<BasicBlock*, bool> needsFetch(proc.size(), false);
    for (BasicBlock* block : proc) {
        if (fetchAt[block]) {
            someFetch = fetchAt[block];
            needsFetch[block] = true;
        }
        for (BasicBlock* successor : block->successorBlocks())
            needsFetch[block] |= graph.isLeftOut(block, successor);
    }

    WTF::Dominators<CFGCutAtCycles> dominators(graph);
    IndexMap<BasicBlock*, unsigned> depth(proc.size(), 0u);
    Vector<BasicBlock*> deepestFirst;
    for (BasicBlock* block : proc) {
        deepestFirst.append(block);
        Vector<BasicBlock*, 16> chain;
        BasicBlock* above = block;
        for (; above && !depth[above]; above = dominators.idom(above))
            chain.append(above);
        unsigned depthAbove = above ? depth[above] : 0;
        for (unsigned i = chain.size(); i--;)
            depth[chain[i]] = ++depthAbove;
    }
    std::ranges::sort(deepestFirst, [&](BasicBlock* a, BasicBlock* b) {
        return depth[a] > depth[b];
    });

    struct Candidate {
        BasicBlock* block;
        double cost;
        bool isOnCycle;
    };
    IndexMap<BasicBlock*, Vector<Candidate, 1>> candidatesAt(proc.size());
    IndexMap<BasicBlock*, bool> isPlace(proc.size(), false);
    for (BasicBlock* block : deepestFirst) {
        auto candidates = std::exchange(candidatesAt[block], { });
        double cost = 0;
        bool standsForCycle = graph.isOnCycle(block);
        candidates.removeAllMatching([&](const Candidate& candidate) {
            if (!candidate.isOnCycle || block != proc[0]) {
                cost += candidate.cost;
                standsForCycle |= candidate.isOnCycle;
                return false;
            }
            isPlace[candidate.block] = true;
            return true;
        });
        if (needsFetch[block] || cost > block->frequency()) {
            candidates.shrink(0);
            candidates.append(Candidate { block, block->frequency(), standsForCycle });
        }
        if (BasicBlock* parent = dominators.idom(block)) {
            candidatesAt[parent].appendVector(candidates);
            continue;
        }
        for (auto& candidate : candidates)
            isPlace[candidate.block] = true;
    }

    B3::Variable* variable = proc.addVariable(Int64);
    InsertionSet insertionSet(proc);
    for (BasicBlock* block : proc) {
        if (isPlace[block] && !fetchAt[block]) {
            fetchAt[block] = proc.clone(someFetch);
            block->appendNonTerminal(fetchAt[block]);
        }
    }
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            auto* fetch = block->at(index)->as<PatchpointValue>();
            if (!fetch || fetch->generator() != generator)
                continue;
            if (isPlace[block] && fetch == fetchAt[block])
                insertionSet.insert<VariableValue>(index + 1, B3::Set, fetch->origin(), variable, fetch);
            else
                fetch->replaceWithIdentity(insertionSet.insert<VariableValue>(index, B3::Get, fetch->origin(), variable));
        }
        insertionSet.execute(block);
    }
}

void placeSlotBlockFetches(Procedure& proc, LoweringContext& context)
{
    if (context.numberOfSlotBlockFetches < 2)
        return;
    auto isFetch = [&](B3::Value* value) {
        auto* patchpoint = value->as<PatchpointValue>();
        return patchpoint && patchpoint->generator() == context.slotBlockFetchGenerator.get();
    };
    IndexMap<BasicBlock*, B3::Value*> fetchAt(proc.size(), nullptr);
    for (BasicBlock* block : proc) {
        for (B3::Value* value : *block) {
            if (isFetch(value)) {
                fetchAt[block] = value;
                break;
            }
        }
    }

    auto& dominators = proc.dominators();
    bool hasCycle = false;
    bool isReducible = true;
    {
        enum State : uint8_t { NotSeen, BeingVisited, Done };
        struct Frame {
            BasicBlock* block;
            unsigned nextSuccessor;
        };
        IndexMap<BasicBlock*, State> state(proc.size(), NotSeen);
        Vector<Frame, 16> frames;
        state[proc[0]] = BeingVisited;
        frames.append(Frame { proc[0], 0 });
        while (!frames.isEmpty()) {
            BasicBlock* block = frames.last().block;
            if (frames.last().nextSuccessor == block->numSuccessors()) {
                state[block] = Done;
                frames.removeLast();
                continue;
            }
            BasicBlock* successor = block->successorBlock(frames.last().nextSuccessor++);
            if (state[successor] == NotSeen) {
                state[successor] = BeingVisited;
                frames.append(Frame { successor, 0 });
            } else if (state[successor] == BeingVisited) {
                hasCycle = true;
                isReducible &= dominators.dominates(successor, block);
            }
        }
    }
    if (hasCycle) {
        placeSlotBlockFetchesOnCycles(proc, context.slotBlockFetchGenerator.get(), fetchAt, isReducible);
        return;
    }

    struct Candidate {
        BasicBlock* block;
        double cost;
    };
    IndexMap<BasicBlock*, Vector<Candidate, 1>> candidatesAt(proc.size());
    IndexMap<BasicBlock*, bool> isPlace(proc.size(), false);
    for (BasicBlock* block : proc.blocksInPostOrder()) {
        auto candidates = std::exchange(candidatesAt[block], { });
        double cost = 0;
        for (auto& candidate : candidates)
            cost += candidate.cost;
        if (fetchAt[block] || cost > block->frequency()) {
            candidates.shrink(0);
            candidates.append(Candidate { block, block->frequency() });
        }
        if (BasicBlock* parent = dominators.idom(block)) {
            candidatesAt[parent].appendVector(candidates);
            continue;
        }
        for (auto& candidate : candidates)
            isPlace[candidate.block] = true;
    }

    IndexMap<BasicBlock*, BasicBlock*> placeFor(proc.size(), nullptr);
    for (BasicBlock* block : proc.blocksInPreOrder()) {
        BasicBlock* parent = dominators.idom(block);
        placeFor[block] = isPlace[block] || !parent ? block : placeFor[parent];
    }
    for (BasicBlock* block : proc) {
        BasicBlock* place = placeFor[block];
        if (!place)
            continue;
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* value = block->at(index);
            if (!isFetch(value) || value == fetchAt[place])
                continue;
            if (fetchAt[place]) {
                value->replaceWithIdentity(fetchAt[place]);
                continue;
            }
            RELEASE_ASSERT(place != block);
            block->at(index) = proc.add<B3::Value>(Nop, Void, value->origin());
            place->appendNonTerminal(value);
            fetchAt[place] = value;
        }
    }
}

// Nothing: the function has as many as it can have, the name has no number, or the code runs once.
std::optional<unsigned> Lowering::allocateSite(StringID name, unsigned extra)
{
    if (m_program.runsOnce(m_id) && !m_isInLoop)
        return std::nullopt;
    uint32_t identifier = name.index() < m_program.identifierNumbers.size() ? m_program.identifierNumbers[name.index()] : 0;
    if (!identifier || !Site::fits(identifier, extra) || m_context.sites.size() >= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return std::nullopt;
    Site site { };
    site.identifierAndExtra = identifier | extra << Site::identifierBits;
    m_context.sites.append(site);
    if (!m_context.identifiers.contains(name))
        m_context.identifiers.append(name);
    return m_context.sites.size() - 1;
}

// A hit addresses the slot from the block. The sum is for a stub: where that is called off the usual path the sum is made there, of what the back end does not know to be the same address.
Pointer Lowering::slotAddress(unsigned slot, SlotAddressIsFor use)
{
    LValue data = loadICData();
    if (use == SlotAddressIsFor::ColdBlock)
        data = m_out.opaque(data);
    return Pointer(m_out.add(data, m_out.constIntPtr(AOT::Data::offsetOfSlots() + slot * sizeof(AOT::Slot))));
}

LValue Lowering::callICStub(Stub stub, LType type, const Vector<std::pair<LValue, GPRReg>, 4>& operands, std::optional<std::pair<GPRReg, uint32_t>> immediate, OperationEffects effects)
{
    m_context.emitsCalls = true;
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    applyEffects(patchpoint, effects);
    for (auto& [value, reg] : operands)
        patchpoint->append(ConstrainedValue(value, ValueRep::reg(reg)));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    static const int declaredSets = getenv("AOT_SWITCH_IC_SETS") ? atoi(getenv("AOT_SWITCH_IC_SETS")) : 0;
    if (auto declared = declaredSets ? registersChangedBy(stub) : std::nullopt) {
        declared->add(GPRInfo::returnValueGPR, IgnoreVectors);
        if (immediate)
            declared->add(immediate->first, IgnoreVectors);
        patchpoint->clobberLate(*declared);
        if (declaredSets == 2) {
            for (auto& [value, reg] : operands)
                declared->remove(reg);
            patchpoint->clobberEarly(*declared);
        }
    } else {
        patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
#if CPU(X86_64)
        for (GPRReg reg : stubTemporaryGPRs)
            patchpoint->clobberLate(RegisterSet { reg });
#endif
    }
    if (type != Void)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, stub, immediate, site](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (immediate)
            jit.move(CCallHelpers::TrustedImm32(immediate->second), immediate->first);
        stubCalls->call(jit, stub, site);
    });
    unwindToLandingPad(patchpoint, site);
    return patchpoint;
}

static std::array<std::atomic<uint64_t>, 24>& countsOfReadsAsked()
{
    static std::array<std::atomic<uint64_t>, 24> counts { };
    return counts;
}

static bool refusesReadAsked(bool counts, unsigned row)
{
    if (!counts)
        return false;
    static std::once_flag once;
    std::call_once(once, [] {
        atexit([] {
            auto& rows = countsOfReadsAsked();
            auto at = [&](unsigned i) {
                return rows[i].load();
            };
            dataLogLn("ask: reads that are the subject of their block's branch on a text: ", at(0));
            dataLogLn("ask:    refused: under a handler ", at(1), " | other uses ", at(2), " | something between the read and the branch ", at(3), " | the receiver says something ", at(4), " | a name of its own kind, or a run ", at(5), " | the result is held to a type ", at(6));
            dataLogLn("ask:    refused: the text: 9 to 16 characters ", at(7), " | longer ", at(8), " | one unit beyond Latin-1 ", at(9), " | no slot ", at(10));
            dataLogLn("ask:    fit: ", at(11), " | by length 1 to 8: ", at(12), " ", at(13), " ", at(14), " ", at(15), " ", at(16), " ", at(17), " ", at(18), " ", at(19));
        });
    });
    countsOfReadsAsked()[row]++;
    return false;
}

static bool isSwitchedOn(const char* name)
{
    const char* value = getenv(name);
    return value && *value && !(value[0] == '0' && !value[1]);
}

bool Lowering::asksReadForText(ValueID read, const Value& value)
{
    static const bool isOn = isSwitchedOn("BUN_AOT_SWITCH_ASK");
    static const bool counts = isSwitchedOn("BUN_AOT_SWITCH_ASK_COUNT");
    if ((!isOn && !counts) || m_fusedCasts || isCompact(16) || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return false;
    auto& terminator = m_currentBlock->terminator;
    auto comparison = comparisonWithTextBranchedOn(*m_currentBlock);
    if (!comparison || comparison->subject != read || successorsOf(terminator).size() != 2)
        return false;
    refusesReadAsked(counts, 0);
    if (m_watchingHandler)
        return refusesReadAsked(counts, 1);
    if (numberOfUsesOf(read) != 1)
        return refusesReadAsked(counts, 2);
    auto& values = m_currentBlock->values;
    for (size_t i = m_currentIndexInBlock + 1; i < values.size(); ++i) {
        if (values[i] != terminator.value && !(m_function.at(values[i]).opcode == Opcode::String && isFetchedWhereNeeded(values[i])))
            return refusesReadAsked(counts, 3);
    }
    ValueID object = value.child(0);
    StringID name = value.string();
    if (!typeOf(object).isAnything() || value.isAtKnownPlace() || m_function.at(object).opcode == Opcode::GetGlobal)
        return refusesReadAsked(counts, 4);
    if (name == m_length || m_builtins.callOrApply(m_program.builtinName(name)) || bindIfNamed(name) || bufferAccessorNamed(name) || isNamedLastIndex(name) || runOfCurrentAccess(name))
        return refusesReadAsked(counts, 5);
    if (!typeOf(read).isAnything() || value.rep != Rep::JSValue)
        return refusesReadAsked(counts, 6);
    size_t length = comparison->text.size();
    if (!length)
        return refusesReadAsked(counts, 9);
    if (length > 16)
        return refusesReadAsked(counts, 8);
    if (length > 8)
        return refusesReadAsked(counts, 7);
    if (!isOn) {
        refusesReadAsked(counts, 11);
        return refusesReadAsked(counts, 11 + length);
    }
    auto slot = allocateSite(name, 0);
    if (!slot)
        return refusesReadAsked(counts, 10);
    refusesReadAsked(counts, 11);
    refusesReadAsked(counts, 11 + length);
    uint64_t bytes = 0;
    for (size_t i = 0; i < length; ++i)
        bytes |= static_cast<uint64_t>(comparison->text[i]) << (8 * i);
    m_readAskedForText = ReadAskedForText { read, static_cast<unsigned>(m_currentIndexInBlock), *slot, bytes, static_cast<unsigned>(length) };
    return true;
}

void Lowering::lowerBranchOnTextOfRead()
{
    ReadAskedForText asked = *std::exchange(m_readAskedForText, std::nullopt);
    auto& terminator = m_currentBlock->terminator;
    LBasicBlock isText = edgeTo(terminator.targets[0], true);
    LBasicBlock isNot = edgeTo(terminator.targets[1], true);
    Boxed base = in<Boxed>(m_function.at(asked.read).child(0));
    LValue address = slotAddress(asked.slot).value();
    LValue text = integer(static_cast<int64_t>(asked.bytes)).value();
    Stub stub = static_cast<Stub>(static_cast<unsigned>(Stub::GetByIdIsText1) + asked.length - 1);
    m_context.emitsCalls = true;
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    applyEffects(patchpoint, { });
    patchpoint->effects.terminal = true;
    patchpoint->append(ConstrainedValue(base.value(), ValueRep::reg(firstStubOperandGPR)));
    patchpoint->append(ConstrainedValue(address, ValueRep::reg(GPRInfo::argumentGPR1)));
    patchpoint->append(ConstrainedValue(text, ValueRep::reg(GPRInfo::regCS0)));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
#if CPU(X86_64)
    for (GPRReg reg : stubTemporaryGPRs)
        patchpoint->clobberLate(RegisterSet { reg });
#endif
    auto indexOfBranch = std::exchange(m_currentIndexInBlock, asked.indexInBlock);
    CallSite site { nextSite() };
    m_currentIndexInBlock = indexOfBranch;
    m_out.appendSuccessor(unsure(isNot));
    m_out.appendSuccessor(unsure(isText));
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, stub, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        stubCalls->call(jit, stub, site);
#if CPU(ARM64)
        CCallHelpers::Jump differs = jit.branchOnFlags(CCallHelpers::NotEqual);
#else
        CCallHelpers::Jump differs = CCallHelpers::Jump(jit.m_assembler.jCC(X86Assembler::ConditionNE));
#endif
        CCallHelpers::Jump isSame;
        if (!params.fallsThroughToSuccessor(1))
            isSame = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            differs.linkTo(*labels[0], &jit);
            if (isSame.isSet())
                isSame.linkTo(*labels[1], &jit);
        });
    });
}

// The length of an array, a string or a typed array is no property that a slot could say the place of.
Boxed Lowering::emitGetLength(Boxed base)
{
    if (m_context.sites.size() >= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return AOT_TYPED_OPERATION(GetByName, base, word32(m_length.index()));
    Site site { };
    site.identifierAndExtra = static_cast<uint32_t>(WellKnownIdentifier::Length);
    m_context.sites.append(site);
    Pointer address = slotAddress(m_context.sites.size() - 1);
    return Boxed(callICStub(Stub::GetLength, Int64, { { base.value(), firstStubOperandGPR }, { address.value(), GPRInfo::argumentGPR1 } }));
}

TypedPointer Lowering::knownPlaceIn(CellPointer cell, const Value& access)
{
    static_assert(Value::numberOfKnownPlaces == JSFinalObject::maxInlineCapacity);
    return TypedPointer(m_heaps.JSCellHeaderAndNamedProperties, m_out.add(cell.value(), m_out.constIntPtr(JSObject::offsetOfInlineStorage() + access.knownPlace() * sizeof(EncodedJSValue))));
}

CellPointer Lowering::receiverAtKnownPlace(const Value& access, Boxed base)
{
    if (access.receiverIsNeverNil())
        return asCell(base);
    Type receiver = Type::object();
    receiver.primitives = Primitive::UndefinedOrNull;
    LBasicBlock isNil = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(base, receiver).value(), usually(continuation), rarely(isNil));
    m_out.appendTo(isNil);
    throwNullish(base, access.string(), CellPointer());
    m_out.appendTo(continuation);
    return asCell(base);
}

LValue Lowering::checkedKnownPlace(const Value& access, Boxed base)
{
    uint32_t placeAndFlags = access.knownPlace() | (access.receiverIsNeverNil() ? 0x100 : 0) | (access.opcode == Opcode::PutById ? 0x200 : 0) | (access.isStoreToObjectBornWhole() ? 0x400 : 0);
    LValue holds = AOT_TYPED_OPERATION(CheckKnownPlace, base, word32(access.string().index()), word32(placeAndFlags), word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index())).value();
    LBasicBlock loads = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock nothing = m_out.anchor(m_out.constInt64(0));
    m_out.branch(m_out.notZero32(holds), unsure(loads), unsure(continuation));
    m_out.appendTo(loads);
    ValueFromBlock loaded = m_out.anchor(m_out.load64(knownPlaceIn(asCell(base), access)));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(Int64, nothing, loaded);
}

void Lowering::reportUnlessAtKnownPlace(LValue found, LValue expected, Boxed base)
{
    LBasicBlock doesNotHold = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.bitOr(m_out.isZero64(found), m_out.equal(found, expected)), usually(continuation), rarely(doesNotHold));
    m_out.appendTo(doesNotHold);
    AOT_TYPED_OPERATION(FactDoesNotHold, base, word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

Boxed Lowering::getAtKnownPlace(const Value& access, Boxed base)
{
    if (!Options::validateAOTTypedFacts()) [[likely]]
        return Boxed(m_out.load64(knownPlaceIn(receiverAtKnownPlace(access, base), access)));
    LValue found = checkedKnownPlace(access, base);
    Boxed general = emitGetById(base, access.string());
    reportUnlessAtKnownPlace(found, general.value(), base);
    return general;
}

bool Lowering::isStillAsBorn(const Value& store)
{
    if (!store.isStoreToObjectBornWhole())
        return false;
    if (m_onlyPredecessors.isEmpty()) {
        m_onlyPredecessors.fill(UINT32_MAX, m_function.blocks.size());
        for (uint32_t i = 0; i < m_function.blocks.size(); ++i) {
            for (auto& target : m_function.blocks[i].terminator.targets) {
                uint32_t& only = m_onlyPredecessors[target.block.index()];
                only = only == UINT32_MAX ? i : UINT32_MAX - 1;
            }
        }
    }
    uint32_t row = store.birthOfFirstStore() - 1;
    const Block* block = m_currentBlock;
    size_t index = m_currentIndexInBlock;
    for (unsigned blocks = 0; blocks < 64; ++blocks) {
        while (index--) {
            auto& earlier = m_function.at(block->values[index]);
            if (earlier.hasRowOfBirth() && earlier.rowOfBirth() == row)
                return true;
            bool callsNothing = earlier.opcode == Opcode::Undefined || earlier.opcode == Opcode::Null || earlier.opcode == Opcode::Boolean || earlier.opcode == Opcode::Integer || earlier.opcode == Opcode::Double
                || (earlier.opcode == Opcode::Cast && earlier.castReason() == CastReason::Upcast)
                || (earlier.isStoreToObjectBornWhole() && earlier.isAtKnownPlace() && m_program.rowsOfBirths[earlier.birthOfFirstStore() - 1].isWhole);
            if (!callsNothing)
                return false;
        }
        uint32_t previous = m_onlyPredecessors[block - m_function.blocks.span().data()];
        if (previous >= UINT32_MAX - 1)
            return false;
        block = &m_function.blocks[previous];
        index = block->values.size();
    }
    return false;
}

void Lowering::giveThisItsRow(const Value& store, Boxed self)
{
    auto& row = m_program.rowsOfBirths[store.birthOfFirstStore() - 1];
    Word32 count = word32(row.names.size());
    Word32 names = word32(rowOfNames(row.names.span()));
    bool hasSlot = !(m_program.runsOnce(m_id) && !m_isInLoop) && m_context.sites.size() < std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots);
    if (!hasSlot) {
        AOT_TYPED_OPERATION(GiveThisItsRow, Pointer(m_out.constIntPtr(0)), self, count, names, word32(row.number + 1));
        return;
    }
    m_context.sites.append(Site { });
    Pointer cache = slotAddress(m_context.sites.size() - 1);
    auto inCache = [&](size_t offset) {
        return TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(cache.value(), m_out.constIntPtr(offset)));
    };
    LBasicBlock isObject = m_out.newBlock();
    LBasicBlock isKnown = m_out.newBlock();
    LBasicBlock asks = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(self).value(), usually(isObject), rarely(asks));
    m_out.appendTo(isObject);
    CellPointer cell = asCell(self);
    m_out.branch(m_out.equal(m_out.load32(cell.value(), m_heaps.JSCell_structureID), m_out.load32(inCache(OBJECT_OFFSETOF(AOT::Slot, structureID)))), usually(isKnown), rarely(asks));
    m_out.appendTo(isKnown);
    didEmitWrite(Writes::Heap);
    m_out.store32(m_out.load32(inCache(OBJECT_OFFSETOF(AOT::Slot, newStructureID))), cell.value(), m_heaps.JSCell_structureID);
    m_out.jump(continuation);
    m_out.appendTo(asks);
    AOT_TYPED_OPERATION(GiveThisItsRow, cache, self, count, names, word32(row.number + 1));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

LValue Lowering::isOfNumberedRow(LValue structureID, unsigned row)
{
    return m_out.equal(m_out.load16ZeroExt32(TypedPointer(m_heaps.root, m_out.add(structureWithID(structureID), m_out.constIntPtr(Structure::offsetOfNumberOfRowPlusOne())))), m_out.constInt32(row + 1));
}

void Lowering::putAtKnownPlace(const Value& access, Boxed base, Boxed value)
{
    if (Options::validateAOTTypedFacts() && access.isStoreToObjectBornWhole()) [[unlikely]] {
        emitPutById(base, access.string(), value);
        reportUnlessAtKnownPlace(checkedKnownPlace(access, base), value.value(), base);
        return;
    }
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        LValue found = checkedKnownPlace(access, base);
        emitPutById(base, access.string(), value);
        LBasicBlock loads = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(m_out.notZero64(found), unsure(loads), unsure(continuation));
        m_out.appendTo(loads);
        reportUnlessAtKnownPlace(m_out.load64(knownPlaceIn(asCell(base), access)), value.value(), base);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return;
    }
    CellPointer cell = receiverAtKnownPlace(access, base);
    didEmitWrite(Writes::Heap);
    m_out.store64(value.value(), knownPlaceIn(cell, access));
    if (!typeOf(access.child(1)).hasCells() || isStillAsBorn(access))
        return;
    LBasicBlock storesCell = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(value).value(), unsure(storesCell), unsure(continuation));
    m_out.appendTo(storesCell);
    barrier(cell);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

Boxed Lowering::emitGetById(Boxed base, StringID name, bool isCalled, const Function* definedAs)
{
    if (name == m_length)
        return emitGetLength(base);
    if (BuiltinID method = m_builtins.callOrApply(m_program.builtinName(name)))
        return getCallOrApply(base, name, method, definedAs);
    if (BuiltinID method = bindIfNamed(name))
        return getCallOrApply(base, name, method, definedAs);
    if (auto row = bufferAccessorNamed(name))
        return getBufferAccessor(base, name, *row, isCalled);
    if (isNamedLastIndex(name))
        return getLastIndex(base, name, isCalled);
    return emitCachedGetById(base, name, isCalled);
}

const Lowering::RunOfAccesses* Lowering::runOfCurrentAccess(StringID name)
{
    if (m_runsOfAccesses.isEmpty() || m_currentIndexInBlock >= m_currentBlock->values.size() || !m_out.m_block->frequency())
        return nullptr;
    auto& current = m_function.at(m_currentBlock->values[m_currentIndexInBlock]);
    if ((current.opcode != Opcode::GetById && current.opcode != Opcode::PutById) || current.string() != name)
        return nullptr;
    auto found = m_runsOfAccesses.find(current.child(0).index());
    if (found == m_runsOfAccesses.end() || !found->value.names.contains(name))
        return nullptr;
    return &found->value;
}

LValue Lowering::placeByRun(const RunOfAccesses& run, Boxed base, StringID name, LBasicBlock own)
{
    unsigned index = run.names.find(name);
    uint64_t key = keyOfAccessInRun(m_currentBlockID, m_currentIndexInBlock);
    bool follows = m_accessesThatFollowAnother.contains(key);
    bool learns = !m_accessesThatAnotherDominates.contains(key);
    auto blockOfTest = [&] {
        return follows ? newColdBlock() : m_out.newBlock();
    };
    LBasicBlock tests = blockOfTest();
    LBasicBlock testsCell = blockOfTest();
    LBasicBlock hasPassed = blockOfTest();
    LBasicBlock isKnown = m_out.newBlock();
    LBasicBlock isCovered = m_out.newBlock();
    LValue verdict = m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), run.verdict);
    if (follows)
        m_out.branch(verdict, usually(isKnown), rarely(tests));
    else
        m_out.branch(verdict, unsure(isKnown), unsure(tests));
    m_out.appendTo(tests);
    m_out.branch(isCell(base).value(), usually(testsCell), rarely(own));
    m_out.appendTo(testsCell);
    Pointer header = slotAddress(run.header, follows ? SlotAddressIsFor::ColdBlock : SlotAddressIsFor::UsualPath);
    LValue learned = m_out.load32(TypedPointer(m_heaps.root, m_out.add(header.value(), m_out.constIntPtr(OBJECT_OFFSETOF(AOT::Slot, structureID)))));
    LValue structureID = m_out.load32(TypedPointer(m_heaps.root, m_out.add(base.value(), m_out.constIntPtr(JSCell::structureIDOffset()))));
    LBasicBlock isAnother = learns ? newColdBlock() : own;
    m_out.branch(m_out.equal(learned, structureID), usually(hasPassed), rarely(isAnother));
    if (learns) {
        LBasicBlock asks = newColdBlock();
        m_out.appendTo(isAnother);
        Pointer headerAgain = slotAddress(run.header, SlotAddressIsFor::ColdBlock);
        LValue attempts = m_out.bitAnd(m_out.load32(TypedPointer(m_heaps.root, m_out.add(headerAgain.value(), m_out.constIntPtr(OBJECT_OFFSETOF(AOT::Slot, offset))))), m_out.constInt32(AOT::Slot::attemptsMask));
        m_out.branch(m_out.equal(attempts, m_out.constInt32(AOT::Slot::attemptsMask)), unsure(own), unsure(asks));
        m_out.appendTo(asks);
        std::array<uint64_t, 2> numbers { };
        for (unsigned i = 0; i < 6; ++i) {
            uint64_t number = i < run.names.size() && run.names[i].index() < 0x1fffff ? run.names[i].index() : 0x1fffff;
            numbers[i / 3] |= number << (i % 3 * 21);
        }
        Word32 hasLearned = AOT_TYPED_OPERATION(LearnRun, headerAgain, base, word32((run.names.size() + 7) / 8 << 24 | (run.stored & 0xffffff)), integer(static_cast<int64_t>(numbers[0])), integer(static_cast<int64_t>(numbers[1])));
        m_out.branch(hasLearned.value(), unsure(hasPassed), unsure(own));
    }
    m_out.appendTo(hasPassed);
    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), run.verdict, m_out.int32One);
    m_out.jump(isKnown);
    m_out.appendTo(isKnown);
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        Boxed asOfNow(m_out.opaque(base.value()));
        LBasicBlock isCellNow = m_out.newBlock();
        LBasicBlock isLost = newColdBlock();
        LBasicBlock holds = m_out.newBlock();
        m_out.branch(isCell(asOfNow).value(), usually(isCellNow), rarely(isLost));
        m_out.appendTo(isCellNow);
        LValue learnedNow = m_out.load32(TypedPointer(m_heaps.root, m_out.add(slotAddress(run.header, SlotAddressIsFor::ColdBlock).value(), m_out.constIntPtr(OBJECT_OFFSETOF(AOT::Slot, structureID)))));
        LValue structureNow = m_out.load32(TypedPointer(m_heaps.root, m_out.add(asOfNow.value(), m_out.constIntPtr(JSCell::structureIDOffset()))));
        m_out.branch(m_out.bitOr(m_out.equal(learnedNow, structureNow), m_out.isZero32(learnedNow)), usually(holds), rarely(isLost));
        m_out.appendTo(isLost);
        AOT_TYPED_OPERATION(FactDoesNotHold, base, word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
        m_out.jump(holds);
        m_out.appendTo(holds);
    }
    LValue placeKnown = m_out.load8ZeroExt32(TypedPointer(m_heaps.root, m_out.add(slotAddress(run.header + index / 8).value(), m_out.constIntPtr(OBJECT_OFFSETOF(AOT::Slot, pointer) + index % 8))));
    LBasicBlock hasNoPlace = newColdBlock();
    LBasicBlock asksForPlace = newColdBlock();
    ValueFromBlock known = m_out.anchor(placeKnown);
    m_out.branch(placeKnown, usually(isCovered), rarely(hasNoPlace));
    m_out.appendTo(hasNoPlace);
    Pointer headerOfPlaces = slotAddress(run.header, SlotAddressIsFor::ColdBlock);
    LValue asked = m_out.bitAnd(m_out.load32(TypedPointer(m_heaps.root, m_out.add(headerOfPlaces.value(), m_out.constIntPtr(OBJECT_OFFSETOF(AOT::Slot, offset))))), m_out.constInt32(1u << index));
    m_out.branch(asked, unsure(own), unsure(asksForPlace));
    m_out.appendTo(asksForPlace);
    Word32 placeLearned = AOT_TYPED_OPERATION(LearnPlace, headerOfPlaces, base, word32(index | (run.stored >> index & 1) << 8), word32(name.index()));
    ValueFromBlock learnedNow = m_out.anchor(placeLearned.value());
    m_out.branch(placeLearned.value(), unsure(isCovered), unsure(own));
    m_out.appendTo(isCovered);
    LValue place = m_out.phi(Int32, known, learnedNow);
    return m_out.add(base.value(), m_out.shl(m_out.zeroExtPtr(place), m_out.constInt32(3)));
}

Boxed Lowering::emitCachedGetById(Boxed base, StringID name, bool isCalled)
{
    auto* run = isCalled ? nullptr : runOfCurrentAccess(name);
    if (!run)
        return emitCachedGetByIdOnItsOwn(base, name);
    LBasicBlock own = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock fast = m_out.anchor(m_out.load64(TypedPointer(m_heaps.root, placeByRun(*run, base, name, own))));
    m_out.jump(continuation);
    ValueFromBlock slow;
    inColdBlock(own, [&] {
        slow = m_out.anchor(emitCachedGetByIdOnItsOwn(base, name).value());
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
    return Boxed(m_out.phi(Int64, fast, slow));
}

Boxed Lowering::emitCachedGetByIdOnItsOwn(Boxed base, StringID name)
{
    auto slot = allocateSite(name, 0);
    if (uint32_t identifier = !slot && name.index() < m_program.identifierNumbers.size() ? m_program.identifierNumbers[name.index()] : 0) {
        LValue found = callICStub(Stub::TryGetByNameAndRow, Int64, { { base.value(), firstStubOperandGPR } }, std::pair { GPRInfo::argumentGPR1, identifier }, { Writes::Nothing, false });
        LBasicBlock generally = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fast = m_out.anchor(found);
        m_out.branch(m_out.notZero64(found), usually(continuation), rarely(generally));
        m_out.appendTo(generally);
        ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(GetByName, base, word32(name.index())).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return Boxed(m_out.phi(Int64, fast, slow));
    }
    if (!slot)
        return AOT_TYPED_OPERATION(GetByName, base, word32(name.index()));
    return Boxed(callICStub(Stub::GetById, Int64, { { base.value(), firstStubOperandGPR }, { slotAddress(*slot).value(), GPRInfo::argumentGPR1 } }));
}

void Lowering::emitPutById(Boxed base, StringID name, Boxed value)
{
    if (isNamedLastIndex(name))
        return putLastIndex(base, name, value);
    emitCachedPutById(base, name, value);
}

void Lowering::emitCachedPutById(Boxed base, StringID name, Boxed value)
{
    auto* run = runOfCurrentAccess(name);
    if (!run) {
        emitCachedPutByIdOnItsOwn(base, name, value);
        return;
    }
    LBasicBlock own = newColdBlock();
    LBasicBlock storesCell = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_storesThatKeepStructures.add(m_out.store64(value.value(), TypedPointer(m_heaps.root, placeByRun(*run, base, name, own))));
    m_out.branch(isCell(value).value(), unsure(storesCell), unsure(continuation));
    m_out.appendTo(storesCell);
    barrier(asCell(base));
    m_out.jump(continuation);
    inColdBlock(own, [&] {
        emitCachedPutByIdOnItsOwn(base, name, value, true);
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
}

void Lowering::emitCachedPutByIdOnItsOwn(Boxed base, StringID name, Boxed value, bool onlyThroughStub, unsigned flagsOfSite)
{
    constexpr unsigned isStrict = 2;
    auto slot = allocateSite(name, isStrict | flagsOfSite);
    if (!slot && flagsOfSite) {
        AOT_TYPED_OPERATION(SetByNameInRowOfThis, base, word32(name.index()), value);
        return;
    }
    if (uint32_t identifier = !slot && name.index() < m_program.identifierNumbers.size() ? m_program.identifierNumbers[name.index()] : 0) {
        LValue stored = callICStub(Stub::TryPutByNameAndRow, Int64, { { base.value(), firstStubOperandGPR }, { value.value(), GPRInfo::argumentGPR1 } }, std::pair { GPRInfo::argumentGPR2, identifier }, { Writes::Heap, false });
        LBasicBlock generally = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(m_out.notZero64(stored), usually(continuation), rarely(generally));
        m_out.appendTo(generally);
        AOT_TYPED_OPERATION(SetByName, base, word32(name.index()), value);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return;
    }
    if (!slot) {
        AOT_TYPED_OPERATION(SetByName, base, word32(name.index()), value);
        return;
    }
    auto throughStub = [&](SlotAddressIsFor use) {
        callICStub(Stub::PutById, Void, { { base.value(), firstStubOperandGPR }, { value.value(), GPRInfo::argumentGPR1 }, { slotAddress(*slot, use).value(), GPRInfo::argumentGPR2 } });
    };
    if (onlyThroughStub || (!m_isInLoop && (m_function.isTyped() || !m_program.isHotOutsideItsLoops(m_id)))) {
        throughStub(SlotAddressIsFor::UsualPath);
        return;
    }
    Pointer address = slotAddress(*slot);

    // The usual hit is tested for in place, as generatePutById() tests for it: the object has the structure the slot was filled for and keeps it, nothing is asked of what is
    // stored, and the property is in the object itself.
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(base).value(), usually(isCellCase), rarely(miss));
    m_out.appendTo(isCellCase);
    CellPointer cell = asCell(base);
    // The slot and the structure ID are read as any memory: whatever is written, by whatever call, they are read again behind it. Only Writes::Heap is said to reach their own heaps, and a collection
    // clears slots, a coercion gives an object another structure.
    Integer word(m_out.load64(TypedPointer(m_heaps.root, address.value())));
    static_assert(OBJECT_OFFSETOF(AOT::Slot, newStructureID) == sizeof(uint64_t) && OBJECT_OFFSETOF(AOT::Slot, fieldType) == sizeof(uint64_t) + sizeof(uint32_t));
    LBasicBlock hasStructureCase = m_out.newBlock();
    LBasicBlock keepsStructureCase = m_out.newBlock();
    Word32 hasStructure = equal(low32(word), Word32(m_out.load32(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(JSCell::structureIDOffset()))))));
    m_out.branch(hasStructure.value(), usually(hasStructureCase), rarely(miss));
    m_out.appendTo(hasStructureCase);
    Integer newStructureAndFieldType(m_out.load64(TypedPointer(m_heaps.root, m_out.add(address.value(), m_out.constIntPtr(sizeof(uint64_t))))));
    m_out.branch(isZero(newStructureAndFieldType).value(), usually(keepsStructureCase), rarely(miss));
    m_out.appendTo(keepsStructureCase);
    constexpr uint64_t offsetIsNegative = 1ull << (32 + AOT::Slot::offsetBits - 1);
    Word32 isInObject = isZero(Integer(m_out.bitAnd(word.value(), m_out.constInt64(static_cast<int64_t>(offsetIsNegative)))));
    m_out.branch(isInObject.value(), usually(hit), rarely(miss));
    m_out.appendTo(hit);
    LValue location = m_out.bitAnd(m_out.lShr(word.value(), m_out.constInt32(32)), m_out.constInt64(AOT::Slot::offsetMask));
    m_storesThatKeepStructures.add(m_out.store64(value.value(), TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.shl(location, m_out.constInt32(3))))));
    LBasicBlock storesCell = m_out.newBlock();
    m_out.branch(isCell(value).value(), unsure(storesCell), unsure(continuation));
    m_out.appendTo(storesCell);
    barrier(cell);
    m_out.jump(continuation);
    inColdBlock(miss, [&] {
        throughStub(SlotAddressIsFor::ColdBlock);
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
}

// Nothing: the function has as many sites as it can have.
bool Lowering::emitDeleteByVal(Boxed base, Boxed key)
{
    if (m_context.sites.size() + 2 > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return false;
    m_context.sites.append(Site { });
    m_context.sites.append(Site { });
    callICStub(Stub::DeleteByValCached, Void, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 }, { slotAddress(m_context.sites.size() - 2).value(), GPRInfo::argumentGPR2 } });
    return true;
}

bool Lowering::emitDefineByName(Boxed object, StringID name, Boxed value)
{
    auto text = m_program.string(name);
    bool mayBeIndex = !text.empty() && std::ranges::all_of(text, [](uint8_t character) {
        return isASCIIDigit(character);
    });
    constexpr unsigned isDirectAndStrict = 3;
    auto slot = mayBeIndex ? std::nullopt : allocateSite(name, isDirectAndStrict);
    if (!slot)
        return false;
    callICStub(Stub::PutById, Void, { { object.value(), firstStubOperandGPR }, { value.value(), GPRInfo::argumentGPR1 }, { slotAddress(*slot).value(), GPRInfo::argumentGPR2 } });
    return true;
}

Boxed Lowering::emitGetByVal(Boxed base, Boxed key, bool mayBeName)
{
    return Boxed(callICStub(mayBeName ? Stub::GetByValCached : Stub::GetByVal, Int64, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 } }));
}

void Lowering::emitPutByVal(Boxed base, Boxed key, Boxed value, bool mayBeName)
{
    if (mayBeName) {
        callICStub(Stub::PutByValCached, Void, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 }, { value.value(), GPRInfo::argumentGPR2 } });
        return;
    }
    constexpr uint32_t isStrict = 1;
    callICStub(Stub::PutByVal, Void, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 }, { value.value(), GPRInfo::argumentGPR2 } }, std::pair { GPRInfo::argumentGPR3, isStrict });
}

static Stub behindInt32Test(Stub stub)
{
    switch (stub) {
    case Stub::Add:
        return Stub::AddBehindInt32Test;
    case Stub::Sub:
        return Stub::SubBehindInt32Test;
    case Stub::Mul:
        return Stub::MulBehindInt32Test;
    case Stub::BitAnd:
        return Stub::BitAndBehindInt32Test;
    case Stub::BitOr:
        return Stub::BitOrBehindInt32Test;
    case Stub::BitXor:
        return Stub::BitXorBehindInt32Test;
    case Stub::LShift:
        return Stub::LShiftBehindInt32Test;
    case Stub::RShift:
        return Stub::RShiftBehindInt32Test;
    case Stub::URShift:
        return Stub::URShiftBehindInt32Test;
    case Stub::Less:
        return Stub::LessBehindInt32Test;
    case Stub::LessEq:
        return Stub::LessEqBehindInt32Test;
    case Stub::Greater:
        return Stub::GreaterBehindInt32Test;
    case Stub::GreaterEq:
        return Stub::GreaterEqBehindInt32Test;
    default:
        return stub;
    }
}

// The stubs do numbers themselves and call the engine for the rest.
Boxed Lowering::emitGenericArithmetic(Stub stub, Boxed a, Boxed b, OperationEffects effects)
{
    if (m_hasFailedInt32Test)
        stub = behindInt32Test(stub);
    return Boxed(callICStub(stub, Int64, { { a.value(), firstStubOperandGPR }, { b.value(), GPRInfo::argumentGPR1 } }, std::nullopt, effects));
}

Word32 Lowering::emitGenericCompare(Stub stub, Boxed a, Boxed b, OperationEffects effects)
{
    if (m_hasFailedInt32Test)
        stub = behindInt32Test(stub);
    return Word32(callICStub(stub, Int32, { { a.value(), firstStubOperandGPR }, { b.value(), GPRInfo::argumentGPR1 } }, std::nullopt, effects));
}

// Predictions.
//
// In code written in JavaScript every value is anything, and the checker says what it expects it to be: Value::prediction. That may be
// wrong, so nothing is taken from it but the order in which things are tried. An operation that would be carried out the general way
// looks at what is expected of its own operands and tests for that. Where the tests pass the operands ARE of those types, and the
// operation is lowered the way it is for operands whose types say so. Where one fails, the general way is taken. Then both go on together.
//
// What is done in place is what the language says, in every case that gets there. The lowering for typed operands has cases where it
// goes by a rule of its own (a hole in an array, an accessor among the elements, a field that is absent, a method that is not looked up
// in the receiver). None of those is reached from here: each such function takes a block to go to instead, which is the general way.
//
// To the summaries and to whoever asks what may throw, the operation is what it is without predictions. What is emitted in place
// changes no more than the general way may, and tells the back end what it changes itself.

// Without the primitives: `undefined` and `null` pass for a reference in every description, and what is tested for is the rest.
// Nothing: nothing is expected, a test has settled it where this code runs, or the type says as much.
std::optional<Type> Lowering::predictionOf(ValueID id, uint16_t without) const
{
    if (isCompact(0))
        return std::nullopt;
    for (auto& assumption : m_assumptions) {
        if (assumption.value == id)
            return std::nullopt;
    }
    TypeID prediction = m_function.at(id).prediction;
    if (!prediction)
        return std::nullopt;
    Type expected = m_program.at(prediction).withoutPrimitives(without);
    // Which integers are expected is more than there is to test for: whether the value is boxed as an int32, or is a number at all.
    if (expected.primitives & Primitive::Int && !(expected.primitives & Primitive::Double)) {
        if (!expected.integers.isWithin(Range::int32()))
            expected.primitives |= Primitive::Double;
        expected.integers = expected.primitives & Primitive::Double ? Range::full() : Range::int32();
    }
    auto& type = typeOf(id);
    if (expected.isNever() || !expected.isWithin(type, m_program) || type.isWithin(expected, m_program))
        return std::nullopt;
    return expected;
}

// What a way to carry out an operation is chosen by.
Type Lowering::expectedTypeOf(ValueID id) const
{
    if (auto prediction = predictionOf(id))
        return *prediction;
    return typeOf(id);
}

// Goes on where the value is what is expected, and yields it in the representation of that type.
Operand Lowering::expectOrElse(const Expectation& expectation, LBasicBlock otherwise)
{
    Operand operand = get(expectation.value);
    auto& type = *expectation.type;
    if (m_asksOperandsAnew && operand.rep == Rep::JSValue)
        operand = Boxed(m_out.opaque(operand.as<Boxed>().value()));
    if (operand.rep == Rep::JSValue) {
        Boxed boxed = operand.as<Boxed>();
        // The engine boxes an int32 as one. An integral number that it keeps as a double is for whoever takes numbers.
        if (type.isInteger() && type.integers == Range::int32()) {
            orElse(isInt32(boxed), otherwise);
            return signExtended(low32(boxed));
        }
        // Only an instance has that structure.
        if (expectation.hasOriginalStructure && type.onlyEngineClass()) {
            orElse(isCell(boxed), otherwise);
            orElse(hasOriginalStructure(asCell(boxed), *type.onlyEngineClass()), otherwise);
            return asCell(boxed);
        }
    }
    Operand fitted = fitOrElse(operand, typeOf(expectation.value), type, otherwise);
    if (expectation.hasOriginalStructure && type.onlyEngineClass())
        orElse(hasOriginalStructure(fitted.as<CellPointer>(), *type.onlyEngineClass()), otherwise);
    return fitted;
}

// Whether the operation is only carried out the general way because of operands that are expected to be numbers.
bool Lowering::areExpectedToBeNumbers(std::initializer_list<ValueID> operands) const
{
    bool isExpected = false;
    for (ValueID id : operands) {
        if (isNumericOperand(typeOf(id)))
            continue;
        auto prediction = predictionOf(id);
        if (!prediction || !prediction->isNumber())
            return false;
        isExpected = true;
    }
    return isExpected;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
