/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTRealmTestSupport.h"
#include "AOTRemovableFeature.h"
#include "DOMAttributeGetterSetter.h"
#include "JSCInlines.h"
#include "Lookup.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <csetjmp>
#include <memory>
#include <optional>
#include <tuple>
#include <wtf/Atomics.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#if OS(DARWIN)
#define AOT_TEST_STATE_SECTION ".pushsection __DATA,__data\n"
#define AOT_TEST_STATE_SYMBOL(name) "_" #name
#else
#define AOT_TEST_STATE_SECTION ".pushsection .data\n"
#define AOT_TEST_STATE_SYMBOL(name) #name
#endif

__asm__(
    AOT_TEST_STATE_SECTION
    ".p2align 3\n"
    ".globl " AOT_TEST_STATE_SYMBOL(Bun__removableFeatureState) "\n"
    ".globl " AOT_TEST_STATE_SYMBOL(JSC__AOT__writableFeatureStateForTests) "\n"
    AOT_TEST_STATE_SYMBOL(Bun__removableFeatureState) ":\n"
    AOT_TEST_STATE_SYMBOL(JSC__AOT__writableFeatureStateForTests) ":\n"
    ".space 536\n"
    ".popsection\n");

extern "C" JSC::RemovableFeatureState JSC__AOT__writableFeatureStateForTests;

namespace JSC::AOT {

namespace {

struct HandlerLog {
    std::jmp_buf abortTarget;
    bool isAbortExpected;
    unsigned abortCount;
    uint16_t abortedFeature;
    unsigned throwCount;
    uint16_t thrownFeature;
    JSGlobalObject* thrownRealm;
    std::optional<uint32_t> thrownBuiltin;
};

HandlerLog handlerLog;

constexpr uintptr_t placeholderAddress = 0x1000;

void throwErrorIn(JSGlobalObject* realm)
{
    if (reinterpret_cast<uintptr_t>(realm) == placeholderAddress)
        return;
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwTypeError(realm, scope, "removed"_s);
}

} // namespace

} // namespace JSC::AOT

namespace JSC {

extern "C" void Bun__abortForRemovedFeature(uint16_t feature)
{
    AOT_TEST_CHECK(AOT::handlerLog.isAbortExpected);
    ++AOT::handlerLog.abortCount;
    AOT::handlerLog.abortedFeature = feature;
    std::longjmp(AOT::handlerLog.abortTarget, 1);
}

extern "C" EncodedJSValue Bun__throwForRemovedFeature(JSGlobalObject* realm, uint16_t feature)
{
    ++AOT::handlerLog.throwCount;
    AOT::handlerLog.thrownFeature = feature;
    AOT::handlerLog.thrownRealm = realm;
    AOT::handlerLog.thrownBuiltin = std::nullopt;
    AOT::throwErrorIn(realm);
    return { };
}

extern "C" EncodedJSValue Bun__throwForRemovedFunction(JSGlobalObject* realm, uint16_t feature, StringImpl*)
{
    return Bun__throwForRemovedFeature(realm, feature);
}

extern "C" EncodedJSValue Bun__throwForRemovedBuiltin(JSGlobalObject* realm, uint16_t feature, uint32_t builtin)
{
    ++AOT::handlerLog.throwCount;
    AOT::handlerLog.thrownFeature = feature;
    AOT::handlerLog.thrownRealm = realm;
    AOT::handlerLog.thrownBuiltin = builtin;
    return { };
}

extern "C" const char* Bun__removableFeatureName(uint16_t);

extern "C" const char* Bun__removableFeatureName(uint16_t)
{
    return nullptr;
}

} // namespace JSC

