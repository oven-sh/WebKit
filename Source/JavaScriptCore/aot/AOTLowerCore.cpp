/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#include "AOTImage.h"
#include "AOTCompiler.h"

// The back end is only written for ARM64 so far.
#if ENABLE(AOT) && CPU(ARM64)

#include "AirCode.h"
#include "B3PatchpointValue.h"
#include "B3SlotBaseValue.h"
#include "AirStackSlot.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "BytecodeStructs.h"
#include "CCallHelpers.h"
#include "JSCInlines.h"
#include "UnlinkedCodeBlock.h"

namespace JSC { namespace AOT {

// Whether an error thrown by the node may quote the source text at its position (ErrorInstance::SourceAppender, createTDZError()).
// Most such errors are about a base that is undefined or null, or a callee that is not a function.
static bool errorMayQuoteSource(const Graph& graph, Node* node)
{
    auto mayBeUndefinedOrNull = [&](VirtualRegister base) {
        Node* value = node->use(base);
        return !value || value->type & (TOther | TEmpty);
    };
    // (For a variable that is not a function declaration, what is proven only holds once the variable is initialized.)
    auto calleeIsKnownDeclaration = [&] {
        return graph.calleeIsExact(node) && graph.knownCallee(node)->isDeclaration;
    };
    auto calleeMayNotBeFunction = [&](VirtualRegister callee) {
        if (calleeIsKnownDeclaration())
            return false;
        Node* value = node->use(callee);
        return !value || value->type & ~TFunction;
    };
    switch (node->opcode) {
    case op_get_by_id:
        return mayBeUndefinedOrNull(node->as<OpGetById>().m_base);
    case op_get_length:
        return mayBeUndefinedOrNull(node->as<OpGetLength>().m_base);
    case op_get_by_val:
        return mayBeUndefinedOrNull(node->as<OpGetByVal>().m_base);
    case op_put_by_id:
        return mayBeUndefinedOrNull(node->as<OpPutById>().m_base);
    case op_put_by_val:
        return mayBeUndefinedOrNull(node->as<OpPutByVal>().m_base);
    case op_del_by_id:
        return mayBeUndefinedOrNull(node->as<OpDelById>().m_base);
    case op_del_by_val:
        return mayBeUndefinedOrNull(node->as<OpDelByVal>().m_base);
    case op_call:
        return calleeMayNotBeFunction(node->as<OpCall>().m_callee);
    case op_call_ignore_result:
        return calleeMayNotBeFunction(node->as<OpCallIgnoreResult>().m_callee);
    case op_tail_call:
        return calleeMayNotBeFunction(node->as<OpTailCall>().m_callee);
    case op_construct:
        return !calleeIsKnownDeclaration();
    case op_type_tag: // (It throws on behalf of the access that follows, with the same error.)
    case op_get_by_id_with_this:
    case op_get_by_id_direct:
    case op_get_by_val_with_this:
    case op_get_private_name:
    case op_put_by_val_direct:
    case op_put_private_name:
    case op_set_private_brand:
    case op_check_private_brand:
    case op_has_private_name:
    case op_has_private_brand:
    case op_in_by_id:
    case op_in_by_val:
    case op_instanceof:
    case op_call_direct_eval:
    case op_call_varargs:
    case op_tail_call_varargs:
    case op_construct_varargs:
    case op_super_construct:
    case op_super_construct_varargs:
    case op_iterator_open:
    case op_iterator_next:
    case op_check_tdz:
    case op_to_object:
    case op_get_prototype_of:
    case op_spread:
        return true;
    default:
        return false;
    }
}

uint32_t Lowering::callSiteBitsOf(Node* node)
{
    // (A function that may be inlined into another records all of its sites: noteEverySiteOf().)
    if (node->graph->isOutermost()) {
        // (A Narrow that checks uses the bytecode index of a call, but is not a call.)
        if (node->kind != NodeKind::Narrow && errorMayQuoteSource(m_graph, node))
            m_graph.quotableSites.append(node->bytecodeIndex.offset());
        m_graph.callSites.append(node->bytecodeIndex.offset());
    }
    return siteOf(node);
}

uint32_t Lowering::siteOf(Node* node)
{
    uint32_t bits = CallSiteIndex(node->bytecodeIndex).bits();
    if (m_graph.inlineFrames.isEmpty())
        return bits;
    bool isTailCall = node->graph->inlineFrame() && node->kind == NodeKind::Bytecode && (node->opcode == op_tail_call || node->opcode == op_tail_call_varargs);
    return PackedSite::pack(node->graph->inlineFrame(), bits, isTailCall);
}

void noteEverySiteOf(Graph& graph)
{
    for (const auto& instruction : graph.codeBlock()->instructions()) {
        graph.callSites.append(instruction.offset());
        switch (instruction->opcodeID()) {
        case op_get_by_id:
        case op_get_length:
        case op_get_by_val:
        case op_put_by_id:
        case op_put_by_val:
        case op_del_by_id:
        case op_del_by_val:
        case op_call:
        case op_call_ignore_result:
        case op_tail_call:
        case op_construct:
        case op_get_by_id_with_this:
        case op_get_by_id_direct:
        case op_get_by_val_with_this:
        case op_get_private_name:
        case op_put_by_val_direct:
        case op_put_private_name:
        case op_set_private_brand:
        case op_check_private_brand:
        case op_has_private_name:
        case op_has_private_brand:
        case op_in_by_id:
        case op_in_by_val:
        case op_instanceof:
        case op_call_varargs:
        case op_tail_call_varargs:
        case op_construct_varargs:
        case op_iterator_open:
        case op_iterator_next:
        case op_check_tdz:
        case op_to_object:
        case op_get_prototype_of:
        case op_spread:
            graph.quotableSites.append(instruction.offset());
            break;
        default:
            break;
        }
    }
}

using namespace B3;

Lowering::Lowering(Graph& graph, Procedure& proc)
    : Emitter(proc)
    , m_graph(graph)
{
}

// See Lowering::scratchWord().
static unsigned scratchWordsFor(Node* node)
{
    if (node->kind != NodeKind::Bytecode || !node->instruction)
        return 0;
    switch (node->opcode) {
    case op_new_object:
        if (Graph::typeTagOf(node)) {
            if (auto shape = node->graph->shapeOfLiteral(node))
                return shape->numberOfSlots();
        }
        return node->numberOfLiteralProperties;
    case op_create_this:
        return node->numberOfLiteralProperties;
    case op_new_array:
        return Graph::readsOperandsFromFrame(node) ? 0 : node->as<OpNewArray>().m_argc;
    case op_new_array_with_spread:
        return node->as<OpNewArrayWithSpread>().m_argc;
    // The items of an argument list that is not materialized: at most two words each (Lowering::lowerCallWithItems()).
    case op_call_varargs:
    case op_tail_call_varargs:
    case op_construct_varargs:
    case op_super_construct_varargs: {
        Node* list = Graph::listOfArgumentsOf(node);
        if (!list || !list->isElided)
            return 0;
        return 2 * (list->isBytecode(op_new_array_with_spread) ? list->as<OpNewArrayWithSpread>().m_argc : 1);
    }
    // Arguments that do not fit in registers, or that the callee takes in memory.
    case op_call:
        return node->as<OpCall>().m_argc;
    case op_call_ignore_result:
        return node->as<OpCallIgnoreResult>().m_argc;
    case op_tail_call:
        return node->as<OpTailCall>().m_argc;
    case op_construct:
        return node->as<OpConstruct>().m_argc;
    case op_super_construct:
        return node->as<OpSuperConstruct>().m_argc;
    case op_strcat:
        return node->as<OpStrcat>().m_count;
    case op_enumerator_next:
        return 2;
    case op_iterator_open:
    case op_async_iterator_open:
    case op_iterator_next:
        return 1;
    default:
        return 0;
    }
}

bool Lowering::run()
{
    m_out.initialize(m_heaps);
    m_out.setFrequency(1);

    unsigned numEntrypoints = 1 + m_graph.catchEntrypoints.size();
    m_proc.setNumEntrypoints(numEntrypoints);

    LBasicBlock prologue = m_out.newBlock();
    for (BasicBlock* block : m_graph.m_rpo) {
        m_out.setFrequency(block->isGeneric ? coldFrequency : 1);
        block->lowered = m_out.newBlock();
        if (!block->arraysViewed.isEmpty())
            block->loweredAhead = m_out.newBlock();
    }
    m_out.setFrequency(1);

    m_out.appendTo(prologue);
    m_out.initializeConstants(m_proc, prologue);

    // This runs on every entry to the function, so what it computes may only depend on the frame pointer and on registers that hold
    // the same value throughout.
    m_valueRepresentations = m_graph.valueRepresentations();
    if (unsigned count = m_graph.numberOfRegisterReturnValues) {
        m_returnValueReps = returnValueReps(m_graph.summary(), count);
        m_registerReturnValues.grow(count);
    }
    m_callFrame = m_out.framePointer();
    m_instance = registerOnEntry(instanceGPR);
    m_numberTag = registerOnEntry(GPRInfo::numberTagRegister);
    m_notCellMask = registerOnEntry(GPRInfo::notCellMaskRegister);
    OwnData own = ownData();
    if (m_graph.startsCold) {
        // A function that starts cold has no Data of its own until it has run often enough.
        m_data = m_out.select(own.hasAny, own.data, m_out.loadPtr(m_instance, m_heaps.AOTInstance_sharedData));
        m_dataOnEntry = m_data;
        if (std::ranges::any_of(m_graph.m_rpo, [](BasicBlock* block) { return block->isLoopHeader; })) {
            m_dataInLoops = m_proc.addVariable(pointerType());
            m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), m_dataInLoops, m_data);
        }
        m_constants = wordByIndex(m_out.loadPtr(m_instance, m_heaps.AOTInstance_infos), FunctionInfo::offsetOfConstants(), sizeof(FunctionInfo), false);
    } else
        m_dataOrNull = own.data;
    m_vm = m_out.loadPtr(m_instance, m_heaps.AOTInstance_vm);
    m_globalObject = m_out.loadPtr(m_instance, m_heaps.AOTInstance_globalObject);
    m_table = m_out.loadPtr(m_instance, m_heaps.AOTInstance_runtimeTable);
    if (m_graph.needsFunctionObject())
        m_calleeSlot = m_out.lockedStackSlot(sizeof(EncodedJSValue));
    if (m_graph.convention().signature == Signature::List)
        m_listSlot = m_out.lockedStackSlot(2 * sizeof(EncodedJSValue));
    if (unsigned homes = m_graph.numberOfFrameRegisters())
        m_frameRegisterStorage = m_out.lockedStackSlot(homes * sizeof(EncodedJSValue));
    if (m_dataOrNull) {
        // A function with a loop needs its own inline caches from the start, so its Data is created on the first call.
        LBasicBlock hasNone = m_out.newBlock();
        LBasicBlock hasData = m_out.newBlock();
        ValueFromBlock had = m_out.anchor(m_dataOrNull);
        m_out.branch(own.hasAny, usually(hasData), rarely(hasNone));
        m_out.appendTo(hasNone, hasData);
        callStub(Stub::LinkFunction, Void, { }, { }, StubClobbers::Nothing);
        ValueFromBlock made = m_out.anchor(ownData().data);
        m_out.jump(hasData);
        m_out.appendTo(hasData);
        m_data = m_out.phi(pointerType(), had, made);
    }

