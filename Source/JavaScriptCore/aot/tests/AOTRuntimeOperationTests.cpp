/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTEffectsDeclared.h"
#include "AOTHost.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTOpcode.h"
#include "AOTOperationCallScope.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaOperations.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTTemporaryFile.h"
#include "AOTVector.h"
#include "ClonedArguments.h"
#include "DeferTermination.h"
#include "JSCInlines.h"
#include "JSSet.h"
#include "ObjectConstructor.h"
#include "OperationResult.h"
#include "Options.h"
#include "TopExceptionScope.h"
#include "VMTrapsInlines.h"
#include <bit>
#include <type_traits>
#include <wtf/DataLog.h>
#include <wtf/HashSet.h>
#include <wtf/SetForScope.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static constexpr ASCIILiteral callsAreCountedTest = "runtime-operations/calls-are-counted-with-counts-path"_s;
static constexpr ASCIILiteral callsAreNotCountedTest = "runtime-operations/calls-are-not-counted-by-default"_s;
static constexpr ASCIILiteral unusedEntriesTrapTest = "runtime-operations/unused-entries-trap"_s;
static constexpr ASCIILiteral unmappedArgumentsTest = "runtime-operations/unmapped-arguments-come-from-frame-and-rest"_s;
static constexpr ASCIILiteral readsOfArgumentsTest = "runtime-operations/reads-of-arguments-that-were-never-created"_s;
static constexpr ASCIILiteral appendElementsTest = "runtime-operations/append-elements"_s;
static constexpr ASCIILiteral arrayLiteralsTest = "runtime-operations/array-literals-are-pristine-and-contiguous"_s;
static constexpr ASCIILiteral listsOfMembersTest = "runtime-operations/lists-of-members-have-the-storage-that-arrays-of-the-realm-have"_s;
static constexpr ASCIILiteral badTimeStopsTest = "runtime-operations/a-realm-with-an-image-that-begins-to-have-a-bad-time-stops"_s;
static constexpr ASCIILiteral pushesAndHolesTest = "runtime-operations/pushes-and-holes"_s;
static constexpr ASCIILiteral deferredTerminationTest = "runtime-operations/poll-throws-deferred-termination-after-the-deferral"_s;
static constexpr ASCIILiteral pollInCallStubTest = "runtime-operations/poll-in-call-stub-leaves-pending-out-of-memory-error"_s;

static constexpr ASCIILiteral programText =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0() handler=b2():\n"
    "    v0 = BigInt digits=\"12345678901234567890123\"\n"
    "    v1 = Add v0, v0\n"
    "    Jump -> next b1(v1)\n"
    "  b1(v2):\n"
    "    Return v2\n"
    "  b2() role=CatchHandler:\n"
    "    v3 = CatchValue\n"
    "    Return\n"
    "}\n"_s;

namespace {

struct AppendCase {
    ASCIILiteral target;
    ASCIILiteral source;
};

} // anonymous namespace

static constexpr AppendCase appendCases[] = {
    { "Array.of(1, 2, 3)"_s, "count(100)"_s },
    { "(a => (a.length = 3, a))(count(40))"_s, "[4, 5, 6]"_s },
    { "Array.of('a', 'b')"_s, "[4, 5, 6]"_s },
    { "Array.of('a', 'b')"_s, "[3.5, 4.5]"_s },
    { "Array.of(1.5, 2.5)"_s, "[3.5]"_s },
    { "Array.of(1, 2, 3)"_s, ""_s },
    { "Array.of(1, 2, 3)"_s, "count(200000)"_s },
    { "Array.of(1.5)"_s, "['a']"_s },
    { "Array.of(1)"_s, "[1.5]"_s },
    { "Array.of(1)"_s, "[, 2]"_s },
    { "Array.of(1.5)"_s, "[, 2.5]"_s },
    { "[]"_s, "[1, 2]"_s },
    { "[1, 2, 3]"_s, "[4]"_s },
    { "[1, 2, 3]"_s, "[]"_s },
    { "(a => (a.foo = 1, a))(Array.of(1, 2, 3))"_s, "[4]"_s },
    { "(a => (a[100000] = 1, a))(Array.of(1, 2, 3))"_s, "[4]"_s },
};

