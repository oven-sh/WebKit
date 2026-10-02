/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(AOT) && CPU(ARM64)

#include "AOTCompiler.h"
#include "AOTProgram.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "BytecodeStructs.h"
#include "CCallHelpers.h"
#include "JSCInlines.h"
#include "UnlinkedFunctionCodeBlock.h"

namespace JSC { namespace AOT {

using namespace B3;

Lowering::Arguments Lowering::lowerArguments(Node* node, unsigned argc, unsigned argv)
{
    Arguments result;
    int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
    for (unsigned i = 0; i < argc; ++i)
        result.append(lowJSValue(node->use(VirtualRegister(firstArgument + i))));
    return result;
}

LValue Lowering::storeArgumentsToScratch(const Arguments& arguments)
{
    if (arguments.size() == 1)
        return m_out.intPtrZero;
    for (unsigned i = 1; i < arguments.size(); ++i)
        m_out.store64(arguments[i], scratchWord(i - 1));
    return m_scratch;
}

void Lowering::finishCall(PatchpointValue* patchpoint, CallMode mode, Rep result)
{
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    if (mode == CallMode::TailCall) {
        patchpoint->effects.terminal = true;
        return;
    }
    m_graph.emitsCalls = true;
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(RegisterSet { ARM64Registers::lr });
    patchpoint->resultConstraints = { result == Rep::Double ? ValueRep::reg(FPRInfo::returnValueFPR) : ValueRep::reg(GPRInfo::returnValueGPR) };
}

LValue Lowering::emitCall(Node* node, LValue callee, const Arguments& arguments, CallMode mode, StubIntrinsic intrinsic)
{
    unsigned count = arguments.size() - 1;
    bool inMemory = count > numberOfArgumentGPRs;
    RELEASE_ASSERT(mode != CallMode::TailCall || m_valueRepresentations.result == Rep::JSValue);
    if (inMemory && mode == CallMode::TailCall) {
        m_graph.emitsCalls = true;
        m_graph.alwaysEmitsCalls = true;
    }
    LValue list = inMemory ? storeArgumentsToScratch(arguments) : nullptr;
    LValue countInMemory = inMemory ? m_out.constIntPtr(count) : nullptr;

    PatchpointValue* patchpoint = m_out.patchpoint(mode == CallMode::TailCall ? Void : Int64);
    patchpoint->append(ConstrainedValue(callee, ValueRep::reg(calleeGPR)));
    patchpoint->append(ConstrainedValue(arguments[0], ValueRep::reg(thisGPR)));
    if (inMemory) {
        patchpoint->append(ConstrainedValue(countInMemory, ValueRep::reg(argumentGPR(0))));
        patchpoint->append(ConstrainedValue(list, ValueRep::reg(argumentGPR(1))));
    } else {
        for (unsigned i = 0; i < count; ++i)
            patchpoint->append(ConstrainedValue(arguments[i + 1], ValueRep::reg(argumentGPR(i))));
    }
    finishCall(patchpoint, mode);
    CallSite site { mode == CallMode::TailCall && !inMemory ? StubCall::noCallSite : callSiteBitsOf(node) };
    patchpoint->setGenerator([graph = &m_graph, count, inMemory, mode, intrinsic, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        StubCalls& stubCalls = graph->stubCalls;
        if (inMemory && mode == CallMode::TailCall) {
            emitRestoreBeforeLeaving(jit, *graph, params.code());
            stubCalls.call(jit, Stub::TailCallList, site);
            jit.breakpoint();
            return;
        }
        if (inMemory) {
            stubCalls.call(jit, mode == CallMode::Construct ? Stub::ConstructList : Stub::CallList, site);
            return;
        }
        Stub stub = intrinsic != StubIntrinsic::None ? Stub::CallIntrinsic : mode == CallMode::Construct ? Stub::Construct : Stub::Call;
        uint32_t which = intrinsic != StubIntrinsic::None ? static_cast<uint32_t>(intrinsic) : count;
        if (mode != CallMode::TailCall) {
            stubCalls.call(jit, stub, which, site);
            return;
        }
        emitEpilogueBeforeLeaving(jit, *graph, params.code());
        stubCalls.tailCall(jit, stub, which);
    });
    if (mode == CallMode::TailCall)
        return nullptr;
    return patchpoint;
}

bool Lowering::lowerCallToKnownFunction(Node* node, VirtualRegister calleeRegister, unsigned argv, const Arguments& arguments, CallMode mode, bool hasResult)
{
    bool isConstruct = mode == CallMode::Construct;
    bool isExact = false;
    const KnownFunction* known = m_graph.knownCallee(node, &isExact);
    if (!known || !isExact || !(isConstruct ? known->forConstruct : known->forCall))
        return false;
    if (known->summary && !known->summary->isReached()) {
        coldCall(node, Entry::operationAOTCheckType, m_out.constInt64(JSValue::encode(jsUndefined())), m_out.constInt32(MaskOtherObject));
        m_out.unreachable();
        m_out.appendTo(m_out.newBlock());
        if (hasResult || mode == CallMode::TailCall)
            setJSValue(node, m_out.constInt64(JSValue::encode(jsUndefined())));
        if (node->numberOfReturnValues) {
            for (Node* read : m_graph.outermost().returnValueReads.get(node)) {
                Rep rep = read->rep();
                read->lowered = rep == Rep::Double ? m_out.constDouble(0) : rep == Rep::Int32 || rep == Rep::Boolean ? m_out.int32Zero : rep == Rep::Int64 ? m_out.int64Zero : m_out.constInt64(JSValue::encode(jsUndefined()));
            }
        }
        return true;
    }
    m_graph.remark(isConstruct ? "direct-construct"_s : "direct-call"_s, known->executable ? known->executable->ecmaName().string() : String());
    Convention convention = isConstruct ? known->conventionForConstruct : known->conventionForCall;
    unsigned index = m_graph.knownCalleeIndex(known->keyFor(isConstruct));
    bool passesCallee = !m_graph.passesNoFunctionObject(node);
    bool takesList = convention.signature == Signature::List;
    if (takesList && node->isBytecode(op_tail_call))
        return false;
    ValueRepresentations how = isConstruct ? ValueRepresentations { } : valueRepresentations(known->summary, convention);
    if (how.result != m_valueRepresentations.result && mode == CallMode::TailCall)
        mode = CallMode::Call;

    bool calleeIsInitialized = known->isDeclaration || node->use(calleeRegister)->isReadOnlyForCall;
    LValue callee = passesCallee || !calleeIsInitialized ? lowJSValue(node->use(calleeRegister)) : nullptr;
    if (!calleeIsInitialized) {
        LBasicBlock isNotInitialized = newColdBlock();
        LBasicBlock isInitialized = m_out.newBlock();
        Type calleeType = node->use(calleeRegister)->type;
        if (isSubtype(calleeType, TFunction | TUndefined | TEmpty))
            m_out.branch(m_out.equal(callee, m_out.constInt64(JSValue::ValueUndefined)), rarely(isNotInitialized), usually(isInitialized));
        else {
            if (!isSubtype(calleeType, TCell)) {
                LBasicBlock isCellCase = m_out.newBlock();
                m_out.branch(isCell(callee), usually(isCellCase), rarely(isNotInitialized));
                m_out.appendTo(isCellCase);
            }
            m_out.branch(isCellOfType(callee, JSFunctionType), usually(isInitialized), rarely(isNotInitialized));
        }
        m_out.appendTo(isNotInitialized);
        vmCall(node, Void, isConstruct ? Entry::operationAOTThrowNotAConstructor : Entry::operationAOTThrowNotAFunction, m_instance, callee);
        m_out.unreachable();
        m_out.appendTo(isInitialized);
    }

    unsigned count = arguments.size() - 1;
    LValue list = takesList ? storeArgumentsToScratch(arguments) : nullptr;
    LValue countInMemory = takesList ? m_out.constIntPtr(count) : nullptr;
    LValue undefined = m_out.constInt64(JSValue::ValueUndefined);
    Vector<LValue, 8> passed;
    if (!takesList) {
        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        for (unsigned i = 0; i < convention.numberOfParameters; ++i) {
            if (how.parameters[i] == Rep::JSValue) {
                passed.append(i < count ? arguments[i + 1] : undefined);
                continue;
            }
            RELEASE_ASSERT(i < count);
            passed.append(lowAs(node->use(VirtualRegister(firstArgument + i + 1)), how.parameters[i]));
        }
    }

    Vector<Rep, 8> things;
    if (node->numberOfReturnValues) {
        RELEASE_ASSERT(mode == CallMode::Call);
        things = returnValueReps(known->summary, node->numberOfReturnValues);
    }
    auto typeFor = [](Rep rep) -> LType { return rep == Rep::JSValue ? Int64 : rep == Rep::Double ? Double : Int32; };
    LType resultType = mode == CallMode::TailCall ? Void : typeFor(how.result);
    if (things.size() == 1)
        resultType = typeFor(things[0]);
    else if (!things.isEmpty()) {
        Vector<B3::Type> types;
        for (Rep rep : things)
            types.append(typeFor(rep));
        resultType = m_proc.addTuple(WTF::move(types));
    }
    PatchpointValue* patchpoint = m_out.patchpoint(resultType);
    if (passesCallee)
        patchpoint->append(ConstrainedValue(callee, ValueRep::reg(calleeGPR)));
    if (convention.usesThis)
        patchpoint->append(ConstrainedValue(arguments[0], ValueRep::reg(thisGPR)));
    if (takesList) {
        patchpoint->append(ConstrainedValue(countInMemory, ValueRep::reg(argumentGPR(0))));
        patchpoint->append(ConstrainedValue(list, ValueRep::reg(argumentGPR(1))));
    } else {
        for (unsigned i = 0; i < convention.numberOfParameters; ++i)
            patchpoint->append(ConstrainedValue(passed[i], how.parameters[i] == Rep::Double ? ValueRep::reg(FPRInfo::toArgumentRegister(i)) : ValueRep::reg(argumentGPR(i))));
    }
    finishCall(patchpoint, mode, how.result);
    if (!things.isEmpty()) {
        patchpoint->resultConstraints.shrink(0);
        for (unsigned i = 0; i < things.size(); ++i)
            patchpoint->resultConstraints.append(things[i] == Rep::Double ? ValueRep::reg(FPRInfo::toArgumentRegister(i)) : ValueRep::reg(argumentGPR(i)));
    }
    CallSite site { mode == CallMode::TailCall ? StubCall::noCallSite : callSiteBitsOf(node) };
    patchpoint->setGenerator([graph = &m_graph, index, mode, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (mode != CallMode::TailCall) {
            graph->stubCalls.callFunction(jit, index, site);
            return;
        }
        emitEpilogueBeforeLeaving(jit, *graph, params.code());
        graph->stubCalls.jumpToFunction(jit, index);
    });
    if (mode == CallMode::TailCall) {
        m_out.appendTo(m_out.newBlock());
        setJSValue(node, m_out.int64Zero);
        return true;
    }
    if (!things.isEmpty()) {
        Vector<LValue, 8> handedBack;
        for (unsigned i = 0; i < things.size(); ++i)
            handedBack.append(things.size() == 1 ? static_cast<LValue>(patchpoint) : m_out.extract(patchpoint, i));
        for (Node* read : m_graph.outermost().returnValueReads.get(node))
            setResult(read, handedBack[read->returnValueIndex], things[read->returnValueIndex]);
        return true;
    }
    if (hasResult)
        setResult(node, patchpoint, how.result);
    return true;
}

void Lowering::lowerCall(Node* node, VirtualRegister calleeRegister, unsigned argc, unsigned argv, CallMode mode, bool hasResult)
{
    if (mode == CallMode::TailCall && !node->graph->isInTailPosition)
        mode = CallMode::Call;
    Arguments arguments = lowerArguments(node, argc, argv);
    if (lowerCallToKnownFunction(node, calleeRegister, argv, arguments, mode, hasResult))
        return;
    if (m_valueRepresentations.result != Rep::JSValue && mode == CallMode::TailCall)
        mode = CallMode::Call;

    Node* calleeNode = node->use(calleeRegister);
    LValue callee = lowJSValue(calleeNode);
    if (uint32_t classType = Graph::classRecordedBy(node)) {
        if (uint16_t layoutID = TypeTable::shared()->instanceLayoutIDFor(classType))
            vmCall(node, Void, Entry::operationAOTNoteClass, m_instance, arguments[0], arguments[1], m_out.constInt32(layoutID));
        return;
    }
    if (mode == CallMode::Call && argc == 1 && Graph::linkTimeConstantOf(calleeNode) == LinkTimeConstant::cloneObject) {
        LValue copy = vmCall(node, pointerType(), Entry::operationAOTCloneObject, m_instance, arguments[0], m_out.constInt32(Graph::newObjectLayoutID(node)));
        if (hasResult)
            setJSValue(node, copy);
        return;
    }
    if (mode == CallMode::Call && argc == 2 && Graph::linkTimeConstantOf(calleeNode) == LinkTimeConstant::toLength) {
        Node* argumentNode = node->use(VirtualRegister(-static_cast<int>(argv) + CallFrame::thisArgumentOffset() + 1));
        if (argumentNode->isInteger() && argumentNode->range.min >= 0) {
            if (hasResult)
                setResult(node, lowRaw(argumentNode), argumentNode->rep());
            return;
        }
        LBasicBlock isInteger = m_out.newBlock();
        LBasicBlock otherwise = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isInt32(arguments[1]), usually(isInteger), rarely(otherwise));
        m_out.appendTo(isInteger);
        LValue integer = unboxInt32(arguments[1]);
        ValueFromBlock quick = m_out.anchor(boxInt32(m_out.select(m_out.lessThan(integer, m_out.int32Zero), m_out.int32Zero, integer)));
        m_out.jump(continuation);
        m_out.appendTo(otherwise);
        ValueFromBlock called = m_out.anchor(emitCall(node, callee, arguments, mode, StubIntrinsic::None));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        if (hasResult)
            setJSValue(node, m_out.phi(Int64, quick, called));
        return;
    }
    LBasicBlock afterBuiltin = nullptr;
    Vector<ValueFromBlock, 2> builtinResults;
    if (node->builtinCalled && mode != CallMode::Construct && lowerBuiltinCall(node, calleeNode, argc, argv, arguments, hasResult, afterBuiltin, builtinResults)) {
        if (!afterBuiltin)
            return;
        mode = CallMode::Call;
    }
    StubIntrinsic intrinsic = StubIntrinsic::None;
    if (mode != CallMode::Construct && calleeNode->isBytecode(op_get_by_id))
        intrinsic = stubIntrinsicFor(calleeNode->graph->codeBlock()->identifier(calleeNode->as<OpGetById>().m_property).impl(), argc, hasResult);
    if (mode == CallMode::TailCall) {
        LBasicBlock otherwise = branchIfCalleeIsFunction(calleeNode, callee);
        if (emitCall(node, callee, arguments, mode, intrinsic))
            RELEASE_ASSERT_NOT_REACHED();
        m_out.appendTo(otherwise);
        mode = CallMode::Call;
    }
    LValue result = emitCall(node, callee, arguments, mode, intrinsic);
    if (afterBuiltin) {
        builtinResults.append(m_out.anchor(result));
        m_out.jump(afterBuiltin);
        m_out.appendTo(afterBuiltin);
        result = m_out.phi(Int64, builtinResults);
    }
    if (hasResult)
        setJSValue(node, result);
}

LBasicBlock Lowering::branchIfCalleeIsFunction(Node* calleeNode, LValue callee)
{
    LBasicBlock isFunction = m_out.newBlock();
    LBasicBlock otherwise = newColdBlock();
    if (!isSubtype(calleeNode->type, TCell)) {
        LBasicBlock isCellCase = m_out.newBlock();
        m_out.branch(isCell(callee), usually(isCellCase), rarely(otherwise));
        m_out.appendTo(isCellCase);
    }
    m_out.branch(isCellOfType(callee, JSFunctionType), usually(isFunction), rarely(otherwise));
    m_out.appendTo(isFunction);
    return otherwise;
}

void Lowering::lowerCallVarargs(Node* node, VirtualRegister calleeRegister, VirtualRegister thisRegister, VirtualRegister argumentsRegister, int firstVarArg, CallMode mode)
{
    if (mode == CallMode::TailCall && (m_valueRepresentations.result != Rep::JSValue || !node->graph->isInTailPosition))
        mode = CallMode::Call;
    LValue callee = lowJSValue(node->use(calleeRegister));
    LValue thisValue = lowJSValue(node->use(thisRegister));
    if (Node* listNode = Graph::argumentListFor(node); listNode && listNode->isElided) {
        lowerCallWithItems(node, node->use(calleeRegister), callee, thisValue, listNode, mode);
        return;
    }
    LValue list = lowJSValue(node->use(argumentsRegister));
    LBasicBlock otherwise = mode == CallMode::TailCall ? branchIfCalleeIsFunction(node->use(calleeRegister), callee) : nullptr;
    m_graph.emitsCalls = true;
    m_graph.alwaysEmitsCalls = true;

    for (;;) {
    PatchpointValue* patchpoint = m_out.patchpoint(mode == CallMode::TailCall ? Void : Int64);
    patchpoint->append(ConstrainedValue(callee, ValueRep::reg(calleeGPR)));
    patchpoint->append(ConstrainedValue(thisValue, ValueRep::reg(thisGPR)));
    patchpoint->append(ConstrainedValue(list, ValueRep::reg(argumentGPR(0))));
    finishCall(patchpoint, mode);
    CallSite site { callSiteBitsOf(node) };
    patchpoint->setGenerator([graph = &m_graph, firstVarArg, mode, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.move(CCallHelpers::TrustedImm32(ListDescriptor::forList(firstVarArg)), argumentGPR(1));
        if (mode != CallMode::TailCall) {
            graph->stubCalls.call(jit, mode == CallMode::Construct ? Stub::ConstructVarargs : Stub::CallVarargs, site);
            return;
        }
        emitRestoreBeforeLeaving(jit, *graph, params.code());
        graph->stubCalls.call(jit, Stub::TailCallVarargs, site);
        jit.breakpoint();
    });
    if (mode == CallMode::TailCall) {
        m_out.appendTo(otherwise);
        mode = CallMode::Call;
        continue;
    }
    setJSValue(node, patchpoint);
    return;
    }
}

void Lowering::lowerCallWithItems(Node* node, Node* calleeNode, LValue callee, LValue thisValue, Node* list, CallMode mode)
{
    struct Item {
        ListDescriptor::Kind kind;
        Node* node;
        Node* spread;
    };
    Vector<Item, 4> items;
    auto addSpreadOf = [&](Node* spread) {
        Node* source = spread->use(spread->as<OpSpread>().m_argument);
        items.append({ source->isElided ? ListDescriptor::Passed : ListDescriptor::Spread, source, spread });
    };
    if (list->isBytecode(op_create_cloned_arguments))
        items.append({ ListDescriptor::Passed, list, nullptr });
    else if (list->isBytecode(op_spread))
        addSpreadOf(list);
    else {
        auto bytecode = list->as<OpNewArrayWithSpread>();
        for (unsigned i = 0; i < bytecode.m_argc; ++i) {
            Node* element = list->use(VirtualRegister(bytecode.m_argv.offset() - static_cast<int>(i)));
            if (element->isElided)
                addSpreadOf(element);
            else
                items.append({ ListDescriptor::Value, element, nullptr });
        }
    }
    auto passed = [&](Node* item) -> std::pair<LValue, LValue> {
        unsigned skipped = item->isBytecode(op_create_rest) ? item->as<OpCreateRest>().m_numParametersToSkip : 0;
        LValue count = m_out.zeroExtPtr(numberOfArgumentsPassed());
        if (!skipped)
            return { count, argumentsPassed() };
        LValue left = m_out.sub(count, m_out.constIntPtr(skipped));
        return { m_out.select(m_out.above(count, m_out.constIntPtr(skipped)), left, m_out.intPtrZero), m_out.add(argumentsPassed(), m_out.constIntPtr(skipped * sizeof(EncodedJSValue))) };
    };

    bool forwardsAllArguments = items.size() == 1 && items[0].kind == ListDescriptor::Passed;
    m_graph.emitsCalls = true;
    m_graph.alwaysEmitsCalls = true;
    LValue first;
    LValue second = nullptr;
    uint32_t descriptor = ListDescriptor::forItems(items.size());
    if (forwardsAllArguments)
        std::tie(first, second) = passed(items[0].node);
    else {
        unsigned word = 0;
        for (unsigned i = 0; i < items.size(); ++i) {
            descriptor |= ListDescriptor::itemKind(i, items[i].kind);
            if (items[i].kind == ListDescriptor::Spread)
                m_graph.spreadSites.append({ siteOf(node), i, callSiteBitsOf(items[i].spread) });
            if (items[i].kind == ListDescriptor::Passed) {
                auto [count, where] = passed(items[i].node);
                m_out.store64(count, scratchWord(word++));
                m_out.store64(where, scratchWord(word++));
            } else
                m_out.store64(lowJSValue(items[i].node), scratchWord(word++));
        }
        first = m_scratch;
    }
    LBasicBlock otherwise = mode == CallMode::TailCall ? branchIfCalleeIsFunction(calleeNode, callee) : nullptr;

    for (;;) {
        PatchpointValue* patchpoint = m_out.patchpoint(mode == CallMode::TailCall ? Void : Int64);
        patchpoint->append(ConstrainedValue(callee, ValueRep::reg(calleeGPR)));
        patchpoint->append(ConstrainedValue(thisValue, ValueRep::reg(thisGPR)));
        patchpoint->append(ConstrainedValue(first, ValueRep::reg(argumentGPR(0))));
        if (second)
            patchpoint->append(ConstrainedValue(second, ValueRep::reg(argumentGPR(1))));
        finishCall(patchpoint, mode);
        CallSite site { mode == CallMode::TailCall && forwardsAllArguments ? StubCall::noCallSite : callSiteBitsOf(node) };
        patchpoint->setGenerator([graph = &m_graph, descriptor, forwardsAllArguments, mode, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            bool isConstruct = mode == CallMode::Construct;
            Stub stub = forwardsAllArguments ? (isConstruct ? Stub::ConstructList : Stub::CallList) : (isConstruct ? Stub::ConstructVarargs : Stub::CallVarargs);
            if (!forwardsAllArguments)
                jit.move(CCallHelpers::TrustedImm32(descriptor), argumentGPR(1));
            if (mode != CallMode::TailCall) {
                graph->stubCalls.call(jit, stub, site);
                return;
            }
            if (forwardsAllArguments) {
                emitEpilogueBeforeLeaving(jit, *graph, params.code());
                graph->stubCalls.tailCall(jit, stub);
                return;
            }
            emitRestoreBeforeLeaving(jit, *graph, params.code());
            graph->stubCalls.call(jit, Stub::TailCallVarargs, site);
            jit.breakpoint();
        });
        if (mode == CallMode::TailCall) {
            m_out.appendTo(otherwise);
            mode = CallMode::Call;
            continue;
        }
        setJSValue(node, patchpoint);
        return;
    }
}

void Lowering::lowerCallDirectEval(Node* node)
{
    auto bytecode = node->as<OpCallDirectEval>();
    LValue callee = lowJSValue(node->use(bytecode.m_callee));
    Arguments arguments = lowerArguments(node, bytecode.m_argc, bytecode.m_argv);
    LValue scope = lowCell(node->use(bytecode.m_scope));
    LValue thisValue = lowJSValue(node->use(bytecode.m_thisValue));

    LValue result = vmCall(node, Int64, Entry::operationAOTCallDirectEval, m_instance, callee, m_out.constInt32(arguments.size() - 1),
        arguments.size() > 1 ? arguments[1] : m_out.constInt64(JSValue::ValueUndefined), scope, thisValue, m_out.constInt32(node->bytecodeIndex.asBits()), m_out.constInt32(bytecode.m_lexicallyScopedFeatures));

    LBasicBlock notEval = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock evalResult = m_out.anchor(result);
    m_out.branch(m_out.isZero64(result), rarely(notEval), usually(continuation));

    m_out.appendTo(notEval, continuation);
    ValueFromBlock callResult = m_out.anchor(emitCall(node, callee, arguments));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, evalResult, callResult));
}

