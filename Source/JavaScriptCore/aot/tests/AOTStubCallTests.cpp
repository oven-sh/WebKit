/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTSlimClosure.h"
#include "AOTStubRunner.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsCalls.h"
#include "AOTVector.h"
#include "CallFrame.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "NativeExecutable.h"
#include "Structure.h"
#include <array>
#include <bit>
#include <span>
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr uint64_t boxedInt32(uint32_t number) { return static_cast<uint64_t>(JSValue::NumberTag) | number; }

static constexpr unsigned countInKey = 2;
static constexpr uint64_t undefinedBits = JSValue::ValueUndefined;
static constexpr uint64_t nullBits = JSValue::ValueNull;
static constexpr uint64_t thisValue = boxedInt32(70);
static constexpr uint64_t firstArgument = boxedInt32(71);
static constexpr uint64_t secondArgument = boxedInt32(72);
static constexpr uint64_t resultOfHostFunction = boxedInt32(73);

namespace {

using FakeStructure = FakeObject<sizeof(Structure)>;
using FakeClosure = FakeObject<offsetOfClosureEntry + sizeof(uint64_t) + sizeof(uint64_t)>;
using FakeSlimClosure = FakeObject<offsetOfSlimClosureWord + sizeof(uint64_t)>;
using FakeFunction = FakeObject<sizeof(JSFunction)>;
using FakeBoundFunction = FakeObject<sizeof(JSBoundFunction)>;
using FakeExecutable = FakeObject<sizeof(NativeExecutable)>;

enum class StructureOf : uint8_t { PlainClosure, SlimClosure, AsyncClosure, SlimClosureWithProperties, ClosureOfOtherRealm, HostFunction, BoundFunction, OtherFunction, Object };
constexpr unsigned numberOfStructures = static_cast<unsigned>(StructureOf::Object) + 1;

struct HostCall {
    unsigned numberOfCalls;
    uint64_t realm;
    uint64_t callee;
    uint64_t argumentCountIncludingThis;
    uint64_t thisValue;
    std::array<uint64_t, countInKey> arguments;
};

class Calls {
    WTF_MAKE_NONCOPYABLE(Calls);
public:
    static constexpr uint32_t functionWithProperties = 3;

    explicit Calls(bool hasOneInstance)
        : runner(StubOptions { .hasOneInstance = hasOneInstance, .targetFeatures = CPUFeatureSet::buildTarget() })
    {
        Instance& instance = runner.instance();
        instance.prefix().structureIDBase = m_structures[0].address() & ~static_cast<uint64_t>(UINT32_MAX);
        instance.globalObject = std::bit_cast<JSGlobalObject*>(m_realm.address());
        for (FakeStructure& structure : m_structures) {
            AOT_TEST_CHECK((structure.address() & ~static_cast<uint64_t>(UINT32_MAX)) == instance.prefix().structureIDBase);
            structure.at<uint64_t>(Structure::realmOffset()) = m_realm.address();
            structure.at<uint64_t>(Structure::classInfoOffset()) = m_classInfo.address();
        }
        structureOf(StructureOf::ClosureOfOtherRealm).at<uint64_t>(Structure::realmOffset()) = m_otherRealm.address();
        instance.closures.closureHeaders[0] = idOf(StructureOf::PlainClosure);
        instance.slimClosureHeaders[0] = idOf(StructureOf::SlimClosure);
        instance.functionTables.closureWords = m_closureWords.data();
        instance.realm.hostFunctionStructureID = idOf(StructureOf::HostFunction);
        instance.realm.boundFunctionStructureID = idOf(StructureOf::BoundFunction);
        instance.realm.functionPrototypeCall = std::bit_cast<JSCell*>(m_functionPrototypeCall.address());
    }

    FakeStructure& structureOf(StructureOf kind) { return m_structures[static_cast<unsigned>(kind)]; }
    uint32_t idOf(StructureOf kind) { return static_cast<uint32_t>(structureOf(kind).address()); }
    uint64_t realm() const { return m_realm.address(); }
    uint64_t functionPrototypeCall() const { return m_functionPrototypeCall.address(); }

    void initializeClosure(FakeClosure& closure, StructureOf structure, uint64_t word)
    {
        closure.setHeader(idOf(structure), CompiledClosureType);
        closure.at<uint64_t>(offsetOfClosureEntry) = word;
    }

