/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(FTL_JIT)

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

// See AOTConvention.h. Whatever is called, the arguments are in registers when the call is made, or in memory that is this function's
// own; nothing is made on the stack for the callee, and the stack pointer is where it was when the call comes back.
// A patchpoint goes into the block when it is made: whatever it takes has to have been made before.

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
    RELEASE_ASSERT(mode != CallMode::TailCall || (!inMemory && m_howValuesArePassed.result == Rep::JSValue));
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
    CallSite site { mode == CallMode::TailCall ? StubCall::noCallSite : callSiteBitsOf(node) };
    patchpoint->setGenerator([graph = &m_graph, count, inMemory, mode, intrinsic, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        StubCalls& stubCalls = graph->stubCalls;
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

// A call of what a variable is proven to hold (KnownFunction::isProven): to where the function's code is, which is known when the image
// is put together, with what the function has a use for and nothing else.
bool Lowering::lowerCallToKnownFunction(Node* node, VirtualRegister calleeRegister, unsigned argv, const Arguments& arguments, CallMode mode, bool hasResult)
{
    bool isConstruct = mode == CallMode::Construct;
    bool isProven = false;
    const KnownFunction* known = m_graph.knownCallee(node, &isProven);
    if (!known || !isProven || !(isConstruct ? known->forConstruct : known->forCall))
        return false;
    // Nothing gets here, going by the types; and nothing gets to the function from anywhere else, so there is no code for it. What does get here has been lied to.
    if (known->facts && !known->facts->isReached()) {
        coldCall(node, Entry::operationAOTCheckType, m_out.constInt64(JSValue::encode(jsUndefined())), m_out.constInt32(MaskOtherObject));
        m_out.unreachable();
        m_out.appendTo(m_out.newBlock());
        if (hasResult || mode == CallMode::TailCall)
            setJSValue(node, m_out.constInt64(JSValue::encode(jsUndefined())));
        if (node->numberOfThingsReturned) {
            // (Nothing gets here. Something of the right sort, for what comes next to be made of.)
            for (Node* read : m_graph.outermost().readsOfThingsReturned.get(node)) {
                Rep rep = read->rep();
                read->lowered = rep == Rep::Double ? m_out.constDouble(0) : rep == Rep::Int32 || rep == Rep::Boolean ? m_out.int32Zero : rep == Rep::Int64 ? m_out.int64Zero : m_out.constInt64(JSValue::encode(jsUndefined()));
            }
        }
        return true;
    }
    Convention convention = isConstruct ? known->conventionForConstruct : known->conventionForCall;
    unsigned index = m_graph.indexOfKnownCallee(known->keyFor(isConstruct));
    bool passesCallee = !m_graph.passesNoFunctionObject(node);
    bool takesList = convention.signature == Signature::List;
    HowValuesArePassed how = isConstruct ? HowValuesArePassed { } : howValuesArePassed(known->facts, convention);
    // (What the callee hands back goes to whoever called this function, as it is.)
    if ((takesList || how.result != m_howValuesArePassed.result) && mode == CallMode::TailCall)
        mode = CallMode::Call;

    // (A method that is closed is there from when its class is: there is nothing to see to but that there is an object to read it from, which the read does.)
    bool isSurelyThere = known->isDeclaration || node->use(calleeRegister)->isReadOnlyToBeCalled;
    LValue callee = passesCallee || !isSurelyThere ? lowJSValue(node->use(calleeRegister)) : nullptr;
    if (!isSurelyThere) {
        // What is in the variable until it is initialized is not a function. (If it is the hole, that has been seen to.)
        LBasicBlock isNotInitialized = newColdBlock();
        LBasicBlock isInitialized = m_out.newBlock();
        // (Or, if it is known by what it is and not by where it was read from: whatever else it may be.)
        Type typeOfCallee = node->use(calleeRegister)->type;
        if (!Options::aotFollowsFunctions() || isSubtype(typeOfCallee, TFunction | TUndefined | TEmpty))
            m_out.branch(m_out.equal(callee, m_out.constInt64(JSValue::ValueUndefined)), rarely(isNotInitialized), usually(isInitialized));
        else {
            if (!isSubtype(typeOfCallee, TCell)) {
                LBasicBlock isCellCase = m_out.newBlock();
                m_out.branch(isCell(callee), usually(isCellCase), rarely(isNotInitialized));
                m_out.appendTo(isCellCase);
            }
            m_out.branch(isCellOfType(callee, JSFunctionType), usually(isInitialized), rarely(isNotInitialized));
        }
        m_out.appendTo(isNotInitialized);
        vmCall(node, Void, isConstruct ? Entry::operationAOTThrowNotAConstructor : Entry::operationAOTThrowNotAFunction, m_globalObject, callee);
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
            RELEASE_ASSERT(i < count); // (Or it would have been passed undefined.)
            passed.append(lowAs(node->use(VirtualRegister(firstArgument + i + 1)), how.parameters[i]));
        }
    }

    // What it hands back, if that is several things (Node::numberOfThingsReturned).
    Vector<Rep, 8> things;
    if (node->numberOfThingsReturned) {
        RELEASE_ASSERT(mode == CallMode::Call);
        things = howThingsAreReturned(known->facts, node->numberOfThingsReturned);
    }
    auto typeFor = [](Rep rep) -> LType { return rep == Rep::JSValue ? Int64 : rep == Rep::Double ? Double : Int32; };
    LType typeOfResult = mode == CallMode::TailCall ? Void : typeFor(how.result);
    if (things.size() == 1)
        typeOfResult = typeFor(things[0]);
    else if (!things.isEmpty()) {
        Vector<B3::Type> types;
        for (Rep rep : things)
            types.append(typeFor(rep));
        typeOfResult = m_proc.addTuple(WTF::move(types));
    }
    PatchpointValue* patchpoint = m_out.patchpoint(typeOfResult);
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
        // What comes after this in the bytecode returns what the call returned, if it was made like any other. It is not got to.
        m_out.appendTo(m_out.newBlock());
        setJSValue(node, m_out.int64Zero);
        return true;
    }
    if (!things.isEmpty()) {
        Vector<LValue, 8> handedBack;
        for (unsigned i = 0; i < things.size(); ++i)
            handedBack.append(things.size() == 1 ? static_cast<LValue>(patchpoint) : m_out.extract(patchpoint, i));
        for (Node* read : m_graph.outermost().readsOfThingsReturned.get(node))
            setResult(read, handedBack[read->whichThing], things[read->whichThing]);
        return true;
    }
    if (hasResult)
        setResult(node, patchpoint, how.result);
    return true;
}