    unsigned scratchWords = 0;
    for (BasicBlock* block : m_graph.m_rpo) {
        for (Node* node : block->nodes) {
            scratchWords = std::max(scratchWords, scratchWordsFor(node));
            for (auto& use : node->uses)
                use.node->useCount++;
        }
        for (Node* phi : block->phis) {
            for (auto& use : phi->uses)
                use.node->useCount++;
        }
    }
    if (scratchWords)
        m_scratch = m_out.lockedStackSlot(scratchWords * sizeof(EncodedJSValue));

    if (numEntrypoints > 1) {
        Vector<LBasicBlock> successors;
        successors.append(m_graph.root->lowered);
        for (BasicBlock* entrypoint : m_graph.catchEntrypoints)
            successors.append(entrypoint->lowered);
        m_out.entrySwitch(successors);
    } else
        m_out.jump(m_graph.root->lowered);

    // Phis first: a block's phis are referred to from predecessors that may be lowered before it.
    for (BasicBlock* block : m_graph.m_rpo) {
        for (Node* phi : block->phis) {
            LType type = Int64;
            switch (phi->rep()) {
            case Rep::JSValue:
            case Rep::Int64:
                type = Int64;
                break;
            case Rep::Int32:
            case Rep::Boolean:
                type = Int32;
                break;
            case Rep::Double:
                type = Double;
                break;
            }
            phi->lowered = m_proc.add<Value>(Phi, type, Origin());
        }
    }

    findChainsOfComparisons();
    for (BasicBlock* block : m_graph.m_rpo) {
        lowerBlock(block);
        if (m_graph.failed())
            return false;
    }

    if (m_returnBlock) {
        m_out.appendTo(m_returnBlock);
        Rep rep = m_valueRepresentations.result;
        if (m_graph.numberOfRegisterReturnValues) {
            // (The phis come first, because the return has to be the last value in the block.)
            Vector<LValue, 8> things;
            for (unsigned i = 0; i < m_registerReturnValues.size(); ++i) {
                Rep how = m_returnValueReps[i];
                things.append(m_out.phi(how == Rep::JSValue ? Int64 : how == Rep::Double ? Double : Int32, m_registerReturnValues[i]));
            }
            PatchpointValue* patchpoint = m_out.patchpoint(Void);
            for (unsigned i = 0; i < things.size(); ++i)
                patchpoint->append(ConstrainedValue(things[i], m_returnValueReps[i] == Rep::Double ? ValueRep::reg(FPRInfo::toArgumentRegister(i)) : ValueRep::reg(argumentGPR(i))));
            patchpoint->clobber(RegisterSet::macroClobberedGPRs());
            patchpoint->effects.terminal = true;
            patchpoint->setGenerator([graph = &m_graph](CCallHelpers& jit, const StackmapGenerationParams& params) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                emitEpilogueBeforeLeaving(jit, *graph, params.code());
                jit.ret();
            });
        } else
            m_out.ret(m_out.phi(rep == Rep::JSValue ? Int64 : rep == Rep::Double ? Double : Int32, m_returnValues));
    }
    m_heaps.computeRangesAndDecorateInstructions();
    m_proc.deleteOrphans();
    m_out.applyBlockOrder();
    return true;
}

void Lowering::unsupported(Node* node)
{
    m_graph.fail("no lowering"_s, node->opcode);
}

TypedPointer Lowering::addressFor(VirtualRegister reg)
{
    return m_out.address(m_frameRegisterStorage, m_heaps.variables[m_graph.frameRegisterIndex(reg)]);
}

// Loads the word at base + addend + the function's index * scale. A null base means the Instance.
LValue Lowering::wordByIndex(LValue base, uint32_t addend, uint32_t scale, bool mayChange)
{
    PatchpointValue* patchpoint = m_out.patchpoint(pointerType());
    if (base)
        patchpoint->append(ConstrainedValue(base, ValueRep::SomeRegister));
    patchpoint->effects = Effects::none();
    if (mayChange)
        patchpoint->effects.reads = HeapRange::top();
    patchpoint->setGenerator([indexReferences = &m_graph.indexReferences, hasBase = !!base, addend, scale](CCallHelpers& jit, const StackmapGenerationParams& params) {
        indexReferences->load(jit, hasBase ? params[1].gpr() : instanceGPR, params[0].gpr(), addend, scale);
    });
    return patchpoint;
}