namespace JSC::AOT {

namespace {

using enum RemovedFeatureStub;

using GetterFunction = EncodedJSValue (JIT_OPERATION_ATTRIBUTES *)(JSGlobalObject*, EncodedJSValue, PropertyName);

constexpr EncodedJSValue hostFunctionResult = 42;
constexpr EncodedJSValue getterResult = 43;
constexpr uint32_t testBuiltin = 77;
constexpr RemovableFeature firstEmbedderFeature { firstEmbedderRemovableFeature };
constexpr RemovableFeature lastEmbedderFeature { removableFeatureIndexLimit - 1 };

JSGlobalObject* testRealm()
{
    return reinterpret_cast<JSGlobalObject*>(placeholderAddress);
}

void resetFeatureState()
{
    for (auto& removal : JSC__AOT__writableFeatureStateForTests.removal)
        removal = static_cast<uint8_t>(FeatureRemoval::Present);
    JSC__AOT__writableFeatureStateForTests.isTrimmed = 0;
    JSC__AOT__writableFeatureStateForTests.removedRangeCount = 0;
    JSC__AOT__writableFeatureStateForTests.removedRangesOffset = 0;
    handlerLog = { };
}

template<void (*test)()>
void runWithCleanFeatureState()
{
    resetFeatureState();
    test();
    resetFeatureState();
}

void setRemoval(RemovableFeature feature, FeatureRemoval removal)
{
    JSC__AOT__writableFeatureStateForTests.removal[static_cast<uint16_t>(feature)] = static_cast<uint8_t>(removal);
}

void removeFeature(RemovableFeature feature)
{
    setRemoval(feature, FeatureRemoval::Unused);
}

template<typename Call>
NEVER_INLINE decltype(auto) callAfterStateChange(Call&& call)
{
    WTF::compilerFence();
    return call();
}

template<typename Call>
NEVER_INLINE bool reachesAbort(Call&& call)
{
    WTF::compilerFence();
    handlerLog.isAbortExpected = true;
    if (setjmp(handlerLog.abortTarget)) {
        handlerLog.isAbortExpected = false;
        return true;
    }
    call();
    handlerLog.isAbortExpected = false;
    return false;
}

EncodedJSValue JSC_HOST_CALL_ATTRIBUTES hostFunction(JSGlobalObject*, CallFrame*)
{
    return hostFunctionResult;
}

EncodedJSValue JIT_OPERATION_ATTRIBUTES getter(JSGlobalObject*, EncodedJSValue, PropertyName)
{
    return getterResult;
}

int functionReturningInt(int value)
{
    return value + 1;
}

int replacementFunction(int)
{
    return -1;
}

int noexceptFunction(int value) noexcept
{
    return value + 2;
}

int& functionReturningReference()
{
    static int value;
    return value;
}

void setFlag(bool& flag)
{
    flag = true;
}

[[maybe_unused]] EncodedJSValue functionWithoutRealm()
{
    return hostFunctionResult;
}

[[maybe_unused]] bool functionReturningBool(JSGlobalObject*)
{
    return true;
}

[[maybe_unused]] JSValue functionReturningValue(JSGlobalObject*)
{
    return jsNumber(1);
}

int addArguments(int byValue, int& reference, std::unique_ptr<int> moved)
{
    reference += byValue;
    return *moved + byValue;
}

static_assert(!RemovedFeatureStubFits<Throw, functionWithoutRealm>);
static_assert(RemovedFeatureStubFits<Throw, functionReturningBool>);
static_assert(RemovedFeatureStubFits<Throw, functionReturningValue>);
static_assert(RemovedFeatureStubFits<Throw, getter>);
static_assert(!RemovedFeatureStubFits<Throw, functionReturningInt>);
static_assert(!RemovedFeatureStubFits<ReturnUndefined, functionReturningInt>);
static_assert(!RemovedFeatureStubFits<ReturnUndefined, getter>);
static_assert(!RemovedFeatureStubFits<ReturnDefault, functionReturningReference>);
static_assert(RemovedFeatureStubFits<Abort, functionReturningReference>);
static_assert(std::is_same_v<decltype(removableFunction<RemovableFeature::JIT, Throw, hostFunction>()), EncodedJSValue (JSC_HOST_CALL_ATTRIBUTES *)(JSGlobalObject*, CallFrame*)>);
static_assert(static_cast<uint16_t>(RemovableFeature::JIT) == 0);
static_assert(static_cast<uint16_t>(RemovableFeature::RegExpFromString) == 53);

struct RecordedTableEntry {
    EncodedJSValue (JSC_HOST_CALL_ATTRIBUTES *hostFunction)(JSGlobalObject*, CallFrame*);
    GetterFunction getter;
};

static constinit const RecordedTableEntry recordedTable[] = {
    { hostFunction, getter },
};

void testRecordedPointer()
{
    auto readPointers = [] {
        return std::tuple {
            removableFunction<RemovableFeature::JIT, Abort, functionReturningInt>(),
            removableFunction<RemovableFeature::JIT, Throw, hostFunction>(),
            removableFunction<RemovableFeature::JIT, ReturnDefault, noexceptFunction>(),
            removableBuiltinFunction<RemovableFeature::JIT, Throw, hostFunction, testBuiltin>(),
        };
    };
    static_assert(std::is_same_v<decltype(removableFunction<RemovableFeature::JIT, ReturnDefault, noexceptFunction>()), int (*)(int) noexcept>);
    constexpr std::tuple functions { &functionReturningInt, &hostFunction, &noexceptFunction, &hostFunction };

    AOT_TEST_CHECK(callAfterStateChange(readPointers) == functions);

    removeFeature(RemovableFeature::JIT);
    AOT_TEST_CHECK(callAfterStateChange(readPointers) == functions);
    AOT_TEST_CHECK(!handlerLog.abortCount && !handlerLog.throwCount);
}

void testRecordedTableHoldsFunctions()
{
    recordRemovableTableEntry<recordedTable, RemovableFeature::Temporal, Throw, hostFunction>();
    recordRemovableTableEntry<recordedTable, RemovableFeature::Temporal, Throw, getter, GuardOrigin::Builtin, testBuiltin>();
    AOT_TEST_CHECK(recordedTable[0].hostFunction == hostFunction && recordedTable[0].getter == getter);

    removeFeature(RemovableFeature::Temporal);
    const auto& entry = recordedTable[0];
    AOT_TEST_CHECK(callAfterStateChange([&] { return entry.hostFunction(testRealm(), nullptr); }) == hostFunctionResult);
    AOT_TEST_CHECK(callAfterStateChange([&] { return entry.getter(testRealm(), { }, PropertyName()); }) == getterResult);
    AOT_TEST_CHECK(!handlerLog.abortCount && !handlerLog.throwCount);
}

void testGuardedCall()
{
    int reference = 10;
    auto moved = std::make_unique<int>(5);
    int result = callAfterStateChange([&] { return callRemovable<RemovableFeature::JIT, ReturnDefault, addArguments>(3, reference, WTF::move(moved)); });
    AOT_TEST_CHECK(result == 8);
    AOT_TEST_CHECK(reference == 13);
    AOT_TEST_CHECK(!moved);

    bool flag = false;
    callAfterStateChange([&] { callRemovable<RemovableFeature::JIT, ReturnDefault, setFlag>(flag); });
    AOT_TEST_CHECK(flag);
    AOT_TEST_CHECK(callAfterStateChange([&] { return callRemovable<RemovableFeature::JIT, Throw, hostFunction>(testRealm(), nullptr); }) == hostFunctionResult);

    removeFeature(RemovableFeature::JIT);
    moved = std::make_unique<int>(5);
    result = callAfterStateChange([&] { return callRemovable<RemovableFeature::JIT, ReturnDefault, addArguments>(3, reference, WTF::move(moved)); });
    AOT_TEST_CHECK(!result);
    AOT_TEST_CHECK(reference == 13);
    AOT_TEST_CHECK(moved && *moved == 5);

    flag = false;
    callAfterStateChange([&] { callRemovable<RemovableFeature::JIT, ReturnDefault, setFlag>(flag); });
    AOT_TEST_CHECK(!flag);

    AOT_TEST_CHECK(callAfterStateChange([&] { return callRemovable<RemovableFeature::JIT, Throw, hostFunction>(testRealm(), nullptr); }) == EncodedJSValue { });
    AOT_TEST_CHECK(handlerLog.throwCount == 1 && handlerLog.thrownRealm == testRealm());
    AOT_TEST_CHECK(!reachesAbort([&] { callRemovable<RemovableFeature::JIT, Abort, functionReturningReference>(); }));
    AOT_TEST_CHECK(!handlerLog.abortCount);
}

void testAddressUnlessRemoved()
{
    AOT_TEST_CHECK(callAfterStateChange([&] { return addressUnlessRemoved<RemovableFeature::JIT, functionReturningInt>(replacementFunction); }) == functionReturningInt);

    removeFeature(RemovableFeature::JIT);
    AOT_TEST_CHECK(callAfterStateChange([&] { return addressUnlessRemoved<RemovableFeature::JIT, functionReturningInt>(replacementFunction); }) == replacementFunction);
    AOT_TEST_CHECK(!handlerLog.abortCount && !handlerLog.throwCount);
}

void setFlagInBody(bool& flag)
{
    JSC_BEGIN_REMOVABLE_ENTRY_POINT(JIT, ReturnDefault, void)
    flag = true;
    JSC_END_REMOVABLE_ENTRY_POINT
}

int& referenceBody(int& value)
{
    JSC_BEGIN_REMOVABLE_ENTRY_POINT(JIT, Abort, int&)
    return value;
    JSC_END_REMOVABLE_ENTRY_POINT
}

NEVER_INLINE int definitionBody(int value)
{
    JSC_REMOVABLE_DEFINITION(JIT, definitionBody);
    return value + 1;
}

std::pair<int, int> pairBody(int first, int second)
{
    JSC_BEGIN_REMOVABLE_ENTRY_POINT(JIT, ReturnDefault, std::pair<int, int>)
    return { first, second };
    JSC_END_REMOVABLE_ENTRY_POINT
}

int earlyReturnBody(bool returnEarly)
{
    JSC_BEGIN_REMOVABLE_ENTRY_POINT(JIT, ReturnDefault, int)
    if (returnEarly)
        return 1;
    return 2;
    JSC_END_REMOVABLE_ENTRY_POINT
}

class CapturedThis {
public:
    int addToValue(int addend) const
    {
        JSC_BEGIN_REMOVABLE_ENTRY_POINT(JIT, ReturnDefault, int)
        return m_value + addend;
        JSC_END_REMOVABLE_ENTRY_POINT
    }

private:
    int m_value { 5 };
};

void testGuardedBody()
{
    bool flag = false;
    int value = 0;
    CapturedThis capturedThis;

    callAfterStateChange([&] { setFlagInBody(flag); });
    AOT_TEST_CHECK(flag);
    AOT_TEST_CHECK(&callAfterStateChange([&]() -> int& { return referenceBody(value); }) == &value);
    AOT_TEST_CHECK(callAfterStateChange([&] { return definitionBody(1); }) == 2);
    AOT_TEST_CHECK((callAfterStateChange([&] { return pairBody(3, 4); }) == std::pair<int, int> { 3, 4 }));
    AOT_TEST_CHECK(callAfterStateChange([&] { return earlyReturnBody(true); }) == 1);
    AOT_TEST_CHECK(callAfterStateChange([&] { return earlyReturnBody(false); }) == 2);
    AOT_TEST_CHECK(callAfterStateChange([&] { return capturedThis.addToValue(2); }) == 7);

    removeFeature(RemovableFeature::JIT);
    flag = false;
    callAfterStateChange([&] { setFlagInBody(flag); });
    AOT_TEST_CHECK(!flag);
    AOT_TEST_CHECK(&callAfterStateChange([&]() -> int& { return referenceBody(value); }) == &value);
    AOT_TEST_CHECK(callAfterStateChange([&] { return definitionBody(1); }) == 2);
    AOT_TEST_CHECK(!handlerLog.abortCount);
    AOT_TEST_CHECK((callAfterStateChange([&] { return pairBody(3, 4); }) == std::pair<int, int> { }));
    AOT_TEST_CHECK(!callAfterStateChange([&] { return earlyReturnBody(true); }));
    AOT_TEST_CHECK(!callAfterStateChange([&] { return capturedThis.addToValue(2); }));
}

NEVER_INLINE EncodedJSValue JSC_HOST_CALL_ATTRIBUTES hostFunctionDefinition(JSGlobalObject*, CallFrame*)
{
    JSC_REMOVABLE_HOST_FUNCTION_DEFINITION(Temporal, hostFunctionDefinition);
    return hostFunctionResult;
}

NEVER_INLINE EncodedJSValue JSC_HOST_CALL_ATTRIBUTES builtinHostFunctionDefinition(JSGlobalObject*, CallFrame*)
{
    recordRemovableDefinition<RemovableFeature::Temporal, builtinHostFunctionDefinition, Throw, testBuiltin>();
    return hostFunctionResult;
}

void testHostFunctionDefinition()
{
    AOT_TEST_CHECK(callAfterStateChange([&] { return hostFunctionDefinition(testRealm(), nullptr); }) == hostFunctionResult);
    AOT_TEST_CHECK(callAfterStateChange([&] { return builtinHostFunctionDefinition(testRealm(), nullptr); }) == hostFunctionResult);

    removeFeature(RemovableFeature::Temporal);
    AOT_TEST_CHECK(callAfterStateChange([&] { return hostFunctionDefinition(testRealm(), nullptr); }) == hostFunctionResult);
    AOT_TEST_CHECK(callAfterStateChange([&] { return builtinHostFunctionDefinition(testRealm(), nullptr); }) == hostFunctionResult);
    AOT_TEST_CHECK(!handlerLog.throwCount && !handlerLog.abortCount);
}

void testGuardedBuiltinCall()
{
    auto callBuiltin = [] { return callRemovableBuiltin<RemovableFeature::Temporal, hostFunction, testBuiltin>(testRealm(), nullptr); };
    AOT_TEST_CHECK(callAfterStateChange(callBuiltin) == hostFunctionResult);
    AOT_TEST_CHECK(!handlerLog.throwCount);

    removeFeature(RemovableFeature::Temporal);
    AOT_TEST_CHECK(callAfterStateChange(callBuiltin) == EncodedJSValue { });
    AOT_TEST_CHECK(handlerLog.throwCount == 1 && handlerLog.thrownFeature == static_cast<uint16_t>(RemovableFeature::Temporal));
    AOT_TEST_CHECK(handlerLog.thrownRealm == testRealm() && handlerLog.thrownBuiltin == testBuiltin);
}

void testRemovedFeatureAt()
{
    static uint8_t code[64];
    static RemovedRange ranges[2];
    uintptr_t base = reinterpret_cast<uintptr_t>(&JSC__AOT__writableFeatureStateForTests);
    auto offsetOf = [&](const void* address) {
        return static_cast<int64_t>(reinterpret_cast<uintptr_t>(address) - base);
    };
    ranges[0] = { offsetOf(&code[16]), 8, static_cast<uint16_t>(RemovableFeature::Temporal), 0 };
    ranges[1] = { offsetOf(&code[40]), 4, static_cast<uint16_t>(RemovableFeature::Atomics), 0 };
    JSC__AOT__writableFeatureStateForTests.removedRangeCount = 2;
    JSC__AOT__writableFeatureStateForTests.removedRangesOffset = offsetOf(ranges);

    auto featureAt = [&](unsigned index) {
        return callAfterStateChange([&] { return removedFeatureAt(&code[index]); });
    };
    AOT_TEST_CHECK(!featureAt(16));

    JSC__AOT__writableFeatureStateForTests.isTrimmed = 1;
    AOT_TEST_CHECK(!featureAt(15));
    AOT_TEST_CHECK(featureAt(16) == RemovableFeature::Temporal);
    AOT_TEST_CHECK(featureAt(23) == RemovableFeature::Temporal);
    AOT_TEST_CHECK(!featureAt(24));
    AOT_TEST_CHECK(!featureAt(39));
    AOT_TEST_CHECK(featureAt(40) == RemovableFeature::Atomics);
    AOT_TEST_CHECK(featureAt(43) == RemovableFeature::Atomics);
    AOT_TEST_CHECK(!featureAt(44));
}

struct NativeCalls {
    unsigned removedHostFunction;
    unsigned removedGetter;
    unsigned removedSetter;
    unsigned keptGetter;
    unsigned keptSetter;
};

NativeCalls nativeCalls;

constexpr uint16_t hostFunctionFeature = static_cast<uint16_t>(RemovableFeature::Inspector);
constexpr uint16_t getterFeature = static_cast<uint16_t>(RemovableFeature::Temporal);
constexpr uint16_t setterFeature = static_cast<uint16_t>(RemovableFeature::Atomics);
constexpr int32_t keptValue = 43;

EncodedJSValue JSC_HOST_CALL_ATTRIBUTES removedHostFunction(JSGlobalObject*, CallFrame*)
{
    ++nativeCalls.removedHostFunction;
    return JSValue::encode(jsNumber(1));
}

EncodedJSValue JSC_HOST_CALL_ATTRIBUTES keptHostFunction(JSGlobalObject*, CallFrame*)
{
    return JSValue::encode(jsNumber(keptValue));
}

EncodedJSValue JIT_OPERATION_ATTRIBUTES removedGetter(JSGlobalObject*, EncodedJSValue, PropertyName)
{
    ++nativeCalls.removedGetter;
    return JSValue::encode(jsNumber(2));
}

bool JIT_OPERATION_ATTRIBUTES removedSetter(JSGlobalObject*, EncodedJSValue, EncodedJSValue, PropertyName)
{
    ++nativeCalls.removedSetter;
    return true;
}

EncodedJSValue JIT_OPERATION_ATTRIBUTES keptGetter(JSGlobalObject*, EncodedJSValue, PropertyName)
{
    ++nativeCalls.keptGetter;
    return JSValue::encode(jsNumber(keptValue));
}

bool JIT_OPERATION_ATTRIBUTES keptSetter(JSGlobalObject*, EncodedJSValue, EncodedJSValue, PropertyName)
{
    ++nativeCalls.keptSetter;
    return true;
}

void listRemovedFunctions()
{
    static RemovedRange ranges[3];
    uintptr_t base = reinterpret_cast<uintptr_t>(&JSC__AOT__writableFeatureStateForTests);
    auto offsetOf = [&](auto* address) {
        return static_cast<int64_t>(reinterpret_cast<uintptr_t>(address) - base);
    };
    ranges[0] = { offsetOf(removedHostFunction), 1, hostFunctionFeature, 0 };
    ranges[1] = { offsetOf(removedGetter), 1, getterFeature, 0 };
    ranges[2] = { offsetOf(removedSetter), 1, setterFeature, 0 };
    std::ranges::sort(ranges, { }, &RemovedRange::start);
    JSC__AOT__writableFeatureStateForTests.removedRangeCount = static_cast<uint32_t>(std::size(ranges));
    JSC__AOT__writableFeatureStateForTests.removedRangesOffset = offsetOf(ranges);
    nativeCalls = { };
}

template<void (*test)(Realms&)>
void runInTrimmedExecutable()
{
    resetFeatureState();
    listRemovedFunctions();
    JSC__AOT__writableFeatureStateForTests.isTrimmed = 1;
    callAfterStateChange([] {
        Realms realms;
        test(realms);
    });
    AOT_TEST_CHECK(!nativeCalls.removedHostFunction && !nativeCalls.removedGetter && !nativeCalls.removedSetter);
    resetFeatureState();
}

template<typename Access>
bool throwsForFeature(Realms& realms, uint16_t feature, const Access& access)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(realms.vm());
    unsigned throwCount = handlerLog.throwCount;
    access();
    bool hasThrown = scope.exception();
    scope.clearException();
    return hasThrown && handlerLog.throwCount == throwCount + 1 && handlerLog.thrownFeature == feature && handlerLog.thrownRealm == realms.plain();
}

Identifier removedName(Realms& realms)
{
    return Identifier::fromString(realms.vm(), "removed"_s);
}

Identifier keptName(Realms& realms)
{
    return Identifier::fromString(realms.vm(), "kept"_s);
}

bool store(Realms& realms, JSObject* object, const Identifier& name, PutPropertySlot& slot)
{
    return object->methodTable()->put(object, realms.plain(), name, jsNumber(5), slot);
}

bool readThrows(Realms& realms, JSObject* object, uint16_t feature = getterFeature)
{
    return throwsForFeature(realms, feature, [&] {
        object->get(realms.plain(), removedName(realms));
    });
}

bool storeThrows(Realms& realms, JSObject* object, uint16_t feature = setterFeature)
{
    return throwsForFeature(realms, feature, [&] {
        PutPropertySlot slot(object, true);
        store(realms, object, removedName(realms), slot);
    });
}

JSObject* createObjectInheritingFrom(Realms& realms, JSObject* prototype)
{
    return constructEmptyObject(realms.plain(), prototype);
}

void checkCachesGetThrowingFunctions(Realms& realms, JSObject* holder)
{
    PropertySlot readSlot(holder, PropertySlot::InternalMethodType::Get);
    AOT_TEST_CHECK(holder->getPropertySlot(realms.plain(), removedName(realms), readSlot) && readSlot.isCacheableCustom());
    AOT_TEST_CHECK(readSlot.customGetter() == GetValueFunc(CustomGetterSetter::getRemovedProperty));
    AOT_TEST_CHECK(readSlot.customSetter() == PutValueFunc(CustomGetterSetter::setRemovedProperty));

    PropertySlot keptSlot(holder, PropertySlot::InternalMethodType::Get);
    AOT_TEST_CHECK(holder->getPropertySlot(realms.plain(), keptName(realms), keptSlot) && keptSlot.isCacheableCustom());
    AOT_TEST_CHECK(keptSlot.customGetter() == GetValueFunc(keptGetter) && keptSlot.customSetter() == PutValueFunc(keptSetter));

    PutPropertySlot storeSlot(holder, true);
    AOT_TEST_CHECK(throwsForFeature(realms, setterFeature, [&] {
        store(realms, holder, removedName(realms), storeSlot);
    }));
    AOT_TEST_CHECK(storeSlot.isCacheableCustom() && storeSlot.customSetter().untaggedPtr() == PutValueFunc(CustomGetterSetter::setRemovedProperty).untaggedPtr());
}

void checkAccessorsOf(Realms& realms, JSObject* holder)
{
    AOT_TEST_CHECK(readThrows(realms, holder));
    AOT_TEST_CHECK(readThrows(realms, holder));
    AOT_TEST_CHECK(storeThrows(realms, holder));
    AOT_TEST_CHECK(readThrows(realms, createObjectInheritingFrom(realms, holder)));
    checkCachesGetThrowingFunctions(realms, holder);

    unsigned throwCount = handlerLog.throwCount;
    NativeCalls callsBefore = nativeCalls;
    AOT_TEST_CHECK(holder->get(realms.plain(), keptName(realms)) == jsNumber(keptValue));
    PutPropertySlot slot(holder, true);
    AOT_TEST_CHECK(store(realms, holder, keptName(realms), slot));
    AOT_TEST_CHECK(nativeCalls.keptGetter == callsBefore.keptGetter + 1 && nativeCalls.keptSetter == callsBefore.keptSetter + 1);
    AOT_TEST_CHECK(handlerLog.throwCount == throwCount);
}

JSObject* createHolder(Realms& realms, unsigned attributes)
{
    VM& vm = realms.vm();
    JSObject* holder = constructEmptyObject(realms.plain());
    holder->putDirectCustomAccessor(vm, removedName(realms), CustomGetterSetter::create(vm, removedGetter, removedSetter), attributes);
    holder->putDirectCustomAccessor(vm, keptName(realms), CustomGetterSetter::create(vm, keptGetter, keptSetter), attributes);
    return holder;
}

void testRemovedHostFunction(Realms& realms)
{
    VM& vm = realms.vm();
    JSGlobalObject* realm = realms.plain();
    MarkedArgumentBuffer noArguments;
    JSFunction* removed = JSFunction::create(vm, realm, 0, "removed"_s, removedHostFunction, ImplementationVisibility::Public, NoIntrinsic, removedHostFunction);
    AOT_TEST_CHECK(throwsForFeature(realms, hostFunctionFeature, [&] {
        call(realm, removed, noArguments, "not callable"_s);
    }));
    AOT_TEST_CHECK(throwsForFeature(realms, hostFunctionFeature, [&] {
        construct(realm, removed, noArguments, "not a constructor"_s);
    }));

    JSFunction* kept = JSFunction::create(vm, realm, 0, "kept"_s, keptHostFunction, ImplementationVisibility::Public);
    AOT_TEST_CHECK(call(realm, kept, noArguments, "not callable"_s) == jsNumber(keptValue));
}

void testRemovedCustomAccessor(Realms& realms)
{
    VM& vm = realms.vm();
    CustomGetterSetter* removed = CustomGetterSetter::create(vm, removedGetter, removedSetter);
    AOT_TEST_CHECK(removed->getter() == GetValueFunc(CustomGetterSetter::getRemovedProperty) && removed->setter() == PutValueFunc(CustomGetterSetter::setRemovedProperty));
    CustomGetterSetter* halfRemoved = CustomGetterSetter::create(vm, keptGetter, removedSetter);
    AOT_TEST_CHECK(halfRemoved->getter() == GetValueFunc(keptGetter) && halfRemoved->setter() == PutValueFunc(CustomGetterSetter::setRemovedProperty));
    CustomGetterSetter* readOnly = CustomGetterSetter::create(vm, removedGetter, nullptr);
    AOT_TEST_CHECK(readOnly->getter() == GetValueFunc(CustomGetterSetter::getRemovedProperty) && !readOnly->setter());

    JSObject* holder = createHolder(realms, static_cast<unsigned>(PropertyAttribute::CustomAccessor));
    checkAccessorsOf(realms, holder);
    AOT_TEST_CHECK(storeThrows(realms, createObjectInheritingFrom(realms, holder)));

    JSObject* shadowing = createObjectInheritingFrom(realms, holder);
    shadowing->putDirect(vm, removedName(realms), jsNumber(1));
    PropertySlot slot(shadowing, PropertySlot::InternalMethodType::Get);
    AOT_TEST_CHECK(holder->getPropertySlot(realms.plain(), removedName(realms), slot));
    AOT_TEST_CHECK(throwsForFeature(realms, getterFeature, [&] {
        slot.getValue(realms.plain(), removedName(realms));
    }));
}

void testRemovedCustomValue(Realms& realms)
{
    checkAccessorsOf(realms, createHolder(realms, static_cast<unsigned>(PropertyAttribute::CustomValue)));
}

void testRemovedDOMAttribute(Realms& realms)
{
    VM& vm = realms.vm();
    DOMAttributeAnnotation annotation { JSObject::info(), nullptr };
    JSObject* holder = constructEmptyObject(realms.plain());
    holder->putDirectCustomAccessor(vm, removedName(realms), DOMAttributeGetterSetter::create(vm, removedGetter, removedSetter, annotation), static_cast<unsigned>(PropertyAttribute::CustomAccessor));
    holder->putDirectCustomAccessor(vm, keptName(realms), DOMAttributeGetterSetter::create(vm, keptGetter, keptSetter, annotation), static_cast<unsigned>(PropertyAttribute::CustomAccessor));
    checkAccessorsOf(realms, holder);
}

const HashTableValue accessorTableValues[] = {
    { "removed"_s, static_cast<unsigned>(PropertyAttribute::CustomAccessor), NoIntrinsic, { HashTableValue::GetterSetterType, removedGetter, removedSetter } },
    { "kept"_s, static_cast<unsigned>(PropertyAttribute::CustomAccessor), NoIntrinsic, { HashTableValue::GetterSetterType, keptGetter, keptSetter } },
};

const HashTableValue domAttributeTableValues[] = {
    { "removed"_s, PropertyAttribute::CustomAccessor | PropertyAttribute::DOMAttribute, NoIntrinsic, { HashTableValue::GetterSetterType, removedGetter, removedSetter } },
    { "kept"_s, PropertyAttribute::CustomAccessor | PropertyAttribute::DOMAttribute, NoIntrinsic, { HashTableValue::GetterSetterType, keptGetter, keptSetter } },
};

void testRemovedAccessorsOfReifiedTable(Realms& realms)
{
    JSObject* holder = constructEmptyObject(realms.plain());
    reifyStaticProperties(realms.vm(), JSObject::info(), accessorTableValues, *holder);
    checkAccessorsOf(realms, holder);

    JSObject* holderOfDOMAttributes = constructEmptyObject(realms.plain());
    reifyStaticProperties(realms.vm(), JSObject::info(), domAttributeTableValues, *holderOfDOMAttributes);
    checkAccessorsOf(realms, holderOfDOMAttributes);
}

void testRemovedAccessorsThroughInlineCaches(Realms& realms)
{
    constexpr unsigned iterations = 20000;
    JSGlobalObject* realm = realms.plain();
    realm->putDirect(realms.vm(), Identifier::fromString(realms.vm(), "holder"_s), createHolder(realms, static_cast<unsigned>(PropertyAttribute::CustomAccessor)));
    realm->putDirect(realms.vm(), Identifier::fromString(realms.vm(), "iterations"_s), jsNumber(iterations));

    JSValue thrown = realms.evaluate(realm, R"JS((function () {
        const object = Object.create(holder);
        let thrown = 0;
        for (let i = 0; i < iterations; i++) {
            try { object.removed; } catch { thrown++; }
            try { object.removed = i; } catch { thrown++; }
        }
        return thrown;
    })())JS"_s);
    AOT_TEST_CHECK(thrown == jsNumber(2 * iterations) && handlerLog.throwCount == 2 * iterations);

    JSValue sum = realms.evaluate(realm, R"JS((function () {
        const object = Object.create(holder);
        let sum = 0;
        for (let i = 0; i < iterations; i++) {
            sum += object.kept;
            object.kept = i;
        }
        return sum;
    })())JS"_s);
    AOT_TEST_CHECK(sum == jsNumber(keptValue * iterations) && handlerLog.throwCount == 2 * iterations);
    AOT_TEST_CHECK(nativeCalls.keptGetter == iterations && nativeCalls.keptSetter == iterations);
}

