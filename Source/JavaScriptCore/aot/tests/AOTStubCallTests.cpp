/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTSlimClosure.h"
#include "AOTStubProgramTables.h"
#include "AOTStubRunner.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsCalls.h"
#include "AOTStubsClosures.h"
#include "AOTVector.h"
#include "CallFrame.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "MarkedSpace.h"
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
        instance.classes.heirTemplates = m_heirTemplates.data();
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
    std::array<HeirTemplate, numberOfHeirTemplates> m_heirTemplates { };
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

static void testChangedClosures(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    const uint64_t word = calls.runner.closureWord(2, { .takesNewTarget = true, .borrowedArguments = 3, .endsItsTailCalls = true });
    const uint32_t functionOfWord = ClosureWord::closureNumberOf(word);
    constexpr uint32_t otherFunction = 2;
    AOT_TEST_CHECK(functionOfWord != Calls::functionWithProperties && functionOfWord != otherFunction);

    FakeClosure full;
    FakeClosure fullOfOtherRealm;
    FakeSlimClosure slimWithProperties;
    FakeSlimClosure slimOfOtherRealm;
    FakeSlimClosure slimAsCreated;
    FakeFunction interpretedFunction;
    FakeObject<16> object;
    calls.initializeClosure(full, StructureOf::AsyncClosure, word);
    calls.initializeClosure(fullOfOtherRealm, StructureOf::ClosureOfOtherRealm, word);
    for (FakeSlimClosure* closure : { &slimWithProperties, &slimOfOtherRealm })
        calls.initializeSlimClosureWithProperties(*closure, word);
    slimOfOtherRealm.setHeader(calls.idOf(StructureOf::ClosureOfOtherRealm), SlimClosureWithPropertiesType);
    calls.initializeSlimClosure(slimAsCreated, word);
    interpretedFunction.setHeader(calls.idOf(StructureOf::OtherFunction), JSFunctionType);
    object.setHeader(calls.idOf(StructureOf::Object), FinalObjectType);

    const std::pair<uint64_t, uint32_t> numbersOfCells[] = {
        { full.address(), functionOfWord },
        { fullOfOtherRealm.address(), noClosureNumber },
        { slimWithProperties.address(), Calls::functionWithProperties },
        { slimOfOtherRealm.address(), hasOneInstance ? Calls::functionWithProperties : noClosureNumber },
        { slimAsCreated.address(), noClosureNumber },
        { interpretedFunction.address(), noClosureNumber },
        { object.address(), noClosureNumber },
    };
    StubCatalog catalog(StubOptions { .hasOneInstance = hasOneInstance, .targetFeatures = CPUFeatureSet::buildTarget() });
    auto registersLostBeside = [&](StubKey key, RegisterSet expected) {
        RegisterSet lost = catalog.summaryOf(key).clobbered;
        lost.exclude(RegisterSet::macroClobberedGPRs());
        lost.exclude(RegisterSet::macroClobberedFPRs());
        lost.exclude(expected);
        return lost;
    };

    for (ChangedClosure form : { ChangedClosure::Slim, ChangedClosure::Full }) {
        unsigned numberOfVariants = 0;
        forEachRegisterVariant(changedClosureHasNumber(form), [&](StubKey key) {
            ++numberOfVariants;
            AOT_TEST_CHECK(catalog.isLeaf(key) && registersLostBeside(key, { }).isEmpty());
            for (auto [cell, number] : numbersOfCells) {
                bool hasForm = (form == ChangedClosure::Full) == (cell == full.address());
                for (uint32_t asked : { functionOfWord, Calls::functionWithProperties, otherFunction }) {
                    StubRun run = calls.runner.run(key, { cell }, asked);
                    AOT_TEST_CHECK(run.result == (hasForm && asked == number) && !run.calledOperation && run.changedRegisters.isEmpty());
                }
            }
        });
        AOT_TEST_CHECK(numberOfVariants == 1 + allocatableGPRs.size());
    }

    forEachRegisterVariant(numberOfChangedClosure(), [&](StubKey key) {
        AOT_TEST_CHECK(catalog.isLeaf(key) && registersLostBeside(key, RegisterSet(GPRInfo::returnValueGPR)).isEmpty());
        for (auto [cell, number] : numbersOfCells) {
            StubRun run = calls.runner.run(key, { cell });
            AOT_TEST_CHECK(run.result == number && !run.calledOperation && run.changedRegisters.isEmpty());
        }
    });
}