Lowering::OwnData Lowering::ownData()
{
    LValue state = wordByIndex(nullptr, Instance::offsetOfStates(), sizeof(uint32_t), true);
    return { m_out.aboveOrEqual(state, m_out.constIntPtr(Instance::leastStateWithData)), m_out.add(m_instance, m_out.shl(state, m_out.constInt32(Instance::shiftOfStateWithData))) };
}

LValue Emitter::registerOnEntry(Reg reg)
{
    return m_out.m_block->appendNew<ArgumentRegValue>(m_proc, Origin(), reg);
}

// Emits what is done once, on the entry that callers use.
void Lowering::lowerEntry()
{
    if (m_calleeSlot)
        m_out.store64(registerOnEntry(calleeGPR), m_out.address(m_heaps.variables.atAnyIndex(), m_calleeSlot));
    if (m_listSlot) {
        m_out.store64(registerOnEntry(argumentGPR(0)), m_out.address(m_heaps.root, m_listSlot, 0));
        m_out.store64(registerOnEntry(argumentGPR(1)), m_out.address(m_heaps.root, m_listSlot, sizeof(EncodedJSValue)));
    }
}

LValue Lowering::numberOfArgumentsPassed()
{
    return m_out.load32(m_out.address(m_heaps.root, m_listSlot, 0));
}

LValue Lowering::argumentsPassed()
{
    return m_out.loadPtr(m_out.address(m_heaps.root, m_listSlot, sizeof(EncodedJSValue)));
}

// index: does not count `this`.
LValue Lowering::argumentPassedOrUndefined(unsigned index)
{
    LBasicBlock isThere = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock isNot = m_out.anchor(m_out.constInt64(JSValue::encode(jsUndefined())));
    m_out.branch(m_out.above(numberOfArgumentsPassed(), m_out.constInt32(index)), usually(isThere), rarely(continuation));
    m_out.appendTo(isThere, continuation);
    ValueFromBlock is = m_out.anchor(m_out.load64(m_out.address(m_heaps.root, argumentsPassed(), index * sizeof(EncodedJSValue))));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(Int64, isNot, is);
}

TypedPointer Lowering::slotWord(unsigned slot, unsigned word)
{
    return m_out.address(m_data, m_heaps.AOTData_slotWords[slot * 2 + word]);
}

LValue Lowering::slotAddress(unsigned slot)
{
    return m_out.add(m_data, m_out.constIntPtr(Data::offsetOfSlots() + slot * sizeof(Slot)));
}

TypedPointer Lowering::scratchWord(unsigned index)
{
    RELEASE_ASSERT(m_scratch);
    return m_out.address(m_heaps.root, m_scratch, index * sizeof(EncodedJSValue));
}

LValue Lowering::storeToScratch(Node* node, VirtualRegister first, unsigned count)
{
    if (!count)
        return m_out.intPtrZero;
    for (unsigned i = 0; i < count; ++i)
        m_out.store64(lowJSValue(node->use(VirtualRegister(first.offset() - static_cast<int>(i)))), scratchWord(i));
    return m_scratch;
}

LValue Emitter::structureOf(LValue cell)
{
    // A structure's ID is the low half of its address. The high half is the same for all of them.
    return structureWithID(m_out.load32(cell, m_heaps.JSCell_structureID));
}

LValue Emitter::entry(Entry which)
{
    LValue result = m_out.loadPtr(m_out.address(m_table, m_heaps.AOTRuntimeTable[static_cast<unsigned>(which)]));
    static_cast<MemoryValue*>(result)->setReadsMutability(B3::Mutability::Immutable);
    return result;
}

// Whether the stack may be walked while the stub runs, which requires the caller's call site to be recorded.
static bool mayLookAtStack(Stub stub)
{
    if (isHelper(stub))
        return false;
    switch (stub) {
    case Stub::Prologue:
    case Stub::LinkFunction:
    case Stub::PlainOperation:
    case Stub::PlainOperationWithGlobalObject:
    case Stub::PlainOperationWithInstance:
    case Stub::PlainOperationWithVM:
    case Stub::WriteBarrier:
    case Stub::ToBoolean:
    case Stub::NarrowCharacters:
        return false;
    default:
        return true;
    }
}

bool Lowering::isLiveAfterNextNode(Node* node) const
{
    if (m_nodeIndex >= m_block->nodes.size() || m_block->nodes[m_nodeIndex] != node)
        return true;
    for (unsigned i = m_nodeIndex + 1; i < m_block->nodes.size(); ++i) {
        Node* next = m_block->nodes[i];
        if (next->isElided)
            continue;
        unsigned usesOfIt = 0;
        for (auto& use : next->uses)
            usesOfIt += use.node == node;
        return node->useCount > usesOfIt;
    }
    return node->useCount;
}