static void runtimeColumnNamesOperation()
{
    AOT_TEST_CHECK(Typed::runtimeOf(Typed::Opcode::String) == RuntimeOperation::StringConstant);
}

static void operationMayShareNameWithRuntimeOperation()
{
    AOT_TEST_CHECK(Typed::runtimeOf(Typed::Opcode::NewClass) == RuntimeOperation::NewClass);
}

static void entryWithoutOperationHasNone()
{
    AOT_TEST_CHECK(!Typed::runtimeOf(Typed::Opcode::Upcast));
}

static void collectingOperationNeedsCollectingEntry()
{
    unsigned numberOfOperations = 0;
    unsigned numberOfHiddenCollections = 0;
    for (unsigned index = 0; index < numberOfEnumerators<Typed::Opcode>; ++index) {
        auto opcode = static_cast<Typed::Opcode>(index);
        std::optional<RuntimeOperation> operation = Typed::runtimeOf(opcode);
        if (!operation)
            continue;
        ++numberOfOperations;
        if (!Typed::declaredBoundOf(*operation).mayCollect || Typed::mayCollect(opcode))
            continue;
        dataLogLn(Typed::nameOf(opcode), " does not collect, its run-time operation does, and a call has the effects that both declare");
        ++numberOfHiddenCollections;
    }
    AOT_TEST_CHECK(numberOfOperations);
    AOT_TEST_CHECK(!numberOfHiddenCollections);
}

static void entriesAreNamedAfterWhatTheyHold()
{
    AOT_TEST_CHECK(nameOf(Entry::operationAOTValueAdd) == "operationAOTValueAdd"_s);
    AOT_TEST_CHECK(nameOf(Entry::operationAOTLookupExceptionHandler) == "operationAOTLookupExceptionHandler"_s);
    AOT_TEST_CHECK(nameOf(Entry::operationArrayIndexOfValueInt32OrContiguous) == "operationArrayIndexOfValueInt32OrContiguous"_s);
    AOT_TEST_CHECK(nameOf(Entry::HandleException) == "HandleException"_s);
    AOT_TEST_CHECK(nameOf(Entry::VirtualCallTrampoline) == "VirtualCallTrampoline"_s);
    AOT_TEST_CHECK(nameOf(Entry::RawToString) == "RawToString"_s);
}

static void entryNamesAreDistinct()
{
    HashSet<ASCIILiteral> names;
    for (unsigned index = 0; index < numberOfEntries; ++index) {
        ASCIILiteral name = nameOf(static_cast<Entry>(index));
        AOT_TEST_CHECK(!name.isEmpty());
        AOT_TEST_CHECK(names.add(name).isNewEntry);
    }
}

static bool isUsedByImage(Entry entry)
{
    unsigned index = static_cast<unsigned>(entry);
    return Image::current()->header().entriesUsed[index / 32] >> index % 32 & 1;
}

static void* entryIn(VM& vm, Entry entry)
{
    return vm.aotState().runtimeTable->entries()[static_cast<unsigned>(entry)];
}