    void initializeSlimClosure(FakeSlimClosure& closure, uint64_t word)
    {
        closure.setHeader(idOf(StructureOf::SlimClosure), SlimClosureType);
        closure.at<uint8_t>(JSCell::typeInfoFlagsOffset()) = OverridesGetCallData;
        closure.at<uint64_t>(offsetOfSlimClosureWord) = word;
    }

    void initializeSlimClosureWithProperties(FakeSlimClosure& closure, uint64_t word)
    {
        constexpr ptrdiff_t offsetOfButterfly = 32;
        static_assert(ReifiedSlimClosure::offsetOfFunctionIndexInButterfly() < 0 && ReifiedSlimClosure::offsetOfFunctionIndexInButterfly() >= -offsetOfButterfly);
        closure.setHeader(idOf(StructureOf::SlimClosureWithProperties), SlimClosureWithPropertiesType);
        closure.at<uint64_t>(offsetOfSlimClosureWord) = m_storage.address() + offsetOfButterfly;
        m_storage.at<uint32_t>(offsetOfButterfly + ReifiedSlimClosure::offsetOfFunctionIndexInButterfly()) = functionWithProperties;
        m_closureWords[functionWithProperties] = word;
    }

    StubRunner runner;

private:
    std::array<FakeStructure, numberOfStructures> m_structures;
    std::array<uint64_t, functionWithProperties + 1> m_closureWords { };
    FakeObject<64> m_storage;
    FakeObject<16> m_realm;
    FakeObject<16> m_otherRealm;
    FakeObject<16> m_classInfo;
    FakeObject<16> m_functionPrototypeCall;
};

} // anonymous namespace

static HostCall lastHostCall;

static JSC_DECLARE_HOST_FUNCTION(recordHostCall);

JSC_DEFINE_HOST_FUNCTION(recordHostCall, (JSGlobalObject* realm, CallFrame* callFrame))
{
    auto slot = [&](CallFrameSlot slot, unsigned index = 0) {
        return reinterpret_cast<const uint64_t*>(callFrame)[static_cast<int>(slot) + index];
    };
    ++lastHostCall.numberOfCalls;
    lastHostCall.realm = std::bit_cast<uintptr_t>(realm);
    lastHostCall.callee = slot(CallFrameSlot::callee);
    lastHostCall.argumentCountIncludingThis = slot(CallFrameSlot::argumentCountIncludingThis);
    lastHostCall.thisValue = slot(CallFrameSlot::thisArgument);
    for (unsigned i = 0; i < countInKey; ++i)
        lastHostCall.arguments[i] = slot(CallFrameSlot::firstArgument, i);
    return static_cast<EncodedJSValue>(resultOfHostFunction);
}

static void checkLandedInFunction(const StubRun& run, uint64_t callee, uint64_t expectedThis, std::span<const uint64_t> expectedArguments)
{
    const LandedCall& landed = run.landedCall;
    AOT_TEST_CHECK(landed.place == LandedCall::Place::CompiledFunction && run.result == StubRunner::resultOfCompiledFunction && !run.calledOperation);
    AOT_TEST_CHECK(landed.values.callee == callee && landed.values.thisValue == expectedThis);
    AOT_TEST_CHECK(equalSpans(std::span(landed.values.arguments).first(expectedArguments.size()), expectedArguments));
}

static void checkLandedInEngine(const StubRun& run, uint64_t callee, uint64_t expectedThis, std::span<const uint64_t> expectedArguments)
{
    const LandedCall& landed = run.landedCall;
    AOT_TEST_CHECK(landed.place == LandedCall::Place::EngineCallee && run.result == StubRunner::resultOfEngineCallee && !run.calledOperation);
    AOT_TEST_CHECK(landed.calleeInRegister == callee && landed.values.callee == callee && landed.values.thisValue == expectedThis);
    AOT_TEST_CHECK(static_cast<uint32_t>(landed.count) == expectedArguments.size() + 1);
    AOT_TEST_CHECK(equalSpans(std::span(landed.values.arguments).first(expectedArguments.size()), expectedArguments));
}

