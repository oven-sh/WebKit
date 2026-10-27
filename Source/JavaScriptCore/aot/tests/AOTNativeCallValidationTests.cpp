/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTNativeCallValidation.h"
#include "AOTRealmTestSupport.h"
#include "AOTRuntimeValidation.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSMapInlines.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"
#include <initializer_list>
#include <stdlib.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>

#if OS(WINDOWS)
#include <windows.h>
#endif

namespace JSC { namespace AOT {

static constexpr const char* mismatchRequestVariable = "TESTAOT_MISMATCH_NATIVE_CALL";

static JSC_DECLARE_HOST_FUNCTION(functionOutsideTable);

JSC_DEFINE_HOST_FUNCTION(functionOutsideTable, (JSGlobalObject*, CallFrame*))
{
    return JSValue::encode(jsUndefined());
}

namespace {

// Stands for a native function: the scope sees the callee and the operands, and the body does to them what the function would.
class SimulatedCall {
public:
    SimulatedCall(Realms& realms, ASCIILiteral callee)
        : m_realms(realms)
        , m_callee(realms.object(realms.plain(), callee))
    {
    }

    explicit SimulatedCall(Realms& realms)
        : m_realms(realms)
        , m_callee(JSFunction::create(realms.vm(), realms.plain(), 0, "outsideTable"_s, functionOutsideTable, ImplementationVisibility::Public))
    {
    }

    SimulatedCall& on(JSValue thisValue)
    {
        m_thisValue = thisValue;
        return *this;
    }

    SimulatedCall& expecting(const NativeCallExpectation& expectation)
    {
        m_expectation = expectation;
        return *this;
    }