static void checkUnusedEntriesTrap(LoadedProgram& program)
{
    VM& vm = program.vm();
    void* trap = nullptr;
    for (unsigned index = 0; index < numberOfEntries; ++index) {
        Entry entry = static_cast<Entry>(index);
        if ((!isOperation(entry) && !isRawOperation(entry)) || isUsedByImage(entry))
            continue;
        AOT_TEST_CHECK(entryIn(vm, entry));
        AOT_TEST_CHECK(!trap || entryIn(vm, entry) == trap);
        trap = entryIn(vm, entry);
    }
    AOT_TEST_CHECK(trap);

    unsigned numberOfOperationsUsed = 0;
    for (unsigned index = 0; index < numberOfEntries; ++index) {
        Entry entry = static_cast<Entry>(index);
        if (!isUsedByImage(entry))
            continue;
        numberOfOperationsUsed += isOperation(entry);
        if (entryIn(vm, entry) != trap)
            continue;
        dataLogLn(nameOf(entry), " is used by the image and traps");
        failNativeTest(__FILE__, __LINE__, "an entry that the image uses is not the trap");
    }
    AOT_TEST_CHECK(numberOfOperationsUsed);
    AOT_TEST_CHECK(isUsedByImage(Entry::operationAOTValueAdd));
    AOT_TEST_CHECK(entryIn(vm, Entry::HandleException) && entryIn(vm, Entry::HandleException) != trap);
    AOT_TEST_CHECK(entryIn(vm, Entry::VirtualCallTrampoline) && entryIn(vm, Entry::VirtualCallTrampoline) != trap);
}

template<typename Result>
static auto valueOf(const Result& result)
{
    if constexpr (std::is_same_v<Result, ExceptionOperationResult<void>>)
        return;
    else if constexpr (isExceptionOperationResult<Result>)
        return result.value;
    else
        return result;
}

template<typename Operation, typename... Arguments>
static auto callFromNativeCode(LoadedProgram& program, Operation operation, Arguments... arguments)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    SetForScope topCallFrame(vm.topCallFrame, vm.topCallFrame);
    SetForScope topEntryFrame(vm.topEntryFrame, static_cast<EntryFrame*>(Thread::currentSingleton().stack().origin()));
    auto result = operation(&program.instance(), arguments...);
    AOT_TEST_CHECK(!scope.exception());
    return valueOf(result);
}

static JSValue addThroughTable(LoadedProgram& program, JSValue left, JSValue right)
{
    auto add = untagCFunctionPtr<decltype(&operationAOTValueAdd), OperationPtrTag>(entryIn(program.vm(), Entry::operationAOTValueAdd));
    return JSValue::decode(callFromNativeCode(program, add, JSValue::encode(left), JSValue::encode(right)));
}

static JSArray* createArrayOfNumbers(JSGlobalObject* realm, int32_t first, uint32_t length)
{
    JSArray* array = constructEmptyArray(realm, nullptr);
    for (uint32_t i = 0; i < length; ++i)
        array->push(realm, jsNumber(first + static_cast<int32_t>(i)));
    return array;
}

static void checkUnmappedArguments(LoadedProgram& program, uint32_t count)
{
    constexpr uint32_t numberOfParameters = 2;
    constexpr int32_t firstArgument = 10;
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    EncodedJSValue parameters[numberOfParameters] = { JSValue::encode(jsNumber(firstArgument)), JSValue::encode(jsNumber(firstArgument + 1)) };
    JSArray* rest = createArrayOfNumbers(realm, firstArgument + numberOfParameters, count > numberOfParameters ? count - numberOfParameters : 0);

    auto* arguments = dynamicDowncast<ClonedArguments>(callFromNativeCode(program, operationAOTCreateUnmappedArguments, count, rest, parameters));
    AOT_TEST_CHECK(arguments);
    AOT_TEST_CHECK(arguments->get(realm, vm.propertyNames->length) == jsNumber(count));
    for (uint32_t i = 0; i < count; ++i)
        AOT_TEST_CHECK(arguments->get(realm, i) == jsNumber(firstArgument + static_cast<int32_t>(i)));
    AOT_TEST_CHECK(arguments->get(realm, count).isUndefined());
    AOT_TEST_CHECK(arguments->get(realm, vm.propertyNames->iteratorSymbol) == JSValue(realm->arrayProtoValuesFunction()));
    AOT_TEST_CHECK(!scope.exception());
}