static void checkCallOfClosure(Calls& calls, uint64_t closure, unsigned numberOfParameters)
{
    ArgumentRegisters filled;
    filled.fill(undefinedBits);
    filled[0] = firstArgument;
    filled[1] = secondArgument;
    std::span<const uint64_t> passed = std::span(filled).first(countInKey);
    std::span<const uint64_t> expected = numberOfParameters > countInKey ? std::span<const uint64_t>(filled) : passed;

    StubRun run = calls.runner.runCall(callValue(countInKey), closure, thisValue, { firstArgument, secondArgument });
    checkLandedInFunction(run, closure, thisValue, expected);
    AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == countInKey);

    run = calls.runner.runCall(callValueWithoutThis(countInKey), closure, thisValue, { firstArgument, secondArgument });
    checkLandedInFunction(run, closure, undefinedBits, expected);

    run = calls.runner.runCall(callValueCell(countInKey), closure, thisValue, { firstArgument, secondArgument });
    checkLandedInFunction(run, closure, thisValue, expected);

    run = calls.runner.runCall(callValueCellWithoutThis(countInKey), closure, thisValue, { firstArgument, secondArgument });
    checkLandedInFunction(run, closure, undefinedBits, expected);
    AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == countInKey);

    for (StubKey key : { callValueWithCount(), callCellWithCount() }) {
        run = calls.runner.runCall(key, closure, thisValue, { firstArgument, secondArgument });
        checkLandedInFunction(run, closure, thisValue, filled);
        AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == countInKey);

        run = calls.runner.runCall(key, closure, thisValue, { });
        filled.fill(undefinedBits);
        checkLandedInFunction(run, closure, thisValue, filled);
        AOT_TEST_CHECK(!run.landedCall.count);
        filled[0] = firstArgument;
        filled[1] = secondArgument;
    }
}

// A call that passes an argument in every register leaves all of them as they are.
static void checkCallThatFillsEveryRegister(Calls& calls, uint64_t closure)
{
    static_assert(numberOfArgumentGPRs == 4 || numberOfArgumentGPRs == 8);
    for (StubKey key : { callValueWithCount(), callCellWithCount() }) {
        StubRun run = numberOfArgumentGPRs == 4
            ? calls.runner.runCall(key, closure, thisValue, { firstArgument, secondArgument, firstArgument, secondArgument })
            : calls.runner.runCall(key, closure, thisValue, { firstArgument, secondArgument, firstArgument, secondArgument, firstArgument, secondArgument, firstArgument, secondArgument });
        ArgumentRegisters passed;
        for (unsigned i = 0; i < numberOfArgumentGPRs; ++i)
            passed[i] = i % 2 ? secondArgument : firstArgument;
        checkLandedInFunction(run, closure, thisValue, passed);
        AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == numberOfArgumentGPRs);
    }
}

static void testCallsOfClosures(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    for (unsigned numberOfParameters : { 0u, countInKey, countInKey + 1, numberOfArgumentGPRs }) {
        uint64_t word = calls.runner.closureWord(numberOfParameters);
        FakeClosure plain;
        FakeClosure async;
        FakeSlimClosure slim;
        FakeSlimClosure slimWithProperties;
        calls.initializeClosure(plain, StructureOf::PlainClosure, word);
        calls.initializeClosure(async, StructureOf::AsyncClosure, word);
        calls.initializeSlimClosure(slim, word);
        calls.initializeSlimClosureWithProperties(slimWithProperties, word);
        for (uint64_t closure : { plain.address(), async.address(), slim.address(), slimWithProperties.address() }) {
            checkCallOfClosure(calls, closure, numberOfParameters);
            checkCallThatFillsEveryRegister(calls, closure);
        }
    }
}

// In an image with one instance the word of a slim closure that takes the arguments says all there is to know: the header of the cell is not read.
static void testSlimClosuresAreEnteredByTheirWordAlone()
{
    Calls calls(true);
    FakeSlimClosure slim;
    calls.initializeSlimClosure(slim, calls.runner.closureWord(countInKey));
    slim.setHeader(0, SlimClosureType);
    const std::array passed { firstArgument, secondArgument };
    for (StubKey key : { callValue(countInKey), callValueCell(countInKey) })
        checkLandedInFunction(calls.runner.runCall(key, slim.address(), thisValue, { firstArgument, secondArgument }), slim.address(), thisValue, passed);

    ArgumentRegisters filled;
    filled.fill(undefinedBits);
    filled[0] = firstArgument;
    filled[1] = secondArgument;
    FakeSlimClosure declaresMore;
    calls.initializeSlimClosure(declaresMore, calls.runner.closureWord(countInKey + 1));
    declaresMore.setHeader(0, SlimClosureType);
    for (StubKey key : { callValue(countInKey), callValueCell(countInKey), callValueWithCount(), callCellWithCount() })
        checkLandedInFunction(calls.runner.runCall(key, declaresMore.address(), thisValue, { firstArgument, secondArgument }), declaresMore.address(), thisValue, filled);
}

