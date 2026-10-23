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

    run = calls.runner.runCall(callValueWithCount(), closure, thisValue, { firstArgument, secondArgument });
    checkLandedInFunction(run, closure, thisValue, filled);
    AOT_TEST_CHECK(static_cast<uint32_t>(run.landedCall.count) == countInKey);
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
        for (uint64_t closure : { plain.address(), async.address(), slim.address(), slimWithProperties.address() })
            checkCallOfClosure(calls, closure, numberOfParameters);
    }
}

static void checkCallOfEngineCallee(Calls& calls, uint64_t callee)
{
    const std::array passed { firstArgument, secondArgument };
    checkLandedInEngine(calls.runner.runCall(callValue(countInKey), callee, thisValue, { firstArgument, secondArgument }), callee, thisValue, passed);
    checkLandedInEngine(calls.runner.runCall(callValueWithoutThis(countInKey), callee, thisValue, { firstArgument, secondArgument }), callee, undefinedBits, passed);
    checkLandedInEngine(calls.runner.runCall(callValueWithCount(), callee, thisValue, { firstArgument, secondArgument }), callee, thisValue, passed);
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
    FakeObject<16> object;
    calls.initializeClosure(classObject, StructureOf::PlainClosure, wordOfClass);
    calls.initializeClosure(classObjectWithOtherStructure, StructureOf::AsyncClosure, wordOfClass);
    calls.initializeClosure(closureOfOtherRealm, StructureOf::ClosureOfOtherRealm, calls.runner.closureWord(0));
    calls.initializeSlimClosure(slimClosureThatIsNotCallable, wordOfClass);
    functionExecutable.setHeader(0, FunctionExecutableType);
    interpretedFunction.setHeader(calls.idOf(StructureOf::OtherFunction), JSFunctionType);
    interpretedFunction.at<uint64_t>(JSFunction::offsetOfExecutableOrRareData()) = functionExecutable.address();
    object.setHeader(calls.idOf(StructureOf::Object), FinalObjectType);

    for (uint64_t callee : { classObject.address(), classObjectWithOtherStructure.address(), closureOfOtherRealm.address(), slimClosureThatIsNotCallable.address(), interpretedFunction.address(), object.address() })
        checkCallOfEngineCallee(calls, callee);
}

static void checkThrowsUnlessCallable(const StubRun& run, uint64_t callee)
{
    AOT_TEST_CHECK(run.landedCall.place == LandedCall::Place::Nowhere && run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == callee);
}

static void testCallsOfNonCells(bool hasOneInstance)
{
    Calls calls(hasOneInstance);
    const std::array passed { firstArgument, secondArgument };
    calls.runner.recordCallsOf(Entry::operationAOTThrowUnlessCallable);
    for (uint64_t callee : { undefinedBits, boxedInt32(5), nullBits }) {
        checkThrowsUnlessCallable(calls.runner.runCall(callValue(countInKey), callee, thisValue, { firstArgument, secondArgument }), callee);
        checkThrowsUnlessCallable(calls.runner.runCall(callValueWithoutThis(countInKey), callee, thisValue, { firstArgument, secondArgument }), callee);
        checkThrowsUnlessCallable(calls.runner.runCall(callValue(0), callee, thisValue, { }), callee);
        checkLandedInEngine(calls.runner.runCall(callValueWithCount(), callee, thisValue, { firstArgument, secondArgument }), callee, thisValue, passed);
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
    initializeBoundFunction(calls, callBoundToNumber, calls.functionPrototypeCall(), boundThis, { });
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
        { "stubs/call-value/engine-callees"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testCallsOfEngineCallees>> },
        { "stubs/call-value/non-cells"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testCallsOfNonCells>> },
        { "stubs/call-value/host-functions"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testCallsOfHostFunctions>> },
        { "stubs/call-value/bound-functions"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testCallsOfBoundFunctions>> },
        { "stubs/construct-value/class-objects-and-other-callees"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testConstructions>> },
        { "stubs/super-construct-with-list/lists-of-every-length"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testSuperConstructionsWithLists>> },
        { "stubs/call-with-list/lists-of-every-length"_s, ImageTests::whenExecutable<withAnyNumberOfInstances<testCallsWithLists>> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