    void run(std::initializer_list<JSValue> operands, const Invocable<JSValue()> auto& body)
    {
        MarkedArgumentBuffer arguments;
        for (JSValue operand : operands)
            arguments.append(operand);
        NativeCallValidationScope scope(m_realms.vm(), { m_realms.plain(), m_callee, CodeSpecializationKind::CodeForCall, m_thisValue, arguments }, m_expectation);
        scope.didReturn(body());
    }

private:
    Realms& m_realms;
    JSObject* m_callee;
    JSValue m_thisValue { jsUndefined() };
    NativeCallExpectation m_expectation;
};

} // anonymous namespace

static void addProperty(Realms& realms, JSObject* object)
{
    object->putDirect(realms.vm(), Identifier::fromString(realms.vm(), "added"_s), jsNumber(1));
}

static NativeCallExpectation expectationOfCallWithoutEffects()
{
    NativeCallExpectation expectation;
    expectation.isKnown = true;
    expectation.mayThrow = false;
    expectation.mayCollect = false;
    return expectation;
}

// A call of a callee that the analysis does not know, in a program that runs no unseen code.
static NativeCallExpectation expectationOfUnknownCallee()
{
    NativeCallExpectation expectation;
    expectation.isKnown = true;
    expectation.trustsNativeCode = true;
    expectation.writes = AbstractHeaps::fromRaw((1ull << numberOfEnumerators<AbstractHeap>) - 1);
    return expectation;
}

static bool isMismatchRequested()
{
    return !!getenv(mismatchRequestVariable);
}

static void setMismatchRequestForChildren(bool isRequested)
{
#if OS(WINDOWS)
    SetEnvironmentVariableA(mismatchRequestVariable, isRequested ? "1" : nullptr);
#else
    if (isRequested)
        setenv(mismatchRequestVariable, "1", 1);
    else
        unsetenv(mismatchRequestVariable);
#endif
}

static void checkFailure(const ChildProcessResult& result, ValidationFailureKind kind)
{
    if (result.outcome != ChildProcessOutcome::Crashed)
        dataLogLn(result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.outcome == ChildProcessOutcome::Crashed);
    AOT_TEST_CHECK(result.output.contains(makeString("AOT validation: "_s, nameOf(kind))));
}

static void checkMismatchCrashes(ASCIILiteral test, ValidationFailureKind kind)
{
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(test) };
    setMismatchRequestForChildren(true);
    ChildProcessResult result = launcher.run(arguments.span());
    setMismatchRequestForChildren(false);
    checkFailure(result, kind);
}

static void testDeclaredChangesAreAccepted()
{
    Realms realms(RuntimeValidation::On);
    VM& vm = realms.vm();
    JSGlobalObject* realm = realms.plain();

    JSObject* frozen = realms.object(realm, "({ a: 1 })"_s);
    SimulatedCall(realms, "Object.freeze"_s).run({ frozen }, [&] {
        return objectConstructorFreeze(realm, frozen);
    });

    JSArray* frozenArray = realms.array(realm, "(element => [element, 1])('a')"_s);
    AOT_TEST_CHECK(!isCopyOnWrite(frozenArray->indexingMode()));
    {
        // An array is frozen in place only in a VM that runs compiled code.
        SetForScope runsCompiledCode(vm.aotState().hasCompiledCode, true);
        SimulatedCall(realms, "Object.freeze"_s).run({ frozenArray }, [&] {
            return objectConstructorFreeze(realm, frozenArray);
        });
    }
    AOT_TEST_CHECK(isCopyOnWrite(frozenArray->indexingMode()));

    JSObject* target = realms.object(realm, "({ a: 1 })"_s);
    SimulatedCall(realms, "Object.assign"_s).run({ target, realms.object(realm, "({ added: 1 })"_s) }, [&] {
        addProperty(realms, target);
        return target;
    });

    JSObject* child = realms.object(realm, "({ })"_s);
    JSObject* parent = realms.object(realm, "({ })"_s);
    SimulatedCall(realms, "Object.setPrototypeOf"_s).run({ child, parent }, [&] {
        child->setPrototypeDirect(vm, parent);
        return child;
    });

    JSArray* reparentedArray = realms.array(realm, "(element => [element, 1])('a')"_s);
    JSObject* parentWithIndexedAccessor = realms.object(realm, "Object.defineProperty({ }, 0, { get() { return 1; } })"_s);
    IndexingType indexingModeBefore = reparentedArray->indexingMode();
    SimulatedCall(realms, "Object.setPrototypeOf"_s).run({ reparentedArray, parentWithIndexedAccessor }, [&] {
        reparentedArray->setPrototype(vm, realm, parentWithIndexedAccessor);
        return reparentedArray;
    });
    AOT_TEST_CHECK(reparentedArray->indexingMode() != indexingModeBefore);

    JSArray* array = realms.array(realm, "[1, 2, 3]"_s);
    SimulatedCall(realms, "Array.prototype.push"_s).on(array).run({ jsNumber(4) }, [&] {
        array->push(realm, jsNumber(4));
        return jsNumber(4);
    });
}

static void testConversionOfObjectOperandMayRunCode()
{
    Realms realms(RuntimeValidation::On);
    JSObject* object = realms.object(realms.plain(), "({ toString() { return 'a'; } })"_s);
    SimulatedCall(realms, "String"_s).run({ object }, [&] {
        return realms.evaluate(realms.plain(), "'a'"_s);
    });
    AOT_TEST_CHECK(realms.vm().didEnterVM);
}

// An array becomes a number by way of the string that native code joins: the conversion allocates and enters no JavaScript.
static void testConversionOfObjectOperandMayCollect()
{
    Realms realms(RuntimeValidation::On);
    JSObject* array = realms.object(realms.plain(), "([1, 3])"_s);
    SimulatedCall(realms, "Math.max"_s).run({ array }, [&] {
        realms.vm().heap.collectNow(Synchronousness::Sync, CollectionScope::Eden);
        return jsNaN();
    });
}

// A string that is still in pieces is made whole before it becomes a number.
static void testConversionOfStringOperandMayCollect()
{
    Realms realms(RuntimeValidation::On);
    SimulatedCall(realms, "Math.max"_s).run({ jsNumber(1), jsNontrivialString(realms.vm(), "12"_s) }, [&] {
        realms.vm().heap.collectNow(Synchronousness::Sync, CollectionScope::Eden);
        return jsNumber(12);
    });
}

static void testReadsAreAccepted()
{
    Realms realms(RuntimeValidation::On);
    JSGlobalObject* realm = realms.plain();
    JSObject* function = realms.object(realm, "(function named() { })"_s);
    JSObject* object = realms.object(realm, "({ a: 1 })"_s);
    SimulatedCall(realms).run({ function, object }, [&] {
        function->get(realm, realms.vm().propertyNames->name);
        function->get(realm, realms.vm().propertyNames->prototype);
        object->didBecomePrototype(realms.vm());
        return jsUndefined();
    });
}

// RemoveRedundantBarriers takes every edge to a handler for a collection, so an entry that only throws need not say that it allocates.
static void testCollectionInCallThatThrowsIsAccepted()
{
    Realms realms(RuntimeValidation::On);
    VM& vm = realms.vm();
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    NativeCallExpectation expectation = expectationOfCallWithoutEffects();
    expectation.mayThrow = true;
    SimulatedCall(realms, "Object.freeze"_s).expecting(expectation).run({ jsNumber(1) }, [&] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Eden);
        throwTypeError(realms.plain(), scope, "thrown"_s);
        return JSValue();
    });
    AOT_TEST_CHECK(catchScope.exception());
    catchScope.clearException();
}