namespace {

class LentClosures {
    WTF_MAKE_NONCOPYABLE(LentClosures);
public:
    static constexpr unsigned numberOfReferences = 4;
    static constexpr unsigned numberOfCaptures = 5;
    static constexpr unsigned cellSize = 4 * MarkedSpace::sizeStep;
    static constexpr uint8_t lowByteOfShape = 0xc3;
    static constexpr uint8_t highByteOfShape = 0x5a;
    static_assert(SlimClosure::allocationSize(numberOfCaptures) <= cellSize && cellSize < SlimClosure::allocationSize(numberOfCaptures) + MarkedSpace::sizeStep);
    static_assert(sizeOfClosureInFrame(offsetOfSlimClosureCaptures, numberOfCaptures) == cellSize);
    static_assert(!(highByteOfShape & environmentIsEscapedBit));

    using Closure = FakeObject<cellSize>;
    using Cell = FakeObject<MarkedSpace::sizeStep>;

    explicit LentClosures(Calls& calls)
        : m_calls(calls)
        , m_word(calls.runner.closureWord(0))
    {
        Instance& instance = calls.runner.instance();
        instance.image.closureFactNumbers = m_factNumbers.data();
        instance.image.closureFacts = &m_facts;
        instance.closures.slimClosureAllocators[(numberOfCaptures + 1) / 2] = reinterpret_cast<LocalAllocator*>(&allocator);
        AOT_TEST_CHECK(ClosureWord::closureNumberOf(m_word) < m_factNumbers.size());
        captureCounts() = CaptureCounts { numberOfReferences, numberOfCaptures }.encoded();
        object.setHeader(calls.idOf(StructureOf::Object), FinalObjectType);
    }

    uint8_t& captureCounts() { return m_facts.captureCounts; }

    void initializeEnvironment(Cell& environment)
    {
        environment.setHeader(m_calls.idOf(StructureOf::Object), CompiledEnvironmentType);
        environment.at<uint8_t>(offsetOfEnvironmentShapeLowByte) = lowByteOfShape;
        environment.at<uint8_t>(offsetOfEnvironmentShapeHighByte) = highByteOfShape;
    }

    void initializeClosure(Closure& closure, Cell& held, Cell& heldAsRawWord)
    {
        initializeEnvironment(held);
        initializeEnvironment(heldAsRawWord);
        closure.setHeader(m_calls.idOf(StructureOf::SlimClosure), SlimClosureType);
        closure.at<uint8_t>(JSCell::cellStateOffset()) = frameCellState;
        closure.at<uint64_t>(offsetOfSlimClosureWord) = m_word;
        const std::array<uint64_t, numberOfCaptures> captures { object.address(), held.address(), boxedInt32(9), 0, heldAsRawWord.address() };
        for (unsigned slot = 0; slot < numberOfCaptures; ++slot)
            closure.at<uint64_t>(offsetOfSlimClosureCaptures + slot * sizeof(uint64_t)) = captures[slot];
    }

    static bool isCopyInHeap(const Closure& moved, Closure inFrame)
    {
        inFrame.at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(CellState::DefinitelyWhite);
        return moved.bytes == inFrame.bytes;
    }

    bool isAsMade(Cell& environment) { return hasShapeAndHighByte(environment, highByteOfShape); }
    bool bearsMarkAlone(Cell& environment) { return hasShapeAndHighByte(environment, highByteOfShape | environmentIsEscapedBit); }
    bool objectIsAsMade() { return !object.at<uint8_t>(JSCell::typeInfoFlagsOffset()) && !object.at<uint8_t>(JSCell::indexingTypeAndMiscOffset()) && !object.at<uint64_t>(sizeof(JSCell)); }

    FakeAllocator allocator { cellSize };
    Cell object;

private:

    bool hasShapeAndHighByte(Cell& environment, uint8_t highByte)
    {
        return environment.at<uint32_t>(JSCell::structureIDOffset()) == m_calls.idOf(StructureOf::Object) && environment.at<uint8_t>(JSCell::typeInfoTypeOffset()) == CompiledEnvironmentType
            && environment.at<uint8_t>(offsetOfEnvironmentShapeLowByte) == lowByteOfShape && environment.at<uint8_t>(offsetOfEnvironmentShapeHighByte) == highByte
            && !environment.at<uint8_t>(JSCell::cellStateOffset()) && !environment.at<uint64_t>(offsetOfFirstEnvironmentVariable);
    }