PatchpointValue* Lowering::callStub(Stub stub, LType type, const Vector<StubArgument, 8>& arguments, const Vector<StubImmediate, 2>& immediates, StubClobbers clobbers, Node* place)
{
    m_graph.emitsCalls = true;
    if (!place)
        place = m_node;
    CallSite site;
    if (mayLookAtStack(stub)) {
        RELEASE_ASSERT(place);
        site.bits = callSiteBitsOf(place);
    }

    // The address of a slot is materialized at the call. As a B3 value it would be hoisted out of every loop that uses it and kept
    // in a register, for calls that are rarely made.
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    std::optional<std::pair<GPRReg, int32_t>> slotArgument;
    // The same goes for the address of a stack slot: materializing it takes one instruction, the same as copying it from wherever
    // it would be kept.
    Vector<std::pair<GPRReg, B3::Air::StackSlot*>, 2> slotsOfFrame;
    // The first operand stays in whichever register it is in, if the stub has an entry point for that register
    // (acceptsOperandInAnyRegister()).
    std::optional<uint32_t> valueOfT9;
    for (auto& immediate : immediates) {
        if (immediate.reg == GPRInfo::regT9)
            valueOfT9 = immediate.value;
    }
    bool operandUsesAnyRegister = !type.isTuple() && !arguments.isEmpty() && arguments[0].reg.isGPR() && acceptsOperandInAnyRegister(stub, valueOfT9) && arguments[0].reg.gpr() == defaultOperandRegister(stub)
        && arguments[0].value->opcode() != SlotBase;
    auto isAddressOfSlot = [&](LValue value) { return value->opcode() == Add && value->child(0) == m_data && value->child(1)->hasIntPtr(); };
    bool secondOperandUsesAnyRegister = operandUsesAnyRegister && clobbers == StubClobbers::CallerSavedRegisters && arguments.size() >= 2 && acceptsTwoOperandsInAnyRegisters(stub) && arguments[1].reg == Reg(GPRInfo::argumentGPR1)
        && arguments[1].value->opcode() != SlotBase && !isAddressOfSlot(arguments[1].value);
    // The result is returned in the register it has been assigned, if it is the result of the node being lowered.
    bool resultUsesAssignedRegister = type == Int64 && clobbers == StubClobbers::CallerSavedRegisters && returnsResultInAnyRegister(stub, valueOfT9) && m_node && place == m_node && isLiveAfterNextNode(m_node);
    RegisterSet clobberedBeforeCall;
    for (auto& argument : arguments) {
        LValue value = argument.value;
        if (!slotArgument && value->opcode() == Add && value->child(0) == m_data && value->child(1)->hasIntPtr()) {
            slotArgument = { argument.reg.gpr(), static_cast<int32_t>(value->child(1)->asIntPtr()) };
            clobberedBeforeCall.add(argument.reg, IgnoreVectors);
            continue;
        }
        if (value->opcode() == SlotBase) {
            slotsOfFrame.append(std::pair<GPRReg, B3::Air::StackSlot*> { argument.reg.gpr(), value->as<B3::SlotBaseValue>()->slot() });
            clobberedBeforeCall.add(argument.reg, IgnoreVectors);
            continue;
        }
        if (operandUsesAnyRegister && &argument == &arguments[0]) {
            RELEASE_ASSERT(!patchpoint->numChildren());
            patchpoint->append(ConstrainedValue(value, ValueRep::SomeRegister));
            continue;
        }
        if (secondOperandUsesAnyRegister && &argument == &arguments[1]) {
            RELEASE_ASSERT(patchpoint->numChildren() == 1);
            patchpoint->append(ConstrainedValue(value, ValueRep::SomeRegister));
            continue;
        }
        patchpoint->append(ConstrainedValue(value, ValueRep::reg(argument.reg)));
    }
    if (slotArgument)
        patchpoint->append(ConstrainedValue(m_data, ValueRep::SomeRegister));
    for (auto& immediate : immediates) {
        if (immediate.reg != GPRInfo::regT9)
            clobberedBeforeCall.add(immediate.reg, IgnoreVectors);
    }
    // (The registers written on the way to the stub are written before the operands are read, so an operand must not be in one of
    // them.)
    if (operandUsesAnyRegister || slotArgument)
        patchpoint->clobberEarly(clobberedBeforeCall);
    patchpoint->clobberLate(clobberedBeforeCall);
    if (operandUsesAnyRegister) {
        RegisterSet excludedRegisters;
        for (unsigned i = 0; i < 16; ++i) {
            GPRReg reg = static_cast<GPRReg>(static_cast<unsigned>(ARM64Registers::x0) + i);
            if (!operandMayBeIn(stub, reg) || (secondOperandUsesAnyRegister && i >= 9))
                excludedRegisters.add(reg, IgnoreVectors);
        }
        patchpoint->clobberEarly(excludedRegisters);
        patchpoint->clobberLate(excludedRegisters);
        if (!preservesOperandRegister(stub))
            patchpoint->clobberLate(RegisterSet { defaultOperandRegister(stub) });
    }
    patchpoint->clobberLate(RegisterSet { ARM64Registers::lr }); // See hasNoFrame().
    switch (clobbers) {
    case StubClobbers::CallerSavedRegisters:
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
        break;
    case StubClobbers::Temporaries: {
        RegisterSet temporaries;
        temporaries.add(GPRInfo::regT9, IgnoreVectors);
        temporaries.add(GPRInfo::regT10, IgnoreVectors);
        temporaries.add(GPRInfo::regT11, IgnoreVectors);
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        patchpoint->clobber(temporaries);
        break;
    }
    case StubClobbers::Nothing:
        break;
    }
    if (type == Double)
        patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::returnValueFPR) };
    else if (type.isTuple()) {
        // The caller sets the constraints for a tuple result.
    } else if (type != Void)
        patchpoint->resultConstraints = { resultUsesAssignedRegister ? ValueRep::SomeRegister : ValueRep::reg(GPRInfo::returnValueGPR) };
    unsigned whichIsOperand = type == Void ? 0 : 1;
    patchpoint->setGenerator([stubCalls = &m_graph.stubCalls, stub, immediates, slotArgument, slotsOfFrame, operandUsesAnyRegister, secondOperandUsesAnyRegister, resultUsesAssignedRegister, whichIsOperand, valueOfT9, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (slotArgument)
            jit.addPtr(CCallHelpers::TrustedImm32(slotArgument->second), params[params.size() - 1].gpr(), slotArgument->first);
        for (auto& [reg, slot] : slotsOfFrame)
            jit.addPtr(CCallHelpers::TrustedImm32(slot->offsetFromFP()), GPRInfo::callFrameRegister, reg);
        for (auto& immediate : immediates) {
            if (immediate.reg != GPRInfo::regT9)
                jit.move(CCallHelpers::TrustedImm32(immediate.value), immediate.reg);
        }
        if (resultUsesAssignedRegister)
            stubCalls->callForResultIn(jit, stub, *valueOfT9, operandUsesAnyRegister ? params[whichIsOperand].gpr() : defaultOperandRegister(stub), params[0].gpr(), site);
        else if (secondOperandUsesAnyRegister)
            stubCalls->callWithOperandsIn(jit, stub, params[whichIsOperand].gpr(), params[whichIsOperand + 1].gpr(), site);
        else if (operandUsesAnyRegister)
            stubCalls->callWithOperandIn(jit, stub, valueOfT9, params[whichIsOperand].gpr(), site);
        else if (valueOfT9)
            stubCalls->call(jit, stub, *valueOfT9, site);
        else
            stubCalls->call(jit, stub, site);
    });
    return patchpoint;
}

LValue Lowering::callHelper(Stub stub, const Vector<LValue, 4>& arguments)
{
    RELEASE_ASSERT(isHelper(stub));
    Vector<StubArgument, 8> placed;
    for (unsigned i = 0; i < arguments.size(); ++i)
        placed.append({ arguments[i], GPRInfo::toArgumentRegister(i) });
    return callStub(stub, pointerType(), placed, { });
}

B3::PatchpointValue* Lowering::emitColdCall(Node* node, LType type, Entry function, LValue first, LValue second, ColdCall what)
{
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    if (what == ColdCall::ChangesNothing) {
        patchpoint->effects = Effects();
        patchpoint->effects.reads = HeapRange::top();
        patchpoint->effects.exitsSideways = true;
        patchpoint->effects.controlDependent = true;
    }
    bool returnsValue = type != Void;
    uint32_t valueOfT9 = static_cast<uint32_t>(static_cast<unsigned>(function) * sizeof(void*));
    // (See callStub(). The entry point moves the operand to the argument register, which is therefore clobbered.)
    bool firstOperandUsesAnyRegister = first && acceptsOperandInAnyRegister(returnsValue ? Stub::ColdOperationValue : Stub::ColdOperationVoid, valueOfT9) && acceptsOperandInAnyRegister(returnsValue ? Stub::ColdOperationValueOfLeaf : Stub::ColdOperationVoidOfLeaf, valueOfT9);
    if (firstOperandUsesAnyRegister) {
        patchpoint->append(ConstrainedValue(first, ValueRep::SomeRegister));
        patchpoint->clobberLate(RegisterSet { GPRInfo::argumentGPR1 });
    } else if (first)
        patchpoint->append(ConstrainedValue(first, ValueRep::reg(GPRInfo::argumentGPR1)));
    if (second)
        patchpoint->append(ConstrainedValue(second, ValueRep::reg(GPRInfo::argumentGPR2)));
    RegisterSet temporaries;
    temporaries.add(GPRInfo::regT9, IgnoreVectors);
    temporaries.add(GPRInfo::regT10, IgnoreVectors);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobber(temporaries);
    if (returnsValue)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { callSiteBitsOf(node) };
    patchpoint->setGenerator([graph = &m_graph, valueOfT9, site, returnsValue, firstOperandUsesAnyRegister](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool isLeaf = hasNoFrame(*graph, params.proc().code());
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, GPRInfo::regT10);
        Stub stub = returnsValue ? (isLeaf ? Stub::ColdOperationValueOfLeaf : Stub::ColdOperationValue) : (isLeaf ? Stub::ColdOperationVoidOfLeaf : Stub::ColdOperationVoid);
        if (firstOperandUsesAnyRegister)
            graph->stubCalls.callWithOperandIn(jit, stub, valueOfT9, params[returnsValue ? 1 : 0].gpr(), site);
        else
            graph->stubCalls.call(jit, stub, valueOfT9, site);
    });
    return patchpoint;
}