// The compiler has matched the first overload of the entry, and holds that the call may throw.
static NativeCallExpectation expectationOfFirstOverload(BuiltinID builtin)
{
    NativeCallExpectation expectation;
    expectation.isKnown = true;
    expectation.mayThrow = true;
    expectation.mayCollect = true;
    expectation.overload = *BuiltinTable::singleton().overloadIDsOf(builtin).begin();
    return expectation;
}

static JSValue throwWithoutRunningCode(Realms& realms)
{
    auto scope = DECLARE_THROW_SCOPE(realms.vm());
    throwTypeError(realms.plain(), scope, "thrown"_s);
    return JSValue();
}

static void testProxyThatAnEntryUsesByProtocolThrowsByItself()
{
    Realms realms(RuntimeValidation::On);
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(realms.vm());
    JSObject* proxy = realms.object(realms.plain(), "new Proxy({ }, { getPrototypeOf: 1 })"_s);
    SimulatedCall(realms, "Object.getPrototypeOf"_s).expecting(expectationOfFirstOverload(Builtins::ObjectGetPrototypeOf)).run({ proxy }, [&] {
        return throwWithoutRunningCode(realms);
    });
    AOT_TEST_CHECK(catchScope.exception());
    catchScope.clearException();
}

static void pushAddingPropertyUnder(Realms& realms, ASCIILiteral name)
{
    JSObject* array = realms.object(realms.plain(), "[1, 2]"_s);
    NativeCallExpectation expectation = expectationOfFirstOverload(Builtins::ArrayPrototypePush);
    expectation.writes = { AbstractHeap::Elements, AbstractHeap::ArrayStorage };
    SimulatedCall(realms, "Array.prototype.push"_s).on(array).expecting(expectation).run({ jsNumber(3) }, [&] {
        array->putDirect(realms.vm(), Identifier::fromString(realms.vm(), name), jsNumber(3));
        return throwWithoutRunningCode(realms);
    });
}

// At the largest length push stores under "4294967295", which is no index, and throws. Compiled code takes every access under a name that looks like a number by key, as it takes an element.
static void testPropertyOfAnArrayUnderANumberThatIsNoIndexIsAnElement()
{
    Realms realms(RuntimeValidation::On);
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(realms.vm());
    pushAddingPropertyUnder(realms, "4294967295"_s);
    AOT_TEST_CHECK(catchScope.exception());
    catchScope.clearException();
}

static void testChangesMadeByCodeThatRanAreAccepted()
{
    Realms realms(RuntimeValidation::On);
    JSObject* object = realms.object(realms.plain(), "({ toJSON() { } })"_s);
    SimulatedCall(realms, "JSON.stringify"_s).run({ object }, [&] {
        realms.evaluate(realms.plain(), "1"_s);
        addProperty(realms, object);
        return jsUndefined();
    });
    AOT_TEST_CHECK(realms.vm().didEnterVM);
}