    Calls& m_calls;
    uint64_t m_word;
    std::array<uint16_t, 16> m_factNumbers { };
    ClosureFacts m_facts { };
};

} // anonymous namespace

static void testLentClosuresStayInFrameOfCalleesThatOnlyCallThem(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    LentClosures lent(calls);
    LentClosures::Closure inFrame;
    LentClosures::Cell held;
    LentClosures::Cell heldAsRawWord;
    lent.initializeClosure(inFrame, held, heldAsRawWord);
    LentClosures::Closure room;
    lent.allocator.makeRoomAt(room.address(), LentClosures::cellSize);

    FakeSlimClosure slim;
    FakeClosure full;
    calls.initializeSlimClosure(slim, calls.runner.closureWord(countInKey, { .borrowedArguments = 1 }));
    calls.initializeClosure(full, StructureOf::PlainClosure, calls.runner.closureWord(countInKey, { .borrowedArguments = 3 }));
    for (uint64_t callee : { slim.address(), full.address() }) {
        StubRun run = calls.runner.runCall(callValueLending(countInKey, 1), callee, thisValue, { inFrame.address(), secondArgument });
        checkLandedInFunction(run, callee, thisValue, std::array { inFrame.address(), secondArgument });
        AOT_TEST_CHECK(lent.isAsMade(held) && lent.isAsMade(heldAsRawWord) && lent.objectIsAsMade());
        AOT_TEST_CHECK(lent.allocator.intervalStart() == room.address());
    }
}

static void testLentClosuresAreMovedAndTheirEnvironmentsMarked(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    LentClosures lent(calls);
    LentClosures::Closure inFrame;
    LentClosures::Cell held;
    LentClosures::Cell heldAsRawWord;

    FakeSlimClosure keeps;
    FakeSlimClosure borrowsOtherArgument;
    FakeClosure fullThatKeeps;
    calls.initializeSlimClosure(keeps, calls.runner.closureWord(countInKey));
    calls.initializeSlimClosure(borrowsOtherArgument, calls.runner.closureWord(countInKey, { .borrowedArguments = 2 }));
    calls.initializeClosure(fullThatKeeps, StructureOf::PlainClosure, calls.runner.closureWord(countInKey));
    for (uint64_t callee : { keeps.address(), borrowsOtherArgument.address(), fullThatKeeps.address() }) {
        lent.initializeClosure(inFrame, held, heldAsRawWord);
        LentClosures::Closure moved;
        lent.allocator.makeRoomAt(moved.address(), LentClosures::cellSize);
        StubRun run = calls.runner.runCall(callValueLending(countInKey, 1), callee, thisValue, { inFrame.address(), secondArgument });
        checkLandedInFunction(run, callee, thisValue, std::array { moved.address(), secondArgument });
        AOT_TEST_CHECK(LentClosures::isCopyInHeap(moved, inFrame) && !lent.allocator.hasRoom());
        AOT_TEST_CHECK(lent.bearsMarkAlone(held) && lent.isAsMade(heldAsRawWord) && lent.objectIsAsMade());
    }

    FakeObject<16> objectWithCallData;
    objectWithCallData.setHeader(calls.idOf(StructureOf::Object), ObjectType);
    objectWithCallData.at<uint8_t>(JSCell::typeInfoFlagsOffset()) = OverridesGetCallData;
    {
        lent.initializeClosure(inFrame, held, heldAsRawWord);
        LentClosures::Closure moved;
        lent.allocator.makeRoomAt(moved.address(), LentClosures::cellSize);
        StubRun run = calls.runner.runCall(callValueLending(countInKey, 1), objectWithCallData.address(), thisValue, { inFrame.address(), secondArgument });
        checkLandedInEngine(run, objectWithCallData.address(), thisValue, std::array { moved.address(), secondArgument });
        AOT_TEST_CHECK(LentClosures::isCopyInHeap(moved, inFrame) && lent.bearsMarkAlone(held) && lent.isAsMade(heldAsRawWord) && lent.objectIsAsMade());
    }

    {
        LentClosures::Closure secondInFrame;
        LentClosures::Cell heldBySecond;
        LentClosures::Cell heldAsRawWordBySecond;
        lent.initializeClosure(inFrame, held, heldAsRawWord);
        lent.initializeClosure(secondInFrame, heldBySecond, heldAsRawWordBySecond);
        std::array<LentClosures::Closure, 2> moved;
        lent.allocator.makeRoomAt(moved[0].address(), 2 * LentClosures::cellSize);
        AOT_TEST_CHECK(moved[1].address() == moved[0].address() + LentClosures::cellSize);
        StubRun run = calls.runner.runCall(callValueLending(countInKey, 3), borrowsOtherArgument.address(), thisValue, { inFrame.address(), secondInFrame.address() });
        checkLandedInFunction(run, borrowsOtherArgument.address(), thisValue, std::array { moved[0].address(), moved[1].address() });
        AOT_TEST_CHECK(LentClosures::isCopyInHeap(moved[0], inFrame) && LentClosures::isCopyInHeap(moved[1], secondInFrame) && !lent.allocator.hasRoom());
        AOT_TEST_CHECK(lent.bearsMarkAlone(held) && lent.bearsMarkAlone(heldBySecond) && lent.isAsMade(heldAsRawWord) && lent.isAsMade(heldAsRawWordBySecond));
    }
}