void testRemovedAccessorOfUnrelatedReceiver(Realms& realms)
{
    JSGlobalObject* realm = realms.plain();
    realm->putDirect(realms.vm(), Identifier::fromString(realms.vm(), "holder"_s), createHolder(realms, static_cast<unsigned>(PropertyAttribute::CustomAccessor)));
    static constexpr ASCIILiteral sources[] = {
        "Reflect.get(holder, 'removed', { })"_s,
        "Reflect.set(holder, 'removed', 1, { })"_s,
        "Object.getOwnPropertyDescriptor(holder, 'removed').get.call({ })"_s,
        "Object.getOwnPropertyDescriptor(holder, 'removed').set.call({ }, 1)"_s,
        "new Proxy(holder, { }).removed"_s,
    };
    for (ASCIILiteral source : sources) {
        unsigned throwCount = handlerLog.throwCount;
        AOT_TEST_CHECK_FOR_SOURCE(realms.throwsTypeError(realm, source), source);
        AOT_TEST_CHECK_FOR_SOURCE(handlerLog.throwCount == throwCount + 1 && handlerLog.thrownFeature == removableFeatureIndexLimit, source);
    }
    AOT_TEST_CHECK(realms.evaluate(realm, "Reflect.get(holder, 'kept', { })"_s) == jsNumber(keptValue));
}

