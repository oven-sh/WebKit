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
#include "Completion.h"
#include "JSLock.h"
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

static void checkViolationCrashes(ASCIILiteral test, ValidationFailureKind kind)
{
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(test) };
    setViolationRequestForChildren(true);
    ChildProcessResult result = launcher.run(arguments.span());
    setViolationRequestForChildren(false);
    AOT_TEST_CHECK(result.outcome == ChildProcessOutcome::Crashed);
    AOT_TEST_CHECK(result.output.contains(makeString("AOT validation: "_s, nameOf(kind))));
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
    realm.enterVMInOperation(readOnlyOperation);
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
        { "operation-scope/permissions-follow-declared-effects"_s, testPermissionsFollowDeclaredEffects },
        { "operation-scope/declared-effects-are-accepted"_s, testDeclaredEffectsAreAccepted },
        { "operation-scope/vm-entry-stays-recorded"_s, testVMEntryStaysRecorded },
        { "operation-scope/nothing-is-checked-without-validation"_s, testNothingIsCheckedWithoutValidation },
        { undeclaredCollectionTest, testUndeclaredCollectionCrashes },
        { undeclaredVMEntryTest, testUndeclaredVMEntryCrashes },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