static void testOperandThatAnotherOperandHoldsUnderAnIndexMayArriveThere()
{
    Realms realms(RuntimeValidation::On);
    JSGlobalObject* realm = realms.plain();
    JSObject* target = realms.object(realm, "({ 0: {} })"_s);
    JSArray* list = realms.array(realm, "[{}]"_s);
    JSValue element = list->getIndexQuickly(0);
    SimulatedCall(realms, "Object.assign"_s).run({ target, element, jsNumber(0), list }, [&] {
        target->putDirectIndex(realm, 0, element);
        return target;
    });
    AOT_TEST_CHECK(target->getIndexQuickly(0) == element);
}

static void testBuiltinThatTheCallNamesIsAccepted()
{
    Realms realms(RuntimeValidation::On);
    JSObject* object = realms.object(realms.plain(), "({ a: 1 })"_s);
    NativeCallExpectation expectation = expectationOfUnknownCallee();
    expectation.name(Builtins::ObjectFreeze);
    SimulatedCall(realms, "Object.freeze"_s).expecting(expectation).run({ object }, [&] {
        return objectConstructorFreeze(realms.plain(), object);
    });
}

static void testNoProofRestsOnNativeCodeWhereUnseenCodeRuns()
{
    Realms realms(RuntimeValidation::On);
    JSObject* object = realms.object(realms.plain(), "({ })"_s);
    NativeCallExpectation expectation = expectationOfUnknownCallee();
    expectation.trustsNativeCode = false;
    SimulatedCall(realms).expecting(expectation).run({ object }, [&] {
        addProperty(realms, object);
        return jsUndefined();
    });
}

static void testNothingIsCheckedWithoutValidation()
{
    Realms realms(RuntimeValidation::Off);
    JSObject* object = realms.object(realms.plain(), "({ })"_s);
    SimulatedCall(realms).run({ object }, [&] {
        addProperty(realms, object);
        return object;
    });
}

template<const ASCIILiteral& name, ValidationFailureKind kind, void (*mismatch)(Realms&)>
static void testMismatchCrashes()
{
    if (!isMismatchRequested()) {
        checkMismatchCrashes(name, kind);
        return;
    }
    Realms realms(RuntimeValidation::On);
    mismatch(realms);
}

static void storeFromFunctionOutsideTable(Realms& realms)
{
    JSObject* object = realms.object(realms.plain(), "({ })"_s);
    SimulatedCall(realms).expecting(expectationOfUnknownCallee()).run({ object }, [&] {
        addProperty(realms, object);
        return jsUndefined();
    });
}

static void freezeFromBuiltinThatTheCallDoesNotName(Realms& realms)
{
    JSObject* object = realms.object(realms.plain(), "({ a: 1 })"_s);
    SimulatedCall(realms, "Object.freeze"_s).expecting(expectationOfUnknownCallee()).run({ object }, [&] {
        return objectConstructorFreeze(realms.plain(), object);
    });
}

static void freezeFromEntryThatInspects(Realms& realms)
{
    JSObject* object = realms.object(realms.plain(), "({ a: 1 })"_s);
    SimulatedCall(realms, "Object.isFrozen"_s).run({ object }, [&] {
        objectConstructorFreeze(realms.plain(), object);
        return jsBoolean(true);
    });
}

static void storeUnderKeyThatNoActionCovers(Realms& realms)
{
    JSArray* array = realms.array(realms.plain(), "[1]"_s);
    SimulatedCall(realms, "Array.prototype.push"_s).on(array).run({ jsNumber(2) }, [&] {
        addProperty(realms, array);
        return jsNumber(1);
    });
}

static void runCodeFromEntryWithoutCode(Realms& realms)
{
    SimulatedCall(realms, "Math.abs"_s).run({ jsNumber(1) }, [&] {
        realms.evaluate(realms.plain(), "1"_s);
        return jsNumber(1);
    });
}

static void writeHeapThatTheCallDoesNotWrite(Realms& realms)
{
    JSObject* object = realms.object(realms.plain(), "({ a: 1 })"_s);
    SimulatedCall(realms, "Object.freeze"_s).expecting(expectationOfCallWithoutEffects()).run({ object }, [&] {
        return objectConstructorFreeze(realms.plain(), object);
    });
}

