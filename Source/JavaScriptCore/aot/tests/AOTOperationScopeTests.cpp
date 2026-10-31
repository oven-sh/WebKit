/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTEffectsDeclared.h"
#include "AOTNativeTest.h"
#include "AOTOperationScope.h"
#include "AOTVector.h"
#include "Completion.h"
#include "DeferGCInlines.h"
#include "JSLock.h"
#include "ObjectConstructor.h"
#include "SourceCode.h"
#include <stdlib.h>
#include <wtf/text/MakeString.h>

#if OS(WINDOWS)
#include <windows.h>
#endif

namespace JSC { namespace AOT {

static constexpr Entry readOnlyOperation = Entry::operationAOTToBoolean;
static constexpr Entry collectingOperation = Entry::operationAOTCompareStrictEq;
static constexpr Entry unknownCodeOperation = Entry::operationAOTValueAdd;
static constexpr Entry operationOutsideSchema = Entry::operationAOTLookupExceptionHandler;

static constexpr Typed::Effects declaredEffectsOf(Entry entry)
{
    return Typed::declaredBoundOf(*runtimeOperationOf(entry));
}

static_assert(!declaredEffectsOf(readOnlyOperation).mayCollect && !declaredEffectsOf(readOnlyOperation).mayCallUnknownCode);
static_assert(declaredEffectsOf(collectingOperation).mayCollect && !declaredEffectsOf(collectingOperation).mayCallUnknownCode);
static_assert(declaredEffectsOf(unknownCodeOperation).mayCollect && declaredEffectsOf(unknownCodeOperation).mayCallUnknownCode);
static_assert(!runtimeOperationOf(operationOutsideSchema));

static constexpr const char* violationRequestVariable = "TESTAOT_VIOLATE_OPERATION_EFFECTS";
static constexpr ASCIILiteral undeclaredCollectionTest = "operation-scope/undeclared-collection-crashes"_s;
static constexpr ASCIILiteral undeclaredVMEntryTest = "operation-scope/undeclared-vm-entry-crashes"_s;
static constexpr ASCIILiteral undeclaredAllocationTest = "operation-scope/undeclared-allocation-crashes-where-assertions-are-on"_s;
static constexpr ASCIILiteral allocationInGuardedOperationTest = "operation-scope/allocation-in-a-guarded-operation-crashes"_s;

namespace {

enum class Validation : bool { Off, On };

class TestRealm {
public:
    explicit TestRealm(Validation validation)
        : m_vm(VM::create(HeapType::Small))
        , m_locker(*m_vm)
    {
        m_globalObject.set(*m_vm, JSGlobalObject::create(*m_vm, JSGlobalObject::createStructure(*m_vm, jsNull())));
        m_vm->aotState().validatesRuntime = validation == Validation::On;
    }

    ~TestRealm()
    {
        m_vm->aotState().validatesRuntime = false;
        m_globalObject.clear();
        m_vm = nullptr;
    }

    VM& vm() { return *m_vm; }
    JSGlobalObject* globalObject() { return m_globalObject.get(); }

    void collectInOperation(Entry entry)
    {
        OperationValidationScope scope(vm(), entry);
        vm().heap.collectSync(CollectionScope::Full);
    }

    void enterVMInOperation(Entry entry)
    {
        OperationValidationScope scope(vm(), entry);
        AOT_TEST_CHECK(!vm().didEnterVM);
        NakedPtr<Exception> exception;
        evaluate(m_globalObject.get(), makeSource("1"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted), JSValue(), exception);
        AOT_TEST_CHECK(!exception);
        AOT_TEST_CHECK(vm().didEnterVM);
    }

    void allocateInOperation(Entry entry)
    {
        OperationValidationScope scope(vm(), entry);
        constructEmptyObject(m_globalObject.get());
    }

    void allocateWhileCollectionsArePutOffInOperation(Entry entry)
    {
        OperationValidationScope scope(vm(), entry);
        DeferGCForAWhile deferGC(vm());
        constructEmptyObject(m_globalObject.get());
    }

