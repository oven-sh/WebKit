/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"
#include "B3ExtractValue.h"

#if ENABLE(AOT)

#include "AOTOperationsObjects.h"
#include "B3BasicBlockInlines.h"
#include "B3Dominators.h"
#include "B3InsertionSetInlines.h"
#include "B3NaturalLoops.h"
#include "B3StackmapGenerationParams.h"
#include "B3VariableValue.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

LValue Lowering::loadICData()
{
    PatchpointValue* data = m_out.patchpoint(Int64);
    data->effects = Effects::none();
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
            bool hasFetchBeforeCut = false;
            for (unsigned j = loop.size(); j--;) {
                if (!fetchAt[loop.at(j)])
                    continue;
                hasFetch = true;
                hasFetchBeforeCut |= !dominators.dominates(cut, loop.at(j));
            }
            if (!hasFetch)
                continue;
            if (!hasFetchBeforeCut)
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
            bool isSingleton = openComponents.last() == block;
            for (;;) {
                BasicBlock* member = openComponents.takeLast();
                m_component[member] = number[block];
                if (!isSingleton)
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
    IndexMap<BasicBlock*, bool> isFetchLocation(proc.size(), false);
    for (BasicBlock* block : deepestFirst) {
        auto candidates = std::exchange(candidatesAt[block], { });
        double cost = 0;
        bool representsCycle = graph.isOnCycle(block);
        candidates.removeAllMatching([&](const Candidate& candidate) {
            if (!candidate.isOnCycle || block != proc[0]) {
                cost += candidate.cost;
                representsCycle |= candidate.isOnCycle;
                return false;
            }
            isFetchLocation[candidate.block] = true;
            return true;
        });
        if (needsFetch[block] || cost > block->frequency()) {
            candidates.shrink(0);
            candidates.append(Candidate { block, block->frequency(), representsCycle });
        }
        if (BasicBlock* parent = dominators.idom(block)) {
            candidatesAt[parent].appendVector(candidates);
            continue;
        }
        for (auto& candidate : candidates)
            isFetchLocation[candidate.block] = true;
    }

    B3::Variable* variable = proc.addVariable(Int64);
    InsertionSet insertionSet(proc);
    for (BasicBlock* block : proc) {
        if (isFetchLocation[block] && !fetchAt[block]) {
            fetchAt[block] = proc.clone(someFetch);
            block->appendNonTerminal(fetchAt[block]);
        }
    }
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            auto* fetch = block->at(index)->as<PatchpointValue>();
            if (!fetch || fetch->generator() != generator)
                continue;
            if (isFetchLocation[block] && fetch == fetchAt[block])
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
    IndexMap<BasicBlock*, bool> isFetchLocation(proc.size(), false);
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
            isFetchLocation[candidate.block] = true;
    }

    IndexMap<BasicBlock*, BasicBlock*> fetchLocationFor(proc.size(), nullptr);
    for (BasicBlock* block : proc.blocksInPreOrder()) {
        BasicBlock* parent = dominators.idom(block);
        fetchLocationFor[block] = isFetchLocation[block] || !parent ? block : fetchLocationFor[parent];
    }
    for (BasicBlock* block : proc) {
        BasicBlock* place = fetchLocationFor[block];
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

Pointer Lowering::slotAddress(unsigned slot, SlotAddressUse use)
{
    LValue data = loadICData();
    if (use == SlotAddressUse::ColdBlock)
        data = m_out.opaque(data);
    return Pointer(m_out.add(data, m_out.constIntPtr(static_cast<intptr_t>(AOT::Data::offsetOfSlots() + slot * sizeof(AOT::Slot)) - AOT::BiasedDataPointer::bias)));
}

LValue Lowering::callICStub(Stub stub, LType type, const Vector<std::pair<LValue, GPRReg>, 4>& operands, std::optional<std::pair<GPRReg, uint32_t>> immediate, OperationEffects effects, std::optional<std::pair<unsigned, GPRReg>> slot, LValue number)
{
    m_context.emitsCalls = true;
    LValue data = slot ? loadICData() : nullptr;
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    applyEffects(patchpoint, effects);
    bool takesReceiverInCurrentRegister = hasReceiverGPRVariants(stub) && type != Void && operands.size() == 1 && operands[0].second == firstStubOperandGPR && immediate && immediate->first == stubImmediateGPR && !thunkFor(stub, immediate->second) && !slot && !number;
    for (auto& [value, reg] : operands)
        patchpoint->append(ConstrainedValue(value, takesReceiverInCurrentRegister ? ValueRep::SomeRegister : ValueRep::reg(reg)));
    if (takesReceiverInCurrentRegister)
        patchpoint->clobberEarly(RegisterSet { stubTemporaryGPRs[0], stubTemporaryGPRs[2], stubTemporaryGPRs[4], GPRInfo::argumentGPR1 });
    unsigned dataIndex = (type != Void) + operands.size();
    int32_t offsetOfSlot = 0;
    GPRReg slotGPR = InvalidGPRReg;
    if (slot) {
        patchpoint->append(ConstrainedValue(data, ValueRep::SomeRegister));
        patchpoint->clobberLate(RegisterSet { slot->second });
        offsetOfSlot = static_cast<int32_t>(static_cast<intptr_t>(AOT::Data::offsetOfSlots() + slot->first * sizeof(AOT::Slot)) - AOT::BiasedDataPointer::bias);
        slotGPR = slot->second;
    }
    if (number)
        patchpoint->append(ConstrainedValue(number, ValueRep::reg(FPRInfo::fpRegT0)));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    if (auto declared = registersClobberedBy(stub)) {
        declared->add(GPRInfo::returnValueGPR, IgnoreVectors);
        if (immediate)
            declared->add(immediate->first, IgnoreVectors);
        patchpoint->clobberLate(*declared);
    } else {
        patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
#if CPU(X86_64)
        for (GPRReg reg : stubTemporaryGPRs)
            patchpoint->clobberLate(RegisterSet { reg });
#endif
    }
    if (type != Void)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    bool mayThrow = stub != Stub::ToBoolean && stub != Stub::TryGetByNameAndStructure && stub != Stub::TryPutByNameAndStructure && stub != Stub::PutPropertiesWithStorage;
    CallSite site;
    if (mayThrow)
        site.bits = nextSite();
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, stub, immediate, site, dataIndex, offsetOfSlot, slotGPR, takesReceiverInCurrentRegister, returnsValue = type != Void](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (slotGPR != InvalidGPRReg)
            jit.addPtr(CCallHelpers::TrustedImm32(offsetOfSlot), params[dataIndex].gpr(), slotGPR);
        if (immediate && immediate->first == stubImmediateGPR) {
            RegisterSet live = RegisterSet::registersToSaveForCCall(params.usedRegisters());
            if (returnsValue)
                live.remove(GPRInfo::returnValueGPR);
            Stub called = stubVariantPreserving(stub, live);
            if (takesReceiverInCurrentRegister) {
                called = stubVariantForName(called, immediate->second);
                Stub head = stubVariantForReceiverGPR(called, params[1].gpr());
                if (head == called) {
                    jit.move(params[1].gpr(), firstStubOperandGPR);
                    head = stubVariantForReceiverGPR(called, firstStubOperandGPR);
                }
                jit.move(CCallHelpers::TrustedImm32(immediate->second), GPRInfo::argumentGPR1);
                stubCalls->call(jit, head, site);
                return;
            }
            if (stub == Stub::GetLengthByName) {
                stubCalls->call(jit, called, site);
                return;
            }
            if (!thunkFor(called, immediate->second))
                called = stubVariantForName(called, immediate->second);
            stubCalls->call(jit, called, immediate->second, site);
            return;
        }
        if (immediate)
            jit.move(CCallHelpers::TrustedImm32(immediate->second), immediate->first);
        stubCalls->call(jit, stub, site);
    });
    if (mayThrow)
        unwindToLandingPad(patchpoint, site);
    return patchpoint;
}