static void checkCallOfEngineCallee(Calls& calls, uint64_t callee)
{
    const std::array passed { firstArgument, secondArgument };
    checkLandedInEngine(calls.runner.runCall(callValue(countInKey), callee, thisValue, { firstArgument, secondArgument }), callee, thisValue, passed);
    checkLandedInEngine(calls.runner.runCall(callValueWithoutThis(countInKey), callee, thisValue, { firstArgument, secondArgument }), callee, undefinedBits, passed);
    checkLandedInEngine(calls.runner.runCall(callValueCellWithoutThis(countInKey), callee, thisValue, { firstArgument, secondArgument }), callee, undefinedBits, passed);
    checkLandedInEngine(calls.runner.runCall(callValueWithCount(), callee, thisValue, { firstArgument, secondArgument }), callee, thisValue, passed);
    checkLandedInEngine(calls.runner.runCall(callCellWithCount(), callee, thisValue, { firstArgument, secondArgument }), callee, thisValue, passed);
    checkLandedInEngine(calls.runner.runCall(callValue(0), callee, thisValue, { }), callee, thisValue, { });
}

static void testCallsOfEngineCallees(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    uint64_t wordOfClass = calls.runner.closureWord(0, { .isNotCallable = true, .takesNewTarget = true });
    FakeClosure classObject;
    FakeClosure classObjectWithOtherStructure;
    FakeClosure closureOfOtherRealm;
    FakeSlimClosure slimClosureThatIsNotCallable;
    FakeFunction interpretedFunction;
    FakeExecutable functionExecutable;
    FakeObject<16> objectWithCallData;
    calls.initializeClosure(classObject, StructureOf::PlainClosure, wordOfClass);
    calls.initializeClosure(classObjectWithOtherStructure, StructureOf::AsyncClosure, wordOfClass);
    calls.initializeClosure(closureOfOtherRealm, StructureOf::ClosureOfOtherRealm, calls.runner.closureWord(0));
    calls.initializeSlimClosure(slimClosureThatIsNotCallable, wordOfClass);
    functionExecutable.setHeader(0, FunctionExecutableType);
    interpretedFunction.setHeader(calls.idOf(StructureOf::OtherFunction), JSFunctionType);
    interpretedFunction.at<uint64_t>(JSFunction::offsetOfExecutableOrRareData()) = functionExecutable.address();
    objectWithCallData.setHeader(calls.idOf(StructureOf::Object), ObjectType);
    objectWithCallData.at<uint8_t>(JSCell::typeInfoFlagsOffset()) = OverridesGetCallData;

    for (uint64_t callee : { classObject.address(), classObjectWithOtherStructure.address(), closureOfOtherRealm.address(), slimClosureThatIsNotCallable.address(), interpretedFunction.address(), objectWithCallData.address() })
        checkCallOfEngineCallee(calls, callee);
}

static void checkThrowsUnlessCallable(const StubRun& run, uint64_t callee)
{
    AOT_TEST_CHECK(run.landedCall.place == LandedCall::Place::Nowhere && run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == callee);
}

static void testCallsOfNonCells(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    calls.runner.recordCallsOf(Entry::operationAOTThrowUnlessCallable);
    FakeObject<16> objectWithoutCallData;
    objectWithoutCallData.setHeader(calls.idOf(StructureOf::Object), FinalObjectType);
    for (uint64_t callee : { undefinedBits, boxedInt32(5), nullBits, objectWithoutCallData.address() }) {
        checkThrowsUnlessCallable(calls.runner.runCall(callValue(countInKey), callee, thisValue, { firstArgument, secondArgument }), callee);
        checkThrowsUnlessCallable(calls.runner.runCall(callValueWithoutThis(countInKey), callee, thisValue, { firstArgument, secondArgument }), callee);
        checkThrowsUnlessCallable(calls.runner.runCall(callValue(0), callee, thisValue, { }), callee);
        checkThrowsUnlessCallable(calls.runner.runCall(callValueWithCount(), callee, thisValue, { firstArgument, secondArgument }), callee);
    }
}

static void checkCallOfHostFunction(Calls& calls, StubKey key, uint64_t callee, uint64_t expectedThis)
{
    lastHostCall = { };
    StubRun run = calls.runner.runCall(key, callee, thisValue, { firstArgument, secondArgument });
    AOT_TEST_CHECK(run.landedCall.place == LandedCall::Place::Nowhere && !run.calledOperation);
    AOT_TEST_CHECK(run.result == resultOfHostFunction);
    AOT_TEST_CHECK(lastHostCall.numberOfCalls == 1 && lastHostCall.realm == calls.realm() && lastHostCall.callee == callee);
    AOT_TEST_CHECK(static_cast<uint32_t>(lastHostCall.argumentCountIncludingThis) == countInKey + 1 && lastHostCall.thisValue == expectedThis);
    AOT_TEST_CHECK(lastHostCall.arguments[0] == firstArgument && lastHostCall.arguments[1] == secondArgument);
}

