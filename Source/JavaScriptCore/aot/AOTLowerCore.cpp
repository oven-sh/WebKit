/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#include "AOTCompiler.h"

#if ENABLE(FTL_JIT)

#include "AirCode.h"
#include "B3PatchpointValue.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "BytecodeStructs.h"
#include "CCallHelpers.h"
#include "JSCInlines.h"
#include "UnlinkedCodeBlock.h"

namespace JSC { namespace AOT {

// Whether an error that comes of it may say what the source says there (ErrorInstance::SourceAppender, createTDZError()). Most that
// do are about a base that is undefined or null, or a callee that is no function.
static bool mayBeQuoted(const Graph& graph, Node* node)
{
    auto mayBeNothing = [&](VirtualRegister base) {
        Node* value = node->use(base);
        return !value || value->type & (TOther | TEmpty);
    };
    // (What is proven of a variable that is not a declaration is what it holds once it holds anything.)
    auto isAlwaysTheFunction = [&] {
        return graph.calleeIsProven(node) && graph.knownCallee(node)->isDeclaration;
    };
    auto mayNotBeAFunction = [&](VirtualRegister callee) {
        if (isAlwaysTheFunction())
            return false;
        Node* value = node->use(callee);
        return !value || value->type & ~TFunction;
    };
    switch (node->opcode) {
    case op_get_by_id:
        return mayBeNothing(node->as<OpGetById>().m_base);
    case op_get_length:
        return mayBeNothing(node->as<OpGetLength>().m_base);
    case op_get_by_val:
        return mayBeNothing(node->as<OpGetByVal>().m_base);
    case op_put_by_id:
        return mayBeNothing(node->as<OpPutById>().m_base);
    case op_put_by_val:
        return mayBeNothing(node->as<OpPutByVal>().m_base);
    case op_del_by_id:
        return mayBeNothing(node->as<OpDelById>().m_base);
    case op_del_by_val:
        return mayBeNothing(node->as<OpDelByVal>().m_base);
    case op_call:
        return mayNotBeAFunction(node->as<OpCall>().m_callee);
    case op_call_ignore_result:
        return mayNotBeAFunction(node->as<OpCallIgnoreResult>().m_callee);
    case op_tail_call:
        return mayNotBeAFunction(node->as<OpTailCall>().m_callee);
    case op_construct:
        return !isAlwaysTheFunction();
    case op_type_tag: // (On behalf of the access that comes next, and as if it were that.)
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
    // (A function that may end up as part of another keeps what there is to say about all of its sites: noteEverySiteOf().)
    if (node->graph->isOutermost()) {
        if (mayBeQuoted(m_graph, node))
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
    : m_graph(graph)
    , m_proc(proc)
    , m_out(proc)
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
        return node->as<OpNewArray>().m_argc;
    case op_new_array_with_spread:
        return node->as<OpNewArrayWithSpread>().m_argc;
    // The items of a list that is not made: at most two words each (Lowering::lowerCallWithItems()).
    case op_call_varargs:
    case op_tail_call_varargs:
    case op_construct_varargs:
    case op_super_construct_varargs: {
        Node* list = Graph::listOfArgumentsOf(node);
        if (!list || !list->isElided)
            return 0;
        return 2 * (list->isBytecode(op_new_array_with_spread) ? list->as<OpNewArrayWithSpread>().m_argc : 1);
    }
    // Arguments that there are no registers for, or that the function called wants in memory.
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
    }
    m_out.setFrequency(1);

    m_out.appendTo(prologue);
    m_out.initializeConstants(m_proc, prologue);

    // Runs on every way in, so what it computes may only depend on the frame pointer and on what is in the same register throughout.
    m_howValuesArePassed = m_graph.howValuesArePassed();
    m_callFrame = m_out.framePointer();
    m_instance = registerOnEntry(instanceGPR);
    m_numberTag = registerOnEntry(GPRInfo::numberTagRegister);
    m_notCellMask = registerOnEntry(GPRInfo::notCellMaskRegister);
    OwnData own = ownData();
    if (m_graph.startsCold) {
        // It has none until it has shown that it is worth one.
        m_data = m_out.select(own.hasAny, own.data, m_out.loadPtr(m_instance, m_heaps.AOTInstance_sharedData));
        m_dataOnEntry = m_data;
        m_constants = wordByIndex(m_out.loadPtr(m_instance, m_heaps.AOTInstance_infos), FunctionInfo::offsetOfConstants(), sizeof(FunctionInfo), false);
    } else
        m_dataOrNothing = own.data;
    m_vm = m_out.loadPtr(m_instance, m_heaps.AOTInstance_vm);
    m_globalObject = m_out.loadPtr(m_instance, m_heaps.AOTInstance_globalObject);
    m_table = m_out.loadPtr(m_instance, m_heaps.AOTInstance_runtimeTable);
    if (m_graph.needsFunctionObject())
        m_calleeSlot = m_out.lockedStackSlot(sizeof(EncodedJSValue));
    if (m_graph.convention().signature == Signature::List)
        m_listSlot = m_out.lockedStackSlot(2 * sizeof(EncodedJSValue));
    if (unsigned homes = m_graph.numberOfHomes())
        m_homes = m_out.lockedStackSlot(homes * sizeof(EncodedJSValue));
    if (m_dataOrNothing) {
        // A function that goes round and round wants caches of its own from the start. The first time, they are made now.
        LBasicBlock hasNone = m_out.newBlock();
        LBasicBlock hasData = m_out.newBlock();
        ValueFromBlock had = m_out.anchor(m_dataOrNothing);
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

    for (BasicBlock* block : m_graph.m_rpo) {
        lowerBlock(block);
        if (m_graph.failed())
            return false;
    }

    if (m_returnBlock) {
        m_out.appendTo(m_returnBlock);
        Rep rep = m_howValuesArePassed.result;
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
    return m_out.address(m_homes, m_heaps.variables[m_graph.homeOf(reg)]);
}

// The word at base + addend + the function's index * scale. No base: the Instance.
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

LValue Lowering::registerOnEntry(Reg reg)
{
    return m_out.m_block->appendNew<ArgumentRegValue>(m_proc, Origin(), reg);
}

// What is done once, on the way in that callers use.
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

// index: not counting `this`.
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

LValue Lowering::structureOf(LValue cell)
{
    // A structure's ID is the low half of its address. The high half is the same for all of them, and this process's own.
    return m_out.bitOr(m_out.zeroExtPtr(m_out.load32(cell, m_heaps.JSCell_structureID)), entry(Entry::StructureIDBase));
}

LValue Lowering::entry(Entry which)
{
    LValue result = m_out.loadPtr(m_out.address(m_table, m_heaps.AOTRuntimeTable[static_cast<unsigned>(which)]));
    static_cast<MemoryValue*>(result)->setReadsMutability(B3::Mutability::Immutable);
    return result;
}

// Whether anybody could ask, while the stub is at it, where the function that called it has got to.
static bool mayLookAtStack(Stub stub)
{
    switch (stub) {
    case Stub::Prologue:
    case Stub::LinkFunction:
    case Stub::PlainOperation:
    case Stub::PlainOperationWithGlobalObject:
    case Stub::PlainOperationWithVM:
    case Stub::WriteBarrier:
    case Stub::ToBoolean:
        return false;
    default:
        return true;
    }
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

    // The address of a slot is worked out where it is wanted. As a value it would be worked out ahead of every loop it is wanted
    // in, and kept, for calls that are hardly ever made.
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    std::optional<std::pair<GPRReg, int32_t>> slotArgument;
    for (auto& argument : arguments) {
        LValue value = argument.value;
        if (!slotArgument && value->opcode() == Add && value->child(0) == m_data && value->child(1)->hasIntPtr()) {
            slotArgument = { argument.reg.gpr(), static_cast<int32_t>(value->child(1)->asIntPtr()) };
            continue;
        }
        patchpoint->append(ConstrainedValue(value, ValueRep::reg(argument.reg)));
    }
    if (slotArgument)
        patchpoint->append(ConstrainedValue(m_data, ValueRep::SomeRegister));
    patchpoint->clobberLate(RegisterSet { ARM64Registers::lr }); // hasNoFrame()
    switch (clobbers) {
    case StubClobbers::WhatCallsDo:
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
        // Whoever asked for several results says where they are.
    } else if (type != Void)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    patchpoint->setGenerator([stubCalls = &m_graph.stubCalls, stub, immediates, slotArgument, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (slotArgument)
            jit.addPtr(CCallHelpers::TrustedImm32(slotArgument->second), params[params.size() - 1].gpr(), slotArgument->first);
        std::optional<uint32_t> valueOfT9;
        for (auto& immediate : immediates) {
            if (immediate.reg == GPRInfo::regT9)
                valueOfT9 = immediate.value;
            else
                jit.move(CCallHelpers::TrustedImm32(immediate.value), immediate.reg);
        }
        if (valueOfT9)
            stubCalls->call(jit, stub, *valueOfT9, site);
        else
            stubCalls->call(jit, stub, site);
    });
    return patchpoint;
}

B3::PatchpointValue* Lowering::emitColdCall(Node* node, LType type, Entry function, LValue first, LValue second)
{
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    if (first)
        patchpoint->append(ConstrainedValue(first, ValueRep::reg(GPRInfo::argumentGPR1)));
    if (second)
        patchpoint->append(ConstrainedValue(second, ValueRep::reg(GPRInfo::argumentGPR2)));
    RegisterSet temporaries;
    temporaries.add(GPRInfo::regT9, IgnoreVectors);
    temporaries.add(GPRInfo::regT10, IgnoreVectors);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobber(temporaries);
    bool returnsValue = type != Void;
    if (returnsValue)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { callSiteBitsOf(node) };
    patchpoint->setGenerator([graph = &m_graph, function, site, returnsValue](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool isLeaf = hasNoFrame(*graph, params.proc().code());
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, GPRInfo::regT10);
        Stub stub = returnsValue ? (isLeaf ? Stub::ColdOperationValueOfLeaf : Stub::ColdOperationValue) : (isLeaf ? Stub::ColdOperationVoidOfLeaf : Stub::ColdOperationVoid);
        graph->stubCalls.call(jit, stub, static_cast<uint32_t>(static_cast<unsigned>(function) * sizeof(void*)), site);
    });
    return patchpoint;
}

void Lowering::coldCall(Node* node, Entry function, LValue first, LValue second)
{
    if (!Options::aotKeepsRegistersInColdCalls()) {
        if (second)
            vmCall(node, Void, function, m_globalObject, first, second);
        else if (first)
            vmCall(node, Void, function, m_globalObject, first);
        else
            vmCall(node, Void, function, m_globalObject);
        return;
    }
    emitColdCall(node, Void, function, first, second);
}

LValue Lowering::coldCallForValue(Node* node, Entry function, LValue first, LValue second)
{
    if (!Options::aotKeepsRegistersInColdCalls())
        return second ? vmCall(node, Int64, function, m_globalObject, first, second) : vmCall(node, Int64, function, m_globalObject, first);
    return emitColdCall(node, Int64, function, first, second);
}

LValue Lowering::callOperationThroughStub(Node* node, LType type, Entry function, const Vector<LValue, 8>& arguments)
{
    bool throws = !!node;
    bool withGlobalObject = !arguments.isEmpty() && arguments[0] == m_globalObject;
    bool withVM = !throws && !arguments.isEmpty() && arguments[0] == m_vm;

    Vector<StubArgument, 8> placed;
    unsigned nextGPR = 0;
    unsigned nextFPR = 0;
    for (unsigned i = 0; i < arguments.size(); ++i) {
        if (arguments[i]->type() == Double) {
            placed.append({ arguments[i], FPRInfo::toArgumentRegister(nextFPR++) });
            continue;
        }
        GPRReg reg = GPRInfo::toArgumentRegister(nextGPR++);
        if (!i && (withGlobalObject || withVM))
            continue;
        placed.append({ arguments[i], reg });
    }
    RELEASE_ASSERT(nextGPR <= GPRInfo::numberOfArgumentRegisters && nextFPR <= FPRInfo::numberOfArgumentRegisters);

    Stub stub;
    if (!throws)
        stub = withGlobalObject ? Stub::PlainOperationWithGlobalObject : withVM ? Stub::PlainOperationWithVM : Stub::PlainOperation;
    else if (type == Double)
        stub = withGlobalObject ? Stub::OperationDoubleWithGlobalObject : Stub::OperationDouble;
    else if (type == Void)
        stub = withGlobalObject ? Stub::OperationVoidWithGlobalObject : Stub::OperationVoid;
    else
        stub = withGlobalObject ? Stub::OperationValueWithGlobalObject : Stub::OperationValue;

    Vector<StubImmediate, 2> immediates;
    immediates.append({ GPRInfo::regT9, static_cast<uint32_t>(static_cast<unsigned>(function) * sizeof(void*)) });
    PatchpointValue* result = callStub(stub, type, placed, immediates, StubClobbers::WhatCallsDo, node);
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
    if (!node->graph->hasGuards())
        return allocateSite(node, identifier, extra);
    return m_sharedSites.ensure((static_cast<uint64_t>(node->bytecodeIndex.offset()) << 32 | identifier) ^ static_cast<uint64_t>(node->graph->inlineFrame()) << 56, [&] {
        return allocateSite(node, identifier, extra);
    }).iterator->value;
}

void Lowering::storeBarrier(LValue owner)
{
    if constexpr (usesStubs) {
        // Nothing that compiled code looks at is any different afterwards.
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

LValue Lowering::numberToDouble(LValue value)
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
            // The engine's own functions pass numbers to functions that take it for granted how they are encoded: whatever an int32 can
            // hold is one, as it is when the interpreter has made it. (Nothing that a program can say tells the two apart.)
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
    // To an integer from anything else: it has been proven to be one, and one that fits.
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
    RELEASE_ASSERT(graph.isOutermost()); // (Or its constants would be among the program's.)
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
    // What a lowering makes may be less specific than what the node is known to be, never the other way around.
    // (Is it what it is known to be? As it comes: once it has been made into what a value of that type is held as, it looks the part.)
    // (What was taken never to be reached added nothing to what is known of anything else: so it had better not be.)
    if (Options::aotVerifiesFacts() && rep == Rep::JSValue && (node->wasTakenNeverToBeReached || (node->type && !isSubtype(TAll, node->type)))) [[unlikely]] {
        Type expected = node->wasTakenNeverToBeReached ? TNone : node->type;
        unsigned which = node->kind == NodeKind::Bytecode ? static_cast<unsigned>(node->opcode) * 1000000 + node->bytecodeIndex.offset() : static_cast<unsigned>(node->kind);
        if (node->kind == NodeKind::Argument)
            which += 100 * node->reg.toArgument();
        unsigned identifierPlusOne = 0;
        // Where it came from, if that is a variable: as Options::aotLogsFacts() has it, to look up what was seen to be put there.
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
        // (Something that has a place in the source, for the frame to be reported at.)
        Node* place = node;
        for (unsigned i = m_nodeIndex; place->kind != NodeKind::Bytecode && i < m_block->nodes.size(); ++i)
            place = m_block->nodes[i];
        // (Neither is an address of anything, by the time the program runs.)
        m_graph.wideIntegerConstants.add(static_cast<int64_t>(expected));
        m_graph.wideIntegerConstants.add(static_cast<int64_t>(expected >> 64));
        m_graph.wideIntegerConstants.add(static_cast<int64_t>(std::bit_cast<uintptr_t>(variable.scope)));
        if (place->kind == NodeKind::Bytecode) {
            vmCall(place, Void, Entry::operationAOTVerifyFact, m_globalObject, value, m_out.constInt64(static_cast<int64_t>(expected)), m_out.constInt64(static_cast<int64_t>(expected >> 64)), m_out.constInt32(which), m_out.constInt32(identifierPlusOne),
                m_out.constInt64(std::bit_cast<uintptr_t>(variable.scope)), m_out.constInt32(variable.offset));
        }
    }
    if (Rep to = node->rep(); rep != to && rep != Rep::JSValue && to != Rep::JSValue && (rep == Rep::Boolean || to == Rep::Boolean)) [[unlikely]] {
        dataLog("AOT: WHAT WAS MADE IS NOT WHAT THE NODE IS KNOWN TO BE: ");
        node->dump(WTF::dataFile());
        dataLogLn(" made as ", static_cast<unsigned>(rep), " held as ", static_cast<unsigned>(to), ", inline frame ", node->graph->inlineFrame(), " in ", m_graph.nameForLog());
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
    // Whoever wants it the way it came gets that. If nobody wants it any other way, nothing comes of the conversion.
    if (rep == Rep::JSValue && node->rep() != Rep::JSValue)
        node->loweredAsJSValue = value;
}

void Lowering::setProj(Node* node, VirtualRegister reg, LValue value, Rep rep)
{
    // The projections come right after the instruction, with at most the stores of homed registers among them.
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
    // Of these, what is not a cell is true if it is `true`; and a cell is, unless it is a string that says nothing. (An object of a kind of its own may pass for undefined.)
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
            // (A rope says something.)
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
    results.append(m_out.anchor(m_out.notZero64(plainCall(Int64, Entry::operationAOTToBoolean, m_globalObject, value))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int32, results);
}

// ---- Structure

// Where the block that is being lowered goes to get to a successor.
LBasicBlock Lowering::edgeTo(BasicBlock* successor)
{
    for (auto& [to, edge] : m_edges) {
        if (to == successor)
            return edge;
    }
    return successor->lowered;
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
    m_out.setFrequency(block->isGeneric || block->isSeldomReached ? coldFrequency : 1);
    m_out.appendTo(block->lowered);
    for (Node* phi : block->phis)
        m_out.m_block->append(phi->lowered);
    // A function that starts cold has no loop of its own, but may have been given one (inlineCalls()). What it goes round with is looked
    // for again each time: it is given caches of its own once it has done without often enough, and that may well be on the way round.
    if (m_dataOnEntry) {
        m_data = m_dataOnEntry;
        if (block->isInLoop) {
            OwnData own = ownData();
            m_data = m_out.select(own.hasAny, own.data, m_out.loadPtr(m_instance, m_heaps.AOTInstance_sharedData));
        }
    }
    if (block->endsWithGuard)
        m_exit = newColdBlock();
    if (block == m_graph.root)
        lowerEntry();

    Node* terminal = block->terminal();
    if (terminal && terminal->kind != NodeKind::Guard && !(terminal->kind == NodeKind::Bytecode && (isBranch(terminal->opcode) || isTerminal(terminal->opcode) || isThrow(terminal->opcode))))
        terminal = nullptr;

    // The origin of a B3 value is the opcode it was made for, plus one: for saying what the code's bytes went to.
    auto setOrigin = [&](Node* node) {
        m_node = node && node->instruction ? node : nullptr;
        m_code = node ? node->graph : block->graph;
        unsigned tag = !node ? 0 : node->kind == NodeKind::Bytecode ? node->opcode + 1 : numOpcodeIDs + 1 + static_cast<unsigned>(node->kind);
        m_out.setOrigin(std::bit_cast<DFG::Node*>(static_cast<uintptr_t>(tag) << 4));
    };
    for (m_nodeIndex = 0; m_nodeIndex < block->nodes.size(); ++m_nodeIndex) {
        Node* node = block->nodes[m_nodeIndex];
        if (node == terminal)
            break;
        if (node->isElided)
            continue;
        setOrigin(node);
        lowerNode(node);
        if (m_graph.failed())
            return;
    }
    if (terminal && terminal->kind == NodeKind::Guard) {
        setOrigin(terminal);
        lowerGuard(block, terminal);
        setOrigin(nullptr);
        return;
    }
    setOrigin(nullptr);
    // What a successor's phis are given is given on the way there. Ahead of a branch, a loop would do on every turn what is only wanted
    // when it ends; and of two moves of one value, to a phi at the top of the loop and to one past the end, only one can be done
    // away with.
    bool branches = std::ranges::any_of(block->successors, [&](BasicBlock* successor) { return successor != block->successors[0]; });
    for (BasicBlock* successor : block->successors) {
        if (!branches) {
            emitUpsilons(block, successor);
            break;
        }
        if (!successor->phis.isEmpty() && edgeTo(successor) == successor->lowered)
            m_edges.append({ successor, m_out.newBlock() });
    }
    setOrigin(terminal);
    lowerTerminalOrFallThrough(block, terminal);
    setOrigin(nullptr);
    for (auto& [successor, edge] : std::exchange(m_edges, { })) {
        m_out.appendTo(edge);
        emitUpsilons(block, successor);
        m_out.jump(successor->lowered);
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
        // The Instance has what has been asked for before.
        LBasicBlock isNotThere = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue had = m_out.load64(m_instance, m_heaps.AOTInstance_linkTimeConstants[node->intrinsic]);
        ValueFromBlock quick = m_out.anchor(had);
        m_out.branch(m_out.notZero64(had), usually(continuation), rarely(isNotThere));
        m_out.appendTo(isNotThere);
        ValueFromBlock made = m_out.anchor(plainCall(Int64, Entry::operationAOTLinkTimeConstant, m_instance, m_out.constInt32(node->intrinsic)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, quick, made));
        return;
    }
    case NodeKind::Guard:
        // One that has been moved to a pre-header.
        emitGuard(node);
        return;
    case NodeKind::Narrow:
        // The guard that the block ends in sees to these. Or, if it comes after one, has.
        if (node->narrowedTo)
            setJSValue(node, lowJSValue(node->uses[0].node));
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
            switch (m_howValuesArePassed.parameters[index]) {
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
        // The instruction has seen to it (setProj()).
        RELEASE_ASSERT(node->lowered);
        return;
    case NodeKind::Bytecode:
        lowerBytecode(node);
        return;
    }
}

void Lowering::lowerBytecode(Node* node)
{
    // TEMPORARY-SITE-COUNTS
    if (Options::aotCountsAllocations()) [[unlikely]] {
        TypedPointer count = m_out.address(m_heaps.root, m_instance, Instance::offsetOfCountsOfSites() + kindOfSite(node, isCompact()) * sizeof(uint64_t));
        m_out.store64(m_out.add(m_out.load64(count), m_out.constInt64(1)), count);
    }
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

#endif // ENABLE(FTL_JIT)