bool Lowering::isReadByIndexInLoop(ValueID object)
{
    if (!m_valuesReadByIndexInLoops) {
        m_valuesReadByIndexInLoops.emplace();
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (!m_loopBlocks.get(b))
                continue;
            for (ValueID id : m_function.blocks[b].values) {
                auto& value = m_function.at(id);
                if (value.opcode == Opcode::GetByVal && typeOf(value.child(1)).isInteger())
                    m_valuesReadByIndexInLoops->set(withoutUpcasts(value.child(0)).index());
            }
        }
    }
    return m_valuesReadByIndexInLoops->get(withoutUpcasts(object).index());
}

Boxed Lowering::emitGetLength(Boxed base)
{
    bool isLengthOfIndexedValue = m_isInLoop && m_out.m_block->frequency() && m_currentIndexInBlock < m_currentBlock->values.size();
    if (isLengthOfIndexedValue) {
        auto& read = m_function.at(m_currentBlock->values[m_currentIndexInBlock]);
        isLengthOfIndexedValue = read.opcode == Opcode::GetById && read.string() == m_length && isReadByIndexInLoop(read.child(0));
    }
    if (!isLengthOfIndexedValue || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return emitGetLengthByStub(base);
    LBasicBlock testsType = m_out.newBlock();
    LBasicBlock isArray = m_out.newBlock();
    LBasicBlock fits = m_out.newBlock();
    LBasicBlock general = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(base).value(), usually(testsType), rarely(general));
    m_out.appendTo(testsType);
    CellPointer array = asCell(base);
    m_out.branch(isPlainArray(array).value(), usually(isArray), rarely(general));
    m_out.appendTo(isArray);
    Integer length = arrayLength(array);
    m_out.branch(below(length, integer(1ll << 31)).value(), usually(fits), rarely(general));
    m_out.appendTo(fits);
    ValueFromBlock fast = m_out.anchor(boxInt32(low32(length)).value());
    m_out.jump(continuation);
    ValueFromBlock slow;
    inColdBlock(general, [&] {
        slow = m_out.anchor(emitGetLengthByStub(base).value());
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
    return Boxed(m_out.phi(Int64, fast, slow));
}

Boxed Lowering::emitGetLengthByStub(Boxed base)
{
    if (auto number = tableIndexOfName(m_length)) {
        setNameOfLength(*number);
        return Boxed(callICStub(Stub::GetLengthByName, Int64, { { base.value(), firstStubOperandGPR } }, std::pair { stubImmediateGPR, *number }));
    }
    if (m_context.sites.size() >= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return AOT_TYPED_OPERATION(GetByName, base, word32(m_length.index()));
    Site site { };
    site.identifierAndExtra = static_cast<uint32_t>(WellKnownIdentifier::Length);
    m_context.sites.append(site);
    return Boxed(callICStub(Stub::GetLength, Int64, { { base.value(), firstStubOperandGPR } }, std::nullopt, { }, std::pair { static_cast<unsigned>(m_context.sites.size() - 1), GPRInfo::argumentGPR1 }));
}

TypedPointer Lowering::knownSlotIn(CellPointer cell, const Value& access)
{
    static_assert(Value::numberOfKnownSlots == JSFinalObject::maxInlineCapacity);
    return TypedPointer(m_heaps.JSCellHeaderAndNamedProperties, m_out.add(cell.value(), m_out.constIntPtr(JSObject::offsetOfInlineStorage() + access.knownSlot() * sizeof(EncodedJSValue))));
}

CellPointer Lowering::receiverAtKnownSlot(const Value& access, Boxed base)
{
    if (access.receiverIsNeverNullish())
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

LValue Lowering::checkedKnownSlot(const Value& access, Boxed base)
{
    uint32_t placeAndFlags = access.knownSlot() | (access.receiverIsNeverNullish() ? 0x100 : 0) | (access.opcode == Opcode::PutById ? 0x200 : 0) | (access.isStoreIntoInitialStructure() ? 0x400 : 0);
    LValue holds = AOT_TYPED_OPERATION(CheckKnownSlot, base, word32(access.string().index()), word32(placeAndFlags), word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index())).value();
    LBasicBlock loads = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock nothing = m_out.anchor(m_out.constInt64(0));
    m_out.branch(m_out.notZero32(holds), unsure(loads), unsure(continuation));
    m_out.appendTo(loads);
    ValueFromBlock loaded = m_out.anchor(m_out.load64(knownSlotIn(asCell(base), access)));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(Int64, nothing, loaded);
}

void Lowering::reportUnlessAtKnownSlot(LValue found, LValue expected, Boxed base)
{
    LBasicBlock doesNotHold = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.bitOr(m_out.isZero64(found), m_out.equal(found, expected)), usually(continuation), rarely(doesNotHold));
    m_out.appendTo(doesNotHold);
    AOT_TYPED_OPERATION(FactViolated, base, word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

Boxed Lowering::getAtKnownSlot(const Value& access, Boxed base)
{
    if (!Options::validateAOTTypedFacts()) [[likely]]
        return Boxed(m_out.load64(knownSlotIn(receiverAtKnownSlot(access, base), access)));
    LValue found = checkedKnownSlot(access, base);
    Boxed general = emitGetById(base, access.string());
    reportUnlessAtKnownSlot(found, general.value(), base);
    return general;
}

bool Lowering::hasInitialStructureStill(const Value& store)
{
    if (!store.isStoreIntoInitialStructure())
        return false;
    if (m_singlePredecessors.isEmpty()) {
        m_singlePredecessors.fill(UINT32_MAX, m_function.blocks.size());
        for (uint32_t i = 0; i < m_function.blocks.size(); ++i) {
            for (auto& target : m_function.blocks[i].terminator.targets) {
                uint32_t& only = m_singlePredecessors[target.block.index()];
                only = only == UINT32_MAX ? i : UINT32_MAX - 1;
            }
        }
    }
    uint32_t row = store.initialStructureOfFirstStore() - 1;
    const Block* block = m_currentBlock;
    size_t index = m_currentIndexInBlock;
    for (unsigned blocks = 0; blocks < 64; ++blocks) {
        while (index--) {
            auto& earlier = m_function.at(block->values[index]);
            if (earlier.hasInitialStructure() && earlier.initialStructure() == row)
                return true;
            bool callsNothing = earlier.opcode == Opcode::Undefined || earlier.opcode == Opcode::Null || earlier.opcode == Opcode::Boolean || earlier.opcode == Opcode::Integer || earlier.opcode == Opcode::Double
                || (earlier.opcode == Opcode::Cast && earlier.castReason() == CastReason::Upcast)
                || (earlier.isStoreIntoInitialStructure() && earlier.isAtKnownSlot() && m_program.initialStructures[earlier.initialStructureOfFirstStore() - 1].isWhole);
            if (!callsNothing)
                return false;
        }
        uint32_t previous = m_singlePredecessors[block - m_function.blocks.span().data()];
        if (previous >= UINT32_MAX - 1)
            return false;
        block = &m_function.blocks[previous];
        index = block->values.size();
    }
    return false;
}

void Lowering::assignThisStructure(const Value& store, Boxed self)
{
    auto& row = m_program.initialStructures[store.initialStructureOfFirstStore() - 1];
    Word32 count = word32(row.names.size());
    Word32 names = word32(rowOfNames(row.names.span()));
    bool hasSlot = !(m_program.runsOnce(m_id) && !m_isInLoop) && m_context.sites.size() < std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots);
    if (!hasSlot) {
        AOT_TYPED_OPERATION(AssignThisStructure, Pointer(m_out.constIntPtr(0)), self, count, names, word32(row.number + 1));
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
    AOT_TYPED_OPERATION(AssignThisStructure, cache, self, count, names, word32(row.number + 1));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::putAtKnownSlot(const Value& access, Boxed base, Boxed value)
{
    if (Options::validateAOTTypedFacts() && access.isStoreIntoInitialStructure()) [[unlikely]] {
        emitPutById(base, access.string(), value);
        reportUnlessAtKnownSlot(checkedKnownSlot(access, base), value.value(), base);
        return;
    }
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        LValue found = checkedKnownSlot(access, base);
        emitPutById(base, access.string(), value);
        LBasicBlock loads = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(m_out.notZero64(found), unsure(loads), unsure(continuation));
        m_out.appendTo(loads);
        reportUnlessAtKnownSlot(m_out.load64(knownSlotIn(asCell(base), access)), value.value(), base);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return;
    }
    CellPointer cell = receiverAtKnownSlot(access, base);
    didEmitWrite(Writes::Heap);
    m_out.store64(value.value(), knownSlotIn(cell, access));
    if (!typeOf(access.child(1)).hasCells() || hasInitialStructureStill(access))
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

const Lowering::AccessRun* Lowering::currentAccessRun(StringID name)
{
    if (m_accessRuns.isEmpty() || m_currentIndexInBlock >= m_currentBlock->values.size() || !m_out.m_block->frequency())
        return nullptr;
    auto& current = m_function.at(m_currentBlock->values[m_currentIndexInBlock]);
    if ((current.opcode != Opcode::GetById && current.opcode != Opcode::PutById) || current.string() != name)
        return nullptr;
    auto found = m_accessRuns.find(current.child(0).index());
    if (found == m_accessRuns.end() || !found->value.names.contains(name))
        return nullptr;
    return &found->value;
}

LValue Lowering::slotFromAccessRun(const AccessRun& run, Boxed base, StringID name, LBasicBlock own)
{
    unsigned index = run.names.find(name);
    uint64_t key = accessRunKey(m_currentBlockID, m_currentIndexInBlock);
    bool follows = m_followingAccesses.contains(key);
    bool learns = !m_dominatedAccesses.contains(key);
    auto testBlock = [&] {
        return follows ? newColdBlock() : m_out.newBlock();
    };
    LBasicBlock tests = testBlock();
    LBasicBlock testsCell = testBlock();
    LBasicBlock hasPassed = testBlock();
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
    if (run.findStructureDescriptor != Format::none) {
        LValue descriptorSlotIndex = m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::structureDescriptorsDisplacement() + static_cast<ptrdiff_t>(Instance::numberOfReservedDescriptorIndices + run.findStructureDescriptor) * sizeof(uint32_t)));
        LValue descriptorStructureID = m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, m_out.add(m_instance, m_out.zeroExtPtr(descriptorSlotIndex)), Instance::structureDescriptorsDisplacement() + OBJECT_OFFSETOF(AOT::Slot, structureID)));
        m_out.branch(m_out.equal(descriptorStructureID, m_out.load32(TypedPointer(m_heaps.root, m_out.add(base.value(), m_out.constIntPtr(JSCell::structureIDOffset()))))), usually(hasPassed), rarely(own));
        m_out.appendTo(hasPassed);
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), run.verdict, m_out.int32One);
        m_out.jump(isKnown);
        m_out.appendTo(isKnown);
        m_out.jump(isCovered);
        m_out.appendTo(isCovered);
        return m_out.add(base.value(), m_out.constIntPtr(JSObject::offsetOfInlineStorage() + run.places[index] * sizeof(EncodedJSValue)));
    }
    Pointer header = slotAddress(run.header, follows ? SlotAddressUse::ColdBlock : SlotAddressUse::FastPath);
    LValue learned = m_out.load32(TypedPointer(m_heaps.root, m_out.add(header.value(), m_out.constIntPtr(OBJECT_OFFSETOF(AOT::Slot, structureID)))));
    LValue structureID = m_out.load32(TypedPointer(m_heaps.root, m_out.add(base.value(), m_out.constIntPtr(JSCell::structureIDOffset()))));
    LBasicBlock isAnother = learns ? newColdBlock() : own;
    m_out.branch(m_out.equal(learned, structureID), usually(hasPassed), rarely(isAnother));
    if (learns) {
        LBasicBlock asks = newColdBlock();
        m_out.appendTo(isAnother);
        Pointer headerAgain = slotAddress(run.header, SlotAddressUse::ColdBlock);
        LValue attempts = m_out.bitAnd(m_out.load32(TypedPointer(m_heaps.root, m_out.add(headerAgain.value(), m_out.constIntPtr(OBJECT_OFFSETOF(AOT::Slot, offset))))), m_out.constInt32(AOT::Slot::attemptsMask));
        m_out.branch(m_out.equal(attempts, m_out.constInt32(AOT::Slot::attemptsMask)), unsure(own), unsure(asks));
        m_out.appendTo(asks);
        std::array<uint64_t, 2> numbers { };
        for (unsigned i = 0; i < 6; ++i) {
            uint64_t number = i < run.names.size() && run.names[i].index() < 0x1fffff ? run.names[i].index() : 0x1fffff;
            numbers[i / 3] |= number << (i % 3 * 21);
        }
        Word32 cacheIsPopulated = AOT_TYPED_OPERATION(CachePropertyRun, headerAgain, base, word32((run.names.size() + 7) / 8 << 24 | (run.stored & 0xffffff)), integer(static_cast<int64_t>(numbers[0])), integer(static_cast<int64_t>(numbers[1])));
        m_out.branch(cacheIsPopulated.value(), unsure(hasPassed), unsure(own));
    }
    m_out.appendTo(hasPassed);
    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), run.verdict, m_out.int32One);
    m_out.jump(isKnown);
    m_out.appendTo(isKnown);
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        Boxed currentValueOf(m_out.opaque(base.value()));
        LBasicBlock isCellNow = m_out.newBlock();
        LBasicBlock isNoLongerCell = newColdBlock();
        LBasicBlock holds = m_out.newBlock();
        m_out.branch(isCell(currentValueOf).value(), usually(isCellNow), rarely(isNoLongerCell));
        m_out.appendTo(isCellNow);
        LValue populatedNow = m_out.load32(TypedPointer(m_heaps.root, m_out.add(slotAddress(run.header, SlotAddressUse::ColdBlock).value(), m_out.constIntPtr(OBJECT_OFFSETOF(AOT::Slot, structureID)))));
        LValue structureNow = m_out.load32(TypedPointer(m_heaps.root, m_out.add(currentValueOf.value(), m_out.constIntPtr(JSCell::structureIDOffset()))));
        m_out.branch(m_out.bitOr(m_out.equal(populatedNow, structureNow), m_out.isZero32(populatedNow)), usually(holds), rarely(isNoLongerCell));
        m_out.appendTo(isNoLongerCell);
        AOT_TYPED_OPERATION(FactViolated, base, word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
        m_out.jump(holds);
        m_out.appendTo(holds);
    }
    LValue knownSlotValue = m_out.load8ZeroExt32(TypedPointer(m_heaps.root, m_out.add(slotAddress(run.header + index / 8).value(), m_out.constIntPtr(OBJECT_OFFSETOF(AOT::Slot, pointer) + index % 8))));
    LBasicBlock hasNoSlot = newColdBlock();
    LBasicBlock requestsSlot = newColdBlock();
    ValueFromBlock known = m_out.anchor(knownSlotValue);
    m_out.branch(knownSlotValue, usually(isCovered), rarely(hasNoSlot));
    m_out.appendTo(hasNoSlot);
    Pointer slotsHeader = slotAddress(run.header, SlotAddressUse::ColdBlock);
    LValue asked = m_out.bitAnd(m_out.load32(TypedPointer(m_heaps.root, m_out.add(slotsHeader.value(), m_out.constIntPtr(OBJECT_OFFSETOF(AOT::Slot, offset))))), m_out.constInt32(1u << index));
    m_out.branch(asked, unsure(own), unsure(requestsSlot));
    m_out.appendTo(requestsSlot);
    Word32 cachedSlot = AOT_TYPED_OPERATION(CachePropertyLocation, slotsHeader, base, word32(index | (run.stored >> index & 1) << 8), word32(name.index()));
    ValueFromBlock populatedNow = m_out.anchor(cachedSlot.value());
    m_out.branch(cachedSlot.value(), unsure(isCovered), unsure(own));
    m_out.appendTo(isCovered);
    LValue place = m_out.phi(Int32, known, populatedNow);
    return m_out.add(base.value(), m_out.shl(m_out.zeroExtPtr(place), m_out.constInt32(3)));
}

