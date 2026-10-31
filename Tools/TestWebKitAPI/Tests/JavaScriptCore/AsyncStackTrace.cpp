/*
 * Copyright (C) 2026 Vincent Koc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/Bytecodes.h>
#include <JavaScriptCore/CodeBlock.h>
#include <JavaScriptCore/DeferGCInlines.h>
#include <JavaScriptCore/Interpreter.h>
#include <JavaScriptCore/JSAsyncFunctionGenerator.h>
#include <JavaScriptCore/JSCast.h>
#include <JavaScriptCore/JSCellInlines.h>
#include <JavaScriptCore/JSGlobalObjectInlines.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSPromise.h>
#include <JavaScriptCore/JavaScript.h>
#include <JavaScriptCore/Options.h>
#include <JavaScriptCore/StackFrame.h>
#include <JavaScriptCore/VM.h>
#include <wtf/Scope.h>
#include <wtf/Vector.h>

namespace TestWebKitAPI {

TEST(JavaScriptCore_AsyncStackTrace, SuspendedFrameUsesResumeInstruction)
{
    auto context = JSGlobalContextCreate(nullptr);
    ASSERT_NE(context, nullptr);
    auto releaseContext = makeScopeExit([&] {
        JSGlobalContextRelease(context);
    });

    auto* globalObject = toJS(context);
    auto& vm = globalObject->vm();
    JSC::JSLockHolder locker(vm);

    ASSERT_TRUE(JSC::Options::useAsyncStackTrace());
    ASSERT_FALSE(JSC::Options::runsCompiledImage());

    auto script = JSStringCreateWithUTF8CString(R"JS(
        globalThis.pending = new Promise(() => {});
        globalThis.operation = (async function paused() {
            await globalThis.pending;
        })();
        globalThis.pending;
    )JS");
    ASSERT_NE(script, nullptr);
    JSValueRef exception = nullptr;
    auto result = JSEvaluateScript(context, script, nullptr, nullptr, 1, &exception);
    JSStringRelease(script);
    ASSERT_EQ(exception, nullptr);
    ASSERT_NE(result, nullptr);

    JSC::DeferGCForAWhile deferGC(vm);
    auto* promise = dynamicDowncast<JSC::JSPromise>(toJS(globalObject, result));
    ASSERT_NE(promise, nullptr);
    ASSERT_EQ(promise->status(), JSC::JSPromise::Status::Pending);

    auto* generator = dynamicDowncast<JSC::JSAsyncFunctionGenerator>(promise->asyncStackTraceContext());
    ASSERT_NE(generator, nullptr);
    ASSERT_EQ(generator->state(), 1);

    WTF::Vector<JSC::StackFrame> frames;
    JSC::Interpreter::appendAsyncStackFrame(vm, globalObject, frames, generator);
    ASSERT_EQ(frames.size(), 1u);

    const auto& frame = frames[0];
    auto* codeBlock = frame.codeBlock();
    ASSERT_NE(codeBlock, nullptr);
    ASSERT_TRUE(frame.hasBytecodeIndex());
    ASSERT_EQ(codeBlock->numberOfUnlinkedSwitchJumpTables(), 1u);

    unsigned switchOffset = 0;
    unsigned switchCount = 0;
    for (const auto& instruction : codeBlock->instructions()) {
        if (instruction->opcodeID() == JSC::op_switch_imm) {
            switchOffset = instruction.offset();
            ++switchCount;
        }
    }
    ASSERT_EQ(switchCount, 1u);
    ASSERT_GT(switchOffset, 0u);

    auto relativeOffset = codeBlock->unlinkedSwitchJumpTable(0).offsetForValue(generator->state());
    ASSERT_GT(relativeOffset, 0);
    unsigned resumeOffset = switchOffset + static_cast<unsigned>(relativeOffset);
    bool isResumeInstruction = false;
    for (const auto& instruction : codeBlock->instructions()) {
        if (instruction.offset() == resumeOffset) {
            isResumeInstruction = true;
            break;
        }
    }
    ASSERT_TRUE(isResumeInstruction);

    EXPECT_EQ(frame.bytecodeIndex().offset(), resumeOffset)
        << "switch offset: " << switchOffset << ", relative target: " << relativeOffset;
}

} // namespace TestWebKitAPI