static void throwFromCallThatDoesNotThrow(Realms& realms)
{
    SimulatedCall(realms, "Object.freeze"_s).expecting(expectationOfCallWithoutEffects()).run({ jsNumber(1) }, [&] {
        auto scope = DECLARE_THROW_SCOPE(realms.vm());
        throwTypeError(realms.plain(), scope, "thrown"_s);
        return JSValue();
    });
}

static void throwFromEntryThatDoesNotThrow(Realms& realms)
{
    JSObject* object = realms.object(realms.plain(), "({ })"_s);
    SimulatedCall(realms, "Object.getPrototypeOf"_s).expecting(expectationOfFirstOverload(Builtins::ObjectGetPrototypeOf)).run({ object }, [&] {
        return throwWithoutRunningCode(realms);
    });
}

static void addNamedPropertyFromCallThatWritesElements(Realms& realms)
{
    pushAddingPropertyUnder(realms, "added"_s);
}

static void collectInCallThatDoesNotCollect(Realms& realms)
{
    SimulatedCall(realms, "Object.freeze"_s).expecting(expectationOfCallWithoutEffects()).run({ jsNumber(1) }, [&] {
        realms.vm().heap.collectNow(Synchronousness::Sync, CollectionScope::Eden);
        return jsNumber(1);
    });
}

static void collectFromEntryThatDoesNotAllocate(Realms& realms)
{
    SimulatedCall(realms, "Math.max"_s).run({ jsNumber(1) }, [&] {
        realms.vm().heap.collectNow(Synchronousness::Sync, CollectionScope::Eden);
        return jsNumber(1);
    });
}

static void returnOperandThatIsOnlyInspected(Realms& realms)
{
    JSObject* object = realms.object(realms.plain(), "({ })"_s);
    SimulatedCall(realms, "Object.isFrozen"_s).run({ object }, [&] {
        return object;
    });
}

static void keepOperandThatIsOnlyInspected(Realms& realms)
{
    JSObject* target = realms.object(realms.plain(), "({ a: 1 })"_s);
    JSObject* source = realms.object(realms.plain(), "({ a: 2 })"_s);
    SimulatedCall(realms, "Object.assign"_s).run({ target, source }, [&] {
        target->putDirect(realms.vm(), Identifier::fromString(realms.vm(), "a"_s), source);
        return target;
    });
}

static constexpr ASCIILiteral programCallingShellFunction = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
data d0 root=Array{contents=anything elements=[Number{value=1}, Number{value=2}, Number{value=3}]} module=m0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = Data data=d0
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="ensureArrayStorage" [isFreeName]
    v3 = Call v2, -, v0 calleeName="ensureArrayStorage"
    v4 = PropGet v1 key="print" [isFreeName]
    v5 = PropGet v0 key="length"
    v6 = Call v4, -, v5 calleeName="print"
    v7 = Undefined
    Return v7
}
)IR"_s;

static void testCompiledCallOfFunctionOutsideTable()
{
    checkOutputOfProgramInShell(programCallingShellFunction, "3\n"_s, { });

    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileForShellOnThisThread(programCallingShellFunction, { "validate"_s });
    });
    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments { makeString("--aotImagePath="_s, image.path()), String("-e"_s), makeString("runCompiledEntryPoint("_s, output->entryModuleIDs[0], ')') };
    checkFailure(ChildProcessLauncher(shellPath(), { }).run(arguments.span()), ValidationFailureKind::NativeCallChangedOperand);
}

static constexpr ASCIILiteral programSerializingSetterWithoutGetter = R"IR(layout L0 module=m0 fields=[{key="s" declared={type=anything} flags=[IsRequired]}, {key="x" declared={type=number flags=[IsExpected]} flags=[IsRequired]}]
module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewObject layout=L0
    v1 = NewClosure function=f1
    DefineSetter v0, v1 key="s"
    v3 = Integer integer=1
    PropDefine v0, v3 key="x"
    v5 = FinishObject v0
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="JSON" [isFreeName]
    v8 = PropGet v7 key="stringify"
    v9 = Call v8, v7, v5 calleeKey="stringify" calleeName="stringify"
    v10 = PropGet v6 key="print" [isFreeName]
    v11 = Call v10, -, v9 calleeName="print"
    v12 = Undefined
    Return v12
}
function f1 name="s" module=m0 kind=Setter flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Undefined
    Return v1
}
)IR"_s;

