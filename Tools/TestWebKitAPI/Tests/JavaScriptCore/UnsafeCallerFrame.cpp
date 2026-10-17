/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include <JavaScriptCore/CallFrame.h>
#include <JavaScriptCore/InitializeThreading.h>

namespace TestWebKitAPI {

// The sampling profiler walks stacks through CallFrame::unsafeCallerFrame
// while its EntryFrame cursor can be null: vm.topEntryFrame is null in the
// windows around vmEntryToJavaScript where vm.entryScope is already set, and
// the walk can start from a stale vm.topCallFrame or a half-built entry
// frame. A walked frame whose caller slot reads null used to match the null
// cursor and dereference vmEntryRecord(nullptr), faulting just below address
// zero. Model that exact state with a zeroed frame and a null entry frame:
// unsafeCallerFrame must report the end of the stack instead of crashing.
TEST(JavaScriptCore_CallFrame, UnsafeCallerFrameWithNullEntryFrame)
{
    JSC::initialize();

    alignas(JSC::Register) uint64_t zeroedFrame[16] = { };
    JSC::CallFrame* callFrame = JSC::CallFrame::create(reinterpret_cast<JSC::Register*>(zeroedFrame));

    JSC::EntryFrame* entryFrame = nullptr;
    EXPECT_EQ(callFrame->unsafeCallerFrame(entryFrame), nullptr);
    EXPECT_EQ(entryFrame, nullptr);
}

} // namespace TestWebKitAPI