static void checkUnmappedArgumentsFromRest(LoadedProgram& program, JSArray* rest, std::span<const JSValue> expected)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    uint32_t count = static_cast<uint32_t>(expected.size());
    auto* arguments = dynamicDowncast<ClonedArguments>(callFromNativeCode(program, operationAOTCreateUnmappedArguments, count, rest, static_cast<const EncodedJSValue*>(nullptr)));
    AOT_TEST_CHECK(arguments);
    AOT_TEST_CHECK(arguments->get(realm, vm.propertyNames->length) == jsNumber(count));
    for (uint32_t i = 0; i < count; ++i)
        AOT_TEST_CHECK(arguments->get(realm, i) == expected[i]);
    AOT_TEST_CHECK(!scope.exception());

    arguments->get(realm, vm.propertyNames->callee);
    AOT_TEST_CHECK(scope.exception());
    scope.clearException();
}

static void checkUnmappedArgumentsOfEveryCount(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    for (unsigned round = 0; round < 3; ++round) {
        for (uint32_t count : { 0, 1, 2, 3, 6 })
            checkUnmappedArguments(program, count);

        JSArray* doubles = constructEmptyArray(realm, nullptr);
        doubles->push(realm, jsNumber(1.5));
        doubles->push(realm, jsNumber(2.5));
        checkUnmappedArgumentsFromRest(program, doubles, std::array { jsNumber(1.5), jsNumber(2.5) });

        JSArray* holey = constructEmptyArray(realm, nullptr, 2);
        holey->putDirectIndex(realm, 1, jsNumber(7));
        checkUnmappedArgumentsFromRest(program, holey, std::array { jsUndefined(), jsNumber(7) });

        JSArray* cells = constructEmptyArray(realm, nullptr);
        cells->push(realm, realm);
        cells->push(realm, jsNumber(3));
        checkUnmappedArgumentsFromRest(program, cells, std::array { JSValue(realm), jsNumber(3) });

        // No arguments object is alive here, and the next round finds the Structure of the last one again.
        vm.heap.collectSync(CollectionScope::Full);
    }
}

static void checkReadsOfArguments(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSArray* list = createArrayOfNumbers(program.realm(), 10, 2);
    auto read = [&](JSValue property) {
        return JSValue::decode(callFromNativeCode(program, operationAOTGetFromArguments, list, JSValue::encode(property)));
    };
    AOT_TEST_CHECK(read(jsNumber(0)) == jsNumber(10));
    AOT_TEST_CHECK(read(jsNumber(1)) == jsNumber(11));
    AOT_TEST_CHECK(read(jsNumber(2)).isUndefined());
    AOT_TEST_CHECK(read(jsNontrivialString(vm, "length"_s)) == jsNumber(2));
    AOT_TEST_CHECK(read(jsNontrivialString(vm, "nameNobodyHas"_s)).isUndefined());
}

static void checkCallsAreCounted(LoadedProgram& program)
{
    VM& vm = program.vm();
    AOT_TEST_CHECK(Options::aotCountsPath());
    AOT_TEST_CHECK(vm.aotState().runtimeTable->countsCalls());

    uint64_t countBefore = operationCallCount(vm, Entry::operationAOTValueAdd);
    uint64_t otherCountBefore = operationCallCount(vm, Entry::operationAOTValueSub);
    AOT_TEST_CHECK(addThroughTable(program, jsNumber(1), jsNumber(2)) == jsNumber(3));
    AOT_TEST_CHECK(addThroughTable(program, jsNumber(3), jsNumber(4)) == jsNumber(7));
    AOT_TEST_CHECK(operationCallCount(vm, Entry::operationAOTValueAdd) == countBefore + 2);
    AOT_TEST_CHECK(operationCallCount(vm, Entry::operationAOTValueSub) == otherCountBefore);
}

static void checkCallsAreNotCounted(LoadedProgram& program)
{
    VM& vm = program.vm();
    AOT_TEST_CHECK(!Options::aotCountsPath());
    AOT_TEST_CHECK(!vm.aotState().runtimeTable->countsCalls());
    AOT_TEST_CHECK(addThroughTable(program, jsNumber(1), jsNumber(2)) == jsNumber(3));
    AOT_TEST_CHECK(!operationCallCount(vm, Entry::operationAOTValueAdd));
}