bool Lowering::tryLowerCall(Node* node)
{
    switch (node->opcode) {
    case op_call: {
        auto bytecode = node->as<OpCall>();
        lowerCall(node, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, CallMode::Call, true);
        return true;
    }
    case op_call_ignore_result: {
        auto bytecode = node->as<OpCallIgnoreResult>();
        lowerCall(node, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, CallMode::Call, false);
        return true;
    }
    case op_tail_call: {
        auto bytecode = node->as<OpTailCall>();
        lowerCall(node, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, CallMode::TailCall, true);
        return true;
    }
    case op_construct: {
        auto bytecode = node->as<OpConstruct>();
        lowerCall(node, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, CallMode::Construct, true);
        return true;
    }
    case op_super_construct: {
        auto bytecode = node->as<OpSuperConstruct>();
        lowerCall(node, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, CallMode::Construct, true);
        return true;
    }
    case op_call_varargs: {
        auto bytecode = node->as<OpCallVarargs>();
        lowerCallVarargs(node, bytecode.m_callee, bytecode.m_thisValue, bytecode.m_arguments, bytecode.m_firstVarArg, CallMode::Call);
        return true;
    }
    case op_tail_call_varargs: {
        auto bytecode = node->as<OpTailCallVarargs>();
        lowerCallVarargs(node, bytecode.m_callee, bytecode.m_thisValue, bytecode.m_arguments, bytecode.m_firstVarArg, CallMode::TailCall);
        return true;
    }
    case op_construct_varargs: {
        auto bytecode = node->as<OpConstructVarargs>();
        lowerCallVarargs(node, bytecode.m_callee, bytecode.m_thisValue, bytecode.m_arguments, bytecode.m_firstVarArg, CallMode::Construct);
        return true;
    }
    case op_super_construct_varargs: {
        auto bytecode = node->as<OpSuperConstructVarargs>();
        lowerCallVarargs(node, bytecode.m_callee, bytecode.m_thisValue, bytecode.m_arguments, bytecode.m_firstVarArg, CallMode::Construct);
        return true;
    }
    case op_call_direct_eval:
        lowerCallDirectEval(node);
        return true;
    default:
        return false;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && CPU(ARM64)