void Lowering::lowerCall(Node* node, VirtualRegister calleeRegister, unsigned argc, unsigned argv, CallMode mode, bool hasResult)
{
    if (mode == CallMode::TailCall && (argc - 1 > numberOfArgumentGPRs || !node->graph->isInTailPosition))
        mode = CallMode::Call;
    Arguments arguments = lowerArguments(node, argc, argv);
    if (lowerCallToKnownFunction(node, calleeRegister, argv, arguments, mode, hasResult))
        return;
    // (Whatever else is called hands back a boxed value.)
    if (m_howValuesArePassed.result != Rep::JSValue && mode == CallMode::TailCall)
        mode = CallMode::Call;

    Node* calleeNode = node->use(calleeRegister);
    LValue callee = lowJSValue(calleeNode);
    // A class that the types say something of has been defined.
    if (uint32_t classType = Graph::classNotedBy(node)) {
        if (uint16_t layoutID = TypeTable::shared()->layoutIDOfInstancesOf(classType))
            vmCall(node, Void, Entry::operationAOTNoteClass, m_globalObject, arguments[0], arguments[1], m_out.constInt32(layoutID));
        return;
    }
    // { ...x }
    if (mode == CallMode::Call && argc == 1 && Graph::linkTimeConstantOf(calleeNode) == LinkTimeConstant::cloneObject) {
        LValue copy = vmCall(node, pointerType(), Entry::operationAOTCloneObject, m_globalObject, arguments[0], m_out.constInt32(Graph::layoutIDOfNewObject(node)));
        if (hasResult)
            setJSValue(node, copy);
        return;
    }
    // @toLength() of an integer that is not negative is that integer, and of one that is, zero.
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
    // A function of the language, if what it is called on and with is what it takes: then there is nothing to find out, and no frame to make.
    LBasicBlock afterBuiltin = nullptr;
    Vector<ValueFromBlock, 2> resultsOfBuiltin;
    if (node->builtinCalled && mode != CallMode::Construct && lowerCallOfBuiltin(node, calleeNode, argc, argv, arguments, hasResult, afterBuiltin, resultsOfBuiltin)) {
        if (!afterBuiltin)
            return;
        // (What comes after a call in tail position returns what it returned.)
        mode = CallMode::Call;
    }
    // What the callee was found as says what it may well be.
    StubIntrinsic intrinsic = StubIntrinsic::None;
    if (mode != CallMode::Construct && calleeNode->isBytecode(op_get_by_id))
        intrinsic = stubIntrinsicFor(calleeNode->graph->codeBlock()->identifier(calleeNode->as<OpGetById>().m_property).impl(), argc, hasResult);
    if (mode == CallMode::TailCall) {
        LBasicBlock otherwise = leaveIfFunction(calleeNode, callee);
        if (emitCall(node, callee, arguments, mode, intrinsic)) {
            // (It turned out not to be one that can be made like that.)
            RELEASE_ASSERT_NOT_REACHED();
        }
        m_out.appendTo(otherwise);
        mode = CallMode::Call;
    }
    LValue result = emitCall(node, callee, arguments, mode, intrinsic);
    if (afterBuiltin) {
        resultsOfBuiltin.append(m_out.anchor(result));
        m_out.jump(afterBuiltin);
        m_out.appendTo(afterBuiltin);
        result = m_out.phi(Int64, resultsOfBuiltin);
    }
    if (hasResult)
        setJSValue(node, result);
}

