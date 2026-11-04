/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTNativeTest.h"
#include "AOTOperationScope.h"
#include "AOTVector.h"
#include "JSLock.h"
#include "ObjectConstructor.h"
#include <stdlib.h>

#if OS(WINDOWS)
#include <windows.h>
#endif

namespace JSC { namespace AOT {

static constexpr const char* violationRequestVariable = "TESTAOT_VIOLATE_OPERATION_EFFECTS";
static constexpr ASCIILiteral allocationInGuardedOperationTest = "operation-scope/allocation-in-a-guarded-operation-crashes"_s;

namespace {

class RealmOfOperationScopeTests {
public:
    RealmOfOperationScopeTests()
        : m_vm(VM::create(HeapType::Small))
        , m_locker(*m_vm)
    {
        m_globalObject.set(*m_vm, JSGlobalObject::create(*m_vm, JSGlobalObject::createStructure(*m_vm, jsNull())));
    }

    ~RealmOfOperationScopeTests()
    {
        m_globalObject.clear();
        m_vm = nullptr;
    }

    JSGlobalObject* globalObject() { return m_globalObject.get(); }

private:
    RefPtr<VM> m_vm;
    JSLockHolder m_locker;
    Strong<JSGlobalObject> m_globalObject;
};

} // anonymous namespace

static bool isViolationRequested()
{
    return !!getenv(violationRequestVariable);
}

static void setViolationRequestForChildren(bool isRequested)
{
#if OS(WINDOWS)
    SetEnvironmentVariableA(violationRequestVariable, isRequested ? "1" : nullptr);
#else
    if (isRequested)
        setenv(violationRequestVariable, "1", 1);
    else
        unsetenv(violationRequestVariable);
#endif
}

static void checkViolationCrashes(ASCIILiteral test, const String& report)
{
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(test) };
    setViolationRequestForChildren(true);
    ChildProcessResult result = launcher.run(arguments.span());
    setViolationRequestForChildren(false);
    AOT_TEST_CHECK(result.outcome == ChildProcessOutcome::Crashed);
    AOT_TEST_CHECK(result.output.contains(report));
}

static NEVER_INLINE bool JIT_OPERATION_ATTRIBUTES frameRecordHoldsReturnAddress()
{
    CallerFrameAndPC* frameRecord = AOT_OPERATION_FRAME_RECORD();
    return removeCodePtrTag(frameRecord->returnPC) == OUR_RETURN_ADDRESS;
}

static void testFrameRecordHoldsReturnAddress()
{
    AOT_TEST_CHECK(frameRecordHoldsReturnAddress());
}

static NEVER_INLINE CallFrame* JIT_OPERATION_ATTRIBUTES callerFrameSeenByOperation()
{
    return AOT_OPERATION_FRAME_RECORD()->callerFrame;
}

static NEVER_INLINE void JIT_OPERATION_ATTRIBUTES allocateObject(JSGlobalObject* globalObject)
{
    constructEmptyObject(globalObject);
}

static constexpr bool operationsAreGuarded = CallWithoutCollection<&allocateObject>::call != &allocateObject;
static_assert(!operationsAreGuarded || ASSERT_ENABLED);

// ALWAYS_INLINE inlines nothing in a debug build, and a function of its own reads its own frame pointer.
static inline __attribute__((__always_inline__)) CallFrame* framePointerRegister()
{
    CallFrame* result;
#if CPU(X86_64)
    asm volatile("movq %%rbp, %0" : "=r"(result));
#elif CPU(ARM64)
    asm volatile("mov %0, x29" : "=r"(result));
#else
#error "no name for the frame pointer of this CPU"
#endif
    return result;
}

// The frame of its caller is, to an operation, what the frame pointer held when it was called.
static NEVER_INLINE void testFrameRecordNamesCallerOfGuardedOperation()
{
    CallFrame* framePointer = framePointerRegister();
    AOT_TEST_CHECK(CallWithoutCollection<&callerFrameSeenByOperation>::call() == framePointer);
}

static void testAllocationInGuardedOperationCrashes()
{
    if (operationsAreGuarded && !isViolationRequested()) {
        checkViolationCrashes(allocationInGuardedOperationTest, "AssertNoGC"_s);
        return;
    }
    RealmOfOperationScopeTests realm;
    CallWithoutCollection<&allocateObject>::call(realm.globalObject());
}

std::span<const NativeTest> operationScopeTests()
{
    static constexpr NativeTest tests[] = {
        { "operation-scope/the-frame-record-holds-the-return-address"_s, testFrameRecordHoldsReturnAddress },
        { "operation-scope/the-frame-record-names-the-caller-of-a-guarded-operation"_s, testFrameRecordNamesCallerOfGuardedOperation },
        { allocationInGuardedOperationTest, testAllocationInGuardedOperationCrashes },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