void testListedFunctionsWithoutTrimming()
{
    listRemovedFunctions();
    callAfterStateChange([] {
        Realms realms;
        VM& vm = realms.vm();
        CustomGetterSetter* accessor = CustomGetterSetter::create(vm, removedGetter, removedSetter);
        AOT_TEST_CHECK(accessor->getter() == GetValueFunc(removedGetter) && accessor->setter() == PutValueFunc(removedSetter));
        AOT_TEST_CHECK(!accessor->removedGetterFeature() && !accessor->removedSetterFeature());

        JSObject* holderOfTable = constructEmptyObject(realms.plain());
        reifyStaticProperties(vm, JSObject::info(), domAttributeTableValues, *holderOfTable);
        for (JSObject* holder : { createHolder(realms, static_cast<unsigned>(PropertyAttribute::CustomAccessor)), createHolder(realms, static_cast<unsigned>(PropertyAttribute::CustomValue)), holderOfTable }) {
            AOT_TEST_CHECK(holder->get(realms.plain(), removedName(realms)) == jsNumber(2));
            PutPropertySlot slot(holder, true);
            AOT_TEST_CHECK(store(realms, holder, removedName(realms), slot));
        }

        MarkedArgumentBuffer noArguments;
        JSFunction* function = JSFunction::create(vm, realms.plain(), 0, "removed"_s, removedHostFunction, ImplementationVisibility::Public);
        AOT_TEST_CHECK(call(realms.plain(), function, noArguments, "not callable"_s) == jsNumber(1));
    });
    AOT_TEST_CHECK(nativeCalls.removedGetter == 3 && nativeCalls.removedSetter == 3 && nativeCalls.removedHostFunction == 1 && !handlerLog.throwCount);
}