// A call in tail position leaves nothing behind of the function that makes it. Whatever there is to say about a callee that cannot be
// called is said of the caller, so that call is made like any other: what comes next returns what it returns, if it returns.
// Goes on where the callee is a function, and hands back where it is not.
LBasicBlock Lowering::leaveIfFunction(Node* calleeNode, LValue callee)
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

// f(...list), f.apply(o, list): a stub asks how long the list is, makes room, has it copied there, and makes the call (Stub::CallVarargs).
void Lowering::lowerCallVarargs(Node* node, VirtualRegister calleeRegister, VirtualRegister thisRegister, VirtualRegister argumentsRegister, int firstVarArg, CallMode mode)
{
    if (mode == CallMode::TailCall && (m_howValuesArePassed.result != Rep::JSValue || !node->graph->isInTailPosition))
        mode = CallMode::Call;
    LValue callee = lowJSValue(node->use(calleeRegister));
    LValue thisValue = lowJSValue(node->use(thisRegister));
    if (Node* listNode = Graph::listOfArgumentsOf(node); listNode && listNode->isElided) {
        lowerCallWithItems(node, node->use(calleeRegister), callee, thisValue, listNode, mode);
        return;
    }
    LValue list = lowJSValue(node->use(argumentsRegister));
    LBasicBlock otherwise = mode == CallMode::TailCall ? leaveIfFunction(node->use(calleeRegister), callee) : nullptr;
    // (Stub::TailCallVarargs is called.)
    m_graph.emitsCalls = true;
    m_graph.emitsCallsWhateverIsLeft = true;

    for (;;) {
    PatchpointValue* patchpoint = m_out.patchpoint(mode == CallMode::TailCall ? Void : Int64);
    patchpoint->append(ConstrainedValue(callee, ValueRep::reg(calleeGPR)));
    patchpoint->append(ConstrainedValue(thisValue, ValueRep::reg(thisGPR)));
    patchpoint->append(ConstrainedValue(list, ValueRep::reg(argumentGPR(0))));
    finishCall(patchpoint, mode);
    CallSite site { callSiteBitsOf(node) };
    patchpoint->setGenerator([graph = &m_graph, firstVarArg, mode, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.move(CCallHelpers::TrustedImm32(ListDescriptor::ofList(firstVarArg)), argumentGPR(1));
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

// The list was not made (Graph::findListsOfArguments()).
void Lowering::lowerCallWithItems(Node* node, Node* calleeNode, LValue callee, LValue thisValue, Node* list, CallMode mode)
{
    struct Item {
        ListDescriptor::Kind kind;
        Node* node;
        Node* spread; // The op_spread.
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
    // How many of what this function was passed, and where they are.
    auto passed = [&](Node* item) -> std::pair<LValue, LValue> {
        unsigned skipped = item->isBytecode(op_create_rest) ? item->as<OpCreateRest>().m_numParametersToSkip : 0;
        LValue count = m_out.zeroExtPtr(numberOfArgumentsPassed());
        if (!skipped)
            return { count, argumentsPassed() };
        LValue left = m_out.sub(count, m_out.constIntPtr(skipped));
        return { m_out.select(m_out.above(count, m_out.constIntPtr(skipped)), left, m_out.intPtrZero), m_out.add(argumentsPassed(), m_out.constIntPtr(skipped * sizeof(EncodedJSValue))) };
    };

    bool isJustWhatWasPassed = items.size() == 1 && items[0].kind == ListDescriptor::Passed;
    m_graph.emitsCalls = true;
    m_graph.emitsCallsWhateverIsLeft = true;
    LValue first;
    LValue second = nullptr;
    uint32_t descriptor = ListDescriptor::ofItems(items.size());
    if (isJustWhatWasPassed)
        std::tie(first, second) = passed(items[0].node);
    else {
        unsigned word = 0;
        for (unsigned i = 0; i < items.size(); ++i) {
            descriptor |= ListDescriptor::kindOfItem(i, items[i].kind);
            if (items[i].kind == ListDescriptor::Spread)
                m_graph.sitesOfSpreads.append({ siteOf(node), i, callSiteBitsOf(items[i].spread) });
            if (items[i].kind == ListDescriptor::Passed) {
                auto [count, where] = passed(items[i].node);
                m_out.store64(count, scratchWord(word++));
                m_out.store64(where, scratchWord(word++));
            } else
                m_out.store64(lowJSValue(items[i].node), scratchWord(word++));
        }
        first = m_scratch;
    }
    // (Both ways on from here go by what has been put there.)
    LBasicBlock otherwise = mode == CallMode::TailCall ? leaveIfFunction(calleeNode, callee) : nullptr;

    for (;;) {
        PatchpointValue* patchpoint = m_out.patchpoint(mode == CallMode::TailCall ? Void : Int64);
        patchpoint->append(ConstrainedValue(callee, ValueRep::reg(calleeGPR)));
        patchpoint->append(ConstrainedValue(thisValue, ValueRep::reg(thisGPR)));
        patchpoint->append(ConstrainedValue(first, ValueRep::reg(argumentGPR(0))));
        if (second)
            patchpoint->append(ConstrainedValue(second, ValueRep::reg(argumentGPR(1))));
        finishCall(patchpoint, mode);
        CallSite site { mode == CallMode::TailCall && isJustWhatWasPassed ? StubCall::noCallSite : callSiteBitsOf(node) };
        patchpoint->setGenerator([graph = &m_graph, descriptor, isJustWhatWasPassed, mode, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            bool isConstruct = mode == CallMode::Construct;
            Stub stub = isJustWhatWasPassed ? (isConstruct ? Stub::ConstructList : Stub::CallList) : (isConstruct ? Stub::ConstructVarargs : Stub::CallVarargs);
            if (!isJustWhatWasPassed)
                jit.move(CCallHelpers::TrustedImm32(descriptor), argumentGPR(1));
            if (mode != CallMode::TailCall) {
                graph->stubCalls.call(jit, stub, site);
                return;
            }
            // (What this function was passed is not in its frame. The items are.)
            if (isJustWhatWasPassed) {
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

// A call whose callee is written "eval". If that is what it is, the code runs in the scope of the caller. If not, this is a call like any
// other.
void Lowering::lowerCallDirectEval(Node* node)
{
    auto bytecode = node->as<OpCallDirectEval>();
    LValue callee = lowJSValue(node->use(bytecode.m_callee));
    Arguments arguments = lowerArguments(node, bytecode.m_argc, bytecode.m_argv);
    LValue scope = lowCell(node->use(bytecode.m_scope));
    LValue thisValue = lowJSValue(node->use(bytecode.m_thisValue));

    // Empty: it is not eval after all.
    LValue result = vmCall(node, Int64, Entry::operationAOTCallDirectEval, m_globalObject, callee, m_out.constInt32(arguments.size() - 1),
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
        // What sets it apart from op_construct is what the other tiers learn from it.
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

#endif // ENABLE(FTL_JIT)