// The object reaches no native function that the table lacks: the engine reads the accessor by calling its own function for a missing getter.
static void testReadOfSetterWithoutGetterIsNoCall()
{
    checkOutputOfProgramInShell(programSerializingSetterWithoutGetter, "{\"x\":1}\n"_s, { });
    checkOutputOfProgramInShell(programSerializingSetterWithoutGetter, "{\"x\":1}\n"_s, { "validate"_s });
}

static constexpr ASCIILiteral storeFromFunctionOutsideTableTest = "native-call-validation/function-outside-the-table-stores-a-property"_s;
static constexpr ASCIILiteral freezeFromBuiltinThatTheCallDoesNotNameTest = "native-call-validation/built-in-that-the-call-does-not-name-freezes"_s;
static constexpr ASCIILiteral freezeFromEntryThatInspectsTest = "native-call-validation/entry-that-inspects-freezes"_s;
static constexpr ASCIILiteral storeUnderKeyThatNoActionCoversTest = "native-call-validation/store-under-a-key-that-no-action-covers"_s;
static constexpr ASCIILiteral runCodeFromEntryWithoutCodeTest = "native-call-validation/entry-without-code-runs-code"_s;
// What stands for a run-time operation or a stub that compiled code calls: the operand is photographed, the body does to it what the callee would, and the operand is compared.
static void runPhotographed(Realms& realms, JSObject* operand, AbstractHeaps written, const Invocable<void()> auto& body)
{
    photographOperand(realms.vm(), operand, written, 0);
    body();
    compareOperandWithPhotograph(realms.vm(), operand, PhotographedCall::encode(0, 0), FunctionID(0u), Typed::ValueID(0u));
}

static void testOperationMayWriteWhatItLists()
{
    Realms realms(RuntimeValidation::On);
    JSArray* array = realms.array(realms.plain(), "[1, 2, 3]"_s);
    runPhotographed(realms, array, { }, [] { });
    runPhotographed(realms, array, { AbstractHeap::ArrayStorage, AbstractHeap::Elements }, [&] {
        array->push(realms.plain(), jsNumber(4));
    });
    runPhotographed(realms, array, { AbstractHeap::NamedProperties }, [&] {
        addProperty(realms, array);
    });
    JSObject* map = realms.object(realms.plain(), "new Map([[1, 2]])"_s);
    runPhotographed(realms, map, { AbstractHeap::CollectionStorage }, [&] {
        uncheckedDowncast<JSMap>(map)->set(realms.plain(), jsNumber(3), jsNumber(4));
    });
    // A call that throws is not compared, and the photograph of the next call takes the place.
    photographOperand(realms.vm(), array, { }, 0);
    array->push(realms.plain(), jsNumber(5));
    runPhotographed(realms, array, { }, [] { });
    // An object that is handed over twice may change as either operand may.
    photographOperand(realms.vm(), array, { }, 0);
    photographOperand(realms.vm(), array, { AbstractHeap::ArrayStorage }, 1);
    array->push(realms.plain(), jsNumber(6));
    compareOperandWithPhotograph(realms.vm(), array, PhotographedCall::encode(0, 0), FunctionID(0u), Typed::ValueID(0u));
    compareOperandWithPhotograph(realms.vm(), array, PhotographedCall::encode(1, 0), FunctionID(0u), Typed::ValueID(0u));
}

static void changeOperandThatTheOperationOnlyReads(Realms& realms)
{
    JSArray* target = realms.array(realms.plain(), "[1]"_s);
    JSArray* source = realms.array(realms.plain(), "[2]"_s);
    photographOperand(realms.vm(), target, { AbstractHeap::ArrayStorage, AbstractHeap::Elements }, 0);
    photographOperand(realms.vm(), source, { }, 1);
    source->push(realms.plain(), jsNumber(3));
    compareOperandWithPhotograph(realms.vm(), target, PhotographedCall::encode(0, 0), FunctionID(0u), Typed::ValueID(0u));
    compareOperandWithPhotograph(realms.vm(), source, PhotographedCall::encode(1, 0), FunctionID(0u), Typed::ValueID(0u));
}

