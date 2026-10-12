/*
 * Copyright (C) 2013-2025 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "StackVisitor.h"

#include "AOTRuntime.h"

#include "ClonedArguments.h"
#include "DebuggerPrimitives.h"
#include "ExecutableBaseInlines.h"
#include "InlineCallFrame.h"
#include "JSCInlines.h"
#include "NativeCallee.h"
#include "RegisterAtOffsetList.h"
#include "WasmCallee.h"
#include "WasmIndexOrName.h"
#include "WebAssemblyFunction.h"
#include <wtf/text/MakeString.h>

namespace JSC {

static CallFrame* callerOf(CallFrame* callFrame, EntryFrame*& entryFrame, void*& returnPC)
{
    EntryFrame* calleeEntryFrame = entryFrame;
    returnPC = callFrame->rawReturnPC();
    CallFrame* caller = callFrame->callerFrame(entryFrame);
#if ENABLE(AOT)
    if (entryFrame != calleeEntryFrame && caller && AOT::hasCode())
        returnPC = AOT::returnAddressForFrame(caller, calleeEntryFrame);
#else
    UNUSED_VARIABLE(calleeEntryFrame);
#endif
    return caller;
}

#if ENABLE(AOT)
ALWAYS_INLINE AOT::ImageAddressInfo StackVisitor::classify(const void* address)
{
    if (address == m_classifiedAddress)
        return m_classification;
    m_classifiedAddress = address;
    // What was asked about before is a return address into a function.
    if (AOT::Instance* instance = m_onlyAOTInstance ? m_onlyAOTInstance : m_aotInstance; instance && address) {
        if (auto* asked = instance->findCachedReturnAddress(address))
            return m_classification = { AOT::ImageAddressInfo::Function, asked->function, 0 };
    }
    return m_classification = AOT::classifyAddress(address);
}

static NEVER_INLINE bool isFrameOfHostFunction(CallFrame* callFrame)
{
    JSValue callee = callFrame->guaranteedJSValueCallee();
    return callee.isCell() && callee.asCell()->type() != JSFunctionType && !isSlimClosureType(callee.asCell()->type()) && !callFrame->isZombieFrame();
}
#endif

ALWAYS_INLINE CallFrame* StackVisitor::skipStubFrames(CallFrame* callFrame, EntryFrame*& entryFrame, void*& returnPC, CallFrame*& adapter)
{
#if ENABLE(AOT)
    while (callFrame) {
        switch (classify(removeCodePtrTag(returnPC)).kind) {
        case AOT::ImageAddressInfo::Stub:
            if (m_classification.index == AOT::ImageAddressInfo::frameMayBeOfHostFunction && isFrameOfHostFunction(callFrame)) [[unlikely]] {
                returnPC = nullptr;
                return callFrame;
            }
            if (callFrame->callerFrameOrEntryFrame() == entryFrame) {
                returnPC = nullptr;
                return callFrame;
            }
            returnPC = callFrame->rawReturnPC();
            callFrame = callFrame->callerFrame();
            continue;
        case AOT::ImageAddressInfo::Adapter:
            adapter = callFrame;
            callFrame = callerOf(callFrame, entryFrame, returnPC);
            continue;
        case AOT::ImageAddressInfo::Function:
        case AOT::ImageAddressInfo::NotInImage:
            return callFrame;
        }
    }
#else
    UNUSED_PARAM(entryFrame);
    UNUSED_PARAM(returnPC);
    UNUSED_PARAM(adapter);
#endif
    return callFrame;
}

ALWAYS_INLINE void StackVisitor::findCaller(CallFrame* callFrame)
{
    EntryFrame* entryFrame = m_frame.m_entryFrame;
    void* returnPC = nullptr;
    CallFrame* adapter = nullptr;
    CallFrame* caller;
    if (m_frame.m_aotFunction) {
        returnPC = callFrame->rawReturnPC();
        caller = callFrame->callerFrame();
    } else
        caller = callerOf(callFrame, entryFrame, returnPC);
    m_frame.m_callerFrame = skipStubFrames(caller, entryFrame, returnPC, adapter);
    m_frame.m_callerReturnPC = returnPC;
    m_frame.m_callerEntryFrame = entryFrame;
    m_frame.m_callerIsEntryFrame = entryFrame != m_frame.m_entryFrame;
    m_frame.m_aotAdapterFrame = adapter;
}

#if ENABLE(AOT)
ALWAYS_INLINE bool StackVisitor::tryGotoCompiledCaller()
{
    using Asked = AOT::Instance::CachedAddressInfo;
    CallFrame* callFrame = m_frame.m_callerFrame;
    if (!m_frame.m_aotFunction || !m_aotInstance || !callFrame || callFrame == m_frame.m_callFrame)
        return false;
    void* returnPC = removeCodePtrTag(m_frame.m_callerReturnPC);
    const Asked* found = returnPC ? m_aotInstance->findCachedReturnAddress(returnPC) : nullptr;
    if (!found || found->site >= Asked::hasNoSite || m_aotInstance->callSiteOverrides)
        return false;
    auto& asked = *found;
    AOT::FunctionRef function { m_aotInstance, asked.function };
    auto& info = function.info();
    uint32_t site = asked.site;
    unsigned inlineFrame = 0;
    uint32_t typedFunction = info.isTyped ? info.typedFunction() : AOT::FunctionRef::none;
    if (info.hasInlineFrames) {
        if (!info.isTyped)
            return false;
        if (!m_wantsOnlyHandlers || !info.hasNoHandlers)
            inlineFrame = AOT::PackedSite::inlineFrame(site);
        site = AOT::PackedSite::bits(site);
        if (inlineFrame)
            typedFunction = m_inlineFramesRead.at(*info.function(), inlineFrame).function;
    }
    m_frame.m_entryFrame = m_frame.m_callerEntryFrame;
    m_frame.m_callFrame = callFrame;
    m_previousReturnPC = returnPC;
    m_frame.m_returnPC = returnPC;
    m_frame.m_frameAOTFunction = function;
    m_frame.m_aotFunction = function;
    m_frame.m_aotInlineFrame = inlineFrame;
    m_frame.m_typedAOTFunction = typedFunction;
    m_frame.m_bytecodeIndex = CallSiteIndex(site).bytecodeIndex();
    m_frame.m_codeBlock = function.codeBlockIfExists();
    findCaller(callFrame);
    if (m_frame.m_aotAdapterFrame)
        m_aotInstance = nullptr;
    return true;
}

static ALWAYS_INLINE bool entryIsOfConstruction(AOT::Instance* instance, CallFrame* frameUnderEntryFrame, EntryFrame* entryFrame)
{
    return AOT::isEnteredByConstruction(instance, vmEntryRecord(entryFrame)->prevTopCallFrame()) && frameUnderEntryFrame && frameUnderEntryFrame->callerFrameOrEntryFrame() == entryFrame;
}

NEVER_INLINE bool StackVisitor::isEnteredByConstruction()
{
    return entryIsOfConstruction(m_frame.m_aotFunction.instance, m_frame.m_aotAdapterFrame ? m_frame.m_aotAdapterFrame : m_frame.m_callerFrame, m_frame.m_entryFrame);
}

bool StackVisitor::isAtConstructedCompiledFrame()
{
    if (!m_frame.isTypedAOTFrame() || !m_frame.m_aotFunction.instance)
        return false;
    if (m_frame.m_aotInlineFrame) [[unlikely]]
        return isAtConstructedInlineFrame();
    return isCalledByConstruction();
}

NEVER_INLINE bool StackVisitor::isAtConstructedInlineFrame()
{
    auto& info = m_frame.m_aotFunction.info();
    for (unsigned frame = m_frame.m_aotInlineFrame; frame;) {
        auto& inlineFrame = m_inlineFramesRead.at(*info.function(), frame);
        if (inlineFrame.isConstruction())
            return true;
        frame = inlineFrame.parent;
        if (!AOT::typedFunctionWraps(frame ? m_inlineFramesRead.at(*info.function(), frame).function : info.typedFunction(), m_frame.m_typedAOTFunction))
            return false;
    }
    return isCalledByConstruction();
}

ALWAYS_INLINE bool StackVisitor::isCalledByConstruction()
{
    using Place = AOT::Instance::PlaceInTrace;
    AOT::Instance* instance = m_frame.m_aotFunction.instance;
    void* returnPC = removeCodePtrTag(m_frame.m_callerReturnPC);
    if (m_frame.m_aotAdapterFrame || !returnPC)
        return isEnteredByConstruction();
    auto* found = instance->findCachedReturnAddress(returnPC);
    if (!found) [[unlikely]]
        found = instance->resolveReturnAddress(returnPC);
    if (!found)
        return false;
    size_t index = found - instance->cachedReturnAddresses;
    const Place& place = instance->placesInTraces()[index];
    if (place.kind == Place::IsShown)
        return false;
    if (place.kind == Place::IsHidden) {
        auto* above = instance->findCachedReturnAddress(removeCodePtrTag(m_frame.m_callerFrame->rawReturnPC()));
        if (above && instance->placesInTraces()[above - instance->cachedReturnAddresses].kind == Place::IsShown)
            return false;
    } else if (instance->placesInUnwinding()[index].siteIsInShownFunction && !place.calleeIsConstructed())
        return false;
    return isCalledByConstructionOrConstructedWrapper();
}

NEVER_INLINE bool StackVisitor::isCalledByConstructionOrConstructedWrapper()
{
    using Place = AOT::Instance::PlaceInTrace;
    AOT::Instance* instance = m_frame.m_aotFunction.instance;
    EntryFrame* entryFrame = m_frame.m_callerEntryFrame;
    CallFrame* callFrame = m_frame.m_callerFrame;
    void* returnPC = removeCodePtrTag(m_frame.m_callerReturnPC);
    while (callFrame && returnPC) {
        auto* found = instance->findCachedReturnAddress(returnPC);
        if (!found)
            break;
        size_t index = found - instance->cachedReturnAddresses;
        const Place& place = instance->placesInTraces()[index];
        if (place.kind == Place::IsShown || (instance->placesInUnwinding()[index].siteIsInShownFunction && !place.calleeIsConstructed()))
            return false;
        if (place.kind != Place::IsHidden)
            break;
        CallFrame* adapter = nullptr;
        returnPC = callFrame->rawReturnPC();
        callFrame = skipStubFrames(callFrame->callerFrame(), entryFrame, returnPC, adapter);
        returnPC = removeCodePtrTag(returnPC);
        if (adapter)
            break;
    }
    entryFrame = m_frame.m_callerEntryFrame;
    callFrame = m_frame.m_callerFrame;
    returnPC = removeCodePtrTag(m_frame.m_callerReturnPC);
    while (callFrame && returnPC) {
        auto* found = instance->resolveReturnAddress(returnPC);
        if (!found)
            return false;
        if (instance->placesInTraces()[found - instance->cachedReturnAddresses].calleeIsConstructed())
            return true;
        AOT::FunctionRef function { instance, found->function };
        if (!function.info().isTyped)
            return false;
        auto location = function.locationForReturnAddress(returnPC, callFrame, &m_inlineFramesRead);
        if (!AOT::typedFunctionIsHiddenInStackTraces(location.typedFunction)) {
            if (auto* again = instance->findCachedReturnAddress(returnPC))
                instance->placesInUnwinding()[again - instance->cachedReturnAddresses].siteIsInShownFunction = true;
            return false;
        }
        for (;; location = function.inlineCallSiteLocation(location.inlineFrame, m_inlineFramesRead)) {
            if (!AOT::typedFunctionWraps(location.typedFunction, m_frame.m_typedAOTFunction))
                return false;
            if (!location.inlineFrame)
                break;
            if (location.isConstructed)
                return true;
        }
        CallFrame* adapter = nullptr;
        EntryFrame* entryFrameOfCallee = entryFrame;
        returnPC = callFrame->rawReturnPC();
        callFrame = skipStubFrames(callFrame->callerFrame(), entryFrame, returnPC, adapter);
        returnPC = removeCodePtrTag(returnPC);
        if (adapter)
            return entryIsOfConstruction(instance, adapter, entryFrameOfCallee);
    }
    return !returnPC && entryIsOfConstruction(instance, callFrame, entryFrame);
}

static NEVER_INLINE void markAsConstructed(AOT::FrameInTrace& lastShown, AOT::Instance* instance, const void* returnAddressInCallee)
{
    lastShown.setRootOfConstructed(AOT::FunctionRef { instance, AOT::classifyAddress(returnAddressInCallee).index }.info().typedFunction());
}

unsigned StackVisitor::gotoLastOfPlainCompiledCallers(std::span<AOT::FrameInTrace> shown)
{
    using Asked = AOT::Instance::CachedAddressInfo;
    using Place = AOT::Instance::PlaceInTrace;
    AOT::Instance* instance = m_aotInstance;
    CallFrame* callFrame = m_frame.m_callerFrame;
    if (!m_frame.m_aotFunction || m_frame.m_aotInlineFrame || !instance || instance->callSiteOverrides || callFrame == m_frame.m_callFrame)
        return 0;
    void* returnPC = removeCodePtrTag(m_frame.m_callerReturnPC);
    const Place* places = instance->placesInTraces();
    CallFrame* last = nullptr;
    void* lastReturnPC = nullptr;
    BytecodeIndex bytecodeIndexOfRoot;
    unsigned numberOfFrames = 0;
    unsigned numberShown = 0;
    while (callFrame && returnPC) {
        const Asked* found = instance->findCachedReturnAddress(returnPC);
        if (!found) [[unlikely]] {
            found = instance->resolveReturnAddress(returnPC);
            if (!found)
                break;
        }
        auto& asked = *found;
        const Place& place = places[found - instance->cachedReturnAddresses];
        uint32_t kind = place.kind;
        uint32_t callSiteBits = place.callSiteBits;
        if (kind == Place::IsNotPlain) [[unlikely]] {
            if (!(callSiteBits & Place::hasConstructedCallee))
                break;
            if (numberShown)
                markAsConstructed(shown[numberShown - 1], instance, lastReturnPC);
            kind = place.withoutConstructedCallee().kind;
            callSiteBits = place.withoutConstructedCallee().callSiteBits;
            if (kind == Place::IsNotPlain)
                break;
        }
        unsigned numberOfLogicalFrames = 1;
        if (kind == Place::IsInInlinedBody) {
            if (callSiteBits & Place::hasConstructedCallee && numberShown) [[unlikely]]
                markAsConstructed(shown[numberShown - 1], instance, lastReturnPC);
            AOT::FunctionRef function { instance, asked.function };
            unsigned numberShownBefore = numberShown;
            bool fits = true;
            numberOfLogicalFrames = 0;
            for (auto location = function.locationForReturnAddress(returnPC, callFrame, &m_inlineFramesRead);; location = function.inlineCallSiteLocation(location.inlineFrame, m_inlineFramesRead)) {
                ++numberOfLogicalFrames;
                if (!location.isTailDeleted && !AOT::typedFunctionIsHiddenInStackTraces(location.typedFunction)) {
                    if (numberShown == shown.size()) {
                        fits = false;
                        break;
                    }
                    shown[numberShown++] = AOT::FrameInTrace::create(asked.function, location.typedFunction, location.bytecodeIndex, location.isConstructed);
                } else if (location.isConstructed && numberShown) [[unlikely]]
                    shown[numberShown - 1].setRootOfConstructed(location.typedFunction);
                if (!location.inlineFrame) {
                    bytecodeIndexOfRoot = location.bytecodeIndex;
                    break;
                }
            }
            if (!fits) {
                numberShown = numberShownBefore;
                break;
            }
        } else if (kind == Place::IsShown) {
            if (numberShown == shown.size())
                break;
            auto& frame = shown[numberShown++];
            frame.function = asked.function;
            frame.typedFunction = place.typedFunction;
            frame.callSiteAndRootOfConstructed = CallSiteIndex(callSiteBits).bytecodeIndex().asBits();
        }
        numberOfFrames += numberOfLogicalFrames;
        last = callFrame;
        lastReturnPC = returnPC;
        returnPC = removeCodePtrTag(callFrame->rawReturnPC());
        callFrame = callFrame->callerFrame();
    }
    if (!last)
        return 0;
    const Asked* found = instance->findCachedReturnAddress(lastReturnPC);
    if (!found) [[unlikely]]
        found = instance->resolveReturnAddress(lastReturnPC);
    Place place = places[found - instance->cachedReturnAddresses].ofSite();
    AOT::FunctionRef function { instance, found->function };
    m_frame.m_index += numberOfFrames;
    m_frame.m_isTailDeleted = false;
    m_frame.m_entryFrame = m_frame.m_callerEntryFrame;
    m_frame.m_callFrame = last;
    m_previousReturnPC = lastReturnPC;
    m_frame.m_returnPC = m_previousReturnPC;
    m_frame.m_frameAOTFunction = function;
    m_frame.m_aotFunction = function;
    m_frame.m_typedAOTFunction = place.typedFunction;
    m_frame.m_bytecodeIndex = place.kind == Place::IsInInlinedBody ? bytecodeIndexOfRoot : CallSiteIndex(place.callSiteBits).bytecodeIndex();
    m_frame.m_codeBlock = function.codeBlockIfExists();
    findCaller(last);
    if (numberShown && removeCodePtrTag(m_frame.m_callerReturnPC) != returnPC && (m_frame.m_aotAdapterFrame ? isEnteredByConstruction() : isAtConstructedCompiledFrame())) [[unlikely]]
        shown[numberShown - 1].setRootOfConstructed(place.typedFunction);
    if (m_frame.m_aotAdapterFrame)
        m_aotInstance = nullptr;
    return numberShown;
}

bool StackVisitor::gotoLastOfCompiledCallersWithoutHandlers(CPURegister* calleeSaveRegistersBuffer, std::span<const int8_t> bufferSlotsByRegIndex)
{
    using Asked = AOT::Instance::CachedAddressInfo;
    using Place = AOT::Instance::PlaceInUnwinding;
    AOT::Instance* instance = m_aotInstance;
    CallFrame* callFrame = m_frame.m_callerFrame;
    if (!m_frame.m_aotFunction || m_frame.m_aotInlineFrame || !instance || callFrame == m_frame.m_callFrame)
        return false;
    void* returnPC = removeCodePtrTag(m_frame.m_callerReturnPC);
    const Place* places = instance->placesInUnwinding();
    CallFrame* last = nullptr;
    void* lastReturnPC = nullptr;
    uint32_t lastFunction = 0;
    unsigned numberOfFrames = 0;
    while (callFrame && returnPC) {
        const Asked* found = instance->findCachedReturnAddress(returnPC);
        if (!found) [[unlikely]] {
            found = instance->resolveReturnAddress(returnPC);
            if (!found)
                break;
        }
        const Place* place = &places[found - instance->cachedReturnAddresses];
        if (place->state != Place::IsLeftByWhateverIsThrown) [[unlikely]] {
            if (place->state == Place::IsNotKnown)
                place = &instance->findPlaceInUnwinding(*found);
            if (place->state != Place::IsLeftByWhateverIsThrown)
                break;
        }
        const CPURegister* slot = reinterpret_cast<const CPURegister*>(callFrame->registers()) - place->savedRegistersStart;
        for (uint64_t mask = AOT::ImageFunction::unpackRegisters(place->savedRegisters); mask; mask &= mask - 1, ++slot)
            calleeSaveRegistersBuffer[bufferSlotsByRegIndex[WTF::ctz(mask)]] = *slot;
        ++numberOfFrames;
        last = callFrame;
        lastReturnPC = returnPC;
        lastFunction = found->function;
        returnPC = removeCodePtrTag(callFrame->rawReturnPC());
        callFrame = callFrame->callerFrame();
    }
    if (!last)
        return false;
    AOT::FunctionRef function { instance, lastFunction };
    m_frame.m_index += numberOfFrames;
    m_frame.m_isTailDeleted = false;
    m_frame.m_entryFrame = m_frame.m_callerEntryFrame;
    m_frame.m_callFrame = last;
    m_previousReturnPC = lastReturnPC;
    m_frame.m_returnPC = lastReturnPC;
    m_frame.m_frameAOTFunction = function;
    m_frame.m_aotFunction = function;
    m_frame.m_typedAOTFunction = 0;
    m_frame.m_bytecodeIndex = BytecodeIndex();
    m_frame.m_codeBlock = nullptr;
    findCaller(last);
    if (m_frame.m_aotAdapterFrame)
        m_aotInstance = nullptr;
    return true;
}
#endif

StackVisitor::StackVisitor(CallFrame* startFrame, VM& vm, bool skipFirstFrame, bool wantsOnlyHandlers)
    : m_wantsOnlyHandlers(wantsOnlyHandlers)
{
    if (vm.m_aotInstances.size() == 1)
        m_onlyAOTInstance = vm.m_aotInstances[0];
    CallFrame* topFrame = nullptr;
    if (startFrame) {
        ASSERT(!vm.topCallFrame || static_cast<void*>(vm.topCallFrame) != vm.topEntryFrame);

        m_frame.m_entryFrame = vm.topEntryFrame;
        topFrame = vm.topCallFrame;
        if (topFrame) {
            m_previousReturnPC = vm.maybeReturnPC;
            bool isEngineFrame = true;
#if ENABLE(AOT)
            if (AOT::hasCode()) {
                void* returnPC = AOT::returnAddressForFrame(topFrame, __builtin_frame_address(0));
                if (returnPC && classify(returnPC).kind != AOT::ImageAddressInfo::NotInImage) {
                    topFrame = skipStubFrames(topFrame, m_frame.m_entryFrame, returnPC, m_aotAdapterSkippedAtTop);
                    m_topEntryFrameIsEmpty = m_frame.m_entryFrame != vm.topEntryFrame;
                    m_previousReturnPC = returnPC;
                    isEngineFrame = classify(removeCodePtrTag(returnPC)).kind == AOT::ImageAddressInfo::NotInImage;
                    if (startFrame == vm.topCallFrame)
                        startFrame = topFrame;
                }
            }
#endif
            if (topFrame && !isEngineFrame && skipFirstFrame) {
                CallFrame* first = topFrame;
                readFrame(first);
                gotoNextFrame();
                m_frame.m_index = 0;
                m_topEntryFrameIsEmpty = (m_frame.m_entryFrame != vm.topEntryFrame);
                if (startFrame == first)
                    startFrame = m_frame.callFrame();
                while (m_frame.callFrame() && m_frame.callFrame() != startFrame)
                    gotoNextFrame();
                return;
            }
            if (topFrame && isEngineFrame && (skipFirstFrame || topFrame->isZombieFrame())) {
                CallFrame* first = topFrame;
                CallFrame* adapter = nullptr;
                topFrame = callerOf(first, m_frame.m_entryFrame, m_previousReturnPC);
                topFrame = skipStubFrames(topFrame, m_frame.m_entryFrame, m_previousReturnPC, adapter);
                m_topEntryFrameIsEmpty = (m_frame.m_entryFrame != vm.topEntryFrame);
                if (startFrame == first)
                    startFrame = topFrame;
            }
        }
    }
    readFrame(topFrame);

    // Find the frame the caller wants to start unwinding from.
    while (m_frame.callFrame() && m_frame.callFrame() != startFrame)
        gotoNextFrame();
}

#if ENABLE(AOT)
StackVisitor::StackVisitor(VM& vm, CallFrame* compiledFrame, void* returnPC)
{
    if (vm.m_aotInstances.size() == 1)
        m_onlyAOTInstance = vm.m_aotInstances[0];
    m_frame.m_entryFrame = vm.topEntryFrame;
    m_previousReturnPC = returnPC;
    readFrame(compiledFrame);
    while (m_frame.m_aotInlineFrame)
        gotoNextFrame();
    gotoNextFrame();
}
#endif

void StackVisitor::gotoNextFrame()
{
    m_frame.m_index++;
#if ENABLE(AOT)
    if (m_frame.m_aotInlineFrame) {
        auto location = m_frame.m_frameAOTFunction.inlineCallSiteLocation(m_frame.m_aotInlineFrame, m_inlineFramesRead);
        m_frame.m_aotFunction = location.function;
        m_frame.m_aotInlineFrame = location.inlineFrame;
        m_frame.m_typedAOTFunction = location.typedFunction;
        m_frame.m_isTailDeleted = location.isTailDeleted;
        m_frame.m_bytecodeIndex = location.bytecodeIndex;
        m_frame.m_codeBlock = m_frame.m_aotFunction.codeBlockIfExists();
        return;
    }
    m_frame.m_isTailDeleted = false;
    if (tryGotoCompiledCaller())
        return;
#endif
#if ENABLE(DFG_JIT)
    if (m_frame.isInlinedDFGFrame()) {
        InlineCallFrame* inlineCallFrame = m_frame.inlineCallFrame();
        CodeOrigin* callerCodeOrigin = inlineCallFrame->getCallerSkippingTailCalls();
        if (!callerCodeOrigin) {
            while (inlineCallFrame) {
                readInlinedFrame(m_frame.callFrame(), &inlineCallFrame->directCaller);
                inlineCallFrame = m_frame.inlineCallFrame();
            }
            m_frame.m_entryFrame = m_frame.m_callerEntryFrame;
            readFrame(m_frame.callerFrame());
        } else
            readInlinedFrame(m_frame.callFrame(), callerCodeOrigin);
        return;
    }
#endif // ENABLE(DFG_JIT)
    m_frame.m_entryFrame = m_frame.m_callerEntryFrame;
    readFrame(m_frame.callerFrame());
}

void StackVisitor::unwindToMachineCodeBlockFrame()
{
#if ENABLE(DFG_JIT)
    if (m_frame.isInlinedDFGFrame()) {
        CodeOrigin codeOrigin = m_frame.inlineCallFrame()->directCaller;
        while (codeOrigin.inlineCallFrame())
            codeOrigin = codeOrigin.inlineCallFrame()->directCaller;
        readNonInlinedFrame(m_frame.callFrame(), &codeOrigin);
    }
#endif
}

inline CallFrame* StackVisitor::updatePreviousReturnPCIfNecessary(CallFrame* callFrame)
{
    if (m_frame.m_callFrame) {
        if (m_frame.m_callFrame != callFrame)
            m_previousReturnPC = m_frame.m_callerReturnPC;
    }
    return callFrame;
}

void StackVisitor::readFrame(CallFrame* callFrame)
{
    if (!callFrame) {
        m_frame.setToEnd();
        return;
    }

#if ENABLE(AOT)
    {
        void* returnPC = m_frame.m_callFrame && m_frame.m_callFrame != callFrame ? m_frame.m_callerReturnPC : m_previousReturnPC;
        if (AOT::ImageAddressInfo what = classify(removeCodePtrTag(returnPC)); what.kind == AOT::ImageAddressInfo::Function) {
            readAOTFrame(callFrame, removeCodePtrTag(returnPC), what.index);
            return;
        }
    }
#endif

    if (callFrame->isNativeCalleeFrame()) {
        readInlinableNativeCalleeFrame(callFrame);
        return;
    }

#if !ENABLE(DFG_JIT)
    readNonInlinedFrame(callFrame);

#else // !ENABLE(DFG_JIT)
    // If the frame doesn't have a code block, then it's not a DFG frame.
    // Hence, we're not at an inlined frame.
    CodeBlock* codeBlock = callFrame->codeBlock();
    if (!codeBlock) {
        readNonInlinedFrame(callFrame);
        return;
    }

#if ASSERT_ENABLED
    if (!codeBlock->inherits<CodeBlock>()) {
        dataLogLn("Invalid codeblock type: ", *(JSCell*)codeBlock);
        dataLogLn("Callee: ", RawPointer(callFrame->unsafeCallee().rawPtr()));
        ASSERT_NOT_REACHED();
        readNonInlinedFrame(callFrame);
        return;
    }
#endif

    // If the code block does not have any code origins, then there's no
    // inlining. Hence, we're not at an inlined frame.
    if (!codeBlock->hasCodeOrigins()) {
        readNonInlinedFrame(callFrame);
        return;
    }

    CallSiteIndex index = callFrame->callSiteIndex();
    ASSERT(codeBlock->canGetCodeOrigin(index));
    if (!codeBlock->canGetCodeOrigin(index)) {
        // See assertion above. In release builds, we try to protect ourselves
        // from crashing even though stack walking will be goofed up.
        m_frame.setToEnd();
        return;
    }

    CodeOrigin codeOrigin = codeBlock->codeOrigin(index);
    if (!codeOrigin.inlineCallFrame()) {
        readNonInlinedFrame(callFrame, &codeOrigin);
        return;
    }

    readInlinedFrame(callFrame, &codeOrigin);
#endif // !ENABLE(DFG_JIT)
}

#if ENABLE(AOT)
void StackVisitor::readAOTFrame(CallFrame* callFrame, void* returnPC, uint32_t index)
{
    bool calleeRuns = m_frame.m_callFrame && (m_frame.m_aotFunction || m_frame.m_isWasmFrame || (!m_frame.m_callee.isNativeCallee() && m_frame.m_callee.rawPtr() && m_frame.m_callee.asCell()->isCallable()));
    m_frame.m_callFrame = callFrame;
    m_previousReturnPC = returnPC;
    m_frame.m_returnPC = returnPC;
    m_frame.m_argumentCountIncludingThis = 0;
    m_frame.m_isWasmFrame = false;
    m_frame.m_callee = CalleeBits();
#if ENABLE(DFG_JIT)
    m_frame.m_inlineDFGCallFrame = nullptr;
#endif
    m_frame.m_wasmDistanceFromDeepestInlineFrame = 0;
    if (!m_aotInstance)
        m_aotInstance = m_onlyAOTInstance ? m_onlyAOTInstance : AOT::instanceForFrame(callFrame);
    m_frame.m_frameAOTFunction = { m_aotInstance, index };
    if (m_wantsOnlyHandlers && m_frame.m_frameAOTFunction.info().hasNoHandlers) {
        m_frame.m_aotFunction = m_frame.m_frameAOTFunction;
        m_frame.m_aotInlineFrame = 0;
        m_frame.m_typedAOTFunction = 0;
        m_frame.m_isTailDeleted = false;
        m_frame.m_bytecodeIndex = BytecodeIndex();
        m_frame.m_codeBlock = nullptr;
        findCaller(callFrame);
        if (m_frame.m_aotAdapterFrame)
            m_aotInstance = nullptr;
        return;
    }
    auto location = m_frame.m_frameAOTFunction.locationForReturnAddress(returnPC, callFrame, &m_inlineFramesRead);
    m_frame.m_aotFunction = location.function;
    m_frame.m_aotInlineFrame = location.inlineFrame;
    m_frame.m_typedAOTFunction = location.typedFunction;
    m_frame.m_isTailDeleted = location.isTailCall && calleeRuns;
    m_frame.m_bytecodeIndex = location.bytecodeIndex;
    m_frame.m_codeBlock = m_frame.m_aotFunction.codeBlockIfExists();
    findCaller(callFrame);
    if (m_frame.m_aotAdapterFrame)
        m_aotInstance = nullptr;
}
#endif

void StackVisitor::readNonInlinedFrame(CallFrame* callFrame, CodeOrigin* codeOrigin)
{
    m_frame.m_callFrame = updatePreviousReturnPCIfNecessary(callFrame);
    m_frame.m_returnPC = m_previousReturnPC;
    m_frame.m_argumentCountIncludingThis = callFrame->argumentCountIncludingThis();
    m_frame.m_aotFunction = { };
    m_frame.m_aotInlineFrame = 0;
    findCaller(callFrame);
    m_frame.m_isWasmFrame = false;
    m_frame.m_callee = callFrame->callee();
#if ENABLE(DFG_JIT)
    m_frame.m_inlineDFGCallFrame = nullptr;
#endif
    m_frame.m_wasmDistanceFromDeepestInlineFrame = 0;

    m_frame.m_codeBlock = callFrame->isNativeCalleeFrame() ? nullptr : callFrame->codeBlock();
    m_frame.m_bytecodeIndex = !m_frame.hasCode() ? BytecodeIndex(0)
        : codeOrigin ? codeOrigin->bytecodeIndex()
        : callFrame->bytecodeIndex();

    RELEASE_ASSERT(!callFrame->isNativeCalleeFrame());
}

void StackVisitor::readInlinableNativeCalleeFrame(CallFrame* callFrame)
{
    RELEASE_ASSERT(callFrame->callee().isNativeCallee());
    auto& callee = *callFrame->callee().asNativeCallee();
    switch (callee.category()) {
    case NativeCallee::Category::Wasm: {
#if ENABLE(WEBASSEMBLY)
        auto& wasmCallee = uncheckedDowncast<Wasm::Callee>(callee);
        auto depth = m_frame.m_wasmDistanceFromDeepestInlineFrame;
        m_frame.m_callFrame = updatePreviousReturnPCIfNecessary(callFrame);
        m_frame.m_returnPC = m_previousReturnPC;
        m_frame.m_isWasmFrame = true;
        m_frame.m_argumentCountIncludingThis = callFrame->argumentCountIncludingThis();
        m_frame.m_aotFunction = { };
        findCaller(callFrame);
        m_frame.m_callee = callFrame->callee();
        m_frame.m_codeBlock = nullptr;
        m_frame.m_wasmDistanceFromDeepestInlineFrame = 0;
        m_frame.m_wasmCallSiteIndexBits = callFrame->callSiteIndex().bits();

        m_frame.m_wasmFunctionIndexOrName = wasmCallee.indexOrName();
        m_frame.m_wasmFunctionIndex = wasmCallee.index();

#if ENABLE(WEBASSEMBLY_OMGJIT)
        bool canInline = isAnyOMG(wasmCallee.compilationMode());
        if (!canInline)
            return;

        const auto& omgCallee = uncheckedDowncast<const Wasm::OptimizingJITCallee>(wasmCallee);
        bool isInlined = false;

        // Because PC is just after the call instruction, to query to the origin for the call instruction, we decrease it by 1.
        // While it can be pointing at the broken offset (e.g. all ARM64 instructions are 4-byte aligned), it is still fine since map is controlling pc with range.
        auto callSiteIndexFromPC = omgCallee.tryGetCallSiteIndex(std::bit_cast<void*>(std::bit_cast<uintptr_t>(removeCodePtrTag<void*>(m_frame.m_returnPC)) - 1));
        RELEASE_ASSERT(callSiteIndexFromPC);
        CallSiteIndex callSiteIndex = callSiteIndexFromPC.value();
        m_frame.m_wasmCallSiteIndexBits = callSiteIndex.bits();

        auto codeOrigin = omgCallee.getCodeOrigin(callSiteIndex.bits(), depth, isInlined);
        auto indexOrName = omgCallee.getIndexOrName(codeOrigin);
        if (!isInlined)
            return;

        // The callerFrame just needs to be non-null to indicate that we
        // haven't reached the last frame yet.
        m_frame.m_callerFrame = callFrame;
        m_frame.m_wasmDistanceFromDeepestInlineFrame = depth + 1;
        m_frame.m_wasmFunctionIndexOrName = indexOrName;
        m_frame.m_wasmFunctionIndex = codeOrigin->functionIndex;
#else
        UNUSED_VARIABLE(depth);
#endif
#else
        UNUSED_PARAM(callFrame);
#endif
        break;
    }
    case NativeCallee::Category::InlineCache: {
        m_frame.m_callFrame = updatePreviousReturnPCIfNecessary(callFrame);
        m_frame.m_returnPC = m_previousReturnPC;
        m_frame.m_argumentCountIncludingThis = callFrame->argumentCountIncludingThis();
        m_frame.m_aotFunction = { };
        findCaller(callFrame);
        m_frame.m_isWasmFrame = false;
        m_frame.m_callee = callFrame->callee();
#if ENABLE(DFG_JIT)
        m_frame.m_inlineDFGCallFrame = nullptr;
#endif
        m_frame.m_wasmDistanceFromDeepestInlineFrame = 0;

        m_frame.m_codeBlock = nullptr;
        m_frame.m_bytecodeIndex = BytecodeIndex(0);
        break;
    }
    }
}

#if ENABLE(DFG_JIT)
static int NODELETE inlinedFrameOffset(CodeOrigin* codeOrigin)
{
    InlineCallFrame* inlineCallFrame = codeOrigin->inlineCallFrame();
    int frameOffset = inlineCallFrame ? inlineCallFrame->stackOffset : 0;
    return frameOffset;
}

void StackVisitor::readInlinedFrame(CallFrame* callFrame, CodeOrigin* codeOrigin)
{
    ASSERT(codeOrigin);
    m_frame.m_isWasmFrame = false;
    m_frame.m_wasmDistanceFromDeepestInlineFrame = 0;

    int frameOffset = inlinedFrameOffset(codeOrigin);
    bool isInlined = !!frameOffset;
    if (isInlined) {
        InlineCallFrame* inlineCallFrame = codeOrigin->inlineCallFrame();

        m_frame.m_callFrame = updatePreviousReturnPCIfNecessary(callFrame);
        m_frame.m_returnPC = m_previousReturnPC;
        m_frame.m_inlineDFGCallFrame = inlineCallFrame;
        if (inlineCallFrame->argumentCountRegister.isValid())
            m_frame.m_argumentCountIncludingThis = callFrame->r(inlineCallFrame->argumentCountRegister).unboxedInt32();
        else
            m_frame.m_argumentCountIncludingThis = inlineCallFrame->argumentCountIncludingThis;
        m_frame.m_codeBlock = inlineCallFrame->baselineCodeBlock.get();
        m_frame.m_aotFunction = { };
        m_frame.m_bytecodeIndex = codeOrigin->bytecodeIndex();

        JSFunction* callee = inlineCallFrame->calleeForCallFrame(callFrame);
        m_frame.m_callee = callee;
        ASSERT(!!m_frame.callee().rawPtr());

        // The callerFrame just needs to be non-null to indicate that we
        // haven't reached the last frame yet. Setting it to the root
        // frame (i.e. the callFrame that this inlined frame is called from)
        // would work just fine.
        m_frame.m_callerFrame = callFrame;
        return;
    }

    readNonInlinedFrame(callFrame, codeOrigin);
}
#endif // ENABLE(DFG_JIT)

CodeBlock* StackVisitor::Frame::makeCodeBlock() const
{
#if ENABLE(AOT)
    if (isTypedAOTFrame())
        return nullptr;
    m_codeBlock = m_aotFunction.ensureData()->ensureCodeBlock();
#endif
    return m_codeBlock;
}

bool StackVisitor::Frame::isHiddenAOTFrame(bool isAtTheTop) const
{
#if ENABLE(AOT)
    if (isTypedAOTFrame())
        return isAtTheTop ? AOT::typedFunctionStandsForNoFunction(m_typedAOTFunction) : AOT::typedFunctionIsHiddenInStackTraces(m_typedAOTFunction);
    if (m_aotFunction)
        return !isAtTheTop && m_aotFunction.info().isHiddenInStackTraces;
#else
    UNUSED_PARAM(isAtTheTop);
#endif
    return false;
}

ScriptExecutable* StackVisitor::Frame::ownerExecutable() const
{
#if ENABLE(AOT)
    if (m_aotFunction)
        return m_aotFunction.executable();
#endif
    return m_codeBlock ? m_codeBlock->ownerExecutable() : nullptr;
}

JSCell* StackVisitor::Frame::aotCallee() const
{
#if ENABLE(AOT)
    if (m_aotFunction && !m_aotInlineFrame)
        return m_frameAOTFunction.calleeInFrame(m_callFrame);
#endif
    return nullptr;
}

bool StackVisitor::Frame::isBuiltinFunction() const
{
#if ENABLE(AOT)
    if (m_aotFunction)
        return m_aotFunction.isBuiltinFunction();
#endif
    return m_codeBlock->unlinkedCodeBlock()->isBuiltinFunction();
}

JSGlobalObject* StackVisitor::Frame::lexicalGlobalObject(VM& vm) const
{
#if ENABLE(AOT)
    if (m_aotFunction)
        return m_aotFunction.instance->globalObject;
#endif
    return m_callFrame->lexicalGlobalObject(vm);
}

StackVisitor::Frame::CodeType StackVisitor::Frame::codeType() const
{
    if (isNativeCalleeFrame()) {
        auto* nativeCallee = callee().asNativeCallee();
        switch (nativeCallee->category()) {
        case NativeCallee::Category::Wasm:
            return CodeType::Wasm;
        case NativeCallee::Category::InlineCache:
            return CodeType::Native;
        }
        return CodeType::Native;
    }

    if (!hasCode())
        return CodeType::Native;

#if ENABLE(AOT)
    JSC::CodeType type = isTypedAOTFrame() ? AOT::typedFunctionCodeType(m_typedAOTFunction) : m_aotFunction ? m_aotFunction.codeType() : m_codeBlock->codeType();
#else
    JSC::CodeType type = m_codeBlock->codeType();
#endif
    switch (type) {
    case EvalCode:
        return CodeType::Eval;
    case ModuleCode:
        return CodeType::Module;
    case FunctionCode:
        return CodeType::Function;
    case GlobalCode:
        return CodeType::Global;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return CodeType::Global;
}

#if ENABLE(ASSEMBLER)
#if ENABLE(AOT)
const AOT::ImageFunction* StackVisitor::Frame::compiledFunctionForUnwinding() const
{
    return m_aotFunction ? m_frameAOTFunction.info().function() : nullptr;
}
#endif

const RegisterAtOffsetList* StackVisitor::Frame::calleeSaveRegistersForUnwinding()
{
    if (!NUMBER_OF_CALLEE_SAVES_REGISTERS)
        return nullptr;

    if (isInlinedDFGFrame())
        return nullptr;

    if (isNativeCalleeFrame()) {
        auto* nativeCallee = callee().asNativeCallee();
        switch (nativeCallee->category()) {
        case NativeCallee::Category::Wasm: {
#if ENABLE(WEBASSEMBLY)
            auto* wasmCallee = uncheckedDowncast<Wasm::Callee>(nativeCallee);
            if (auto* calleeSaveRegisters = wasmCallee->calleeSaveRegisters())
                return calleeSaveRegisters;
#endif // ENABLE(WEBASSEMBLY)
            break;
        }
        case NativeCallee::Category::InlineCache: {
            break;
        }
        }
        return nullptr;
    }

#if ENABLE(AOT)
    if (m_aotFunction)
        return AOT::calleeSaveRegistersOf(*m_frameAOTFunction.info().function());
#endif
    if (CodeBlock* codeBlock = this->codeBlock())
        return codeBlock->jitCode()->calleeSaveRegisters();

    return nullptr;
}
#endif // ENABLE(ASSEMBLER)

String StackVisitor::Frame::functionName() const
{
    String traceLine;

    switch (codeType()) {
    case CodeType::Wasm:
        traceLine = makeString(m_wasmFunctionIndexOrName);
        break;
    case CodeType::Eval:
        traceLine = "eval code"_s;
        break;
    case CodeType::Module:
        traceLine = "module code"_s;
        break;
    case CodeType::Native: {
        JSCell* callee = this->callee().asCell();
        if (callee)
            traceLine = getCalculatedDisplayName(callFrame()->deprecatedVM(), uncheckedDowncast<JSObject>(callee)).impl();
        break;
    }
    case CodeType::Function:
#if ENABLE(AOT)
        if (isTypedAOTFrame()) {
            traceLine = AOT::typedFunctionName(m_typedAOTFunction);
            break;
        }
#endif
        if (JSCell* callee = this->callee().asCell())
            traceLine = getCalculatedDisplayName(callFrame()->deprecatedVM(), uncheckedDowncast<JSObject>(callee)).impl();
        else if (auto* executable = dynamicDowncast<FunctionExecutable>(ownerExecutable()))
            traceLine = executable->ecmaNameWithoutGC();
        break;
    case CodeType::Global:
        traceLine = "global code"_s;
        break;
    }
    return traceLine.isNull() ? emptyString() : traceLine;
}

String StackVisitor::Frame::sourceURL() const
{
    String traceLine;

    switch (codeType()) {
    case CodeType::Eval:
    case CodeType::Module:
    case CodeType::Function:
    case CodeType::Global: {
#if ENABLE(AOT)
        if (isTypedAOTFrame()) {
            if (auto position = m_aotFunction.reportedPositionFor(bytecodeIndex()); position && position->source)
                traceLine = AOT::ProgramData::get()->sourceName(position->source);
            break;
        }
#endif
        String sourceURL = ownerExecutable()->sourceURL();
        if (!sourceURL.isEmpty())
            traceLine = sourceURL.impl();
        break;
    }
    case CodeType::Native:
        traceLine = "[native code]"_s;
        break;
    case CodeType::Wasm:
        traceLine = "[wasm code]"_s;
        break;
    }
    return traceLine.isNull() ? emptyString() : traceLine;
}

String StackVisitor::Frame::preRedirectURL() const
{
    String traceLine;

    switch (codeType()) {
    case CodeType::Eval:
    case CodeType::Module:
    case CodeType::Function:
    case CodeType::Global: {
        if (isTypedAOTFrame())
            break;
        String preRedirectURL = ownerExecutable()->preRedirectURL();
        if (!preRedirectURL.isEmpty())
            traceLine = preRedirectURL.impl();
        break;
    }
    case CodeType::Native:
    case CodeType::Wasm:
        break;
    }

    return traceLine.isNull() ? emptyString() : traceLine;
}

String StackVisitor::Frame::toString() const
{
    String functionName = this->functionName();
    String sourceURL = this->sourceURL();
    auto separator = !sourceURL.isEmpty() && !functionName.isEmpty() ? "@"_s : ""_s;

    if (sourceURL.isEmpty() || !hasLineAndColumnInfo())
        return makeString(functionName, separator, sourceURL);

    auto lineColumn = computeLineAndColumn();
    return makeString(functionName, separator, sourceURL, ':', lineColumn.line, ':', lineColumn.column);
}

SourceID StackVisitor::Frame::sourceID()
{
    if (ScriptExecutable* executable = ownerExecutable())
        return executable->sourceID();
    return noSourceID;
}

ClonedArguments* StackVisitor::Frame::createArguments(VM& vm)
{
    ASSERT(m_callFrame);
    CallFrame* physicalFrame = m_callFrame;
    // FIXME: Revisit JSGlobalObject.
    // https://bugs.webkit.org/show_bug.cgi?id=203204
    JSGlobalObject* globalObject = physicalFrame->lexicalGlobalObject(vm);
    ClonedArguments* arguments;
    ArgumentsMode mode;
    if (Options::useFunctionDotArguments())
        mode = ArgumentsMode::Cloned;
    else
        mode = ArgumentsMode::FakeValues;
#if ENABLE(DFG_JIT)
    if (isInlinedDFGFrame()) {
        ASSERT(m_inlineDFGCallFrame);
        arguments = ClonedArguments::createWithInlineFrame(globalObject, physicalFrame, m_inlineDFGCallFrame, mode);
    } else 
#endif
        arguments = ClonedArguments::createWithMachineFrame(globalObject, physicalFrame, mode);
    return arguments;
}

bool StackVisitor::Frame::hasLineAndColumnInfo() const
{
    return hasCode();
}

LineColumn StackVisitor::Frame::computeLineAndColumn() const
{
    if (!hasCode())
        return { };

    ScriptExecutable* executable = ownerExecutable();
#if ENABLE(AOT)
    auto lineColumn = m_aotFunction ? m_aotFunction.lineColumnFor(bytecodeIndex()) : m_codeBlock->lineColumnForBytecodeIndex(bytecodeIndex());
#else
    auto lineColumn = m_codeBlock->lineColumnForBytecodeIndex(bytecodeIndex());
#endif

    if (!executable)
        return lineColumn;
    if (std::optional<int> overrideLineNumber = executable->overrideLineNumber(executable->vm()))
        lineColumn.line = overrideLineNumber.value();

    return lineColumn;
}

void StackVisitor::Frame::setToEnd()
{
    m_callFrame = nullptr;
#if ENABLE(DFG_JIT)
    m_inlineDFGCallFrame = nullptr;
#endif
    m_isWasmFrame = false;
}

bool StackVisitor::Frame::isFrameOf(JSCell* function) const
{
    if (callee().isNativeCallee())
        return false;
#if ENABLE(AOT)
    if (isTypedAOTFrame())
        return AOT::isClosureOfTypedFunction(function, m_typedAOTFunction);
#endif
    if (JSCell* callee = this->callee().asCell())
        return callee == function;
    if (JSCell* callee = aotCallee())
        return callee == function;
    auto* jsFunction = dynamicDowncast<JSFunction>(function);
    return jsFunction && hasCode() && jsFunction->executable() == ownerExecutable();
}

bool StackVisitor::Frame::isImplementationVisibilityPrivate() const
{
    if (isTypedAOTFrame())
        return false;
    ImplementationVisibility implementationVisibility = [&] () -> ImplementationVisibility {
        if (hasCode()) {
            if (auto* executable = ownerExecutable())
                return executable->implementationVisibility();
            return ImplementationVisibility::Public;
        }

#if ENABLE(WEBASSEMBLY)
        if (isNativeCalleeFrame())
            return callee().asNativeCallee()->implementationVisibility();
#endif

        if (callee().isCell()) {
            if (auto* callee = this->callee().asCell()) {
                if (auto* jsFunction = dynamicDowncast<JSFunction>(callee)) {
                    if (auto* executable = jsFunction->executable())
                        return executable->implementationVisibility();
                    return ImplementationVisibility::Public;
                }
            }
        }

        return ImplementationVisibility::Public;
    }();
    switch (implementationVisibility) {
    case ImplementationVisibility::Public:
        return false;

    case ImplementationVisibility::Private:
    case ImplementationVisibility::PrivateRecursive:
        return !Options::showPrivateScriptsInStackTraces();
    }

    ASSERT_NOT_REACHED();
    return false;
}

size_t StackVisitor::Frame::wasmFunctionIndex() const
{
    ASSERT(isNativeCalleeFrame());
    ASSERT(m_isWasmFrame);
    return m_wasmFunctionIndex;
}

CallSiteIndex StackVisitor::Frame::wasmCallSiteIndex() const
{
    return CallSiteIndex::fromBits(m_wasmCallSiteIndexBits);
}

void StackVisitor::Frame::dump(PrintStream& out, Indenter indent) const
{
    dump(out, indent, [] (PrintStream&) { });
}

void StackVisitor::Frame::dump(PrintStream& out, Indenter indent, WTF::Function<void(PrintStream&)> prefix) const
{
    if (!this->callFrame()) {
        out.print(indent, "frame 0x0\n");
        return;
    }

    if (m_aotFunction) {
        out.print(indent);
        prefix(out);
        out.print("frame ", RawPointer(this->callFrame()), " { name: ", functionName(), " sourceURL: ", sourceURL(), " ", bytecodeIndex(), " }\n");
        return;
    }

    CodeBlock* codeBlock = this->codeBlock();
    out.print(indent);
    prefix(out);
    out.print("frame ", RawPointer(this->callFrame()), " {\n");

    {
        indent++;

        CallFrame* callFrame = m_callFrame;
        CallFrame* callerFrame = this->callerFrame();
        const void* returnPC = callFrame->hasReturnPC() ? callFrame->returnPCForInspection() : nullptr;

        out.print(indent, "name: ", functionName(), "\n");
        out.print(indent, "sourceURL: ", sourceURL(), "\n");

        bool isInlined = false;
#if ENABLE(DFG_JIT)
        isInlined = isInlinedDFGFrame();
        out.print(indent, "isInlinedDFGFrame: ", isInlinedDFGFrame(), "\n");
        if (isInlinedDFGFrame())
            out.print(indent, "InlineCallFrame: ", RawPointer(m_inlineDFGCallFrame), "\n");
#endif

        out.print(indent, "callee: ", RawPointer(callee().rawPtr()), "\n");
        out.print(indent, "returnPC: ", RawPointer(returnPC), "\n");
        out.print(indent, "callerFrame: ", RawPointer(callerFrame), "\n");
        uintptr_t locationRawBits = callFrame->callSiteAsRawBits();
        out.print(indent, "rawLocationBits: ", locationRawBits,
            " ", RawHex(locationRawBits), "\n");
        out.print(indent, "codeBlock: ", RawPointer(codeBlock));
        if (codeBlock)
            out.print(" ", *codeBlock);
        out.print("\n");
        if (codeBlock && !isInlined) {
            indent++;

            if (callFrame->callSiteBitsAreBytecodeOffset()) {
                BytecodeIndex bytecodeIndex = callFrame->bytecodeIndex();
                out.print(indent, bytecodeIndex, " of ", codeBlock->instructions().size(), "\n");
#if ENABLE(DFG_JIT)
            } else {
                out.print(indent, "hasCodeOrigins: ", codeBlock->hasCodeOrigins(), "\n");
                if (codeBlock->hasCodeOrigins()) {
                    CallSiteIndex callSiteIndex = callFrame->callSiteIndex();
                    out.print(indent, "callSiteIndex: ", callSiteIndex.bits(), " of ", codeBlock->codeOrigins().size(), "\n");

                    JITType jitType = codeBlock->jitType();
                    if (jitType != JITType::FTLJIT) {
                        RefPtr jitCode = codeBlock->jitCode();
                        out.print(indent, "jitCode: ", RawPointer(jitCode.get()),
                            " start ", RawPointer(jitCode->start()),
                            " end ", RawPointer(jitCode->end()), "\n");
                    }
                }
#endif
            }
            auto lineColumn = computeLineAndColumn();
            out.print(indent, "line: ", lineColumn.line, "\n");
            out.print(indent, "column: ", lineColumn.column, "\n");

            indent--;
        }
        out.print(indent, "EntryFrame: ", RawPointer(m_entryFrame), "\n");
        indent--;
    }
    out.print(indent, "}\n");
}

} // namespace JSC