static void testCallsOfHostFunctions(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    FakeExecutable executable;
    executable.setHeader(0, NativeExecutableType);
    executable.at<const void*>(NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall)) = TaggedNativeFunction(recordHostCall).taggedPtr();

    for (StructureOf structure : { StructureOf::HostFunction, StructureOf::OtherFunction }) {
        FakeFunction function;
        function.setHeader(calls.idOf(structure), JSFunctionType);
        function.at<uint64_t>(JSCallee::offsetOfScopeChain()) = calls.realm();
        function.at<uint64_t>(JSFunction::offsetOfExecutableOrRareData()) = executable.address();
        checkCallOfHostFunction(calls, callValue(countInKey), function.address(), thisValue);
        checkCallOfHostFunction(calls, callValueWithoutThis(countInKey), function.address(), undefinedBits);
    }
}

static void initializeBoundFunction(Calls& calls, FakeBoundFunction& function, uint64_t target, uint64_t boundThis, std::span<const uint64_t> boundArguments)
{
    AOT_TEST_CHECK(boundArguments.size() <= JSBoundFunction::maxEmbeddedArgs);
    function.setHeader(calls.idOf(StructureOf::BoundFunction), JSFunctionType);
    function.at<uint64_t>(JSBoundFunction::offsetOfTargetFunction()) = target;
    function.at<uint64_t>(JSBoundFunction::offsetOfBoundThis()) = boundThis;
    function.at<uint32_t>(JSBoundFunction::offsetOfBoundArgsLength()) = static_cast<uint32_t>(boundArguments.size());
    for (size_t i = 0; i < boundArguments.size(); ++i)
        function.at<uint64_t>(JSBoundFunction::offsetOfBoundArgs() + i * sizeof(uint64_t)) = boundArguments[i];
}

static void testCallsOfBoundFunctions(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    constexpr uint64_t boundThis = boxedInt32(80);
    constexpr std::array boundArguments { boxedInt32(81), boxedInt32(82), boxedInt32(83) };
    static_assert(boundArguments.size() == JSBoundFunction::maxEmbeddedArgs);
    FakeClosure target;
    calls.initializeClosure(target, StructureOf::PlainClosure, calls.runner.closureWord(0));

    for (size_t numberOfBoundArguments = 0; numberOfBoundArguments <= boundArguments.size(); ++numberOfBoundArguments) {
        FakeBoundFunction function;
        initializeBoundFunction(calls, function, target.address(), boundThis, std::span(boundArguments).first(numberOfBoundArguments));
        StubRun run = calls.runner.runCall(callValue(countInKey), function.address(), thisValue, { firstArgument, secondArgument });
        Vector<uint64_t> expected(std::span(boundArguments).first(numberOfBoundArguments));
        expected.append(firstArgument);
        expected.append(secondArgument);
        checkLandedInFunction(run, target.address(), boundThis, expected.span().first(std::min<size_t>(expected.size(), numberOfArgumentGPRs)));
        AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == expected.size());
    }

    FakeBoundFunction boundCall;
    initializeBoundFunction(calls, boundCall, calls.functionPrototypeCall(), target.address(), { });
    StubRun run = calls.runner.runCall(callValue(countInKey), boundCall.address(), thisValue, { firstArgument, secondArgument });
    checkLandedInFunction(run, target.address(), firstArgument, std::array { secondArgument });
    AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == countInKey - 1);

    run = calls.runner.runCall(callValue(0), boundCall.address(), thisValue, { });
    checkLandedInFunction(run, target.address(), undefinedBits, { });
    AOT_TEST_CHECK(!static_cast<uint32_t>(run.landedCall.count));

    FakeBoundFunction callBoundToNumber;
    FakeExecutable executableOfBoundFunction;
    executableOfBoundFunction.setHeader(0, NativeExecutableType);
    initializeBoundFunction(calls, callBoundToNumber, calls.functionPrototypeCall(), boundThis, { });
    callBoundToNumber.at<uint64_t>(JSFunction::offsetOfExecutableOrRareData()) = executableOfBoundFunction.address();
    run = calls.runner.runCall(callValue(countInKey), callBoundToNumber.address(), thisValue, { firstArgument, secondArgument });
    checkLandedInEngine(run, callBoundToNumber.address(), thisValue, std::array { firstArgument, secondArgument });
}