static void growArrayInOperationThatWritesNoArrayStorage(Realms& realms)
{
    JSArray* array = realms.array(realms.plain(), "[1, 2, 3]"_s);
    runPhotographed(realms, array, { AbstractHeap::NamedProperties, AbstractHeap::ObjectFields, AbstractHeap::AllocatorState }, [&] {
        array->push(realms.plain(), jsNumber(4));
    });
}

static void changeTableInOperationThatOnlyReads(Realms& realms)
{
    JSObject* map = realms.object(realms.plain(), "new Map([[1, 2]])"_s);
    runPhotographed(realms, map, { AbstractHeap::AllocatorState }, [&] {
        uncheckedDowncast<JSMap>(map)->set(realms.plain(), jsNumber(3), jsNumber(4));
    });
}

static constexpr ASCIILiteral growArrayInOperationThatWritesNoArrayStorageTest = "native-call-validation/operation-changes-array-storage-that-it-does-not-list"_s;
static constexpr ASCIILiteral changeOperandThatTheOperationOnlyReadsTest = "native-call-validation/operation-changes-an-operand-that-it-only-reads"_s;
static constexpr ASCIILiteral changeTableInOperationThatOnlyReadsTest = "native-call-validation/operation-that-only-reads-changes-a-table"_s;
static constexpr ASCIILiteral writeHeapThatTheCallDoesNotWriteTest = "native-call-validation/call-writes-a-heap-outside-its-effects"_s;
static constexpr ASCIILiteral throwFromCallThatDoesNotThrowTest = "native-call-validation/call-that-does-not-throw-throws"_s;
static constexpr ASCIILiteral throwFromEntryThatDoesNotThrowTest = "native-call-validation/entry-that-does-not-throw-throws"_s;
static constexpr ASCIILiteral addNamedPropertyFromCallThatWritesElementsTest = "native-call-validation/call-that-writes-elements-adds-a-named-property"_s;
static constexpr ASCIILiteral collectInCallThatDoesNotCollectTest = "native-call-validation/call-that-does-not-collect-collects"_s;
static constexpr ASCIILiteral collectFromEntryThatDoesNotAllocateTest = "native-call-validation/entry-that-does-not-allocate-collects"_s;
static constexpr ASCIILiteral returnOperandThatIsOnlyInspectedTest = "native-call-validation/inspected-operand-is-returned"_s;
static constexpr ASCIILiteral keepOperandThatIsOnlyInspectedTest = "native-call-validation/inspected-operand-is-stored-in-another"_s;