Boxed Lowering::emitCachedGetById(Boxed base, StringID name, bool isCalled)
{
    auto* run = isCalled ? nullptr : currentAccessRun(name);
    if (!run)
        return emitStandaloneCachedGetById(base, name);
    LBasicBlock own = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock fast = m_out.anchor(m_out.load64(TypedPointer(m_heaps.root, slotFromAccessRun(*run, base, name, own))));
    m_out.jump(continuation);
    ValueFromBlock slow;
    inColdBlock(own, [&] {
        slow = m_out.anchor(emitStandaloneCachedGetById(base, name, true).value());
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
    return Boxed(m_out.phi(Int64, fast, slow));
}

std::optional<uint32_t> Lowering::tableIndexOfName(StringID name)
{
    uint32_t identifier = name.index() < m_program.identifierNumbers.size() ? m_program.identifierNumbers[name.index()] : 0;
    if (!identifier || identifier > PropertyLookupTable::maxName || (m_program.runsOnce(m_id) && !m_isInLoop))
        return std::nullopt;
    if (!m_context.identifiers.contains(name))
        m_context.identifiers.append(name);
    return PropertyLookupTable::nameForIdentifierIndex(identifier);
}

bool Lowering::usesSiteCaches(StringID name) const
{
    if (name && m_program.possibleAccessorNames.get(name.index()))
        return false;
    return isX86_64() && m_out.m_block->frequency() && m_function.module && m_program.modulesRunPerRequest.get(m_function.module.index());
}

Boxed Lowering::getByNameThroughSiteCache(Boxed base, uint32_t name)
{
    m_context.emitsCalls = true;
    unsigned word = m_context.numberOfSiteCacheWords;
    m_context.numberOfSiteCacheWords += SiteCaches::wordsOfLocation;
    constexpr GPRReg cached = stubTemporaryGPRs[2];
    constexpr GPRReg holder = stubTemporaryGPRs[4];
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    applyEffects(patchpoint, OperationEffects { });
    patchpoint->append(ConstrainedValue(base.value(), ValueRep::SomeRegister));
    patchpoint->clobberEarly(RegisterSet { cached, holder });
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    RegisterSet clobbered = *registersClobberedBy(Stub::GetByName);
    clobbered.add(GPRInfo::returnValueGPR, IgnoreVectors);
    clobbered.add(stubImmediateGPR, IgnoreVectors);
    patchpoint->clobberLate(clobbered);
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site;
    site.bits = nextSite();
    patchpoint->setGenerator([context = &m_context, name, word, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg receiver = params[1].gpr();
        CCallHelpers::Label start = jit.label();
        CCallHelpers::JumpList misses;
        misses.append(jit.branchIfNotCell(receiver));
        context->indexReferences.loadSiteCacheWord(jit, cached, word);
        misses.append(jit.branch32(CCallHelpers::NotEqual, cached, CCallHelpers::Address(receiver, JSCell::structureIDOffset())));
        context->indexReferences.loadSiteCacheWord(jit, holder, word + 1);
        IndexReferences::loadFromCachedLocation(jit, receiver, cached, holder, GPRInfo::returnValueGPR);
        CCallHelpers::Label done = jit.label();
        context->rangesKeptInline.append({ start, done });
        RegisterSet live = RegisterSet::registersToSaveForCCall(params.usedRegisters());
        live.remove(GPRInfo::returnValueGPR);
        Stub stub = stubVariantPreserving(Stub::GetByName, live) == Stub::GetByName ? Stub::GetByNameAndFillSiteCache : Stub::GetByNameAndFillSiteCachePreservingTwo;
        params.addLatePath([=](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            misses.link(&jit);
            jit.move(receiver, firstStubOperandGPR);
            jit.move(CCallHelpers::TrustedImm32(name), stubImmediateGPR);
            context->indexReferences.getAddressOfSiteCacheWord(jit, GPRInfo::argumentGPR1, word);
            context->stubCalls.call(jit, stub, site);
            jit.jump().linkTo(done, &jit);
        });
    });
    unwindToLandingPad(patchpoint, site);
    return Boxed(patchpoint);
}

void Lowering::putByNameThroughSiteCache(bool isSloppy, Boxed base, uint32_t name, Boxed value)
{
    m_context.emitsCalls = true;
    unsigned word = m_context.numberOfSiteCacheWords;
    m_context.numberOfSiteCacheWords += SiteCaches::wordsOfLocation;
    constexpr GPRReg cached = stubTemporaryGPRs[2];
    constexpr GPRReg structureAfterStore = stubTemporaryGPRs[4];
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    applyEffects(patchpoint, OperationEffects { });
    patchpoint->append(ConstrainedValue(base.value(), ValueRep::reg(firstStubOperandGPR)));
    patchpoint->append(ConstrainedValue(value.value(), ValueRep::reg(GPRInfo::argumentGPR1)));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    RegisterSet clobbered = *registersClobberedBy(Stub::PutByName);
    clobbered.add(GPRInfo::returnValueGPR, IgnoreVectors);
    clobbered.add(stubImmediateGPR, IgnoreVectors);
    patchpoint->clobberLate(clobbered);
    CallSite site;
    site.bits = nextSite();
    patchpoint->setGenerator([context = &m_context, isSloppy, name, word, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::JumpList misses;
        CCallHelpers::Label start = jit.label();
        misses.append(jit.branchIfNotCell(firstStubOperandGPR));
        context->indexReferences.loadSiteCacheWord(jit, cached, word);
        misses.append(jit.branch32(CCallHelpers::NotEqual, cached, CCallHelpers::Address(firstStubOperandGPR, JSCell::structureIDOffset())));
        context->indexReferences.loadSiteCacheWord(jit, structureAfterStore, word + 1);
        jit.urshift64(CCallHelpers::TrustedImm32(SiteCaches::offsetShift), cached);
        jit.store64(GPRInfo::argumentGPR1, CCallHelpers::BaseIndex(firstStubOperandGPR, cached, CCallHelpers::TimesOne));
        jit.store32(structureAfterStore, CCallHelpers::Address(firstStubOperandGPR, JSCell::structureIDOffset()));
        jit.load8(CCallHelpers::Address(firstStubOperandGPR, JSCell::cellStateOffset()), cached);
        CCallHelpers::Jump needsBarrier = jit.branch32(CCallHelpers::BelowOrEqual, cached, CCallHelpers::Address(instanceGPR, Instance::offsetOfBarrierThreshold()));
        CCallHelpers::Label done = jit.label();
        context->rangesKeptInline.append({ start, done });
        bool preservesTwo = stubVariantPreserving(Stub::PutByName, RegisterSet::registersToSaveForCCall(params.usedRegisters())) != Stub::PutByName;
        Stub stub = static_cast<Stub>(static_cast<unsigned>(isSloppy ? Stub::PutByNameSloppyAndFillSiteCache : Stub::PutByNameAndFillSiteCache) + preservesTwo);
        params.addLatePath([=](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            needsBarrier.link(&jit);
            context->stubCalls.call(jit, Stub::WriteBarrier, CallSite { });
            jit.jump().linkTo(done, &jit);
            misses.link(&jit);
            jit.move(CCallHelpers::TrustedImm32(name), stubImmediateGPR);
            context->indexReferences.getAddressOfSiteCacheWord(jit, stubTemporaryGPRs[3], word);
            context->stubCalls.call(jit, stub, site);
            jit.jump().linkTo(done, &jit);
        });
    });
    unwindToLandingPad(patchpoint, site);
}

Boxed Lowering::emitStandaloneCachedGetById(Boxed base, StringID name, bool isInRun)
{
    if (auto number = isInRun ? std::nullopt : tableIndexOfName(name)) {
        if (usesSiteCaches(name))
            return getByNameThroughSiteCache(base, *number);
        return Boxed(callICStub(Stub::GetByName, Int64, { { base.value(), firstStubOperandGPR } }, std::pair { stubImmediateGPR, *number }));
    }
    auto slot = allocateSite(name, 0);
    if (!slot && !isInRun && name.index() < m_program.identifierNumbers.size() && m_program.identifierNumbers[name.index()])
        return Boxed(callICStub(Stub::GetByNameUncached, Int64, { { base.value(), firstStubOperandGPR } }, std::pair { GPRInfo::argumentGPR1, name.index() }));
    if (uint32_t identifier = !slot && name.index() < m_program.identifierNumbers.size() ? m_program.identifierNumbers[name.index()] : 0) {
        LValue found = callICStub(Stub::TryGetByNameAndStructure, Int64, { { base.value(), firstStubOperandGPR } }, std::pair { GPRInfo::argumentGPR1, identifier }, { Writes::Nothing, false });
        constexpr bool mayThrow = OperationSignature<std::remove_pointer_t<decltype(operationAOTTypedGetByName)>>::mayThrow;
        if (Vector<LValue, 8> arguments { base.value(), word32(name.index()).value() }; canCallOnLatePath(Entry::operationAOTTypedGetByName, mayThrow, arguments)) {
            return Boxed(callOnLatePath(Entry::operationAOTTypedGetByName, mayThrow, effectsForOperation<operationAOTTypedGetByName>(), arguments, { }, createSharedTask<CCallHelpers::JumpList(CCallHelpers&, GPRReg, std::span<const GPRReg>)>([](CCallHelpers& jit, GPRReg result, std::span<const GPRReg>) {
                return CCallHelpers::JumpList(jit.branchTest64(CCallHelpers::Zero, result));
            }), found));
        }
        LBasicBlock genericPath = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fast = m_out.anchor(found);
        m_out.branch(m_out.notZero64(found), usually(continuation), rarely(genericPath));
        m_out.appendTo(genericPath);
        ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(GetByName, base, word32(name.index())).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return Boxed(m_out.phi(Int64, fast, slow));
    }
    if (!slot)
        return AOT_TYPED_OPERATION(GetByName, base, word32(name.index()));
    return Boxed(callICStub(Stub::GetById, Int64, { { base.value(), firstStubOperandGPR } }, std::nullopt, { }, std::pair { *slot, GPRInfo::argumentGPR1 }));
}

std::optional<Stub> Lowering::getByNameStubForType(const Type& type) const
{
    static_assert(static_cast<unsigned>(Stub::GetByIdAsDouble) + 1 == static_cast<unsigned>(Stub::GetByIdAsDoubleOrUndefined) && static_cast<unsigned>(Stub::GetByIdAsDouble) + 2 == static_cast<unsigned>(Stub::GetByIdAsDoubleOrNull) && static_cast<unsigned>(Stub::GetByIdAsDouble) + 3 == static_cast<unsigned>(Stub::GetByIdAsDoubleOrNullish));
    static_assert(static_cast<unsigned>(Stub::GetByIdAsString) + 1 == static_cast<unsigned>(Stub::GetByIdAsStringOrUndefined) && static_cast<unsigned>(Stub::GetByIdAsString) + 2 == static_cast<unsigned>(Stub::GetByIdAsStringOrNull) && static_cast<unsigned>(Stub::GetByIdAsString) + 3 == static_cast<unsigned>(Stub::GetByIdAsStringOrNullish));
    static_assert(static_cast<unsigned>(Stub::GetByIdAsBoolean) + 1 == static_cast<unsigned>(Stub::GetByIdAsBooleanOrUndefined) && static_cast<unsigned>(Stub::GetByIdAsBoolean) + 2 == static_cast<unsigned>(Stub::GetByIdAsBooleanOrNull) && static_cast<unsigned>(Stub::GetByIdAsBoolean) + 3 == static_cast<unsigned>(Stub::GetByIdAsBooleanOrNullish));
    unsigned variant = (type.primitives & Primitive::Undefined ? 1 : 0) | (type.primitives & Primitive::Null ? 2 : 0);
    Type rest = type.withoutPrimitives(Primitive::UndefinedOrNull);
    if (rest == m_program.at(m_program.types.number))
        return static_cast<Stub>(static_cast<unsigned>(Stub::GetByIdAsDouble) + variant);
    if (rest == m_program.at(m_program.types.string))
        return static_cast<Stub>(static_cast<unsigned>(Stub::GetByIdAsString) + variant);
    if (rest == m_program.at(m_program.types.boolean))
        return static_cast<Stub>(static_cast<unsigned>(Stub::GetByIdAsBoolean) + variant);
    static_assert(static_cast<unsigned>(Stub::GetByIdAsArray) + 1 == static_cast<unsigned>(Stub::GetByIdAsArrayOrUndefined) && static_cast<unsigned>(Stub::GetByIdAsArray) + 2 == static_cast<unsigned>(Stub::GetByIdAsArrayOrNull) && static_cast<unsigned>(Stub::GetByIdAsArray) + 3 == static_cast<unsigned>(Stub::GetByIdAsArrayOrNullish));
    if (auto* engineClass = rest.primitives ? nullptr : rest.singleEngineClass(); engineClass && engineClass->type == ArrayType)
        return static_cast<Stub>(static_cast<unsigned>(Stub::GetByIdAsArray) + variant);
    return std::nullopt;
}

std::optional<Operand> Lowering::getByIdWithTypeCheck(Boxed base, StringID name, TypeID held, std::optional<size_t> castIndex)
{
    auto& type = m_program.at(held);
    auto stub = getByNameStubForType(type);
    if (stub && type.hasObjects()) {
        SetForScope currentIndex(m_currentIndexInBlock, castIndex.value_or(m_currentIndexInBlock));
        if (resultMayInstantiateType(m_program.at(m_program.types.anything), type))
            return std::nullopt;
    }
    if (!stub || m_fusedCasts || m_activeHandler || handlerEnteredByBranch() || m_program.logsTypeErrors || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return std::nullopt;
    if (name == m_length || m_builtins.callOrApply(m_program.builtinName(name)) || bindIfNamed(name) || bufferAccessorNamed(name) || isNamedLastIndex(name) || currentAccessRun(name))
        return std::nullopt;
    Rep rep = repOf(type);
    if (auto number = tableIndexOfName(name)) {
        m_context.emitsCalls = true;
        static_assert(static_cast<unsigned>(Stub::GetByNameAsArrayOrNullish) - static_cast<unsigned>(Stub::GetByNameAsDouble) == 2 * (static_cast<unsigned>(Stub::GetByIdAsArrayOrNullish) - static_cast<unsigned>(Stub::GetByIdAsDouble)));
        Stub byTable = static_cast<Stub>(static_cast<unsigned>(Stub::GetByNameAsDouble) + 2 * (static_cast<unsigned>(*stub) - static_cast<unsigned>(Stub::GetByIdAsDouble)));
        PatchpointValue* patchpoint = m_out.patchpoint(b3TypeOf(rep));
        applyEffects(patchpoint, OperationEffects { });
        patchpoint->append(ConstrainedValue(base.value(), ValueRep::reg(firstStubOperandGPR)));
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        RegisterSet changed = *registersClobberedBy(Stub::GetByName);
        changed.add(GPRInfo::returnValueGPR, IgnoreVectors);
        changed.add(stubImmediateGPR, IgnoreVectors);
        patchpoint->clobberLate(changed);
        patchpoint->resultConstraints = { b3TypeOf(rep) == Double ? ValueRep::reg(FPRInfo::fpRegT0) : ValueRep::reg(GPRInfo::returnValueGPR) };
        CallSite site { nextSiteWithCastItem(castIndex) };
        patchpoint->setGenerator([context = &m_context, byTable, number = *number, site, words = throwSiteWords(ThrowKind::TypeMismatch, Rep::JSValue, static_cast<unsigned>(firstStubOperandGPR), held.index(), std::nullopt)](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            bool keepsTwoIntegers = stubVariantPreserving(Stub::GetByName, RegisterSet::registersToSaveForCCall(params.usedRegisters())) != Stub::GetByName;
            jit.move(CCallHelpers::TrustedImm32(number), stubImmediateGPR);
            context->callThrowSites.append({ jit.labelIgnoringWatchpoints(), words });
            context->stubCalls.call(jit, static_cast<Stub>(static_cast<unsigned>(byTable) + keepsTwoIntegers), site);
        });
        unwindToLandingPad(patchpoint, site);
        if (holdsCells(rep))
            return convert(Boxed(patchpoint), type, rep);
        return Operand(rep, patchpoint);
    }
    auto slot = allocateSite(name, 0);
    if (!slot)
        return std::nullopt;
    m_context.emitsCalls = true;
    LValue data = loadICData();
    PatchpointValue* patchpoint = m_out.patchpoint(b3TypeOf(rep));
    applyEffects(patchpoint, OperationEffects { });
    patchpoint->append(ConstrainedValue(base.value(), ValueRep::reg(firstStubOperandGPR)));
    patchpoint->append(ConstrainedValue(data, ValueRep::SomeRegister));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
#if CPU(X86_64)
    for (GPRReg reg : stubTemporaryGPRs)
        patchpoint->clobberLate(RegisterSet { reg });
#endif
    patchpoint->resultConstraints = { b3TypeOf(rep) == Double ? ValueRep::reg(FPRInfo::fpRegT0) : ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSiteWithCastItem(castIndex) };
    int32_t offsetOfSlot = static_cast<int32_t>(static_cast<intptr_t>(AOT::Data::offsetOfSlots() + *slot * sizeof(AOT::Slot)) - AOT::BiasedDataPointer::bias);
    patchpoint->setGenerator([context = &m_context, stub = *stub, site, offsetOfSlot, words = throwSiteWords(ThrowKind::TypeMismatch, Rep::JSValue, static_cast<unsigned>(firstStubOperandGPR), held.index(), std::nullopt)](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.addPtr(CCallHelpers::TrustedImm32(offsetOfSlot), params[2].gpr(), GPRInfo::argumentGPR1);
        context->callThrowSites.append({ jit.labelIgnoringWatchpoints(), words });
        context->stubCalls.call(jit, stub, site);
    });
    unwindToLandingPad(patchpoint, site);
    if (holdsCells(rep))
        return convert(Boxed(patchpoint), type, rep);
    return Operand(rep, patchpoint);
}

Operand Lowering::getByIdAndCheckType(Boxed base, StringID name, const Type& type, const Function* definedAs)
{
    if (TypeID given = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type; given && &m_program.at(given) == &type) {
        if (auto held = getByIdWithTypeCheck(base, name, given))
            return *held;
    }
    return ensure(emitGetById(base, name, false, definedAs), type);
}

std::optional<Operand> Lowering::getByNameForFollowingCast(ValueID result, const Value& read)
{
    auto& values = m_currentBlock->values;
    size_t next = m_currentIndexInBlock + 1;
    if (m_fusedCasts || next >= values.size() || read.rep != Rep::JSValue || read.methodGetByIdBuiltin() || useCountOf(result) != 1 || m_function.at(read.child(0)).opcode == Opcode::GetGlobal)
        return std::nullopt;
    auto& cast = m_function.at(values[next]);
    if (cast.opcode != Opcode::Cast || cast.child(0) != result || cast.string() || typeOf(result).isSubsetOf(m_program.at(cast.type), m_program))
        return std::nullopt;
    if (cast.castReason() != CastReason::As && cast.castReason() != CastReason::Claim && cast.castReason() != CastReason::Store && cast.castReason() != CastReason::Flow)
        return std::nullopt;
    auto held = getByIdWithTypeCheck(in<Boxed>(read.child(0)), read.string(), cast.type, next);
    if (!held)
        return std::nullopt;
    m_castFusedIntoRead = CastFusedIntoRead { m_currentBlock, next, *held };
    return placeholder(read.rep);
}

bool Lowering::isPlainReadByName(size_t index)
{
    ValueID id = m_currentBlock->values[index];
    auto& read = m_function.at(id);
    if (read.opcode != Opcode::GetById || read.methodGetByIdBuiltin() || read.rep != Rep::JSValue || read.isAtKnownSlot() || read.isOnSlowPath || isLazilyFetched(id) || isLazilyFetched(read.child(0)))
        return false;
    auto& anything = m_program.at(m_program.types.anything);
    if (typeOf(id) != anything || typeOf(read.child(0)) != anything || m_function.at(read.child(0)).rep != Rep::JSValue || m_function.at(read.child(0)).opcode == Opcode::GetGlobal)
        return false;
    if (predictionOf(read.child(0), Primitive::All) || predictionOf(read.child(0)) || m_accessRuns.contains(read.child(0).index()) || fusedCastCount(index) || fusedUseIndex(index, 0))
        return false;
    if (index + 1 < m_currentBlock->values.size() && m_function.at(m_currentBlock->values[index + 1]).opcode == Opcode::Cast && m_function.at(m_currentBlock->values[index + 1]).child(0) == id)
        return false;
    StringID name = read.string();
    if (name == m_length || m_builtins.callOrApply(m_program.builtinName(name)) || bindIfNamed(name) || bufferAccessorNamed(name) || isNamedLastIndex(name) || iteratorResultSlot(read.child(0), name))
        return false;
    if (m_program.possibleAccessorNames.get(name.index()) || usesSiteCaches(name))
        return false;
    return !!tableIndexOfName(name);
}

std::optional<Operand> Lowering::getByNameWithFollowingRead(ValueID result, const Value& read)
{
    auto& values = m_currentBlock->values;
    size_t next = m_currentIndexInBlock + 1;
    if (m_fusedCasts || m_activeHandler || handlerEnteredByBranch() || next >= values.size() || m_program.logsTypeErrors || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return std::nullopt;
    auto& second = m_function.at(values[next]);
    if (second.opcode != Opcode::GetById || second.inlineFrame != read.inlineFrame || !isPlainReadByName(m_currentIndexInBlock) || !isPlainReadByName(next))
        return std::nullopt;
    bool isChain = second.child(0) == result;
    bool returnsBoth = !isChain || useCountOf(result) != 1;
    bool hasSameReceiver = second.child(0) == read.child(0);
    uint32_t names = *tableIndexOfName(read.string()) | *tableIndexOfName(second.string()) << 16;
    m_context.emitsCalls = true;
    PatchpointValue* patchpoint = m_out.patchpoint(returnsBoth ? m_proc.addTuple({ Int64, Int64 }) : Int64);
    applyEffects(patchpoint, OperationEffects { });
    patchpoint->append(ConstrainedValue(in<Boxed>(read.child(0)).value(), ValueRep::reg(firstStubOperandGPR)));
    if (!isChain && !hasSameReceiver)
        patchpoint->append(ConstrainedValue(in<Boxed>(second.child(0)).value(), ValueRep::reg(secondReceiverGPR)));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    RegisterSet changed = *registersClobberedBy(Stub::GetByName);
    changed.add(GPRInfo::returnValueGPR, IgnoreVectors);
    changed.add(stubImmediateGPR, IgnoreVectors);
    changed.add(pairedNamesGPR, IgnoreVectors);
    if (returnsBoth)
        changed.add(firstOfTwoResultsGPR, IgnoreVectors);
    patchpoint->clobberLate(changed);
    if (returnsBoth)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR), ValueRep::reg(firstOfTwoResultsGPR) };
    else
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite firstSite;
    firstSite.bits = nextSite();
    CallSite site;
    {
        SetForScope currentIndex(m_currentIndexInBlock, next);
        site.bits = nextSite();
    }
    Stub stub = !returnsBoth ? Stub::GetByNameChain : isChain ? Stub::GetByNameChainReturningBoth : hasSameReceiver ? Stub::GetTwoByName : Stub::GetByNameFromTwo;
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, names, firstSite, site, stub](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        Stub called = stub;
        if (stub == Stub::GetByNameChain) {
            RegisterSet live = RegisterSet::registersToSaveForCCall(params.usedRegisters());
            live.remove(GPRInfo::returnValueGPR);
            if (stubVariantPreserving(Stub::GetByName, live) != Stub::GetByName)
                called = Stub::GetByNameChainPreservingTwo;
        }
        jit.move(CCallHelpers::TrustedImm32(names), pairedNamesGPR);
        stubCalls->callForTwoReads(jit, called, firstSite, site);
    });
    if (!returnsBoth) {
        m_readFusedIntoRead = CastFusedIntoRead { m_currentBlock, next, Operand(Boxed(patchpoint)) };
        return placeholder(read.rep);
    }
    auto resultAt = [&](int32_t index) {
        return Operand(Boxed(m_out.m_block->appendNew<B3::ExtractValue>(m_proc, m_out.origin(), Int64, patchpoint, index)));
    };
    m_readFusedIntoRead = CastFusedIntoRead { m_currentBlock, next, resultAt(0) };
    return resultAt(1);
}