    void doNothingInOperation(Entry entry)
    {
        OperationValidationScope scope(vm(), entry);
    }

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

static void checkViolationCrashes(ASCIILiteral test, ValidationFailureKind kind)
{
    checkViolationCrashes(test, makeString("AOT validation: "_s, nameOf(kind)));
}

static NEVER_INLINE bool JIT_OPERATION_ATTRIBUTES frameRecordHoldsReturnAddress()
{
    CallerFrameAndPC* frameRecord = AOT_OPERATION_FRAME_RECORD();
    return removeReturnAddressSignature(frameRecord->returnPC) == OUR_RETURN_ADDRESS;
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

static void testPermissionsFollowDeclaredEffects()
{
    AOT_TEST_CHECK(!permissionsOf(readOnlyOperation).mayCollect && !permissionsOf(readOnlyOperation).mayEnterVM);
    AOT_TEST_CHECK(permissionsOf(collectingOperation).mayCollect && !permissionsOf(collectingOperation).mayEnterVM);
    AOT_TEST_CHECK(permissionsOf(unknownCodeOperation).mayCollect && permissionsOf(unknownCodeOperation).mayEnterVM);
    AOT_TEST_CHECK(permissionsOf(operationOutsideSchema).mayCollect && permissionsOf(operationOutsideSchema).mayEnterVM);
}

static void testDeclaredEffectsAreAccepted()
{
    TestRealm realm(Validation::On);
    realm.doNothingInOperation(readOnlyOperation);
    realm.collectInOperation(collectingOperation);
    realm.collectInOperation(unknownCodeOperation);
    realm.enterVMInOperation(unknownCodeOperation);
    realm.collectInOperation(operationOutsideSchema);
    realm.enterVMInOperation(operationOutsideSchema);
}

static void testVMEntryStaysRecorded()
{
    TestRealm realm(Validation::On);
    VM& vm = realm.vm();

    vm.didEnterVM = false;
    realm.doNothingInOperation(readOnlyOperation);
    AOT_TEST_CHECK(!vm.didEnterVM);

    realm.enterVMInOperation(unknownCodeOperation);
    AOT_TEST_CHECK(vm.didEnterVM);

    realm.doNothingInOperation(readOnlyOperation);
    AOT_TEST_CHECK(vm.didEnterVM);
}

static void testNothingIsCheckedWithoutValidation()
{
    TestRealm realm(Validation::Off);
    realm.vm().didEnterVM = false;
    realm.collectInOperation(readOnlyOperation);
    realm.enterVMInOperation(collectingOperation);
}

static void testUndeclaredAllocationCrashesWhereAssertionsAreOn()
{
    if (ASSERT_ENABLED && !isViolationRequested()) {
        checkViolationCrashes(undeclaredAllocationTest, "AssertNoGC"_s);
        return;
    }
    TestRealm realm(Validation::Off);
    realm.allocateInOperation(collectingOperation);
    realm.allocateWhileCollectionsArePutOffInOperation(readOnlyOperation);
    realm.allocateInOperation(readOnlyOperation);
}

static void testAllocationInGuardedOperationCrashes()
{
    if (operationsAreGuarded && !isViolationRequested()) {
        checkViolationCrashes(allocationInGuardedOperationTest, "AssertNoGC"_s);
        return;
    }
    TestRealm realm(Validation::Off);
    CallWithoutCollection<&allocateObject>::call(realm.globalObject());
}

static void testUndeclaredCollectionCrashes()
{
    if (!isViolationRequested()) {
        checkViolationCrashes(undeclaredCollectionTest, ValidationFailureKind::OperationCollected);
        return;
    }
    TestRealm realm(Validation::On);
    realm.collectInOperation(readOnlyOperation);
}

static void testUndeclaredVMEntryCrashes()
{
    if (!isViolationRequested()) {
        checkViolationCrashes(undeclaredVMEntryTest, ValidationFailureKind::OperationEnteredVM);
        return;
    }
    TestRealm realm(Validation::On);
    realm.enterVMInOperation(collectingOperation);
}

std::span<const NativeTest> operationScopeTests()
{
    static constexpr NativeTest tests[] = {
        { "operation-scope/the-frame-record-holds-the-return-address"_s, testFrameRecordHoldsReturnAddress },
        { "operation-scope/the-frame-record-names-the-caller-of-a-guarded-operation"_s, testFrameRecordNamesCallerOfGuardedOperation },
        { "operation-scope/permissions-follow-declared-effects"_s, testPermissionsFollowDeclaredEffects },
        { "operation-scope/declared-effects-are-accepted"_s, testDeclaredEffectsAreAccepted },
        { "operation-scope/vm-entry-stays-recorded"_s, testVMEntryStaysRecorded },
        { "operation-scope/nothing-is-checked-without-validation"_s, testNothingIsCheckedWithoutValidation },
        { undeclaredCollectionTest, testUndeclaredCollectionCrashes },
        { undeclaredVMEntryTest, testUndeclaredVMEntryCrashes },
        { undeclaredAllocationTest, testUndeclaredAllocationCrashesWhereAssertionsAreOn },
        { allocationInGuardedOperationTest, testAllocationInGuardedOperationCrashes },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