std::span<const NativeTest> nativeCallValidationTests()
{
    using enum ValidationFailureKind;
    static constexpr NativeTest tests[] = {
        { "native-call-validation/declared-changes-are-accepted"_s, testDeclaredChangesAreAccepted },
        { "native-call-validation/reads-are-accepted"_s, testReadsAreAccepted },
        { "native-call-validation/conversion-of-an-object-operand-may-run-code"_s, testConversionOfObjectOperandMayRunCode },
        { "native-call-validation/conversion-of-an-object-operand-may-collect"_s, testConversionOfObjectOperandMayCollect },
        { "native-call-validation/conversion-of-a-string-operand-may-collect"_s, testConversionOfStringOperandMayCollect },
        { "native-call-validation/changes-made-by-code-that-ran-are-accepted"_s, testChangesMadeByCodeThatRanAreAccepted },
        { "native-call-validation/collection-in-a-call-that-throws-is-accepted"_s, testCollectionInCallThatThrowsIsAccepted },
        { "native-call-validation/proxy-that-an-entry-uses-by-a-protocol-throws-by-itself"_s, testProxyThatAnEntryUsesByProtocolThrowsByItself },
        { "native-call-validation/property-of-an-array-under-a-number-that-is-no-index-is-an-element"_s, testPropertyOfAnArrayUnderANumberThatIsNoIndexIsAnElement },
        { "native-call-validation/operand-that-another-operand-holds-under-an-index-may-arrive-there"_s, testOperandThatAnotherOperandHoldsUnderAnIndexMayArriveThere },
        { "native-call-validation/built-in-that-the-call-names-is-accepted"_s, testBuiltinThatTheCallNamesIsAccepted },
        { "native-call-validation/no-proof-rests-on-native-code-where-unseen-code-runs"_s, testNoProofRestsOnNativeCodeWhereUnseenCodeRuns },
        { "native-call-validation/nothing-is-checked-without-validation"_s, testNothingIsCheckedWithoutValidation },
        { storeFromFunctionOutsideTableTest, testMismatchCrashes<storeFromFunctionOutsideTableTest, NativeCallChangedOperand, storeFromFunctionOutsideTable> },
        { freezeFromBuiltinThatTheCallDoesNotNameTest, testMismatchCrashes<freezeFromBuiltinThatTheCallDoesNotNameTest, NativeCallChangedOperand, freezeFromBuiltinThatTheCallDoesNotName> },
        { freezeFromEntryThatInspectsTest, testMismatchCrashes<freezeFromEntryThatInspectsTest, NativeCallChangedOperand, freezeFromEntryThatInspects> },
        { storeUnderKeyThatNoActionCoversTest, testMismatchCrashes<storeUnderKeyThatNoActionCoversTest, NativeCallChangedOperand, storeUnderKeyThatNoActionCovers> },
        { runCodeFromEntryWithoutCodeTest, testMismatchCrashes<runCodeFromEntryWithoutCodeTest, NativeCallRanCode, runCodeFromEntryWithoutCode> },
        { writeHeapThatTheCallDoesNotWriteTest, testMismatchCrashes<writeHeapThatTheCallDoesNotWriteTest, NativeCallWroteHeap, writeHeapThatTheCallDoesNotWrite> },
        { throwFromCallThatDoesNotThrowTest, testMismatchCrashes<throwFromCallThatDoesNotThrowTest, NativeCallThrew, throwFromCallThatDoesNotThrow> },
        { throwFromEntryThatDoesNotThrowTest, testMismatchCrashes<throwFromEntryThatDoesNotThrowTest, NativeCallThrew, throwFromEntryThatDoesNotThrow> },
        { addNamedPropertyFromCallThatWritesElementsTest, testMismatchCrashes<addNamedPropertyFromCallThatWritesElementsTest, NativeCallWroteHeap, addNamedPropertyFromCallThatWritesElements> },
        { collectInCallThatDoesNotCollectTest, testMismatchCrashes<collectInCallThatDoesNotCollectTest, NativeCallCollected, collectInCallThatDoesNotCollect> },
        { collectFromEntryThatDoesNotAllocateTest, testMismatchCrashes<collectFromEntryThatDoesNotAllocateTest, NativeCallCollected, collectFromEntryThatDoesNotAllocate> },
        { returnOperandThatIsOnlyInspectedTest, testMismatchCrashes<returnOperandThatIsOnlyInspectedTest, NativeCallKeptOperand, returnOperandThatIsOnlyInspected> },
        { keepOperandThatIsOnlyInspectedTest, testMismatchCrashes<keepOperandThatIsOnlyInspectedTest, NativeCallKeptOperand, keepOperandThatIsOnlyInspected> },
        { "native-call-validation/operation-may-write-what-it-lists"_s, testOperationMayWriteWhatItLists },
        { growArrayInOperationThatWritesNoArrayStorageTest, testMismatchCrashes<growArrayInOperationThatWritesNoArrayStorageTest, OperationWroteHeap, growArrayInOperationThatWritesNoArrayStorage> },
        { changeOperandThatTheOperationOnlyReadsTest, testMismatchCrashes<changeOperandThatTheOperationOnlyReadsTest, OperationWroteHeap, changeOperandThatTheOperationOnlyReads> },
        { changeTableInOperationThatOnlyReadsTest, testMismatchCrashes<changeTableInOperationThatOnlyReadsTest, OperationWroteHeap, changeTableInOperationThatOnlyReads> },
        { "native-call-validation/compiled-call-of-a-function-outside-the-table"_s, testCompiledCallOfFunctionOutsideTable },
        { "native-call-validation/read-of-a-setter-without-getter-is-no-call"_s, testReadOfSetterWithoutGetterIsNoCall },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