static void testLentClosuresThatStubCannotMoveAreLeftToOperation(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    LentClosures lent(calls);
    LentClosures::Closure inFrame;
    LentClosures::Cell held;
    LentClosures::Cell heldAsRawWord;
    FakeSlimClosure keeps;
    calls.initializeSlimClosure(keeps, calls.runner.closureWord(countInKey));
    calls.runner.recordCallsOf(Entry::operationAOTMoveClosureToHeap);

    auto checkMovedByOperation = [&] {
        LentClosures::Closure fromOperation;
        calls.runner.setResultOfRecordedOperation(fromOperation.address());
        StubRun run = calls.runner.runCall(callValueLending(countInKey, 1), keeps.address(), thisValue, { inFrame.address(), secondArgument });
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == inFrame.address());
        const LandedCall& landed = run.landedCall;
        AOT_TEST_CHECK(landed.place == LandedCall::Place::CompiledFunction && landed.values.callee == keeps.address() && landed.values.thisValue == thisValue);
        AOT_TEST_CHECK(landed.values.arguments[0] == fromOperation.address() && landed.values.arguments[1] == secondArgument);
        AOT_TEST_CHECK(lent.isAsMade(heldAsRawWord) && lent.objectIsAsMade());
    };

    lent.initializeClosure(inFrame, held, heldAsRawWord);
    checkMovedByOperation();
    AOT_TEST_CHECK(lent.bearsMarkAlone(held));

    lent.initializeClosure(inFrame, held, heldAsRawWord);
    lent.captureCounts() = countsAreListed;
    LentClosures::Closure room;
    lent.allocator.makeRoomAt(room.address(), LentClosures::cellSize);
    checkMovedByOperation();
    AOT_TEST_CHECK(lent.isAsMade(held) && lent.allocator.intervalStart() == room.address());

    LentClosures::Closure heldInFrame;
    LentClosures::Cell heldByClosureHeld;
    LentClosures::Cell heldAsRawWordByClosureHeld;
    lent.initializeClosure(heldInFrame, heldByClosureHeld, heldAsRawWordByClosureHeld);
    lent.initializeClosure(inFrame, held, heldAsRawWord);
    lent.captureCounts() = CaptureCounts { LentClosures::numberOfReferences, LentClosures::numberOfCaptures }.encoded();
    inFrame.at<uint64_t>(offsetOfSlimClosureCaptures) = heldInFrame.address();
    checkMovedByOperation();
    AOT_TEST_CHECK(lent.isAsMade(heldByClosureHeld) && lent.allocator.intervalStart() == room.address());
    calls.runner.countCallsOf(Entry::operationAOTMoveClosureToHeap);
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
        { "stubs/call-value-lending/closures-stay-in-the-frame-of-callees-that-only-call-them"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testLentClosuresStayInFrameOfCalleesThatOnlyCallThem>> },
        { "stubs/call-value-lending/closures-are-moved-and-their-environments-marked"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testLentClosuresAreMovedAndTheirEnvironmentsMarked>> },
        { "stubs/call-value-lending/closures-that-the-stub-cannot-move-are-left-to-the-operation"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testLentClosuresThatStubCannotMoveAreLeftToOperation>> },
        { "stubs/construct-value/class-objects-and-other-callees"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testConstructions>> },
        { "stubs/super-construct-with-array/arrays-of-every-shape-and-length"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testSuperConstructionsWithArrays>> },
        { "stubs/changed-closure/every-kind-of-cell-in-every-register"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testChangedClosures>> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
