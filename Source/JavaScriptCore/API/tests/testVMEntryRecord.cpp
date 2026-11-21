/*
 * Copyright (C) 2026 Peter Steinberger All rights reserved.
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
#include "VMEntryRecord.h"

#include "APICast.h"
#include "CallFrame.h"
#include "CodeBlock.h"
#include "EntryFrame.h"
#include "InitializeThreading.h"
#include "JSGlobalObject.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "JavaScript.h"
#include "LLIntThunks.h"
#include "Options.h"
#include "ProtoCallFrameInlines.h"
#include "VM.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <wtf/MainThread.h>
#include <wtf/text/ASCIILiteral.h>

#if !OS(LINUX) || !CPU(X86_64) || ENABLE(C_LOOP)
#error This assembly probe requires Linux x86-64 SysV and the native LLInt.
#endif

using JSC::CallFrame;
using JSC::CodeBlock;
using JSC::EncodedJSValue;
using JSC::EntryFrame;
using JSC::JSCell;
using JSC::JSLockHolder;
using JSC::JSObject;
using JSC::JSValue;
using JSC::Options;
using JSC::ProtoCallFrame;
using JSC::VM;
using JSC::jsNumber;
using JSC::jsUndefined;
using JSC::vmEntryRecord;
using JSC::vmEntryToJavaScript;
using JSC::vmEntryToJavaScriptWith0Arguments;
using JSC::vmEntryToJavaScriptWith1Arguments;
using JSC::vmEntryToJavaScriptWith2Arguments;
using JSC::vmEntryToJavaScriptWith3Arguments;
using JSC::vmEntryToJavaScriptWith4Arguments;
using JSC::vmEntryToJavaScriptWith5Arguments;
using JSC::vmEntryToJavaScriptWith6Arguments;
using JSC::vmEntryToNative;

struct Invocation;
struct EntryRecordProbe {
    intptr_t tailOffset;
    uintptr_t tailWordCount;
    uintptr_t poison;
    uintptr_t expectedEntryFrame { 0 };
    uintptr_t observedEntryFrame { 0 };
    std::array<uintptr_t, 32> captured { };
    Invocation* invocation { nullptr };
    EntryFrame* priorEntryFrame { nullptr };
    CallFrame* priorCallFrame { nullptr };
    bool allowNested { false };
    bool headerMatched { false };
    bool locatorPreserved { false };
    bool nestedPassed { false };
    bool outerPreserved { false };
};

struct Invocation {
    void* thunk;
    void* target;
    VM* vm;
    CodeBlock* codeBlock;
    JSObject* callee;
    EncodedJSValue thisValue;
    JSCell* context;
    std::array<EncodedJSValue, 6> arguments;
    ProtoCallFrame* proto;
    EntryRecordProbe* probe;
    uintptr_t generic;
};

static_assert(sizeof(uintptr_t) == 8 && sizeof(CPURegister) == 8);
static_assert(!offsetof(EntryRecordProbe, tailOffset));
static_assert(offsetof(EntryRecordProbe, tailWordCount) == 8);
static_assert(offsetof(EntryRecordProbe, poison) == 16);
static_assert(offsetof(EntryRecordProbe, expectedEntryFrame) == 24);
static_assert(offsetof(EntryRecordProbe, observedEntryFrame) == 32);
static_assert(offsetof(EntryRecordProbe, captured) == 40);
static_assert(!offsetof(Invocation, thunk));
static_assert(offsetof(Invocation, target) == 8);
static_assert(offsetof(Invocation, vm) == 16);
static_assert(offsetof(Invocation, codeBlock) == 24);
static_assert(offsetof(Invocation, callee) == 32);
static_assert(offsetof(Invocation, thisValue) == 40);
static_assert(offsetof(Invocation, context) == 48);
static_assert(offsetof(Invocation, arguments) == 56);
static_assert(offsetof(Invocation, proto) == 104);
static_assert(offsetof(Invocation, probe) == 112);
static_assert(offsetof(Invocation, generic) == 120);

extern "C" {
// One serial test thread owns this pointer. No other thread invokes the probe.
EntryRecordProbe* vmEntryRecordProbeCurrent = nullptr;
EncodedJSValue vmEntryRecordProbeInvoke(Invocation*);
void vmEntryRecordProbeCallee();
EncodedJSValue vmEntryRecordProbeFinish(EntryFrame*);
}

static bool capturedZero(const EntryRecordProbe& probe)
{
    for (size_t index = 0; index < probe.tailWordCount; ++index) {
        if (probe.captured[index])
            return false;
    }
    return true;
}

static bool passed(const EntryRecordProbe& probe)
{
    return probe.expectedEntryFrame == probe.observedEntryFrame
        && capturedZero(probe) && probe.headerMatched && probe.locatorPreserved
        && probe.nestedPassed && probe.outerPreserved;
}

extern "C" EncodedJSValue vmEntryRecordProbeFinish(EntryFrame* entryFrame)
{
    // The assembly callee already copied the original bytes, before this C++
    // call could introduce a legitimate unwind/callee-save writer.
    EntryRecordProbe& probe = *vmEntryRecordProbeCurrent;
    Invocation& invocation = *probe.invocation;
    auto* record = vmEntryRecord(entryFrame);
    probe.headerMatched = invocation.vm->topEntryFrame == entryFrame
        && record->m_vm == invocation.vm && record->m_context == invocation.context
        && record->prevTopCallFrame() == probe.priorCallFrame
        && record->prevTopEntryFrame() == probe.priorEntryFrame;

    auto* tail = reinterpret_cast<volatile uintptr_t*>(reinterpret_cast<uintptr_t>(entryFrame) + probe.tailOffset);
    std::array<uintptr_t, 32> original { };
    const auto token = [](size_t index) -> uintptr_t {
        return 0x5a5a5a5a5a5a0000ULL + index;
    };
    for (size_t index = 0; index < probe.tailWordCount; ++index) {
        original[index] = tail[index];
        tail[index] = token(index);
    }

    // Computing the address of an already-active record must not clear it.
    auto* lookedUp = vmEntryRecord(entryFrame);
    probe.locatorPreserved = lookedUp == record;
    for (size_t index = 0; index < probe.tailWordCount; ++index)
        probe.locatorPreserved &= tail[index] == token(index);

    probe.nestedPassed = true;
    if (probe.allowNested) {
        EntryRecordProbe nested { probe.tailOffset, probe.tailWordCount, probe.poison };
        Invocation nestedInvocation = invocation;
        nestedInvocation.thunk = reinterpret_cast<void*>(vmEntryToNative);
        nestedInvocation.generic = 1;
        nestedInvocation.probe = &nested;
        nested.invocation = &nestedInvocation;
        nested.priorEntryFrame = invocation.vm->topEntryFrame;
        nested.priorCallFrame = invocation.vm->topCallFrame;
        vmEntryRecordProbeCurrent = &nested;
        EncodedJSValue result = vmEntryRecordProbeInvoke(&nestedInvocation);
        vmEntryRecordProbeCurrent = &probe;
        probe.nestedPassed = passed(nested) && JSValue::decode(result) == jsNumber(42)
            && invocation.vm->topEntryFrame == nested.priorEntryFrame
            && invocation.vm->topCallFrame == nested.priorCallFrame;
    }

    // These non-pointer tokens stand in for an active frame's later contents.
    // A nested entry must initialize itself, never clear its predecessor.
    probe.outerPreserved = true;
    for (size_t index = 0; index < probe.tailWordCount; ++index) {
        probe.outerPreserved &= tail[index] == token(index);
        tail[index] = original[index];
    }
    return JSValue::encode(jsNumber(42));
}

static JSValueRef unusedNativeCallee(JSContextRef context, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*)
{
    return JSValueMakeNumber(context, 42);
}

static bool ordinaryJavaScriptUnwindSmoke(JSGlobalContextRef context)
{
    // This is semantic coverage only; it does not claim a particular JIT tier.
    JSStringRef source = JSStringCreateWithUTF8CString(
        "(function outer(input) {"
        "  const saved = input.value;"
        "  try { (function inner() { throw input; })(); }"
        "  catch (error) { return saved + error.increment; }"
        "})({value: 40, increment: 2})");
    JSValueRef exception = nullptr;
    JSValueRef value = JSEvaluateScript(context, source, nullptr, nullptr, 1, &exception);
    JSStringRelease(source);
    return !exception && value && JSValueToNumber(context, value, &exception) == 42 && !exception;
}

int main()
{
    WTF::initializeMainThread();
    JSC::initialize();
    // An assertion-only verification wrapper adds another entry-sized frame.
    // The exact-stack probe tests the production entry shape without that wrapper.
#if ASSERT_ENABLED
    if (!Options::setOptions("validateVMEntryCalleeSaves=false"))
        return 2;
#endif
    JSGlobalContextRef context = JSGlobalContextCreate(nullptr);
    if (!context)
        return 2;
    auto* globalObject = toJS(context);
    VM& vm = globalObject->vm();
    // Bun's C API leaves locking and atom-table ownership to its embedder.
    JSLockHolder lock(vm);
    JSObjectRef calleeRef = JSObjectMakeFunctionWithCallback(context, nullptr, unusedNativeCallee);
    JSValueProtect(context, calleeRef);
    bool allPassed = true;
    {
        const intptr_t tailOffset = EntryFrame::calleeSaveRegistersBufferOffset();
        const size_t tailBytes = static_cast<size_t>(-tailOffset);
        // End at the entry frame, covering both the save buffer and its trailing
        // frame-alignment padding, rather than just sizeof(VMEntryRecord).
        // Poisoning stays within the SysV 128-byte red zone until the call
        // claims it; the return address and saved rbp consume another 16 bytes.
        if (tailOffset >= 0 || tailBytes % sizeof(uintptr_t) || tailBytes > 112)
            return 2;
        const size_t wordCount = tailBytes / sizeof(uintptr_t);
        struct EntryCase {
            WTF::ASCIILiteral name;
            void* thunk;
            unsigned arguments;
            bool generic;
        };
        const EntryCase cases[] = {
            { "fixed0"_s, reinterpret_cast<void*>(vmEntryToJavaScriptWith0Arguments), 0, false },
            { "fixed1"_s, reinterpret_cast<void*>(vmEntryToJavaScriptWith1Arguments), 1, false },
            { "fixed2"_s, reinterpret_cast<void*>(vmEntryToJavaScriptWith2Arguments), 2, false },
            { "fixed3"_s, reinterpret_cast<void*>(vmEntryToJavaScriptWith3Arguments), 3, false },
            { "fixed4"_s, reinterpret_cast<void*>(vmEntryToJavaScriptWith4Arguments), 4, false },
            { "fixed5"_s, reinterpret_cast<void*>(vmEntryToJavaScriptWith5Arguments), 5, false },
            { "fixed6"_s, reinterpret_cast<void*>(vmEntryToJavaScriptWith6Arguments), 6, false },
            { "genericJS"_s, reinterpret_cast<void*>(vmEntryToJavaScript), 6, true },
            { "genericNative"_s, reinterpret_cast<void*>(vmEntryToNative), 6, true },
        };
        for (const auto& entry : cases) {
            EntryRecordProbe probe { tailOffset, wordCount, 0xa5a5a5a5a5a5a5a5ULL };
            ProtoCallFrame proto;
            Invocation invocation {
                entry.thunk, reinterpret_cast<void*>(vmEntryRecordProbeCallee), &vm,
                nullptr, toJS(calleeRef), JSValue::encode(jsUndefined()), globalObject,
                { }, &proto, &probe, entry.generic,
            };
            for (size_t index = 0; index < invocation.arguments.size(); ++index)
                invocation.arguments[index] = JSValue::encode(jsNumber(static_cast<int>(100 + index)));
            proto.init(nullptr, globalObject, invocation.callee, jsUndefined(), invocation.context,
                entry.arguments + 1, invocation.arguments.data());
            probe.invocation = &invocation;
            probe.priorEntryFrame = vm.topEntryFrame;
            probe.priorCallFrame = vm.topCallFrame;
            probe.allowNested = true;
            vmEntryRecordProbeCurrent = &probe;
            EncodedJSValue result = vmEntryRecordProbeInvoke(&invocation);
            vmEntryRecordProbeCurrent = nullptr;
            bool ok = passed(probe) && JSValue::decode(result) == jsNumber(42)
                && vm.topEntryFrame == probe.priorEntryFrame && vm.topCallFrame == probe.priorCallFrame;
            SAFE_PRINTF("%s: %s tailWords=%zu zero=%d sameFrame=%d headers=%d locator=%d nested=%d outer=%d\n",
                entry.name, ok ? "PASS"_s : "FAIL"_s, wordCount, capturedZero(probe),
                probe.expectedEntryFrame == probe.observedEntryFrame, probe.headerMatched,
                probe.locatorPreserved, probe.nestedPassed, probe.outerPreserved);
            if (!ok) {
                for (size_t index = 0; index < wordCount; ++index)
                    SAFE_PRINTF("  tail[%zu]=0x%016llx\n", index, static_cast<unsigned long long>(probe.captured[index]));
            }
            allPassed &= ok;
        }
    }
    bool smokePassed = ordinaryJavaScriptUnwindSmoke(context);
    SAFE_PRINTF("ordinaryJavaScriptUnwind: %s\n", smokePassed ? "PASS"_s : "FAIL"_s);
    JSValueUnprotect(context, calleeRef);
    JSGlobalContextRelease(context);
    return allPassed && smokePassed ? 0 : 1;
}