static void testConstructions(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    constexpr uint64_t newTarget = boxedInt32(90);
    const std::array passed { newTarget, firstArgument, secondArgument };
    FakeClosure classObject;
    FakeClosure classObjectOfOtherRealm;
    FakeObject<16> object;
    uint64_t wordOfClass = calls.runner.closureWord(passed.size(), { .isNotCallable = true, .takesNewTarget = true });
    calls.initializeClosure(classObject, StructureOf::PlainClosure, wordOfClass);
    calls.initializeClosure(classObjectOfOtherRealm, StructureOf::ClosureOfOtherRealm, wordOfClass);
    object.setHeader(calls.idOf(StructureOf::Object), FinalObjectType);

    constexpr std::pair<ConstructSyntax, Entry> forms[] = {
        { ConstructSyntax::NewExpression, Entry::operationAOTConstruct },
        { ConstructSyntax::SuperCall, Entry::operationAOTSuperConstruct },
    };
    for (auto [syntax, operation] : forms) {
        StubRun run = calls.runner.runCall(constructValue(syntax), classObject.address(), undefinedBits, { newTarget, firstArgument, secondArgument });
        checkLandedInFunction(run, classObject.address(), undefinedBits, passed);
        AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == passed.size());

        calls.runner.recordCallsOf(operation);
        for (uint64_t callee : { classObjectOfOtherRealm.address(), object.address(), undefinedBits }) {
            run = calls.runner.runCall(constructValue(syntax), callee, undefinedBits, { newTarget, firstArgument, secondArgument });
            AOT_TEST_CHECK(run.landedCall.place == LandedCall::Place::Nowhere && run.numberOfRecordedCalls == 1);
            AOT_TEST_CHECK(run.recordedArguments[0] == callee && run.recordedArguments[1] == newTarget && static_cast<uint32_t>(run.recordedArguments[2]) == passed.size() - 1);
        }
        calls.runner.countCallsOf(operation);
    }
}

static void testSuperConstructionsWithLists(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    constexpr uint64_t newTarget = boxedInt32(90);
    constexpr unsigned maxLengthInRegisters = numberOfArgumentGPRs - 1;
    std::array<uint64_t, numberOfArgumentGPRs + 1> list;
    for (size_t i = 0; i < list.size(); ++i)
        list[i] = boxedInt32(static_cast<uint32_t>(100 + i));
    const uint64_t words = std::bit_cast<uintptr_t>(list.data());
    FakeClosure classObject;
    FakeObject<16> object;
    calls.initializeClosure(classObject, StructureOf::PlainClosure, calls.runner.closureWord(numberOfArgumentGPRs, { .isNotCallable = true, .takesNewTarget = true }));
    object.setHeader(calls.idOf(StructureOf::Object), FinalObjectType);

    for (unsigned length = 0; length <= maxLengthInRegisters; ++length) {
        ArgumentRegisters expected;
        expected.fill(undefinedBits);
        expected[0] = newTarget;
        std::ranges::copy(std::span(list).first(length), expected.begin() + 1);
        // The upper half of the count is not the caller's to clear.
        StubRun run = calls.runner.run(superConstructWithList(), { classObject.address(), newTarget, length | 7ull << 32, words });
        checkLandedInFunction(run, classObject.address(), undefinedBits, expected);
        AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == length + 1);
    }

    calls.runner.recordCallsOf(Entry::operationAOTSuperConstruct);
    for (uint64_t parent : { classObject.address(), object.address() }) {
        for (unsigned length = 0; length <= list.size(); ++length) {
            if (parent == classObject.address() && length <= maxLengthInRegisters)
                continue;
            StubRun run = calls.runner.run(superConstructWithList(), { parent, newTarget, length, words });
            AOT_TEST_CHECK(run.landedCall.place == LandedCall::Place::Nowhere && run.numberOfRecordedCalls == 1);
            AOT_TEST_CHECK(run.recordedArguments[0] == parent && run.recordedArguments[1] == newTarget && static_cast<uint32_t>(run.recordedArguments[2]) == length);
            if (length > maxLengthInRegisters)
                AOT_TEST_CHECK(run.recordedArguments[3] == words);
        }
    }
    calls.runner.countCallsOf(Entry::operationAOTSuperConstruct);
}