std::optional<Operand> Lowering::getByNameForFollowingBranch(ValueID result, const Value& read)
{
    auto& values = m_currentBlock->values;
    size_t next = m_currentIndexInBlock + 1;
    if (m_fusedCasts || m_activeHandler || handlerEnteredByBranch() || next + 1 != values.size() || m_program.logsTypeErrors || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return std::nullopt;
    auto& test = m_function.at(values[next]);
    auto& terminator = m_currentBlock->terminator;
    if (test.opcode != Opcode::ToBoolean || test.child(0) != result || useCountOf(result) != 1 || terminator.kind != TerminatorKind::Branch || terminator.value != values[next])
        return std::nullopt;
    if (!m_isTruthinessOnlyBranchedOn.get(values[next].index()) || isLazilyFetched(values[next]) || m_isFusedWithBranch.get(values[next].index()) || fusedUseIndex(next, 0) || !(prefersGenericPathInColdCode() || isCompact()))
        return std::nullopt;
    if (!isPlainReadByName(m_currentIndexInBlock) || !toBooleanNeedsStub(typeOf(result)))
        return std::nullopt;
    m_readDeferredToBranch = result;
    return placeholder(read.rep);
}

void Lowering::branchOnTruthinessOfRead(const Value& read, FTL::WeightedTarget isTruthy, FTL::WeightedTarget isFalsy)
{
    m_context.emitsCalls = true;
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    applyEffects(patchpoint, OperationEffects { });
    patchpoint->effects.terminal = true;
    patchpoint->append(ConstrainedValue(in<Boxed>(read.child(0)).value(), ValueRep::reg(firstStubOperandGPR)));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    RegisterSet changed = *registersClobberedBy(Stub::GetByName);
    changed.add(GPRInfo::returnValueGPR, IgnoreVectors);
    changed.add(stubImmediateGPR, IgnoreVectors);
    changed.add(pairedNamesGPR, IgnoreVectors);
    patchpoint->clobberLate(changed);
    m_out.appendSuccessor(isFalsy);
    m_out.appendSuccessor(isTruthy);
    CallSite site;
    {
        SetForScope currentIndex(m_currentIndexInBlock, m_currentBlock->values.size() - 2);
        site.bits = nextSite();
    }
    patchpoint->setGenerator([context = &m_context, name = *tableIndexOfName(read.string()), site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool preservesTwo = stubVariantPreserving(Stub::GetByName, RegisterSet::registersToSaveForCCall(params.usedRegisters())) != Stub::GetByName;
        jit.move(CCallHelpers::TrustedImm32(name), pairedNamesGPR);
        context->stubCalls.call(jit, preservesTwo ? Stub::GetByNameIsTruthyPreservingTwo : Stub::GetByNameIsTruthy, site);
        bool leavesIfTruthy = params.fallsThroughToSuccessor(0);
#if CPU(X86_64)
        CCallHelpers::Jump leaves = CCallHelpers::Jump(jit.m_assembler.jCC(leavesIfTruthy ? X86Assembler::ConditionNE : X86Assembler::ConditionE));
#else
        CCallHelpers::Jump leaves = jit.branchOnFlags(leavesIfTruthy ? CCallHelpers::NotEqual : CCallHelpers::Equal);
#endif
        CCallHelpers::Jump isTruthy;
        if (!leavesIfTruthy && !params.fallsThroughToSuccessor(1))
            isTruthy = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            leaves.linkTo(*labels[leavesIfTruthy], &jit);
            if (isTruthy.isSet())
                isTruthy.linkTo(*labels[1], &jit);
        });
    });
}