void testRemovalOfUnusedFeature()
{
    constexpr auto feature = RemovableFeature::Inspector;
    AOT_TEST_CHECK(callAfterStateChange([&] { return removalOf(feature); }) == FeatureRemoval::Present);
    AOT_TEST_CHECK(!callAfterStateChange([&] { return isFeatureRemoved(feature); }));

    setRemoval(feature, FeatureRemoval::Unused);
    AOT_TEST_CHECK(callAfterStateChange([&] { return removalOf(feature); }) == FeatureRemoval::Unused);
    AOT_TEST_CHECK(callAfterStateChange([&] { return isFeatureRemoved(feature); }));
}

void testEmbedderFeature()
{
    auto callFirst = [] { return callRemovable<firstEmbedderFeature, ReturnDefault, functionReturningInt>(1); };
    auto callLast = [] { return callRemovable<lastEmbedderFeature, ReturnDefault, functionReturningInt>(1); };
    AOT_TEST_CHECK(callAfterStateChange(callFirst) == 2);
    AOT_TEST_CHECK(callAfterStateChange(callLast) == 2);

    removeFeature(firstEmbedderFeature);
    AOT_TEST_CHECK(!callAfterStateChange(callFirst));
    AOT_TEST_CHECK(callAfterStateChange(callLast) == 2);

    removeFeature(lastEmbedderFeature);
    AOT_TEST_CHECK(!callAfterStateChange(callLast));
}

