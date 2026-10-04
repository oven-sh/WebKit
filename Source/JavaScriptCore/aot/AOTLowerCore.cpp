/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#include "AOTCompiler.h"
#include "AOTImage.h"
#include "AOTOpcodeTraits.h"

#if ENABLE(AOT) && (CPU(ARM64) || CPU(X86_64))

#include "AirCode.h"
#include "AirStackSlot.h"
#include "B3PatchpointValue.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "BytecodeStructs.h"
#include "CCallHelpers.h"
#include "JSCInlines.h"
#include "JSGenerator.h"
#include "UnlinkedCodeBlock.h"

namespace JSC { namespace AOT {

static bool errorMayQuoteSource(const Graph& graph, Node* node)
{
    auto mayBeUndefinedOrNull = [&](VirtualRegister base) {
        Node* value = node->use(base);
        return !value || value->type & (TOther | TEmpty);
    };
    auto calleeIsKnownDeclaration = [&] {
        return graph.calleeIsExact(node) && graph.knownCallee(node)->isDeclaration;
    };
    auto calleeMayBeNonFunction = [&](VirtualRegister callee) {
        if (calleeIsKnownDeclaration())
            return false;
        Node* value = node->use(callee);
        return !value || value->type & ~TFunction || !functionNumberOf(value->type);
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
        return calleeMayBeNonFunction(node->as<OpCall>().m_callee);
    case op_call_ignore_result:
        return calleeMayBeNonFunction(node->as<OpCallIgnoreResult>().m_callee);
    case op_tail_call:
        return calleeMayBeNonFunction(node->as<OpTailCall>().m_callee);
    case op_construct:
        return !calleeIsKnownDeclaration();
    case op_type_tag:
        return true;
    default:
        return traitsOf(node->opcode) & OpcodeTraits::ErrorMayQuoteSource;
    }
}

uint32_t Lowering::callSiteBitsOf(Node* node)
{
    if (node->callSiteOrigin)
        node = node->callSiteOrigin;
    if (node->graph->isOutermost()) {
        if (node->kind != NodeKind::Narrow && errorMayQuoteSource(m_graph, node))
            m_graph.quotableSites.append(node->bytecodeIndex.offset());
        m_graph.callSites.append(node->bytecodeIndex.offset());
    }
    return siteOf(node);
}

uint32_t Lowering::siteOf(Node* node)
{
    if (node->callSiteOrigin)
        node = node->callSiteOrigin;
    uint32_t bits = CallSiteIndex(node->bytecodeIndex).bits();
    if (m_graph.inlineFrames.isEmpty())
        return bits;
    bool isTailCall = node->graph->inlineFrame() && node->kind == NodeKind::Bytecode && (node->opcode == op_tail_call || node->opcode == op_tail_call_varargs);
    return PackedSite::pack(node->graph->inlineFrame(), bits, isTailCall);
}

void recordAllSitesOf(Graph& graph)
{
    for (const auto& instruction : graph.codeBlock()->instructions()) {
        graph.callSites.append(instruction.offset());
        if (traitsOf(instruction->opcodeID()) & OpcodeTraits::ErrorMayQuoteSource)
            graph.quotableSites.append(instruction.offset());
    }
}

using namespace B3;

Lowering::Lowering(Graph& graph, Procedure& proc)
    : Emitter(proc)
    , m_graph(graph)
{
    m_out.noteFoldedConstantsIn(graph.wideIntegerConstants);
}

static unsigned scratchWordsForCall(Node* node, unsigned argc, bool isConstruct)
{
    if (argc - 1 > numberOfArgumentGPRs || node->builtinCalled)
        return argc;
    bool isExact = false;
    const KnownFunction* known = node->graph->knownCallee(node, &isExact);
    if (known && isExact && (isConstruct ? known->conventionForConstruct : known->conventionForCall).signature == Signature::List)
        return argc;
    return 0;
}

static unsigned scratchWordsFor(Node* node)
{
    if (node->kind != NodeKind::Bytecode || !node->instruction)
        return 0;
    switch (node->opcode) {
    case op_new_object:
        if (Graph::typeTagOf(node)) {
            if (auto shape = node->graph->literalShape(node))
                return shape->numberOfSlots();
        }
        return node->numberOfLiteralProperties;
    case op_create_this:
        return node->numberOfLiteralProperties;
    case op_new_array:
        return Graph::readsOperandsFromFrame(node) ? 0 : node->as<OpNewArray>().m_argc;
    case op_new_array_with_spread:
        return node->as<OpNewArrayWithSpread>().m_argc;
    case op_call_varargs:
    case op_tail_call_varargs:
    case op_construct_varargs:
    case op_super_construct_varargs: {
        Node* list = Graph::argumentListFor(node);
        if (!list || !list->isElided)
            return 0;
        return 2 * (list->isBytecode(op_new_array_with_spread) ? list->as<OpNewArrayWithSpread>().m_argc : 1);
    }
    case op_call:
        return scratchWordsForCall(node, node->as<OpCall>().m_argc, false);
    case op_call_ignore_result:
        return scratchWordsForCall(node, node->as<OpCallIgnoreResult>().m_argc, false);
    case op_tail_call:
        return scratchWordsForCall(node, node->as<OpTailCall>().m_argc, false);
    case op_construct:
        return scratchWordsForCall(node, node->as<OpConstruct>().m_argc, true);
    case op_super_construct:
        return scratchWordsForCall(node, node->as<OpSuperConstruct>().m_argc, true);
    case op_call_direct_eval:
        return node->as<OpCallDirectEval>().m_argc;
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
        m_out.setFrequency(block->isGeneric || block->isRarelyExecuted ? coldFrequency : 1);
        block->lowered = m_out.newBlock();
        if (!block->arraysViewed.isEmpty())
            block->loweredAhead = m_out.newBlock();
    }
    m_out.setFrequency(1);

    m_out.appendTo(prologue);
    m_out.initializeConstants(m_proc, prologue);

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
        m_data = own.data;
        m_dataOnEntry = m_data;
        if (std::ranges::any_of(m_graph.m_rpo, [](BasicBlock* block) { return block->isLoopHeader; })) {
            m_dataInLoops = m_proc.addVariable(pointerType());
            m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), m_dataInLoops, m_data);
        }
    } else
        m_dataOrNull = own.data;
    m_vm = m_out.loadPtr(m_instance, m_heaps.AOTInstance_vm);
    m_globalObject = m_out.loadPtr(m_instance, m_heaps.AOTInstance_globalObject);
    m_table = m_out.loadPtr(m_instance, m_heaps.AOTInstance_runtimeTable);
    if (m_graph.needsFunctionObject()) {
        m_calleeSlot = m_out.lockedStackSlot(sizeof(EncodedJSValue));
        if (m_graph.codeBlock()->codeType() == FunctionCode && AOT::needsFunctionObject(m_graph.codeBlock()) && !(m_graph.summary() && m_graph.summary()->takesScopeAsCallee))
            m_graph.calleeSlot = m_calleeSlot->as<B3::SlotBaseValue>()->slot();
    }
    if (m_graph.convention().signature == Signature::List)
        m_listSlot = m_out.lockedStackSlot(2 * sizeof(EncodedJSValue));
    if (unsigned homes = m_graph.numberOfFrameRegisters())
        m_frameRegisterStorage = m_out.lockedStackSlot(homes * sizeof(EncodedJSValue));
    if (m_dataOrNull) {
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
    for (BasicBlock* block : m_graph.m_rpo)
        findPropertyRuns(block);
    for (auto& run : m_propertyRuns)
        scratchWords = std::max<unsigned>(scratchWords, run.size());
    m_graph.returnValueReads.clear();
    for (BasicBlock* block : m_graph.m_rpo) {
        for (Node* node : block->nodes) {
            scratchWords = std::max(scratchWords, scratchWordsFor(node));
            for (auto& use : node->uses)
                use.node->useCount++;
            if (node->kind == NodeKind::Proj && !node->isElided)
                m_graph.returnValueReads.add(node->uses[0].node, Vector<Node*, 8> { }).iterator->value.append(node);
        }
        for (Node* phi : block->phis) {
            for (auto& use : phi->uses)
                use.node->useCount++;
        }
    }
    m_numberOfScratchWords = scratchWords;
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

    findComparisonChains();
    for (BasicBlock* block : m_graph.m_rpo) {
        lowerBlock(block);
        if (m_graph.failed())
            return false;
    }

    if (m_returnBlock) {
        m_out.appendTo(m_returnBlock);
        Rep rep = m_valueRepresentations.result;
        if (m_graph.numberOfRegisterReturnValues) {
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

void Lowering::incrementTypeCoverageCounter(uint32_t counter)
{
    LValue counters = m_out.loadPtr(m_out.address(m_heaps.root, m_instance, Instance::offsetOfTypeCoverageCounters()));
    TypedPointer address = m_out.address(m_heaps.root, counters, static_cast<ptrdiff_t>(counter) * sizeof(uint32_t));
    m_out.store32(m_out.add(m_out.load32(address), m_out.int32One), address);
}

void Lowering::coverOperation(Node* node, BasicBlock* block, bool isElided)
{
    size_t first = m_graph.coverage.size();
    m_graph.beginCoveredOperation(node, block, isElided);
    m_blockWhereOperationStarts = m_out.m_block;
    if (!Options::useAOTTypeCoverageCounters())
        return;
    for (size_t i = first; i < m_graph.coverage.size(); ++i)
        incrementTypeCoverageCounter(m_graph.coverage[i].counter);
}

void Lowering::coverCall(StringView name, uint32_t whichCounter)
{
    bool isOffUsualPath = m_out.m_block != m_blockWhereOperationStarts && (m_out.m_block->frequency() <= coldFrequency || (m_blocksReachedRarely.contains(m_out.m_block) && !m_blocksReachedOtherwise.contains(m_out.m_block)));
    m_graph.remark("calls"_s, name, isOffUsualPath);
    if (Options::useAOTTypeCoverageCounters() && m_graph.isCoveringOperation()) [[unlikely]]
        incrementTypeCoverageCounter(m_graph.coverage.last().counter + whichCounter);
}

void Lowering::trap()
{
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams&) {
        jit.breakpoint();
    });
    m_out.unreachable();
}

void Lowering::unsupported(Node* node)
{
    m_graph.fail("no lowering"_s, node->opcode);
}

TypedPointer Lowering::addressFor(VirtualRegister reg)
{
    return m_out.address(m_frameRegisterStorage, m_heaps.variables[m_graph.frameRegisterIndex(reg)]);
}

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
    PatchpointValue* number = m_out.patchpoint(pointerType());
    number->effects = Effects::none();
    number->effects.reads = HeapRange::top();
    number->setGenerator([indexReferences = &m_graph.indexReferences](CCallHelpers& jit, const StackmapGenerationParams& params) {
        indexReferences->load16(jit, instanceGPR, params[0].gpr(), Instance::offsetOfStates(), sizeof(uint32_t));
    });
    LValue data = m_out.loadPtr(TypedPointer(m_heaps.root, m_out.add(m_instance, m_out.shl(number, m_out.constInt32(3)))));
    return { m_out.notZero64(number), data };
}