void Lowering::emitPutById(Boxed base, StringID name, Boxed value)
{
    if (isNamedLastIndex(name))
        return putLastIndex(base, name, value);
    emitCachedPutById(base, name, value);
}

void Lowering::emitCachedPutById(Boxed base, StringID name, Boxed value)
{
    auto* run = currentAccessRun(name);
    if (!run) {
        emitStandaloneCachedPutById(base, name, value);
        return;
    }
    LBasicBlock own = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_structurePreservingStores.add(m_out.store64(value.value(), TypedPointer(m_heaps.root, slotFromAccessRun(*run, base, name, own))));
    if (!m_namedStoreValueIsNotCell) {
        LBasicBlock storesCell = m_out.newBlock();
        m_out.branch(isCell(value).value(), unsure(storesCell), unsure(continuation));
        m_out.appendTo(storesCell);
        barrier(asCell(base));
    }
    m_out.jump(continuation);
    inColdBlock(own, [&] {
        emitStandaloneCachedPutById(base, name, value, true);
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
}

bool Lowering::isInSloppyCode() const
{
    auto& value = m_function.at(m_currentBlock->values[m_currentIndexInBlock]);
    auto& sourceFunction = value.inlineFrame ? m_program.at(m_function.inlineFrames[value.inlineFrame].callee) : m_function;
    return sourceFunction.has(Format::FunctionIsSloppy);
}

void Lowering::setByName(Boxed base, StringID name, Boxed value)
{
    if (isInSloppyCode())
        AOT_TYPED_OPERATION(SetByNameSloppy, base, word32(name.index()), value);
    else
        AOT_TYPED_OPERATION(SetByName, base, word32(name.index()), value);
}

void Lowering::emitPutByNameThroughTable(Stub stub, Boxed base, uint32_t identifier, Boxed value)
{
    callICStub(stub, Void, { { base.value(), firstStubOperandGPR }, { value.value(), GPRInfo::argumentGPR1 } }, std::pair { stubImmediateGPR, identifier });
}

void Lowering::emitStandaloneCachedPutById(Boxed base, StringID name, Boxed value, bool onlyThroughStub, unsigned siteFlags)
{
    bool isSloppy = isInSloppyCode();
    auto* valueBoxing = value.value()->as<PatchpointValue>();
    bool storesNumberJustBoxed = valueBoxing && valueBoxing->generator() == m_context.numberBoxingGenerator.get();
    if (auto number = siteFlags || onlyThroughStub || storesNumberJustBoxed ? std::nullopt : tableIndexOfName(name)) {
        if (usesSiteCaches(name))
            putByNameThroughSiteCache(isSloppy, base, *number, value);
        else
            emitPutByNameThroughTable(isSloppy ? Stub::PutByNameSloppy : Stub::PutByName, base, *number, value);
        return;
    }
    unsigned isStrict = isSloppy ? 0 : 2;
    auto slot = allocateSite(name, isStrict | siteFlags);
    if (!slot && siteFlags && !isSloppy) {
        AOT_TYPED_OPERATION(SetByNameInThisStructure, base, word32(name.index()), value);
        return;
    }
    if (!slot && name.index() < m_program.identifierNumbers.size() && m_program.identifierNumbers[name.index()]) {
        callICStub(isSloppy ? Stub::PutByNameUncachedSloppy : Stub::PutByNameUncached, Void, { { base.value(), firstStubOperandGPR }, { value.value(), GPRInfo::argumentGPR1 } }, std::pair { GPRInfo::argumentGPR2, name.index() });
        return;
    }
    if (!slot) {
        setByName(base, name, value);
        return;
    }
    auto callStubPath = [&](SlotAddressUse) {
        if (auto* boxing = value.value()->as<PatchpointValue>(); boxing && boxing->generator() == m_context.numberBoxingGenerator.get()) {
            callICStub(Stub::PutByIdDouble, Void, { { base.value(), firstStubOperandGPR } }, std::nullopt, { }, std::pair { *slot, GPRInfo::argumentGPR2 }, boxing->child(0));
            return;
        }
        callICStub(Stub::PutById, Void, { { base.value(), firstStubOperandGPR }, { value.value(), GPRInfo::argumentGPR1 } }, std::nullopt, { }, std::pair { *slot, GPRInfo::argumentGPR2 });
    };
    if (onlyThroughStub || (!m_isInLoop && (m_function.isTyped() || !m_program.isHotOutsideLoops(m_id)))) {
        callStubPath(SlotAddressUse::FastPath);
        return;
    }
    Pointer address = slotAddress(*slot);

    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(base).value(), usually(isCellCase), rarely(miss));
    m_out.appendTo(isCellCase);
    CellPointer cell = asCell(base);
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
    Word32 isInlineEntry = isZero(Integer(m_out.bitAnd(word.value(), m_out.constInt64(static_cast<int64_t>(offsetIsNegative)))));
    m_out.branch(isInlineEntry.value(), usually(hit), rarely(miss));
    m_out.appendTo(hit);
    LValue location = m_out.bitAnd(m_out.lShr(word.value(), m_out.constInt32(32)), m_out.constInt64(AOT::Slot::offsetMask));
    m_structurePreservingStores.add(m_out.store64(value.value(), TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.shl(location, m_out.constInt32(3))))));
    if (!m_namedStoreValueIsNotCell) {
        LBasicBlock storesCell = m_out.newBlock();
        m_out.branch(isCell(value).value(), unsure(storesCell), unsure(continuation));
        m_out.appendTo(storesCell);
        barrier(cell);
    }
    m_out.jump(continuation);
    inColdBlock(miss, [&] {
        callStubPath(SlotAddressUse::ColdBlock);
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
}

std::optional<Word32> Lowering::emitDeleteByVal(Boxed base, Boxed key)
{
    if (m_context.sites.size() + 2 > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return std::nullopt;
    m_context.sites.append(Site { });
    m_context.sites.append(Site { });
    if (isInSloppyCode())
        return Word32(callICStub(Stub::DeleteByValCachedSloppy, Int32, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 } }, std::nullopt, { }, std::pair { static_cast<unsigned>(m_context.sites.size() - 2), GPRInfo::argumentGPR2 }));
    callICStub(Stub::DeleteByValCached, Void, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 } }, std::nullopt, { }, std::pair { static_cast<unsigned>(m_context.sites.size() - 2), GPRInfo::argumentGPR2 });
    return boolean(true);
}