void testOneFeatureDoesNotRemoveAnother()
{
    removeFeature(RemovableFeature::JIT);
    AOT_TEST_CHECK(callAfterStateChange([] { return callRemovable<RemovableFeature::Interpreter, ReturnDefault, functionReturningInt>(1); }) == 2);
    AOT_TEST_CHECK(!handlerLog.abortCount);
}

} // namespace

std::span<const NativeTest> removableFeatureTests()
{
    static constexpr NativeTest tests[] = {
        { "removable-feature/recorded-pointer"_s, runWithCleanFeatureState<testRecordedPointer> },
        { "removable-feature/recorded-table-holds-functions"_s, runWithCleanFeatureState<testRecordedTableHoldsFunctions> },
        { "removable-feature/guarded-call"_s, runWithCleanFeatureState<testGuardedCall> },
        { "removable-feature/address-unless-removed"_s, runWithCleanFeatureState<testAddressUnlessRemoved> },
        { "removable-feature/guarded-body"_s, runWithCleanFeatureState<testGuardedBody> },
        { "removable-feature/host-function-definition"_s, runWithCleanFeatureState<testHostFunctionDefinition> },
        { "removable-feature/guarded-builtin-call"_s, runWithCleanFeatureState<testGuardedBuiltinCall> },
        { "removable-feature/removed-feature-at"_s, runWithCleanFeatureState<testRemovedFeatureAt> },
        { "removable-feature/removed-host-function"_s, runInTrimmedExecutable<testRemovedHostFunction> },
        { "removable-feature/removed-custom-accessor"_s, runInTrimmedExecutable<testRemovedCustomAccessor> },
        { "removable-feature/removed-custom-value"_s, runInTrimmedExecutable<testRemovedCustomValue> },
        { "removable-feature/removed-dom-attribute"_s, runInTrimmedExecutable<testRemovedDOMAttribute> },
        { "removable-feature/removed-accessors-of-reified-table"_s, runInTrimmedExecutable<testRemovedAccessorsOfReifiedTable> },
        { "removable-feature/removed-accessors-through-inline-caches"_s, runInTrimmedExecutable<testRemovedAccessorsThroughInlineCaches> },
        { "removable-feature/removed-accessor-of-unrelated-receiver"_s, runInTrimmedExecutable<testRemovedAccessorOfUnrelatedReceiver> },
        { "removable-feature/listed-functions-without-trimming"_s, runWithCleanFeatureState<testListedFunctionsWithoutTrimming> },
        { "removable-feature/removal-of-unused-feature"_s, runWithCleanFeatureState<testRemovalOfUnusedFeature> },
        { "removable-feature/embedder-feature"_s, runWithCleanFeatureState<testEmbedderFeature> },
        { "removable-feature/one-feature-does-not-remove-another"_s, runWithCleanFeatureState<testOneFeatureDoesNotRemoveAnother> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