LValue Emitter::registerOnEntry(Reg reg)
{
    return m_out.m_block->appendNew<ArgumentRegValue>(m_proc, Origin(), reg);
}

void Lowering::lowerEntry()
{
    if (m_calleeSlot)
        m_out.store64(registerOnEntry(calleeGPR), m_out.address(m_heaps.variables.atAnyIndex(), m_calleeSlot));
    if (m_graph.convention().signature == Signature::List)
        m_graph.remark("takes-argument-list"_s);
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

LValue Lowering::argumentPassed(unsigned index)
{
    return m_out.load64(m_out.address(m_heaps.root, argumentsPassed(), index * sizeof(EncodedJSValue)));
}

LValue Lowering::argumentPassedOrUndefined(unsigned index)
{
    LBasicBlock isThere = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock isNot = m_out.anchor(m_out.constInt64(JSValue::encode(jsUndefined())));
    m_out.branch(m_out.above(numberOfArgumentsPassed(), m_out.constInt32(index)), usually(isThere), rarely(continuation));
    m_out.appendTo(isThere, continuation);
    ValueFromBlock is = m_out.anchor(argumentPassed(index));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(Int64, isNot, is);
}

LValue Lowering::dataHere()
{
    if (!m_dataOnEntry || !m_block || m_block->isGeneric || m_block->isRarelyExecuted || m_out.m_block->frequency() > coldFrequency)
        return m_data;
    return ownData().data;
}

LValue Lowering::vmHere()
{
    return m_out.loadPtr(m_instance, m_heaps.AOTInstance_vm);
}

LValue Lowering::globalObjectHere()
{
    return m_out.loadPtr(m_instance, m_heaps.AOTInstance_globalObject);
}

TypedPointer Lowering::slotWord(unsigned slot, unsigned word)
{
    return m_out.address(dataHere(), m_heaps.AOTData_slotWords[slot * 2 + word]);
}

LValue Lowering::slotAddress(unsigned slot)
{
    return m_out.add(dataHere(), m_out.constIntPtr(Data::offsetOfSlots() + slot * sizeof(Slot)));
}

TypedPointer Lowering::scratchWord(unsigned index)
{
    RELEASE_ASSERT(m_scratch && index < m_numberOfScratchWords);
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
    return structureWithID(m_out.load32(cell, m_heaps.JSCell_structureID));
}

LValue Emitter::entry(Entry which)
{
    LValue result = m_out.loadPtr(m_out.address(m_table, m_heaps.AOTRuntimeTable[static_cast<unsigned>(which)]));
    static_cast<MemoryValue*>(result)->setReadsMutability(B3::Mutability::Immutable);
    return result;
}

static bool mayInspectStack(Stub stub)
{
    if (isHelper(stub))
        return false;
    switch (stub) {
    case Stub::Prologue:
    case Stub::LinkFunction:
    case Stub::Constant:
    case Stub::ConstantFromSlot:
    case Stub::PlainOperation:
    case Stub::PlainOperationWithGlobalObject:
    case Stub::PlainOperationWithInstance:
    case Stub::PlainOperationWithVM:
    case Stub::WriteBarrier:
    case Stub::ToBoolean:
    case Stub::Latin1Characters:
    case Stub::WeakMapGet:
    case Stub::WeakMapHas:
    case Stub::WeakSetHas:
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
        unsigned numberOfUses = 0;
        for (auto& use : next->uses)
            numberOfUses += use.node == node;
        return node->useCount > numberOfUses;
    }
    return node->useCount;
}

PatchpointValue* Lowering::callStub(Stub stub, LType type, const Vector<StubArgument, 8>& arguments, const Vector<StubImmediate, 2>& immediates, StubClobbers clobbers, Node* place)
{
    m_graph.emitsCalls = true;
    coverCall(nameOf(stub), CoveredOperation::stubCallsCounter);
    if (!place)
        place = m_node;
    CallSite site;
    if (mayInspectStack(stub)) {
        RELEASE_ASSERT(place);
        site.bits = callSiteBitsOf(place);
    }

    PatchpointValue* patchpoint = m_out.patchpoint(type);
    std::optional<std::pair<GPRReg, int32_t>> slotArgument;
    Vector<std::pair<GPRReg, B3::Air::StackSlot*>, 2> frameSlots;
    std::optional<uint32_t> t9Value;
    for (auto& immediate : immediates) {
        if (immediate.reg == stubImmediateGPR)
            t9Value = immediate.value;
    }
    bool operandUsesAnyRegister = !type.isTuple() && !arguments.isEmpty() && arguments[0].reg.isGPR() && acceptsOperandInAnyRegister(stub, t9Value) && arguments[0].reg.gpr() == defaultOperandRegister(stub)
        && arguments[0].value->opcode() != SlotBase;
    auto isSlotAddress = [&](LValue value) { return value->opcode() == Add && value->child(0) == m_data && value->child(1)->hasIntPtr(); };
    bool secondOperandUsesAnyRegister = operandUsesAnyRegister && clobbers == StubClobbers::CallerSavedRegisters && arguments.size() >= 2 && acceptsTwoOperandsInAnyRegisters(stub) && arguments[1].reg == Reg(GPRInfo::argumentGPR1)
        && arguments[1].value->opcode() != SlotBase && !isSlotAddress(arguments[1].value);
    bool resultUsesAssignedRegister = type == Int64 && clobbers == StubClobbers::CallerSavedRegisters && returnsResultInAnyRegister(stub, t9Value) && m_node && place == m_node && isLiveAfterNextNode(m_node);
    RegisterSet clobberedBeforeCall;
    bool placesFirstOperandLast = isX86_64() && !operandUsesAnyRegister;
    for (unsigned i = 0; i < arguments.size(); ++i) {
        auto& argument = arguments[placesFirstOperandLast ? arguments.size() - 1 - i : i];
        LValue value = argument.value;
        if (!slotArgument && value->opcode() == Add && value->child(0) == m_data && value->child(1)->hasIntPtr()) {
            slotArgument = { argument.reg.gpr(), static_cast<int32_t>(value->child(1)->asIntPtr()) };
            clobberedBeforeCall.add(argument.reg, IgnoreVectors);
            continue;
        }
        if (value->opcode() == SlotBase) {
            frameSlots.append(std::pair<GPRReg, B3::Air::StackSlot*> { argument.reg.gpr(), value->as<B3::SlotBaseValue>()->slot() });
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
    if (slotArgument) {
        patchpoint->append(ConstrainedValue(m_data, ValueRep::SomeRegister));
        m_graph.patchpointsTakingData.add(patchpoint);
    }
    for (auto& immediate : immediates) {
        if (immediate.reg != stubImmediateGPR)
            clobberedBeforeCall.add(immediate.reg, IgnoreVectors);
    }
    if (operandUsesAnyRegister || slotArgument)
        patchpoint->clobberEarly(clobberedBeforeCall);
    patchpoint->clobberLate(clobberedBeforeCall);
    if (operandUsesAnyRegister) {
        RegisterSet excludedRegisters;
        for (unsigned i = 0; i < 16; ++i) {
            GPRReg reg = static_cast<GPRReg>(i);
            if (!operandAllowedInRegister(stub, reg) || (secondOperandUsesAnyRegister && i >= 9))
                excludedRegisters.add(reg, IgnoreVectors);
        }
        patchpoint->clobberEarly(excludedRegisters);
        patchpoint->clobberLate(excludedRegisters);
        if (!preservesOperandRegister(stub))
            patchpoint->clobberLate(RegisterSet { defaultOperandRegister(stub) });
    }
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    switch (clobbers) {
    case StubClobbers::CallerSavedRegisters:
        RELEASE_ASSERT(!registersChangedBy(stub));
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
#if CPU(X86_64)
        patchpoint->clobberLate(registersClobberedByCalls());
#endif
        break;
    case StubClobbers::Temporaries:
    case StubClobbers::Nothing:
        patchpoint->clobber(*registersChangedBy(stub));
        break;
    }
    if (type == Double)
        patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::returnValueFPR) };
    else if (type.isTuple()) {
    } else if (type != Void)
        patchpoint->resultConstraints = { resultUsesAssignedRegister ? ValueRep::SomeRegister : ValueRep::reg(GPRInfo::returnValueGPR) };
    unsigned operandChildIndex = type == Void ? 0 : 1;
    patchpoint->setGenerator([stubCalls = &m_graph.stubCalls, stub, immediates, slotArgument, frameSlots, operandUsesAnyRegister, secondOperandUsesAnyRegister, resultUsesAssignedRegister, operandChildIndex, t9Value, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (slotArgument)
            jit.addPtr(CCallHelpers::TrustedImm32(slotArgument->second), params[params.size() - 1].gpr(), slotArgument->first);
        for (auto& [reg, slot] : frameSlots)
            jit.addPtr(CCallHelpers::TrustedImm32(slot->offsetFromFP()), GPRInfo::callFrameRegister, reg);
        for (auto& immediate : immediates) {
            if (immediate.reg != stubImmediateGPR)
                jit.move(CCallHelpers::TrustedImm32(immediate.value), immediate.reg);
        }
        if (resultUsesAssignedRegister)
            stubCalls->callWithResultInRegister(jit, stub, *t9Value, operandUsesAnyRegister ? params[operandChildIndex].gpr() : defaultOperandRegister(stub), params[0].gpr(), site);
        else if (secondOperandUsesAnyRegister)
            stubCalls->callWithOperandsInRegisters(jit, stub, params[operandChildIndex].gpr(), params[operandChildIndex + 1].gpr(), site);
        else if (operandUsesAnyRegister)
            stubCalls->callWithOperandInRegister(jit, stub, t9Value, params[operandChildIndex].gpr(), site);
        else if (t9Value)
            stubCalls->call(jit, stub, *t9Value, site);
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
    if (m_out.m_block->frequency() > coldFrequency)
        m_graph.remark("cold-call-on-usual-path"_s, nameOf(function));
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    if (what == ColdCall::ChangesNothing) {
        patchpoint->effects = Effects();
        patchpoint->effects.reads = HeapRange::top();
        patchpoint->effects.exitsSideways = true;
        patchpoint->effects.controlDependent = true;
    }
    bool returnsValue = type != Void;
    uint32_t t9Value = static_cast<uint32_t>(static_cast<unsigned>(function) * sizeof(void*));
    bool firstOperandUsesAnyRegister = first && acceptsOperandInAnyRegister(returnsValue ? Stub::ColdOperationValue : Stub::ColdOperationVoid, t9Value) && acceptsOperandInAnyRegister(returnsValue ? Stub::LeafColdOperationValue : Stub::LeafColdOperationVoid, t9Value);
    if (firstOperandUsesAnyRegister) {
        patchpoint->append(ConstrainedValue(first, ValueRep::SomeRegister));
        patchpoint->clobberLate(RegisterSet { GPRInfo::argumentGPR1 });
    } else if (first)
        patchpoint->append(ConstrainedValue(first, ValueRep::reg(GPRInfo::argumentGPR1)));
    if (second)
        patchpoint->append(ConstrainedValue(second, ValueRep::reg(GPRInfo::argumentGPR2)));
    patchpoint->clobber(*registersChangedBy(returnsValue ? Stub::ColdOperationValue : Stub::ColdOperationVoid));
    if (!hasStubsForFunctionsWithoutFrame) {
        m_graph.emitsCalls = true;
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    }
    if (returnsValue)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { callSiteBitsOf(node) };
    patchpoint->setGenerator([graph = &m_graph, t9Value, site, returnsValue, firstOperandUsesAnyRegister](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool isLeaf = hasNoFrame(*graph, params.proc().code());
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        Stub stub = returnsValue ? (isLeaf ? Stub::LeafColdOperationValue : Stub::ColdOperationValue) : (isLeaf ? Stub::LeafColdOperationVoid : Stub::ColdOperationVoid);
        if (firstOperandUsesAnyRegister)
            graph->stubCalls.callWithOperandInRegister(jit, stub, t9Value, params[returnsValue ? 1 : 0].gpr(), site);
        else
            graph->stubCalls.call(jit, stub, t9Value, site);
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
    coverCall(nameOf(function), CoveredOperation::runtimeCallsCounter);
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
        RELEASE_ASSERT(nextGPR < numberOfOperationArgumentGPRs);
        GPRReg reg = operationArgumentGPR(nextGPR++);
        if (!i && (withGlobalObject || withInstance || withVM))
            continue;
        placed.append({ arguments[i], reg });
    }
    RELEASE_ASSERT(nextFPR <= FPRInfo::numberOfArgumentRegisters);
    RELEASE_ASSERT(throws || nextGPR <= GPRInfo::numberOfArgumentRegisters);

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
    immediates.append({ stubImmediateGPR, static_cast<uint32_t>(static_cast<unsigned>(function) * sizeof(void*)) });
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
    if (!node->graph->hasGuards() || node->skippedEnvironments)
        return allocateSite(node, identifier, extra);
    return m_sharedSites.ensure((static_cast<uint64_t>(node->bytecodeIndex.offset()) << 32 | identifier) ^ static_cast<uint64_t>(node->graph->inlineFrame()) << 56, [&] {
        return allocateSite(node, identifier, extra);
    }).iterator->value;
}

void Lowering::storeBarrier(LValue owner)
{
    bool isWhereOperationStarts = m_out.m_block == m_blockWhereOperationStarts;
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    PatchpointValue* threshold = m_out.patchpoint(Int32);
    threshold->effects = Effects::none();
    threshold->effects.reads = HeapRange::top();
    threshold->effects.writesLocalState = true;
    threshold->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.loadPtr(CCallHelpers::Address(instanceGPR, Instance::offsetOfVM()), params[0].gpr());
        jit.load32(CCallHelpers::Address(params[0].gpr(), VM::offsetOfHeapBarrierThreshold()), params[0].gpr());
    });
    m_out.branch(m_out.above(m_out.load8ZeroExt32(owner, m_heaps.JSCell_cellState), threshold), usually(continuation), rarely(slowPath));
    m_out.appendTo(slowPath, continuation);
    if (usesDataStubs()) {
        PatchpointValue* patchpoint = callStub(Stub::WriteBarrier, Void, { { owner, firstStubOperandGPR } }, { }, StubClobbers::Temporaries);
        patchpoint->effects = Effects::none();
        patchpoint->effects.controlDependent = true;
        m_heaps.decoratePatchpointRead(&m_heaps.JSCell_cellState, patchpoint);
        m_heaps.decoratePatchpointWrite(&m_heaps.JSCell_cellState, patchpoint);
    } else
        plainCall(Void, Entry::operationAOTWriteBarrier, m_vm, owner);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    if (isWhereOperationStarts)
        m_blockWhereOperationStarts = continuation;
}

LValue Emitter::numberToDouble(LValue value)
{
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
    LBasicBlock slowPath = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue truncated = m_out.doubleToInt64(value);
    ValueFromBlock fastResult = m_out.anchor(m_out.castToInt32(truncated));
    LValue isOutOfRange = m_out.equal(truncated, m_out.constInt64(std::numeric_limits<int64_t>::min()));
    if (isARM64())
        isOutOfRange = m_out.bitOr(isOutOfRange, m_out.equal(truncated, m_out.constInt64(std::numeric_limits<int64_t>::max())));
    m_out.branch(isOutOfRange, rarely(slowPath), usually(continuation));
    m_out.appendTo(slowPath, continuation);
    ValueFromBlock slowResult = m_out.anchor(plainCall(Int32, Entry::operationAOTDoubleToInt32, value));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(Int32, fastResult, slowResult);
}

LValue Lowering::convert(LValue value, Rep from, Type fromType, Rep to)
{
    if (from == to)
        return value;
    switch (to) {
    case Rep::JSValue:
        switch (from) {
        case Rep::Int32:
            if (!mayBe(fromType, TInt32))
                return boxDouble(m_out.intToDouble(value));
            return boxInt32(value);
        case Rep::Int64: {
            if (!mayBe(fromType, TInt32))
                return boxDouble(m_out.intToDouble(value));
            LValue narrow = m_out.castToInt32(value);
            return m_out.select(m_out.equal(m_out.signExt32To64(narrow), value), boxInt32(narrow), boxDouble(m_out.intToDouble(value)));
        }
        case Rep::Double:
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

void Lowering::dropGuardIfSelected(const char* file, unsigned line)
{
    StringView path = StringView::fromLatin1(file);
    size_t slash = path.reverseFind('/');
    String site = makeString(slash == notFound ? path : path.substring(slash + 1), ':', line);
    if (site != StringView::fromLatin1(byteCast<char>(Options::aotGuardToDropForTesting())))
        return;
    m_out.dropNextGuardForTesting();
    if (Options::verboseAOTCompilation())
        dataLogLn("AOT: dropped the guard at ", site);
}

void Lowering::noteIndexConstant(int32_t index)
{
    uint64_t wide = static_cast<uint32_t>(index);
    for (unsigned shift = 1; shift <= 3; ++shift)
        m_graph.wideIntegerConstants.add(static_cast<int64_t>(wide << shift));
}

LValue Lowering::lowRaw(Node* node)
{
    switch (node->kind) {
    case NodeKind::Constant:
        switch (node->rep()) {
        case Rep::Int32:
            noteIndexConstant(static_cast<int32_t>(node->constant.asNumber()));
            return m_out.constInt32(static_cast<int32_t>(node->constant.asNumber()));
        case Rep::Int64:
            m_graph.wideIntegerConstants.add(static_cast<int64_t>(node->constant.asNumber()));
            return m_out.constInt64(static_cast<int64_t>(node->constant.asNumber()));
        case Rep::Double:
            return m_out.constDouble(node->constant.asNumber());
        case Rep::Boolean:
            return m_out.constInt32(node->constant.asBoolean());
        case Rep::JSValue:
            if (node->constant.isInt32())
                noteIndexConstant(node->constant.asInt32());
            return m_out.constInt64(JSValue::encode(node->constant));
        }
        break;
    case NodeKind::ConstantCell:
        if (node->ownerOfConstant)
            return programConstant(programConstantIndex(node));
        return lowConstantRegister(*node->graph, node->reg);
    case NodeKind::Intrinsic:
        if (node->intrinsic == ImmutableIntrinsics::globalObject)
            return globalObjectHere();
        return m_out.load64(m_instance, m_heaps.AOTInstance_intrinsics[node->intrinsic]);
    default:
        break;
    }
    RELEASE_ASSERT(node->lowered);
    return node->lowered;
}

LValue Lowering::lowConstantRegister(Graph& graph, VirtualRegister reg)
{
    uint32_t number = programConstantIndicesFor(graph.codeBlock())->at(reg.toConstantIndex());
    RELEASE_ASSERT(number != invalidConstantIndex);
    JSCell* constant = graph.codeBlock()->getConstant(reg).asCell();
    if (constant->inherits<JSTemplateObjectDescriptor>())
        return constantThroughStub(number, Stub::TemplateObject);
    if (isInRunOnceCode() && &graph == &m_graph)
        return constantThroughStub(number, Stub::TransientConstant);
    return programConstant(number);
}

LValue Lowering::constantThroughStub(uint32_t number, Stub stub)
{
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    patchpoint->effects = Effects::none();
    patchpoint->clobber(*registersChangedBy(stub));
    if (!hasStubsForFunctionsWithoutFrame) {
        m_graph.emitsCalls = true;
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    }
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    patchpoint->setGenerator([graph = &m_graph, number, stub](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
#if CPU(ARM64)
        bool isLeaf = hasNoFrame(*graph, params.proc().code());
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#else
        UNUSED_PARAM(params);
#endif
        graph->stubCalls.call(jit, stub, number, CallSite { });
#if CPU(ARM64)
        if (isLeaf)
            jit.move(stubTemporaryGPRs[1], CCallHelpers::linkRegister);
#endif
    });
    return patchpoint;
}

LValue Lowering::programConstant(uint32_t number)
{
    if (!usesDataStubs())
        return constantThroughStub(number, Stub::Constant);
    unsigned slot = m_constantSlots.ensure(number, [&] {
        return allocateSlot();
    }).iterator->value;
    bool readsInline = !isCompact();
    m_graph.remark(readsInline ? "constant-read-inline"_s : "constant-from-slot"_s);
    LValue dataOfFunction = dataHere();
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    patchpoint->effects = Effects::none();
    patchpoint->append(ConstrainedValue(dataOfFunction, ValueRep::SomeLateRegister));
    m_graph.patchpointsTakingData.add(patchpoint);
    patchpoint->clobber(*registersChangedBy(Stub::ConstantFromSlot));
    if (!hasStubsForFunctionsWithoutFrame) {
        m_graph.emitsCalls = true;
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    }
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    patchpoint->setGenerator([graph = &m_graph, number, slot, readsInline](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg data = params[1].gpr();
        int32_t offset = static_cast<int32_t>(Data::offsetOfSlots() + slot * sizeof(Slot));
        CCallHelpers::Jump isThere;
        if (readsInline) {
            jit.load64(CCallHelpers::Address(data, offset + OBJECT_OFFSETOF(Slot, pointer)), GPRInfo::returnValueGPR);
            isThere = jit.branchTest64(CCallHelpers::NonZero, GPRInfo::returnValueGPR);
        }
        jit.addPtr(CCallHelpers::TrustedImm32(offset), data, stubTemporaryGPRs[2]);
#if CPU(ARM64)
        bool isLeaf = hasNoFrame(*graph, params.proc().code());
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        graph->stubCalls.call(jit, Stub::ConstantFromSlot, number, CallSite { });
#if CPU(ARM64)
        if (isLeaf)
            jit.move(stubTemporaryGPRs[1], CCallHelpers::linkRegister);
#endif
        if (readsInline)
            isThere.link(&jit);
    });
    return patchpoint;
}

uint32_t Lowering::programConstantIndex(Node* node)
{
    RELEASE_ASSERT(node->kind == NodeKind::ConstantCell && node->reg.isConstant());
    uint32_t number = programConstantIndicesFor(node->codeBlockOfConstant())->at(node->reg.toConstantIndex());
    RELEASE_ASSERT(number != invalidConstantIndex);
    return number;
}

LValue Lowering::lowJSValue(Node* node)
{
    if (node->loweredAsJSValue)
        return node->loweredAsJSValue;
    return convert(lowRaw(node), node->rep(), node->type, Rep::JSValue);
}

LValue Lowering::lowJSValuePreferringInt32(Node* node)
{
    Rep rep = node->rep();
    if (rep == Rep::Int32 || rep == Rep::Int64)
        return convert(lowRaw(node), rep, TNumber, Rep::JSValue);
    if (rep != Rep::Double || node->isConstant())
        return lowJSValue(node);
    m_graph.remark("boxes-integral-double-as-int32"_s);
    LValue number = lowRaw(node);
    LValue narrow = m_out.doubleToInt32(number);
    return m_out.select(m_out.equal(m_out.bitCast(m_out.intToDouble(narrow), Int64), m_out.bitCast(number, Int64)), boxInt32(narrow), boxDouble(number));
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

void Lowering::verifyInferredType(Node* node, LValue value)
{
    if (!node->wasInferredUnreachable && (!node->type || isSubtype(TAll, node->type)))
        return;
    Type expected = node->wasInferredUnreachable ? TNone : node->type;
    if (const KnownFunction* known = programFunctions() ? programFunctions()->function(functionNumberOf(expected)) : nullptr; known && known->summary && known->summary->takesScopeAsCallee)
        expected = (expected & ~TFunction) | TOtherObject;
    if (Options::aotFunctionWithWrongTypesForTesting()) [[unlikely]] {
        const KnownFunction* function = programFunctions() && m_graph.summary() ? programFunctions()->function(m_graph.summary()->number) : nullptr;
        if (function && function->executable && StringView { function->executable->ecmaName().string() } == StringView::fromLatin1(byteCast<char>(Options::aotFunctionWithWrongTypesForTesting())))
            expected = TNone;
    }
    unsigned which = node->kind == NodeKind::Bytecode ? static_cast<unsigned>(node->opcode) * 1000000 + node->bytecodeIndex.offset() : static_cast<unsigned>(node->kind);
    if (node->kind == NodeKind::Argument)
        which += 100 * node->reg.toArgument();
    unsigned identifierPlusOne = 0;
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
    Node* place = node;
    for (unsigned i = m_nodeIndex; place->kind != NodeKind::Bytecode && i < m_block->nodes.size(); ++i)
        place = m_block->nodes[i];
    m_graph.wideIntegerConstants.add(static_cast<int64_t>(expected));
    m_graph.wideIntegerConstants.add(static_cast<int64_t>(expected >> 64));
    m_graph.wideIntegerConstants.add(static_cast<int64_t>(std::bit_cast<uintptr_t>(variable.scope)));
    if (place->kind == NodeKind::Bytecode) {
        vmCall(place, Void, Entry::operationAOTVerifyInferredType, m_instance, value, m_out.constInt64(static_cast<int64_t>(expected)), m_out.constInt64(static_cast<int64_t>(expected >> 64)), m_out.constInt32(which), m_out.constInt32(identifierPlusOne),
            m_out.constInt64(std::bit_cast<uintptr_t>(variable.scope)), m_out.constInt32(variable.offset));
    }
}

void Lowering::setResult(Node* node, LValue value, Rep rep)
{
    if (Options::validateAOTInferredTypes() && rep == Rep::JSValue) [[unlikely]]
        verifyInferredType(node, value);
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
    if (m_aliasTarget && m_aliasTarget->loweredAsJSValue && node->rep() != Rep::JSValue)
        node->loweredAsJSValue = m_aliasTarget->loweredAsJSValue;
    if (rep == Rep::JSValue && node->rep() != Rep::JSValue)
        node->loweredAsJSValue = value;
}

void Lowering::setProj(Node* node, VirtualRegister reg, LValue value, Rep rep)
{
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
    Type type = node->type & ~TEmpty;
    if (isSubtype(type, TOther))
        return m_out.booleanFalse;
    if (isSubtype(type, TBoolean | TOther))
        return m_out.equal(value, m_out.constInt64(JSValue::ValueTrue));
    if (isSubtype(type, TInt32 | TOther)) {
        m_graph.remark("inline-truthiness-of-number"_s);
        return m_out.above(value, m_out.constInt64(JSValue::NumberTag));
    }
    if (isSubtype(type, TNumber | TUndefined)) {
        m_graph.remark("inline-truthiness-of-number"_s);
        return m_out.doubleNotEqualAndOrdered(numberToDouble(value), m_out.constDouble(0));
    }
    if (isSubtype(type, TBoolean | TOther | TString | TSymbol | TAnyObject)) {
        bool mayMasquerade = mayBe(type, TOtherObject);
        if (!mayBe(type, TString) && !mayBe(type, TBoolean) && !mayMasquerade)
            return isSubtype(type, TCell) ? m_out.booleanTrue : isCell(value);
        if (mayBe(type, TString))
            m_graph.remark("truthiness-of-string-by-identity"_s);
        if (!mayMasquerade) {
            LValue isTruthyCell = mayBe(type, TString) ? m_out.notEqual(value, emptyString()) : m_out.booleanTrue;
            if (!isSubtype(type, TCell))
                isTruthyCell = m_out.bitAnd(isCell(value), isTruthyCell);
            return mayBe(type, TBoolean) ? m_out.bitOr(isTruthyCell, m_out.equal(value, m_out.constInt64(JSValue::ValueTrue))) : isTruthyCell;
        }
        if (mayMasquerade)
            m_graph.remark("inline-truthiness-of-object"_s);
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        LBasicBlock objectCase = mayMasquerade ? m_out.newBlock() : continuation;
        Vector<ValueFromBlock, 6> results;
        if (!isSubtype(type, TCell)) {
            results.append(m_out.anchor(mayBe(type, TBoolean) ? m_out.equal(value, m_out.constInt64(JSValue::ValueTrue)) : m_out.booleanFalse));
            m_out.branch(isCell(value), unsure(cellCase), unsure(continuation));
        } else
            m_out.jump(cellCase);
        m_out.appendTo(cellCase);
        if (mayBe(type, TString)) {
            if (!isSubtype(type & TCell, TString)) {
                LBasicBlock stringCase = m_out.newBlock();
                if (!mayMasquerade)
                    results.append(m_out.anchor(m_out.booleanTrue));
                m_out.branch(isCellOfType(value, StringType), unsure(stringCase), unsure(objectCase));
                m_out.appendTo(stringCase);
            }
            results.append(m_out.anchor(m_out.notEqual(value, emptyString())));
            m_out.jump(continuation);
        } else if (mayMasquerade)
            m_out.jump(objectCase);
        else {
            results.append(m_out.anchor(m_out.booleanTrue));
            m_out.jump(continuation);
        }
        if (mayMasquerade) {
            LBasicBlock masquerades = newColdBlock();
            m_out.appendTo(objectCase);
            results.append(m_out.anchor(m_out.booleanTrue));
            m_out.branch(m_out.testNonZero32(m_out.load8ZeroExt32(value, m_heaps.JSCell_typeInfoFlags), m_out.constInt32(MasqueradesAsUndefined)), rarely(masquerades), usually(continuation));
            m_out.appendTo(masquerades);
            results.append(m_out.anchor(m_out.notEqual(m_out.loadPtr(structureOf(value), m_heaps.Structure_realm), globalObjectHere())));
            m_out.jump(continuation);
        }
        m_out.appendTo(continuation);
        return m_out.phi(Int32, results);
    }
    if (isCompact()) {
        PatchpointValue* patchpoint = callStub(Stub::ToBoolean, Int32, { { value, firstStubOperandGPR } }, { }, StubClobbers::Temporaries);
        patchpoint->effects = Effects::none();
        patchpoint->effects.reads = HeapRange::top();
        patchpoint->effects.controlDependent = true;
        return patchpoint;
    }

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

LBasicBlock Lowering::edgeTo(BasicBlock* successor)
{
    for (auto& [to, edge] : m_edges) {
        if (to == successor)
            return edge;
    }
    return entryBlockFor(successor);
}

LBasicBlock Lowering::entryBlockFor(BasicBlock* successor)
{
    if (successor->loweredAhead && !successor->loopBody.get(m_block->index))
        return successor->loweredAhead;
    return successor->lowered;
}

const Lowering::ArrayView* Lowering::viewOf(Node* access, Node* base)
{
    if (!access->viewedAheadOf)
        return nullptr;
    UNUSED_PARAM(base);
    auto get = [&](B3::Variable* variable) -> LValue {
        return m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Get, m_out.origin(), variable);
    };
    for (auto& [header, array, variables] : m_arrayViews) {
        if (header != access->viewedAheadOf || array != access->arrayViewed)
            continue;
        m_graph.remark("array-view"_s);
        m_arrayViewHere = { get(variables.butterfly), get(variables.length), get(variables.limit) };
        return &m_arrayViewHere;
    }
    return nullptr;
}

void Lowering::loadArrayView(Node* origin, Node* base, const ArrayViewVariables& variables)
{
    auto set = [&](B3::Variable* variable, LValue value) {
        m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), variable, value);
    };
    LValue array = lowCell(base);
    LValue arrayMode = m_out.bitAnd(m_out.load8ZeroExt32(array, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingTypeMask));
    LValue butterfly = m_out.loadPtr(array, m_heaps.JSObject_butterfly);
    LValue length = lengthOfArray(origin, array);
    set(variables.butterfly, butterfly);
    set(variables.length, length);
    set(variables.limit, m_out.select(m_out.bitOr(m_out.equal(arrayMode, m_out.constInt32(ArrayWithInt32)), m_out.equal(arrayMode, m_out.constInt32(ArrayWithContiguous))), length, m_out.int64Zero));
}

void Lowering::reloadArrayViews()
{
    for (auto& [header, array, variables] : m_arrayViews) {
        if (!header->loopBody.get(m_block->index))
            continue;
        m_graph.remark("reloads-array-view"_s);
        loadArrayView(m_node, array, variables);
    }
}

void Lowering::hoistArrayStorageLoadsAheadOf(BasicBlock* header)
{
    m_out.appendTo(header->loweredAhead);
    auto firstAccessTo = [&](Node* base) -> Node* {
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (node->viewedAheadOf == header && node->arrayViewed == base)
                    return node;
            }
        }
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    };
    for (Node* base : header->arraysViewed) {
        ArrayViewVariables variables { m_proc.addVariable(pointerType()), m_proc.addVariable(Int64), m_proc.addVariable(Int64) };
        loadArrayView(firstAccessTo(base), base, variables);
        m_arrayViews.append({ header, base, variables });
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
    m_baseWithKnownStructure = nullptr;
    m_availableFields.shrink(0);
    if (block->predecessors.size() == 1 && !block->isCatchEntrypoint && block != m_graph.root) {
        if (auto available = m_availableFieldsAtEndOf.find(block->predecessors[0]); available != m_availableFieldsAtEndOf.end())
            m_availableFields = available->value;
    }
    m_out.setFrequency(block->isGeneric || block->isRarelyExecuted ? coldFrequency : 1);
    if (block->loweredAhead)
        hoistArrayStorageLoadsAheadOf(block);
    m_out.appendTo(block->lowered);
    for (Node* phi : block->phis)
        m_out.m_block->append(phi->lowered);
    findReadsAvailableAtHeadOf(block);
    if (m_dataOnEntry) {
        m_data = m_dataOnEntry;
        if (block->isInLoop && m_dataInLoops) {
            if (block->isLoopHeader) {
                m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), m_dataInLoops, ownData().data);
            }
            m_data = m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Get, m_out.origin(), m_dataInLoops);
        }
    }
    if (block->endsWithGuard)
        m_exit = newColdBlock();
    if (block == m_graph.root)
        lowerEntry();
    if (Options::aotTypeCoveragePath()) [[unlikely]] {
        if (auto inlined = m_graph.inlinedCalls.find(block); inlined != m_graph.inlinedCalls.end()) {
            coverOperation(inlined->value.call, inlined->value.call->block);
            m_graph.remark(inlined->value.what, inlined->value.name);
            if (Options::useAOTTypeCoverageCounters() && m_graph.isCoveringOperation())
                incrementTypeCoverageCounter(m_graph.coverage.last().counter + 1);
            m_graph.beginCoveredOperation(nullptr, block);
        }
    }

    Node* terminal = block->terminal();
    if (terminal && terminal->kind != NodeKind::Guard && !(terminal->kind == NodeKind::Bytecode && (isBranch(terminal->opcode) || isTerminal(terminal->opcode) || isThrow(terminal->opcode))))
        terminal = nullptr;

    m_newCells.shrink(0);
    auto setCurrentNode = [&](Node* node) {
        m_node = node && node->instruction ? node : nullptr;
        m_code = node ? node->graph : block->graph;
        if (Options::aotTypeCoveragePath()) [[unlikely]]
            coverOperation(node, block);
    };
    if (Options::validateAOTInferredTypes() && Options::validateAOTTypesOfPhis()) [[unlikely]] {
        setCurrentNode(nullptr);
        m_nodeIndex = 0;
        for (Node* phi : block->phis) {
            if (phi->useCount && phi->rep() == Rep::JSValue)
                verifyInferredType(phi, phi->lowered);
        }
    }
    for (m_nodeIndex = 0; m_nodeIndex < block->nodes.size(); ++m_nodeIndex) {
        Node* node = block->nodes[m_nodeIndex];
        if (node == terminal)
            break;
        if (node->isElided) {
            if (Options::aotTypeCoveragePath()) [[unlikely]]
                coverOperation(node, block, true);
            continue;
        }
        if (auto run = m_propertyRunOfStore.find(node); run != m_propertyRunOfStore.end()) {
            const PropertyRun& stores = m_propertyRuns[run->value];
            if (node != stores.last())
                continue;
            if (Options::aotTypeCoveragePath()) [[unlikely]] {
                for (unsigned i = 1; i < stores.size(); ++i) {
                    coverOperation(stores[i], block);
                    m_graph.remark("stored-in-property-run"_s);
                }
            }
            setCurrentNode(stores[0]);
            lowerPropertyRun(stores);
            m_baseWithKnownStructure = nullptr;
            m_availableFields.shrink(0);
            m_availableReads.shrink(0);
            m_newCells.shrink(0);
            continue;
        }
        setCurrentNode(node);
        if (Options::aotRemarksPath() || Options::aotTypeCoveragePath()) [[unlikely]] {
            if (auto kind = allocationKind(node); kind && node->escape != Escape::NotAnalyzed && !node->isPromoted && !Graph::makesNoFunctionObject(node)) {
                if (stays(node->escape))
                    m_graph.remark(node->escape == Escape::StaysHere ? "allocation-stays-here"_s : "allocation-is-only-borrowed"_s, nameOf(*kind));
                else
                    m_graph.remark("allocation-escapes"_s, makeString(nameOf(*kind), ": "_s, nameOf(node->escape)));
            }
        }
        m_nodePreservesFields = false;
        m_nodeKeepsKnownStructure = false;
        m_blockWhereNodeStarts = m_out.m_block;
        if (!m_newCells.isEmpty() && (mayCollectOrThrow(node) || Graph::makesNoFunctionObject(node)))
            m_newCells.shrink(0);
        lowerNode(node);
        if (m_graph.failed())
            return;
        if (Options::aotTypeCoveragePath() && m_out.m_block == m_blockWhereOperationStarts) [[unlikely]]
            m_graph.remark("emits-no-branch"_s);
        if (node->isBytecode(op_put_to_scope))
            m_newCells.removeAll(node->use(node->as<OpPutToScope>().m_value));
        else if (node->kind == NodeKind::SetStack)
            m_newCells.removeAll(node->uses[0].node);
        else if (node->isBytecode(op_create_lexical_environment) && !node->isPromoted)
            m_newCells.append(node);
        if (!m_nodePreservesFields && !m_availableFields.isEmpty() && !preservesFields(node))
            m_availableFields.shrink(0);
        if (!m_nodeKeepsKnownStructure && (node->kind == NodeKind::Bytecode || node->kind == NodeKind::Guard))
            m_baseWithKnownStructure = nullptr;
        forgetReadsChangedBy(node);
    }
    if (terminal && !preservesFields(terminal))
        m_availableFields.shrink(0);
    if (terminal)
        forgetReadsChangedBy(terminal);
    publishAvailableReads(block);
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
        lowerComparisonChain(block, m_chains[chain->value]);
        setCurrentNode(nullptr);
        return;
    }
    setCurrentNode(nullptr);
    if (block->branchFoldedByTypes) [[unlikely]]
        verifyBranchFoldedByTypes(block);
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
        didUseLinkTimeConstant(node->intrinsic);
        LValue constant = m_out.load64(m_instance, m_heaps.AOTInstance_linkTimeConstants[node->intrinsic]);
        static_cast<MemoryValue*>(constant)->setReadsMutability(B3::Mutability::Immutable);
        setJSValue(node, constant);
        return;
    }
    case NodeKind::Guard:
        emitGuard(node);
        return;
    case NodeKind::Narrow:
        if (Node* origin = node->fieldOrigin) {
            Node* valueNode = node->uses[0].node;
            TypeTable::FieldType fieldType = node->fieldType;
            LValue value = lowJSValue(valueNode);
            LBasicBlock otherwise = newColdBlock();
            LBasicBlock continuation = m_out.newBlock();
            bool mayBeRejected = branchUnlessAccepted(valueNode, value, fieldType, otherwise);
            LValue valueInField = toFieldRepresentation(valueNode, value, fieldType);
            if (!mayBeRejected) {
                m_out.jump(continuation);
                m_out.appendTo(otherwise);
                m_out.unreachable();
                m_out.appendTo(continuation);
                setJSValue(node, valueInField);
                return;
            }
            ValueFromBlock inField = m_out.anchor(valueInField);
            m_out.jump(continuation);
            m_out.appendTo(otherwise);
            uint64_t packedFieldType = static_cast<uint64_t>(fieldType.packedKinds()) | static_cast<uint64_t>(fieldType.first) << 16 | static_cast<uint64_t>(fieldType.last) << 32;
            m_graph.wideIntegerConstants.add(static_cast<int64_t>(packedFieldType));
            ValueFromBlock converted = m_out.anchor(vmCall(origin, Int64, Entry::operationAOTToFieldValue, m_instance, value, m_out.constInt64(packedFieldType), m_out.constInt32(numberOf(origin, node->fieldIdentifier))));
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            setJSValue(node, m_out.phi(Int64, inField, converted));
            return;
        }
        if (node->narrowedTo)
            setJSValue(node, lowJSValue(node->uses[0].node));
        return;
    case NodeKind::Argument:
        if (node->reg == VirtualRegister(CallFrameSlot::callee))
            setJSValue(node, callee());
        else if (!node->reg.toArgument())
            setJSValue(node, registerOnEntry(thisGPR));
        else if (m_graph.convention().signature == Signature::List) {
            unsigned index = node->reg.toArgument() - 1;
            bool isAlwaysPassed = !mayBe(node->type, TUndefined);
            if (isAlwaysPassed)
                m_graph.remark("argument-is-always-passed"_s, String::number(index + 1));
            setJSValue(node, isAlwaysPassed ? argumentPassed(index) : argumentPassedOrUndefined(index));
        } else {
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

#endif // ENABLE(AOT) && (CPU(ARM64) || CPU(X86_64))