bool Lowering::emitDefineByName(Boxed object, StringID name, Boxed value)
{
    auto text = m_program.string(name);
    bool mayBeIndex = !text.empty() && std::ranges::all_of(text, [](uint8_t character) {
        return isASCIIDigit(character);
    });
    constexpr unsigned isDirectAndStrict = 3;
    if (auto number = mayBeIndex ? std::nullopt : tableIndexOfName(name)) {
        emitPutByNameThroughTable(Stub::DefineByName, object, *number, value);
        return true;
    }
    auto slot = mayBeIndex ? std::nullopt : allocateSite(name, isDirectAndStrict);
    if (!slot)
        return false;
    if (auto* boxing = value.value()->as<PatchpointValue>(); boxing && boxing->generator() == m_context.numberBoxingGenerator.get()) {
        callICStub(Stub::PutByIdDouble, Void, { { object.value(), firstStubOperandGPR } }, std::nullopt, { }, std::pair { *slot, GPRInfo::argumentGPR2 }, boxing->child(0));
        return true;
    }
    callICStub(Stub::PutById, Void, { { object.value(), firstStubOperandGPR }, { value.value(), GPRInfo::argumentGPR1 } }, std::nullopt, { }, std::pair { *slot, GPRInfo::argumentGPR2 });
    return true;
}