void Lowering::coldCall(Node* node, Entry function, LValue first, LValue second, ColdCall what)
{
    emitColdCall(node, Void, function, first, second, what);
}

LValue Lowering::coldCallForValue(Node* node, Entry function, LValue first, LValue second, ColdCall what)
{
    return emitColdCall(node, Int64, function, first, second, what);
}

LValue Lowering::callOperationThroughStub(Node* node, LType type, Entry function, const Vector<LValue, 8>& arguments)
{
    bool throws = !!node;
    bool withGlobalObject = !arguments.isEmpty() && arguments[0] == m_globalObject;
    bool withInstance = !arguments.isEmpty() && arguments[0] == m_instance;
    bool withVM = !throws && !arguments.isEmpty() && arguments[0] == m_vm;
    RELEASE_ASSERT(withInstance == takesInstance(function) && withGlobalObject == takesGlobalObject(function));

    Vector<StubArgument, 8> placed;
    unsigned nextGPR = 0;
    unsigned nextFPR = 0;
    for (unsigned i = 0; i < arguments.size(); ++i) {
        if (arguments[i]->type() == Double) {
            placed.append({ arguments[i], FPRInfo::toArgumentRegister(nextFPR++) });
            continue;
        }
        GPRReg reg = GPRInfo::toArgumentRegister(nextGPR++);
        if (!i && (withGlobalObject || withInstance || withVM))
            continue;
        placed.append({ arguments[i], reg });
    }
    RELEASE_ASSERT(nextGPR <= GPRInfo::numberOfArgumentRegisters && nextFPR <= FPRInfo::numberOfArgumentRegisters);

    Stub stub;
    if (!throws)
        stub = withGlobalObject ? Stub::PlainOperationWithGlobalObject : withInstance ? Stub::PlainOperationWithInstance : withVM ? Stub::PlainOperationWithVM : Stub::PlainOperation;
    else if (type == Double)
        stub = withGlobalObject ? Stub::OperationDoubleWithGlobalObject : withInstance ? Stub::OperationDoubleWithInstance : Stub::OperationDouble;
    else if (type == Void)
        stub = withGlobalObject ? Stub::OperationVoidWithGlobalObject : withInstance ? Stub::OperationVoidWithInstance : Stub::OperationVoid;
    else
        stub = withGlobalObject ? Stub::OperationValueWithGlobalObject : withInstance ? Stub::OperationValueWithInstance : Stub::OperationValue;

    Vector<StubImmediate, 2> immediates;
    immediates.append({ GPRInfo::regT9, static_cast<uint32_t>(static_cast<unsigned>(function) * sizeof(void*)) });
    PatchpointValue* result = callStub(stub, type, placed, immediates, StubClobbers::CallerSavedRegisters, node);
    return type == Void ? nullptr : result;
}

unsigned Lowering::allocateSite(Node*, unsigned identifier, unsigned extra)
{
    RELEASE_ASSERT(Site::fits(identifier, extra));
    unsigned slot = allocateSlot();
    while (m_graph.sites.size() <= slot)
        m_graph.sites.append(Site { });
    m_graph.sites[slot].identifierAndExtra = identifier | extra << Site::identifierBits;
    return slot;
}

unsigned Lowering::sharedSite(Node* node, unsigned identifier, unsigned extra)
{
    // (A site may cache how many scopes out its variable is, which depends on where the search starts.)
    if (!node->graph->hasGuards() || node->environmentsPassedOver)
        return allocateSite(node, identifier, extra);
    return m_sharedSites.ensure((static_cast<uint64_t>(node->bytecodeIndex.offset()) << 32 | identifier) ^ static_cast<uint64_t>(node->graph->inlineFrame()) << 56, [&] {
        return allocateSite(node, identifier, extra);
    }).iterator->value;
}

void Lowering::storeBarrier(LValue owner)
{
    if constexpr (usesStubs) {
        // The barrier changes nothing that compiled code reads.
        PatchpointValue* patchpoint = callStub(Stub::WriteBarrier, Void, { { owner, GPRInfo::argumentGPR0 } }, { }, StubClobbers::Temporaries);
        patchpoint->effects = Effects::none();
        patchpoint->effects.controlDependent = true;
        m_heaps.decoratePatchpointRead(&m_heaps.JSCell_cellState, patchpoint);
        m_heaps.decoratePatchpointWrite(&m_heaps.JSCell_cellState, patchpoint);
        return;
    }
    LBasicBlock slowPath = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue threshold = m_out.load32(m_vm, m_heaps.VM_heap_barrierThreshold);
    m_out.branch(m_out.above(m_out.load8ZeroExt32(owner, m_heaps.JSCell_cellState), threshold), usually(continuation), rarely(slowPath));
    m_out.appendTo(slowPath, continuation);
    plainCall(Void, Entry::operationAOTWriteBarrier, m_vm, owner);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

// ---- Values

LValue Emitter::numberToDouble(LValue value)
{
    // Both are computed and one is picked: cheaper than a branch that does not predict.
    return m_out.select(isInt32(value), m_out.intToDouble(unboxInt32(value)), unboxDouble(value));
}

LValue Lowering::doubleToInt32(LValue value)
{
#if CPU(ARM64)
    if (MacroAssemblerARM64::supportsDoubleToInt32ConversionUsingJavaScriptSemantics()) {
        PatchpointValue* patchpoint = m_out.patchpoint(Int32);
        patchpoint->append(ConstrainedValue(value, ValueRep::SomeRegister));
        patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
        });
        patchpoint->effects = Effects::none();
        return patchpoint;
    }
#endif
    return plainCall(Int32, Entry::operationAOTDoubleToInt32, value);
}