static Vector<EncodedJSValue> elementsOf(JSGlobalObject* realm, JSArray* array)
{
    return Vector<EncodedJSValue>(array->length(), [&](size_t index) {
        JSValue element = array->getIndex(realm, static_cast<unsigned>(index));
        return JSValue::encode(element.isNumber() ? jsNumber(element.asNumber()) : element);
    });
}

static void checkAppendElements(LoadedProgram& program)
{
    Realms& realms = program.realms();
    JSGlobalObject* realm = program.realm();
    realms.evaluate(realm, "function count(length) { var a = []; for (var i = 0; i < length; i++) a.push(i); return a; }"_s);
    for (const AppendCase& appendCase : appendCases) {
        JSArray* target = realms.array(realm, appendCase.target);
        JSArray* source = appendCase.source.isEmpty() ? target : realms.array(realm, appendCase.source);
        bool hadArrayStorage = hasAnyArrayStorage(target->indexingType());
        Vector<EncodedJSValue> sourceElements = elementsOf(realm, source);
        Vector<EncodedJSValue> expected = elementsOf(realm, target);
        expected.appendVector(sourceElements);

        callFromNativeCode(program, operationAOTAppendElements, target, source);
        AOT_TEST_CHECK_FOR_SOURCE(elementsOf(realm, target) == expected, appendCase.target);
        AOT_TEST_CHECK_FOR_SOURCE(source == target || elementsOf(realm, source) == sourceElements, appendCase.target);
        AOT_TEST_CHECK_FOR_SOURCE(hasAnyArrayStorage(target->indexingType()) == hadArrayStorage, appendCase.target);
        target->validatePristine(program.vm());
    }
}

static void checkListsOfMembers(LoadedProgram& program)
{
    Realms& realms = program.realms();
    JSGlobalObject* realm = program.realm();
    struct SetAndMembers {
        ASCIILiteral set;
        ASCIILiteral members;
    };
    static constexpr SetAndMembers cases[] = {
        { "new Set()"_s, "[]"_s },
        { "new Set([1, 2, 3])"_s, "[1, 2, 3]"_s },
        { "new Set([1.5, 2.5])"_s, "[1.5, 2.5]"_s },
        { "new Set([true, 1, null])"_s, "[true, 1, null]"_s },
    };
    for (const auto& [source, membersSource] : cases) {
        JSObject* set = realms.object(realm, source);
        Vector<EncodedJSValue> members = elementsOf(realm, realms.array(realm, membersSource));

        auto* list = dynamicDowncast<JSArray>(callFromNativeCode(program, operationAOTNewArrayOfMembers, set, static_cast<uint64_t>(ArrayWithInt32)));
        AOT_TEST_CHECK_FOR_SOURCE(list && elementsOf(realm, list) == members, source);
        AOT_TEST_CHECK_FOR_SOURCE(!hasAnyArrayStorage(list->indexingType()), source);

        JSArray* target = realms.array(realm, "[0].slice()"_s);
        callFromNativeCode(program, operationAOTAppendMembers, target, set);
        members.insert(0, JSValue::encode(jsNumber(0)));
        AOT_TEST_CHECK_FOR_SOURCE(elementsOf(realm, target) == members, source);
        AOT_TEST_CHECK_FOR_SOURCE(!hasAnyArrayStorage(target->indexingType()), source);
    }
}

static void haveABadTime(LoadedProgram& program)
{
    program.realm()->haveABadTime(program.vm());
}

static void checkArrayLiterals(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    EncodedJSValue values[] = { JSValue::encode(jsNumber(1)), JSValue::encode(jsUndefined()), JSValue::encode(jsNumber(2.5)), JSValue::encode(realm) };
    for (uint32_t count = 0; count <= std::size(values); ++count) {
        auto* array = dynamicDowncast<JSArray>(callFromNativeCode(program, operationAOTNewArray, count, values));
        AOT_TEST_CHECK(array && array->length() == count);
        AOT_TEST_CHECK(array->indexingType() == ArrayWithContiguous && array->isPristine());
        AOT_TEST_CHECK(elementsOf(realm, array) == Vector<EncodedJSValue>(std::span { values }.first(count)));
        array->validatePristine(program.vm());
    }
}

