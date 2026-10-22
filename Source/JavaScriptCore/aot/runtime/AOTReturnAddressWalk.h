/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledFrame.h"
#include "AOTState.h"
#include "CallFrame.h"
#include "VM.h"
#include <wtf/IterationStatus.h>

namespace JSC { namespace AOT {

// Goes over the stack of the thread that runs the VM, from the youngest frame to the oldest, and calls
//     IterationStatus functor(void*& returnAddress, const CallSiteCache::Entry& site)
// for every return address into a compiled function: whoever returns to it, a compiled function with or without a frame record, a stub, an operation or a function of the host.
// It crosses stubs, native frames, frames of the engine and entry frames as a capture of a stack trace does. It ends where functor answers Done.
// returnAddress is the word on the stack itself: functor may store into it any address for which the tables answer what they answer for the one that was there.
// site is valid until functor asks the call site cache about another address.
// For an operation, and for native code below one: vm.topCallFrame and vm.topEntryFrame say where the thread left compiled code or the engine.
template<typename Functor>
SUPPRESS_ASAN void forEachReturnAddressIntoCompiledFunctions(VM& vm, const Functor& functor)
{
    if (!vm.hasCompiledCode())
        return;
    const VMState& state = vm.aotState();
    CallSiteCache& cache = *state.callSiteCache;
    const Image& image = *state.image;
    auto placeOfReturnAddress = [](CallFrame* frame) -> void*& {
        return reinterpret_cast<CallerFrameAndPC*>(frame)->returnPC;
    };
    auto isAdapter = [](StubFrameKind kind) {
        return kind == StubFrameKind::Adapter || kind == StubFrameKind::ConstructAdapter || kind == StubFrameKind::SavedFrameAdapter || kind == StubFrameKind::HandlerOfJob;
    };
    auto findSite = [&](CallFrame* frame) {
        const void* pc = removeCodePtrTag(placeOfReturnAddress(frame));
        const CallSiteCache::Entry* site = cache.find(pc);
        return site ? site : cache.resolve(image, pc);
    };

    EntryFrame* entryFrame = vm.topEntryFrame;
    // Each of these has a frame record: an operation's, a host function's, one of the engine.
    for (CallFrame* frameOutsideImage = vm.topCallFrame; frameOutsideImage;) {
        const CallSiteCache::Entry* site = findSite(frameOutsideImage);
        if (!site) {
            frameOutsideImage = frameOutsideImage->callerFrame(entryFrame);
            continue;
        }

        // A run of compiled functions and stubs. callee returns to the address that site describes; framePointer is what that register holds there.
        CallFrame* callee = frameOutsideImage;
        CallFrame* framePointer = reinterpret_cast<CallerFrameAndPC*>(callee)->callerFrame;
        unsigned bytesAboveReturnAddress = 0;
        for (;;) {
            CallFrame* frame = frameAtReturnSite(site->frame, stackPointerOfCaller(callee, bytesAboveReturnAddress), framePointer);
            bool hasFrameRecord = site->frame.hasFrameRecord();
            bool endsRun = site->isStubReturnSite() && isAdapter(site->stubFrameKind);
            bytesAboveReturnAddress = bytesAboveReturnAddressOf(*site);
            if (!site->isStubReturnSite() && functor(placeOfReturnAddress(callee), *site) == IterationStatus::Done)
                return;

            if (hasFrameRecord) {
                if (endsRun || frame->callerFrameOrEntryFrame() == entryFrame) {
                    frameOutsideImage = frame->callerFrame(entryFrame);
                    break;
                }
                framePointer = frame->callerFrame();
            } else
                ASSERT(!endsRun);
            callee = frame;
            site = findSite(callee);
            if (!site) {
                frameOutsideImage = framePointer;
                break;
            }
        }
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