namespace {

struct alignas(16) FakeList {
    WTF_MAKE_NONCOPYABLE(FakeList);
public:
    static constexpr uint32_t capacity = numberOfArgumentGPRs + 1;

    FakeList(IndexingType shape, uint32_t length, uint8_t flags = IsPristine)
        : indexingTypeAndMisc(IsArray | shape)
        , flags(flags)
        , publicLength(length)
    {
        butterfly = elements.data();
        elements.fill(0);
    }

    uint64_t address() const { return std::bit_cast<uintptr_t>(this); }

    uint32_t structureID { 0 };
    uint8_t indexingTypeAndMisc;
    uint8_t type { ArrayType };
    uint8_t flags;
    uint8_t cellState { 0 };
    uint64_t* butterfly { nullptr };
    uint32_t publicLength;
    uint32_t vectorLength { capacity };
    std::array<uint64_t, capacity> elements;
};
static_assert(OBJECT_OFFSETOF(FakeList, indexingTypeAndMisc) == JSCell::indexingTypeAndMiscOffset() && OBJECT_OFFSETOF(FakeList, flags) == JSCell::typeInfoFlagsOffset());
static_assert(OBJECT_OFFSETOF(FakeList, butterfly) == JSObject::butterflyOffset());
static_assert(static_cast<ptrdiff_t>(OBJECT_OFFSETOF(FakeList, publicLength)) - static_cast<ptrdiff_t>(OBJECT_OFFSETOF(FakeList, elements)) == Butterfly::offsetOfPublicLength());

} // anonymous namespace

static void testSuperConstructionsWithArrays(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    constexpr uint64_t newTarget = boxedInt32(90);
    constexpr unsigned maxLengthInRegisters = numberOfArgumentGPRs - 1;
    FakeClosure classObject;
    calls.initializeClosure(classObject, StructureOf::PlainClosure, calls.runner.closureWord(numberOfArgumentGPRs, { .isNotCallable = true, .takesNewTarget = true }));

    auto fill = [](FakeList& list) {
        bool holdsDoubles = (list.indexingTypeAndMisc & IndexingShapeMask) == DoubleShape;
        for (uint32_t i = 0; i < list.publicLength; ++i)
            list.elements[i] = holdsDoubles ? std::bit_cast<uint64_t>(i + 0.5) : boxedInt32(100 + i);
    };
    auto boxedElement = [](const FakeList& list, uint32_t i) {
        bool holdsDoubles = (list.indexingTypeAndMisc & IndexingShapeMask) == DoubleShape;
        return holdsDoubles ? static_cast<uint64_t>(JSValue::encode(jsDoubleNumber(i + 0.5))) : list.elements[i];
    };

    for (IndexingType shape : { Int32Shape, ContiguousShape, DoubleShape, static_cast<IndexingType>(Int32Shape | CopyOnWrite) }) {
        for (unsigned length = 0; length <= maxLengthInRegisters; ++length) {
            FakeList list(shape, length);
            fill(list);
            ArgumentRegisters expected;
            expected.fill(undefinedBits);
            expected[0] = newTarget;
            for (unsigned i = 0; i < length; ++i)
                expected[i + 1] = boxedElement(list, i);
            StubRun run = calls.runner.run(superConstructWithArray(), { classObject.address(), newTarget, list.address() });
            checkLandedInFunction(run, classObject.address(), undefinedBits, expected);
            AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == length + 1);
        }
    }
    {
        FakeList empty(UndecidedShape, 0);
        ArgumentRegisters expected;
        expected.fill(undefinedBits);
        expected[0] = newTarget;
        StubRun run = calls.runner.run(superConstructWithArray(), { classObject.address(), newTarget, empty.address() });
        checkLandedInFunction(run, classObject.address(), undefinedBits, expected);
        AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == 1);
    }

    calls.runner.recordCallsOf(Entry::operationAOTSuperConstructList);
    auto checkLeftToOperation = [&](FakeList& list) {
        StubRun run = calls.runner.run(superConstructWithArray(), { classObject.address(), newTarget, list.address() });
        AOT_TEST_CHECK(run.landedCall.place == LandedCall::Place::Nowhere && run.numberOfRecordedCalls == 1);
        AOT_TEST_CHECK(run.recordedArguments[0] == classObject.address() && run.recordedArguments[1] == newTarget && run.recordedArguments[2] == list.address());
    };
    for (IndexingType shape : { Int32Shape, ContiguousShape, DoubleShape }) {
        FakeList tooLong(shape, maxLengthInRegisters + 1);
        fill(tooLong);
        checkLeftToOperation(tooLong);

        FakeList withHole(shape, 2);
        fill(withHole);
        withHole.elements[1] = shape == DoubleShape ? std::bit_cast<uint64_t>(PNaN) : 0;
        checkLeftToOperation(withHole);

        FakeList changed(shape, 1, 0);
        fill(changed);
        checkLeftToOperation(changed);
    }
    FakeList holesOnly(UndecidedShape, 1);
    checkLeftToOperation(holesOnly);
    FakeList sparse(ArrayStorageShape, 0);
    checkLeftToOperation(sparse);
    calls.runner.countCallsOf(Entry::operationAOTSuperConstructList);
}