static void checkPushesAndHoles(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    JSArray* array = createArrayOfNumbers(realm, 10, 1);
    AOT_TEST_CHECK(JSValue::decode(callFromNativeCode(program, operationAOTArrayPush, array, JSValue::encode(jsNumber(11)))) == jsNumber(2));
    AOT_TEST_CHECK(callFromNativeCode(program, operationAOTAppendHole, array) == array);
    AOT_TEST_CHECK(array->length() == 3 && !array->canGetIndexQuickly(2u));
    AOT_TEST_CHECK(JSValue::decode(callFromNativeCode(program, operationAOTArrayPushNumber, array, std::bit_cast<uint64_t>(1.5))) == jsNumber(4));
    AOT_TEST_CHECK(elementsOf(realm, array) == Vector<EncodedJSValue>({ JSValue::encode(jsNumber(10)), JSValue::encode(jsNumber(11)), JSValue::encode(jsUndefined()), JSValue::encode(jsNumber(1.5)) }));
    AOT_TEST_CHECK(!hasAnyArrayStorage(array->indexingType()));
    array->validatePristine(program.vm());
}

static void checkPollThrowsDeferredTerminationAfterTheDeferral(LoadedProgram& program)
{
    VM& vm = program.vm();
    vm.ensureTerminationException();
    {
        DeferTerminationForAWhile deferTermination(vm);
        vm.notifyNeedTermination();
        callFromNativeCode(program, operationAOTPollSlow);
        AOT_TEST_CHECK(vm.traps().needHandling(VMTraps::NeedTermination));
    }
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    callOperationFromTest(vm, operationAOTPollSlow, &program.instance());
    AOT_TEST_CHECK(scope.exception() && vm.hasPendingTerminationException());
    scope.clearException();
    vm.clearHasTerminationRequest();
}

// No collection has run inside the polled call.
static void checkPollInCallStubLeavesPendingOutOfMemoryError(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    vm.heap.cage().didFindNoRoomWithoutCollecting();
    vm.traps().setNeedsOutOfMemoryError(true);
    void* returnAddressesOfPolledCall[2] = { &vm, &vm };
    callOperationFromTest(vm, operationAOTPollBeforeEntering, &program.instance(), static_cast<void*>(returnAddressesOfPolledCall + 2));
    AOT_TEST_CHECK(!scope.exception() && vm.traps().needsOutOfMemoryError() && vm.heap.cage().owesCollection());
    vm.traps().setNeedsOutOfMemoryError(false);
}

static void unusedEntriesTrap()
{
    runWithLoadedProgram(unusedEntriesTrapTest, programText, checkUnusedEntriesTrap);
}

static void unmappedArgumentsComeFromFrameAndRest()
{
    runWithLoadedProgram(unmappedArgumentsTest, programText, checkUnmappedArgumentsOfEveryCount);
}

static void readsOfArgumentsThatWereNeverCreated()
{
    runWithLoadedProgram(readsOfArgumentsTest, programText, checkReadsOfArguments);
}

static void appendElements()
{
    runWithLoadedProgram(appendElementsTest, programText, checkAppendElements);
}

static void arrayLiteralsArePristineAndContiguous()
{
    runWithLoadedProgram(arrayLiteralsTest, programText, checkArrayLiterals);
}

static void listsOfMembersHaveTheStorageThatArraysOfTheRealmHave()
{
    runWithLoadedProgram(listsOfMembersTest, programText, checkListsOfMembers);
}

static void realmWithImageThatBeginsToHaveABadTimeStops()
{
    runWithLoadedProgramThatStops(badTimeStopsTest, programText, haveABadTime, "AOT: the realm of a compiled program is having a bad time"_s);
}