Boxed Lowering::emitGetByVal(Boxed base, Boxed key, bool mayBeName)
{
    return Boxed(callICStub(mayBeName ? Stub::GetByValCached : Stub::GetByVal, Int64, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 } }));
}

void Lowering::emitPutByVal(Boxed base, Boxed key, Boxed value, bool mayBeName)
{
    bool isStrict = !isInSloppyCode();
    if (mayBeName) {
        callICStub(isStrict ? Stub::PutByValCached : Stub::PutByValCachedSloppy, Void, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 }, { value.value(), GPRInfo::argumentGPR2 } });
        return;
    }
    callICStub(isStrict ? Stub::PutByVal : Stub::PutByValSloppy, Void, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 }, { value.value(), GPRInfo::argumentGPR2 } });
}

static Stub stubAfterInt32Test(Stub stub)
{
    switch (stub) {
    case Stub::Add:
        return Stub::AddAfterInt32Check;
    case Stub::Sub:
        return Stub::SubAfterInt32Check;
    case Stub::Mul:
        return Stub::MulAfterInt32Check;
    case Stub::BitAnd:
        return Stub::BitAndAfterInt32Check;
    case Stub::BitOr:
        return Stub::BitOrAfterInt32Check;
    case Stub::BitXor:
        return Stub::BitXorAfterInt32Check;
    case Stub::LShift:
        return Stub::LShiftAfterInt32Check;
    case Stub::RShift:
        return Stub::RShiftAfterInt32Check;
    case Stub::URShift:
        return Stub::URShiftAfterInt32Check;
    case Stub::Less:
        return Stub::LessAfterInt32Check;
    case Stub::LessEq:
        return Stub::LessEqAfterInt32Check;
    case Stub::Greater:
        return Stub::GreaterAfterInt32Check;
    case Stub::GreaterEq:
        return Stub::GreaterEqAfterInt32Check;
    default:
        return stub;
    }
}