LValue Lowering::convert(LValue value, Rep from, Type fromType, Rep to)
{
    if (from == to)
        return value;
    switch (to) {
    case Rep::JSValue:
        switch (from) {
        case Rep::Int32:
            return boxInt32(value);
        case Rep::Int64: {
            LValue narrow = m_out.castToInt32(value);
            return m_out.select(m_out.equal(m_out.signExt32To64(narrow), value), boxInt32(narrow), boxDouble(m_out.intToDouble(value)));
        }
        case Rep::Double:
            // Builtins pass numbers to functions that assume the canonical encoding: a number that fits in an int32 is boxed as
            // one, as the interpreter would. (JavaScript code cannot observe the difference.)
            if (code().codeBlock()->isBuiltinFunction()) {
                LValue narrow = m_out.doubleToInt32(value);
                return m_out.select(m_out.equal(m_out.bitCast(m_out.intToDouble(narrow), Int64), m_out.bitCast(value, Int64)), boxInt32(narrow), boxDouble(value));
            }
            return boxDouble(value);
        case Rep::Boolean:
            return boxBoolean(value);
        case Rep::JSValue:
            break;
        }
        break;
    case Rep::Double:
        if (from == Rep::Int32 || from == Rep::Int64)
            return m_out.intToDouble(value);
        if (from == Rep::JSValue) {
            if (isSubtype(fromType, TInt32))
                return m_out.intToDouble(unboxInt32(value));
            if (isSubtype(fromType, TDouble))
                return unboxDouble(value);
            return numberToDouble(value);
        }
        break;
    // Conversion to an integer from any other representation: the value has been proven to be an integer that fits.
    case Rep::Int32:
        if (from == Rep::JSValue) {
            if (isSubtype(fromType, TInt32))
                return unboxInt32(value);
            return m_out.select(isInt32(value), unboxInt32(value), m_out.doubleToInt32(unboxDouble(value)));
        }
        if (from == Rep::Int64)
            return m_out.castToInt32(value);
        if (from == Rep::Double)
            return m_out.doubleToInt32(value);
        break;
    case Rep::Int64:
        if (from == Rep::JSValue) {
            if (isSubtype(fromType, TInt32))
                return m_out.signExt32To64(unboxInt32(value));
            return m_out.select(isInt32(value), m_out.signExt32To64(unboxInt32(value)), m_out.doubleToInt64(unboxDouble(value)));
        }
        if (from == Rep::Int32)
            return m_out.signExt32To64(value);
        if (from == Rep::Double)
            return m_out.doubleToInt64(value);
        break;
    case Rep::Boolean:
        if (from == Rep::JSValue)
            return unboxBoolean(value);
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

LValue Lowering::lowRaw(Node* node)
{
    switch (node->kind) {
    case NodeKind::Constant:
        switch (node->rep()) {
        case Rep::Int32:
            return m_out.constInt32(static_cast<int32_t>(node->constant.asNumber()));
        case Rep::Int64:
            m_graph.wideIntegerConstants.add(static_cast<int64_t>(node->constant.asNumber()));
            return m_out.constInt64(static_cast<int64_t>(node->constant.asNumber()));
        case Rep::Double:
            return m_out.constDouble(node->constant.asNumber());
        case Rep::Boolean:
            return m_out.constInt32(node->constant.asBoolean());
        case Rep::JSValue:
            return m_out.constInt64(JSValue::encode(node->constant));
        }
        break;
    case NodeKind::ConstantCell:
        return lowConstantRegister(*node->graph, node->reg);
    case NodeKind::Intrinsic:
        if (node->intrinsic == ImmutableIntrinsics::globalObject)
            return m_globalObject;
        return m_out.load64(m_instance, m_heaps.AOTInstance_intrinsics[node->intrinsic]);
    default:
        break;
    }
    RELEASE_ASSERT(node->lowered);
    return node->lowered;
}

LValue Lowering::lowConstantRegister(Graph& graph, VirtualRegister reg)
{
    if (auto* numbers = numbersOfConstantsOfProgramFor(graph.codeBlock())) {
        uint32_t number = numbers->at(reg.toConstantIndex());
        RELEASE_ASSERT(number != notAConstantOfProgram);
        return m_out.load64(m_out.address(m_out.loadPtr(m_instance, m_heaps.AOTInstance_constantsOfProgram), m_heaps.AOTConstants[number]));
    }
    RELEASE_ASSERT(graph.isOutermost()); // (An inlined function's constants are among the program's.)
    LValue constants = m_graph.startsCold ? m_constants : m_out.loadPtr(m_data, m_heaps.AOTData_constants);
    return m_out.load64(m_out.address(constants, m_heaps.AOTConstants[reg.toConstantIndex()]));
}

LValue Lowering::lowJSValue(Node* node)
{
    if (node->loweredAsJSValue)
        return node->loweredAsJSValue;
    return convert(lowRaw(node), node->rep(), node->type, Rep::JSValue);
}

LValue Lowering::lowInt32(Node* node)
{
    RELEASE_ASSERT(node->rep() == Rep::Int32);
    return lowRaw(node);
}

LValue Lowering::lowInt64(Node* node)
{
    RELEASE_ASSERT(node->isInteger());
    return convert(lowRaw(node), node->rep(), node->type, Rep::Int64);
}

LValue Lowering::lowDouble(Node* node)
{
    RELEASE_ASSERT(isSubtype(node->type, TNumber));
    return convert(lowRaw(node), node->rep(), node->type, Rep::Double);
}

LValue Lowering::lowBoolean(Node* node)
{
    RELEASE_ASSERT(node->rep() == Rep::Boolean);
    return lowRaw(node);
}

void Lowering::setResult(Node* node, LValue value, Rep rep)
{
    // The representation that a lowering produces may be less specific than the node's type, never more.
    // (With validation, the value is checked before it is converted, because after conversion to the representation for its type it
    // would always pass.)
    // (A node that was inferred to be unreachable contributed nothing to the types of other nodes, so it must never run.)
    if (Options::validateAOTInferredTypes() && rep == Rep::JSValue && (node->wasInferredUnreachable || (node->type && !isSubtype(TAll, node->type)))) [[unlikely]] {
        Type expected = node->wasInferredUnreachable ? TNone : node->type;
        unsigned which = node->kind == NodeKind::Bytecode ? static_cast<unsigned>(node->opcode) * 1000000 + node->bytecodeIndex.offset() : static_cast<unsigned>(node->kind);
        if (node->kind == NodeKind::Argument)
            which += 100 * node->reg.toArgument();
        unsigned identifierPlusOne = 0;
        // The variable that the value was read from, if any, identified as Options::logAOTTypeInference() prints it, so that the
        // stores to it can be found in the log.
        const Node* origin = node;
        for (unsigned depth = 0; depth < 4; ++depth) {
            if (origin->isBytecode(op_check_type))
                origin = origin->use(origin->as<OpCheckType>().m_value);
            else if (origin->kind == NodeKind::Narrow || (origin->kind == NodeKind::Phi && origin->uses.size() == 1))
                origin = origin->uses[0].node;
            else
                break;
        }
        Variable variable;
        if (origin->isBytecode(op_get_from_scope)) {
            identifierPlusOne = numberOf(origin, origin->as<OpGetFromScope>().m_var) + 1;
            variable = m_graph.variableAccessedBy(origin);
        } else if (node->isBytecode(op_get_by_id))
            identifierPlusOne = numberOf(node, node->as<OpGetById>().m_property) + 1;
        // (A node with a bytecode index, so that the frame can report a position.)
        Node* place = node;
        for (unsigned i = m_nodeIndex; place->kind != NodeKind::Bytecode && i < m_block->nodes.size(); ++i)
            place = m_block->nodes[i];
        // (These are plain integers, not addresses to relocate.)
        m_graph.wideIntegerConstants.add(static_cast<int64_t>(expected));
        m_graph.wideIntegerConstants.add(static_cast<int64_t>(expected >> 64));
        m_graph.wideIntegerConstants.add(static_cast<int64_t>(std::bit_cast<uintptr_t>(variable.scope)));
        if (place->kind == NodeKind::Bytecode) {
            vmCall(place, Void, Entry::operationAOTVerifyInferredType, m_instance, value, m_out.constInt64(static_cast<int64_t>(expected)), m_out.constInt64(static_cast<int64_t>(expected >> 64)), m_out.constInt32(which), m_out.constInt32(identifierPlusOne),
                m_out.constInt64(std::bit_cast<uintptr_t>(variable.scope)), m_out.constInt32(variable.offset));
        }
    }
    if (Rep to = node->rep(); rep != to && rep != Rep::JSValue && to != Rep::JSValue && (rep == Rep::Boolean || to == Rep::Boolean)) [[unlikely]] {
        dataLog("AOT: lowered representation does not match the node: ");
        node->dump(WTF::dataFile());
        dataLogLn(" lowered as rep ", static_cast<unsigned>(rep), ", expected rep ", static_cast<unsigned>(to), ", inline frame ", node->graph->inlineFrame(), " in ", m_graph.nameForLog());
        for (auto& use : node->uses) {
            dataLog("    uses ");
            use.node->dump(WTF::dataFile());
            dataLogLn(" of inline frame ", use.node->graph->inlineFrame());
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    node->lowered = convert(value, rep, node->type, node->rep());
    if (m_sameAs && m_sameAs->loweredAsJSValue && node->rep() != Rep::JSValue)
        node->loweredAsJSValue = m_sameAs->loweredAsJSValue;
    // Users that want a JSValue get the original. If nothing uses the converted value, the conversion is dead code.
    if (rep == Rep::JSValue && node->rep() != Rep::JSValue)
        node->loweredAsJSValue = value;
}

void Lowering::setProj(Node* node, VirtualRegister reg, LValue value, Rep rep)
{
    // The projections come immediately after the instruction, with at most stores to stack slots in between.
    auto& nodes = m_block->nodes;
    RELEASE_ASSERT(nodes[m_nodeIndex] == node);
    for (unsigned i = m_nodeIndex + 1; i < nodes.size(); ++i) {
        Node* candidate = nodes[i];
        if (candidate->kind == NodeKind::SetStack)
            continue;
        if (candidate->kind != NodeKind::Proj || candidate->uses[0].node != node)
            break;
        if (candidate->reg == reg) {
            setResult(candidate, value, rep);
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Lowering::setJSValue(Node* node, LValue value) { setResult(node, value, Rep::JSValue); }
void Lowering::setInt32(Node* node, LValue value) { setResult(node, value, Rep::Int32); }
void Lowering::setInt64(Node* node, LValue value) { setResult(node, value, Rep::Int64); }
void Lowering::setDouble(Node* node, LValue value) { setResult(node, value, Rep::Double); }
void Lowering::setBoolean(Node* node, LValue value) { setResult(node, value, Rep::Boolean); }

LValue Lowering::toBoolean(Node* node)
{
    switch (node->rep()) {
    case Rep::Boolean:
        return lowRaw(node);
    case Rep::Int32:
        return m_out.notZero32(lowRaw(node));
    case Rep::Int64:
        return m_out.notZero64(lowRaw(node));
    case Rep::Double:
        return m_out.doubleNotEqualAndOrdered(lowRaw(node), m_out.constDouble(0));
    case Rep::JSValue:
        break;
    }
    LValue value = lowRaw(node);
    if (isSubtype(node->type, TOther))
        return m_out.booleanFalse;
    if (isSubtype(node->type, TBoolean | TOther))
        return m_out.equal(value, m_out.constInt64(JSValue::ValueTrue));
    // For these types: a value that is not a cell is truthy only if it is `true`, and a cell is truthy unless it is the empty
    // string. (TOtherObject is excluded because such an object may masquerade as undefined.)
    if (isSubtype(node->type, TBoolean | TOther | TString | TSymbol | ((TAnyObject) & ~TOtherObject))) {
        if (!mayBe(node->type, TString) && !mayBe(node->type, TBoolean))
            return isSubtype(node->type, TCell) ? m_out.booleanTrue : isCell(value);
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        Vector<ValueFromBlock, 4> results;
        if (!isSubtype(node->type, TCell)) {
            results.append(m_out.anchor(m_out.equal(value, m_out.constInt64(JSValue::ValueTrue))));
            m_out.branch(isCell(value), unsure(cellCase), unsure(continuation));
        } else
            m_out.jump(cellCase);
        m_out.appendTo(cellCase);
        if (mayBe(node->type, TString)) {
            if (!isSubtype(node->type & TCell, TString)) {
                LBasicBlock stringCase = m_out.newBlock();
                results.append(m_out.anchor(m_out.booleanTrue));
                m_out.branch(isCellOfType(value, StringType), unsure(stringCase), unsure(continuation));
                m_out.appendTo(stringCase);
            }
            // (A rope is never empty.)
            LBasicBlock notRope = m_out.newBlock();
            LValue impl = m_out.loadPtr(value, m_heaps.JSRopeString_fiber0);
            results.append(m_out.anchor(m_out.booleanTrue));
            m_out.branch(m_out.testNonZeroPtr(impl, m_out.constIntPtr(JSString::isRopeInPointer)), rarely(continuation), usually(notRope));
            m_out.appendTo(notRope);
            results.append(m_out.anchor(m_out.notZero32(m_out.load32(impl, m_heaps.StringImpl_length))));
        } else
            results.append(m_out.anchor(m_out.booleanTrue));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return m_out.phi(Int32, results);
    }
    if (isCompact())
        return callStub(Stub::ToBoolean, Int32, { { value, GPRInfo::argumentGPR0 } }, { }, StubClobbers::Temporaries);

    LBasicBlock notBoolean = m_out.newBlock();
    LBasicBlock notInt32 = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    results.append(m_out.anchor(unboxBoolean(value)));
    m_out.branch(isBoolean(value), unsure(continuation), unsure(notBoolean));

    m_out.appendTo(notBoolean, notInt32);
    results.append(m_out.anchor(m_out.notZero32(unboxInt32(value))));
    m_out.branch(isInt32(value), unsure(continuation), unsure(notInt32));

    m_out.appendTo(notInt32, continuation);
    results.append(m_out.anchor(m_out.notZero64(plainCall(Int64, Entry::operationAOTToBoolean, m_instance, value))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int32, results);
}

// ---- Structure

// The B3 block that the block being lowered jumps to in order to reach a successor.
LBasicBlock Lowering::edgeTo(BasicBlock* successor)
{
    for (auto& [to, edge] : m_edges) {
        if (to == successor)
            return edge;
    }
    return entryBlockFor(successor);
}

// (As seen from the block being lowered.)
LBasicBlock Lowering::entryBlockFor(BasicBlock* successor)
{
    if (successor->loweredAhead && !successor->bodyOfLoop.get(m_block->index))
        return successor->loweredAhead;
    return successor->lowered;
}

const Lowering::ArrayView* Lowering::viewOf(Node* access, Node* base)
{
    if (!access->viewedAheadOf)
        return nullptr;
    UNUSED_PARAM(base);
    for (auto& [header, array, view] : m_arrayViews) {
        if (header == access->viewedAheadOf && array == access->arrayViewed)
            return &view;
    }
    return nullptr;
}

void Lowering::hoistArrayStorageLoadsAheadOf(BasicBlock* header)
{
    m_out.appendTo(header->loweredAhead);
    for (Node* base : header->arraysViewed) {
        LValue array = lowCell(base);
        ArrayView view;
        LValue shape = m_out.bitAnd(m_out.load8ZeroExt32(array, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask));
        view.butterfly = m_out.loadPtr(array, m_heaps.JSObject_butterfly);
        // (Array.prototype is an array without a butterfly, so there is no length to load.)
        LBasicBlock hasElements = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock none = m_out.anchor(m_out.int64Zero);
        m_out.branch(m_out.notZero32(shape), usually(hasElements), rarely(continuation));
        m_out.appendTo(hasElements);
        ValueFromBlock some = m_out.anchor(m_out.zeroExt(m_out.load32(view.butterfly, m_heaps.Butterfly_publicLength), Int64));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        view.length = m_out.phi(Int64, none, some);
        // Int32Shape and ContiguousShape hold JSValues; DoubleShape sits between them.
        view.limit = m_out.select(m_out.bitOr(m_out.equal(shape, m_out.constInt32(Int32Shape)), m_out.equal(shape, m_out.constInt32(ContiguousShape))), view.length, m_out.int64Zero);
        m_arrayViews.append({ header, base, view });
    }
    m_out.jump(header->lowered);
}

void Lowering::emitUpsilons(BasicBlock* block, BasicBlock* successor)
{
    unsigned predecessorIndex = successor->predecessors.find(block);
    RELEASE_ASSERT(predecessorIndex != notFound);
    for (Node* phi : successor->phis) {
        Node* input = phi->uses[predecessorIndex].node;
        LValue value = convert(lowRaw(input), input->rep(), input->type, phi->rep());
        m_out.addIncomingToPhi(phi->lowered, m_out.anchor(value));
    }
}

void Lowering::lowerBlock(BasicBlock* block)
{
    m_block = block;
    if (m_blocksInsideChains.contains(block)) {
        m_out.appendTo(block->lowered);
        m_out.unreachable();
        return;
    }
    m_availableFields.shrink(0);
    if (block->predecessors.size() == 1 && !block->isCatchEntrypoint && block != m_graph.root) {
        if (auto inHand = m_availableFieldsAtEndOf.find(block->predecessors[0]); inHand != m_availableFieldsAtEndOf.end())
            m_availableFields = inHand->value;
    }
    m_out.setFrequency(block->isGeneric || block->isRarelyExecuted ? coldFrequency : 1);
    if (block->loweredAhead)
        hoistArrayStorageLoadsAheadOf(block);
    m_out.appendTo(block->lowered);
    for (Node* phi : block->phis)
        m_out.m_block->append(phi->lowered);
    // A function that starts cold has no loop of its own, but inlineCalls() may have given it one. The Data is reloaded on every
    // iteration, because the function gets its own inline caches once it has run often enough, which may happen during the loop.
    if (m_dataOnEntry) {
        m_data = m_dataOnEntry;
        if (block->isInLoop && m_dataInLoops) {
            if (block->isLoopHeader) {
                OwnData own = ownData();
                m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), m_dataInLoops,
                    m_out.select(own.hasAny, own.data, m_out.loadPtr(m_instance, m_heaps.AOTInstance_sharedData)));
            }
            m_data = m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Get, m_out.origin(), m_dataInLoops);
        }
    }
    if (block->endsWithGuard)
        m_exit = newColdBlock();
    if (block == m_graph.root)
        lowerEntry();

    Node* terminal = block->terminal();
    if (terminal && terminal->kind != NodeKind::Guard && !(terminal->kind == NodeKind::Bytecode && (isBranch(terminal->opcode) || isTerminal(terminal->opcode) || isThrow(terminal->opcode))))
        terminal = nullptr;

    auto setCurrentNode = [&](Node* node) {
        m_node = node && node->instruction ? node : nullptr;
        m_code = node ? node->graph : block->graph;
    };
    for (m_nodeIndex = 0; m_nodeIndex < block->nodes.size(); ++m_nodeIndex) {
        Node* node = block->nodes[m_nodeIndex];
        if (node == terminal)
            break;
        if (node->isElided)
            continue;
        setCurrentNode(node);
        m_nodePreservesFields = false;
        lowerNode(node);
        if (m_graph.failed())
            return;
        if (!m_nodePreservesFields && !m_availableFields.isEmpty() && !preservesFields(node))
            m_availableFields.shrink(0);
    }
    if (terminal && !preservesFields(terminal))
        m_availableFields.shrink(0);
    if (!m_availableFields.isEmpty())
        m_availableFieldsAtEndOf.set(block, m_availableFields);
    if (terminal && terminal->kind == NodeKind::Guard) {
        setCurrentNode(terminal);
        lowerGuard(block, terminal);
        setCurrentNode(nullptr);
        return;
    }
    if (auto chain = m_chainsByFirstBlock.find(block); chain != m_chainsByFirstBlock.end()) {
        setCurrentNode(terminal);
        lowerChainOfComparisons(block, m_chains[chain->value]);
        setCurrentNode(nullptr);
        return;
    }
    setCurrentNode(nullptr);
    // The values for a successor's phis are set on the edge to it. Set before a branch, a loop would do on every iteration what is
    // only needed when it exits. And of two moves of one value, to a phi at the loop header and to a phi after the loop, only one
    // can be coalesced.
    bool branches = std::ranges::any_of(block->successors, [&](BasicBlock* successor) { return successor != block->successors[0]; });
    for (BasicBlock* successor : block->successors) {
        if (!branches) {
            emitUpsilons(block, successor);
            break;
        }
        if (!successor->phis.isEmpty() && edgeTo(successor) == entryBlockFor(successor))
            m_edges.append({ successor, m_out.newBlock() });
    }
    setCurrentNode(terminal);
    lowerTerminalOrFallThrough(block, terminal);
    setCurrentNode(nullptr);
    for (auto& [successor, edge] : std::exchange(m_edges, { })) {
        m_out.appendTo(edge);
        emitUpsilons(block, successor);
        m_out.jump(entryBlockFor(successor));
    }
}

void Lowering::lowerNode(Node* node)
{
    switch (node->kind) {
    case NodeKind::Constant:
    case NodeKind::ConstantCell:
    case NodeKind::Intrinsic:
    case NodeKind::Phi:
        RELEASE_ASSERT_NOT_REACHED();
        return;
    case NodeKind::LinkTimeConstant: {
        // The Instance holds every link-time constant that the image uses (ImageHeader::linkTimeConstantsUsed).
        noteThatLinkTimeConstantIsUsed(node->intrinsic);
        LValue constant = m_out.load64(m_instance, m_heaps.AOTInstance_linkTimeConstants[node->intrinsic]);
        static_cast<MemoryValue*>(constant)->setReadsMutability(B3::Mutability::Immutable);
        setJSValue(node, constant);
        return;
    }
    case NodeKind::Guard:
        // A guard that was hoisted to a pre-header.
        emitGuard(node);
        return;
    case NodeKind::Narrow:
        // The guard that ends the block checks these, or the guard before the block already has.
        if (node->narrowedTo) {
            Node* valueNode = node->uses[0].node;
            LValue value = lowJSValue(valueNode);
            if (node->checksNarrowedType && !isSubtype(valueNode->type, node->narrowedTo)) {
                RELEASE_ASSERT(node->narrowedTo == TArray);
                LBasicBlock isThat = m_out.newBlock();
                LBasicBlock isNot = newColdBlock();
                emitTypeTests(valueNode, value, MaskArray, isThat, isNot);
                m_out.appendTo(isNot);
                coldCall(node, Entry::operationAOTCheckType, value, m_out.constInt32(MaskArray));
                m_out.unreachable();
                m_out.appendTo(isThat);
            }
            setJSValue(node, value);
        }
        return;
    case NodeKind::Argument:
        if (node->reg == VirtualRegister(CallFrameSlot::callee))
            setJSValue(node, callee());
        else if (!node->reg.toArgument())
            setJSValue(node, registerOnEntry(thisGPR));
        else if (m_graph.convention().signature == Signature::List)
            setJSValue(node, argumentPassedOrUndefined(node->reg.toArgument() - 1));
        else {
            unsigned index = node->reg.toArgument() - 1;
            switch (m_valueRepresentations.parameters[index]) {
            case Rep::Int32:
                setInt32(node, m_out.castToInt32(registerOnEntry(argumentGPR(index))));
                break;
            case Rep::Boolean:
                setBoolean(node, m_out.castToInt32(registerOnEntry(argumentGPR(index))));
                break;
            case Rep::Double:
                setDouble(node, registerOnEntry(FPRInfo::toArgumentRegister(index)));
                break;
            default:
                setJSValue(node, registerOnEntry(argumentGPR(index)));
                break;
            }
        }
        return;
    case NodeKind::GetStack:
        setJSValue(node, m_out.load64(addressFor(node->reg)));
        return;
    case NodeKind::SetStack:
        m_out.store64(lowJSValue(node->uses[0].node), addressFor(node->reg));
        return;
    case NodeKind::Proj:
        // Already lowered by its instruction (setProj()).
        RELEASE_ASSERT(node->lowered);
        return;
    case NodeKind::Bytecode:
        lowerBytecode(node);
        return;
    }
}

void Lowering::lowerBytecode(Node* node)
{
    if (node->guard && node->guard->isHandled) {
        lowerGuarded(node);
        return;
    }
    if (tryLowerArith(node) || tryLowerAccess(node) || tryLowerCall(node) || tryLowerMisc(node) || tryLowerObjects(node) || tryLowerIteration(node))
        return;
    if (m_graph.failed())
        return;
    unsupported(node);
}

LBasicBlock Lowering::blockFor(Node* branch, int relativeOffset)
{
    BasicBlock* target = m_block->graph->targetFrom(m_block, branch->bytecodeIndex.offset() + relativeOffset);
    RELEASE_ASSERT(target && target->lowered);
    return edgeTo(target);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && CPU(ARM64)