static void pushesAndHoles()
{
    runWithLoadedProgram(pushesAndHolesTest, programText, checkPushesAndHoles);
}

// A build with assertions checks in the child that somebody looks for an exception behind every call that may throw.
static void pollThrowsDeferredTerminationAfterTheDeferral()
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(deferredTerminationTest, programText, checkPollThrowsDeferredTerminationAfterTheDeferral);
        return;
    }

    TemporaryFile image(".image"_s, compileProgramText(programText).span());
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments {
        String(nativeSubcommandName), String(singleProcessFlag), String(deferredTerminationTest),
        makeString("--aotImagePath="_s, image.path()), "--validateExceptionChecks=true"_s,
    };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process that polls ", result.description());
        failNativeTest(__FILE__, __LINE__, deferredTerminationTest.characters());
    }
}

static void pollInCallStubLeavesPendingOutOfMemoryError()
{
    runWithLoadedProgram(pollInCallStubTest, programText, checkPollInCallStubLeavesPendingOutOfMemoryError);
}

static void callsAreNotCountedByDefault()
{
    runWithLoadedProgram(callsAreNotCountedTest, programText, checkCallsAreNotCounted);
}

static void callsAreCountedWithCountsPath()
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(callsAreCountedTest, programText, checkCallsAreCounted);
        return;
    }

    TemporaryFile image(".image"_s, compileProgramText(programText).span());
    TemporaryFile counts(".counts"_s, { });
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments {
        String(nativeSubcommandName), String(singleProcessFlag), String(callsAreCountedTest),
        makeString("--aotImagePath="_s, image.path()), makeString("--aotCountsPath="_s, counts.path()),
    };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process that counts calls ", result.description());
        failNativeTest(__FILE__, __LINE__, callsAreCountedTest.characters());
    }
}

static void callCountIsZeroWithoutImage()
{
    Realms realms;
    AOT_TEST_CHECK(!realms.vm().aotState().runtimeTable);
    AOT_TEST_CHECK(!operationCallCount(realms.vm(), Entry::operationAOTValueAdd));
}

std::span<const NativeTest> runtimeOperationTests()
{
    static constexpr NativeTest tests[] = {
        { "runtimeColumnNamesOperation"_s, runtimeColumnNamesOperation },
        { "operationMayShareNameWithRuntimeOperation"_s, operationMayShareNameWithRuntimeOperation },
        { "entryWithoutOperationHasNone"_s, entryWithoutOperationHasNone },
        { "collectingOperationNeedsCollectingEntry"_s, collectingOperationNeedsCollectingEntry },
        { "runtime-operations/entries-are-named-after-what-they-hold"_s, entriesAreNamedAfterWhatTheyHold },
        { "runtime-operations/entry-names-are-distinct"_s, entryNamesAreDistinct },
        { unusedEntriesTrapTest, unusedEntriesTrap },
        { callsAreNotCountedTest, callsAreNotCountedByDefault },
        { callsAreCountedTest, callsAreCountedWithCountsPath },
        { "runtime-operations/call-count-is-zero-without-image"_s, callCountIsZeroWithoutImage },
        { unmappedArgumentsTest, unmappedArgumentsComeFromFrameAndRest },
        { readsOfArgumentsTest, readsOfArgumentsThatWereNeverCreated },
        { appendElementsTest, appendElements },
        { arrayLiteralsTest, arrayLiteralsArePristineAndContiguous },
        { listsOfMembersTest, listsOfMembersHaveTheStorageThatArraysOfTheRealmHave },
        { badTimeStopsTest, realmWithImageThatBeginsToHaveABadTimeStops },
        { pushesAndHolesTest, pushesAndHoles },
        { deferredTerminationTest, pollThrowsDeferredTerminationAfterTheDeferral },
        { pollInCallStubTest, pollInCallStubLeavesPendingOutOfMemoryError },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