Boxed Lowering::emitGenericArithmetic(Stub stub, Boxed a, Boxed b, OperationEffects effects)
{
    if (m_hasFailedInt32Test)
        stub = stubAfterInt32Test(stub);
    return Boxed(callICStub(stub, Int64, { { a.value(), firstStubOperandGPR }, { b.value(), GPRInfo::argumentGPR1 } }, std::nullopt, effects));
}

Word32 Lowering::emitGenericCompare(Stub stub, Boxed a, Boxed b, OperationEffects effects)
{
    if (m_hasFailedInt32Test)
        stub = stubAfterInt32Test(stub);
    return Word32(callICStub(stub, Int32, { { a.value(), firstStubOperandGPR }, { b.value(), GPRInfo::argumentGPR1 } }, std::nullopt, effects));
}

std::optional<Type> Lowering::predictionOf(ValueID id, uint16_t without, PredictionUse use) const
{
    if (isCompact() && use == PredictionUse::ChoosesFasterPath)
        return std::nullopt;
    for (auto& assumption : m_assumptions) {
        if (assumption.value == id)
            return std::nullopt;
    }
    TypeID prediction = m_function.at(id).prediction();
    if (!prediction)
        return std::nullopt;
    Type expected = m_program.at(prediction).withoutPrimitives(without);
    if (expected.primitives & Primitive::Int && !(expected.primitives & Primitive::Double)) {
        if (!expected.integers.isSubsetOf(Range::int32()))
            expected.primitives |= Primitive::Double;
        expected.integers = expected.primitives & Primitive::Double ? Range::full() : Range::int32();
    }
    auto& type = typeOf(id);
    if (expected.isNever() || !expected.isSubsetOf(type, m_program) || type.isSubsetOf(expected, m_program))
        return std::nullopt;
    return expected;
}

Type Lowering::predictedTypeOf(ValueID id) const
{
    if (auto prediction = predictionOf(id))
        return *prediction;
    return typeOf(id);
}

Operand Lowering::speculateOrElse(const Expectation& expectation, LBasicBlock otherwise)
{
    Operand operand = get(expectation.value);
    auto& type = *expectation.type;
    if (m_retestsOperands && operand.rep == Rep::JSValue)
        operand = Boxed(m_out.opaque(operand.as<Boxed>().value()));
    if (operand.rep == Rep::JSValue) {
        Boxed boxed = operand.as<Boxed>();
        if (type.isInteger() && type.integers == Range::int32()) {
            orElse(isInt32(boxed), otherwise);
            return signExtended(low32(boxed));
        }
        if (expectation.hasOriginalStructure && type.singleEngineClass()) {
            orElse(isCell(boxed), otherwise);
            orElse(hasOriginalStructure(asCell(boxed), *type.singleEngineClass()), otherwise);
            return asCell(boxed);
        }
    }
    Operand fitted = narrowOrElse(operand, typeOf(expectation.value), type, otherwise);
    if (expectation.hasOriginalStructure && type.singleEngineClass())
        orElse(hasOriginalStructure(fitted.as<CellPointer>(), *type.singleEngineClass()), otherwise);
    return fitted;
}

bool Lowering::arePredictedNumbers(std::initializer_list<ValueID> operands) const
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