static void testCallsWithLists(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    std::array<uint64_t, 2 * numberOfArgumentGPRs + 3> list;
    for (size_t i = 0; i < list.size(); ++i)
        list[i] = boxedInt32(static_cast<uint32_t>(100 + i));
    const uint64_t words = std::bit_cast<uintptr_t>(list.data());
    FakeClosure closure;
    calls.initializeClosure(closure, StructureOf::PlainClosure, calls.runner.closureWord(0));

    for (unsigned length = 0; length <= list.size(); ++length) {
        ArgumentRegisters expected;
        expected.fill(undefinedBits);
        std::ranges::copy(std::span(list).first(std::min<size_t>(length, numberOfArgumentGPRs)), expected.begin());
        StubRun run = calls.runner.run(callWithList(), { closure.address(), thisValue, length | 7ull << 32, words });
        checkLandedInFunction(run, closure.address(), thisValue, expected);
        AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == length);
    }
}

static void testCallsWithLongLists(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    constexpr unsigned length = maxListLengthCopiedTwice + 1;
    static std::array<uint64_t, length> list;
    for (size_t i = 0; i < list.size(); ++i)
        list[i] = boxedInt32(static_cast<uint32_t>(100 + i));
    const uint64_t words = std::bit_cast<uintptr_t>(list.data());
    FakeClosure closure;
    FakeObject<16> object;
    calls.initializeClosure(closure, StructureOf::PlainClosure, calls.runner.closureWord(0));
    object.setHeader(calls.idOf(StructureOf::Object), FinalObjectType);

    ArgumentRegisters expected;
    std::ranges::copy(std::span(list).first(numberOfArgumentGPRs), expected.begin());
    StubRun run = calls.runner.run(callWithList(), { closure.address(), thisValue, length, words });
    checkLandedInFunction(run, closure.address(), thisValue, expected);
    AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == length);

    calls.runner.recordCallsOf(Entry::operationAOTCallEngineCallee);
    for (uint64_t callee : { object.address(), undefinedBits }) {
        StubRun run = calls.runner.run(callWithList(), { callee, thisValue, length, words });
        AOT_TEST_CHECK(run.landedCall.place == LandedCall::Place::Nowhere && run.numberOfRecordedCalls == 1);
        AOT_TEST_CHECK(run.recordedArguments[0] == callee && run.recordedArguments[1] == thisValue && static_cast<uint32_t>(run.recordedArguments[2]) == length && run.recordedArguments[3] == words);
    }
    calls.runner.countCallsOf(Entry::operationAOTCallEngineCallee);
}

template<void (*test)(bool hasOneInstance)>
static void withAnyNumberOfInstances()
{
    test(false);
    test(true);
}

std::span<const NativeTest> stubCallTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/call-value/closures"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testCallsOfClosures>> },
        { "stubs/call-value/slim-closures-are-entered-by-their-word-alone"_s, ImageTests::whenExecutable<testSlimClosuresAreEnteredByTheirWordAlone> },
        { "stubs/call-value/engine-callees"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testCallsOfEngineCallees>> },
        { "stubs/call-value/non-cells"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testCallsOfNonCells>> },
        { "stubs/call-value/host-functions"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testCallsOfHostFunctions>> },
        { "stubs/call-value/bound-functions"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testCallsOfBoundFunctions>> },
        { "stubs/construct-value/class-objects-and-other-callees"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testConstructions>> },
        { "stubs/super-construct-with-list/lists-of-every-length"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testSuperConstructionsWithLists>> },
        { "stubs/super-construct-with-array/arrays-of-every-shape-and-length"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testSuperConstructionsWithArrays>> },
        { "stubs/call-with-list/lists-of-every-length"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testCallsWithLists>> },
        { "stubs/call-with-list/long-lists-by-callee"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testCallsWithLongLists>> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
