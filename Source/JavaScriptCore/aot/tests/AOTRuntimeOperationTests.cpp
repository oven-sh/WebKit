/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTChildProcess.h"
#include "AOTEffectsDeclared.h"
#include "AOTGrowingText.h"
#include "AOTHost.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTOpcode.h"
#include "AOTOperationCallScope.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaOperations.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTStubTestSupport.h"
#include "AOTTemporaryFile.h"
#include "AOTVector.h"
#include "ClonedArguments.h"
#include "Completion.h"
#include "DeferTermination.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "OperationResult.h"
#include "Options.h"
#include "TopExceptionScope.h"
#include "VMEntryScopeInlines.h"
#include "VMTrapsInlines.h"
#include "Watchdog.h"
#include <setjmp.h>
#include <stdlib.h>
#include <type_traits>
#include <wtf/DataLog.h>
#include <wtf/FileSystem.h>
#include <wtf/HashSet.h>
#include <wtf/ProcessID.h>
#include <wtf/SetForScope.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

static constexpr ASCIILiteral callsThroughRuntimeTableTest = "runtime-operations/operations-are-called-through-the-table"_s;
static constexpr ASCIILiteral unusedEntriesTrapTest = "runtime-operations/unused-entries-trap"_s;
static constexpr ASCIILiteral unmappedArgumentsTest = "runtime-operations/unmapped-arguments-come-from-frame-and-rest"_s;
static constexpr ASCIILiteral appendElementsTest = "runtime-operations/append-elements"_s;
#if OS(UNIX)
static constexpr ASCIILiteral radixOutsideOfItsRangeTest = "runtime-operations/a-radix-outside-of-two-to-thirty-six-stops-the-process"_s;
#endif
static constexpr ASCIILiteral arrayLiteralsTest = "runtime-operations/array-literals-are-pristine-and-contiguous"_s;
static constexpr ASCIILiteral badTimeStopsTest = "runtime-operations/a-realm-with-an-image-that-begins-to-have-a-bad-time-stops"_s;
static constexpr ASCIILiteral pushesAndHolesTest = "runtime-operations/pushes-and-holes"_s;
static constexpr ASCIILiteral literalBornHoldingListTest = "runtime-operations/literal-born-holding-list"_s;
static constexpr ASCIILiteral growingTextTest = "runtime-operations/a-text-that-is-added-to-grows-in-a-buffer"_s;
static constexpr ASCIILiteral longStringSharesBufferTest = "runtime-operations/a-long-text-shares-the-characters-of-its-buffer"_s;
static constexpr ASCIILiteral partsOfOtherWidthTest = "runtime-operations/a-growing-text-takes-parts-whose-characters-have-changed-width"_s;
static constexpr ASCIILiteral deferredTerminationTest = "runtime-operations/poll-throws-deferred-termination-after-the-deferral"_s;
static constexpr ASCIILiteral pollShowsRegistersTest = "runtime-operations/poll-that-faults-shows-every-register-to-the-collector"_s;
static constexpr ASCIILiteral pollInCallStubTest = "runtime-operations/poll-in-call-stub-leaves-pending-out-of-memory-error"_s;
static constexpr ASCIILiteral callsAreCountedTest = "runtime-operations/calls-are-counted-under-a-counts-path"_s;
static constexpr ASCIILiteral callsAreNotCountedTest = "runtime-operations/calls-are-not-counted-by-default"_s;
static constexpr ASCIILiteral countsAtExitTest = "runtime-operations/counts-are-written-once-when-the-process-exits"_s;
static constexpr ASCIILiteral countsPathNamesProcessTest = "runtime-operations/counts-path-names-the-process"_s;
static constexpr ASCIILiteral vmWithoutImageCountsTest = "runtime-operations/a-vm-without-an-image-counts-what-it-interprets"_s;

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
        if (!isOperation(entry) || isUsedByImage(entry))
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

        vm.heap.collectSync(CollectionScope::Full);
    }
}

static void checkCallsThroughRuntimeTable(LoadedProgram& program)
{
    AOT_TEST_CHECK(addThroughTable(program, jsNumber(1), jsNumber(2)) == jsNumber(3));
    AOT_TEST_CHECK(addThroughTable(program, jsNumber(3), jsNumber(4)) == jsNumber(7));
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

static void checkLiteralsBornHoldingLists(LoadedProgram& program)
{
    struct Case {
        ASCIILiteral list;
        std::optional<IndexingType> byWhatItHolds;
        std::optional<IndexingType> whereValuesAreNamed;
        bool isOfFractions { false };
    };
    static constexpr Case cases[] = {
        { "[1, 2, 3]"_s, ArrayWithInt32, ArrayWithContiguous },
        { "Array.of(1, 2, 3)"_s, ArrayWithInt32, ArrayWithContiguous },
        { "['x', 1, 2, 3].slice(1)"_s, ArrayWithInt32, ArrayWithContiguous },
        { "[]"_s, ArrayWithInt32, ArrayWithContiguous },
        { "Reflect.construct(Array, [])"_s, std::nullopt, std::nullopt },
        { "(a => (a.pop(), a))([1])"_s, ArrayWithInt32, ArrayWithContiguous },
        { "['x'].slice(1)"_s, ArrayWithInt32, ArrayWithContiguous },
        { "['x', 2, 3]"_s, ArrayWithContiguous, ArrayWithContiguous },
        { "[1, 'x', 3]"_s, ArrayWithContiguous, ArrayWithContiguous },
        { "[1, 2, 'x']"_s, ArrayWithContiguous, ArrayWithContiguous },
        { "['x', 1, 2.5, 3].slice(1)"_s, ArrayWithContiguous, ArrayWithContiguous },
        { "[1, , 3]"_s, ArrayWithContiguous, ArrayWithContiguous },
        { "['x', , 3]"_s, ArrayWithContiguous, ArrayWithContiguous },
        { "[0.5, 1.5, -0, Infinity]"_s, ArrayWithDouble, ArrayWithContiguous, true },
        { "(a => (a.pop(), a))([0.5])"_s, ArrayWithDouble, ArrayWithContiguous, true },
        { "(a => (delete a[1], a))([0.5, 1.5, 2.5])"_s, ArrayWithContiguous, ArrayWithContiguous, true },
        { "(a => (delete a[0], a))([0.5, 1.5])"_s, ArrayWithContiguous, ArrayWithContiguous, true },
        { "(a => (a[0] = 4, delete a[2], a))([0.5, 1, 2, 3])"_s, ArrayWithContiguous, ArrayWithContiguous, true },
        { "(a => (a[0] = 4, a))([0.5, 1, 2, 3])"_s, ArrayWithDouble, ArrayWithContiguous, true },
        { "(a => (a[1000] = 1, a))([1])"_s, std::nullopt, std::nullopt },
    };
    Realms& realms = program.realms();
    JSGlobalObject* realm = program.realm();
    for (const Case& given : cases) {
        for (bool namesValues : { false, true }) {
            JSArray* list = realms.array(realm, given.list);
            AOT_TEST_CHECK_FOR_SOURCE(hasDouble(list->indexingType()) == given.isOfFractions, given.list);
            if (list->length() > maxHoleArrayCapacity) {
                AOT_TEST_CHECK_FOR_SOURCE(!callFromNativeCode(program, operationAOTNewArrayOfList, list, maxHoleArrayCapacity << 1 | namesValues), given.list);
                continue;
            }
            const uint32_t capacity = (list->length() + 2) | 1;
            Vector<EncodedJSValue> elements = elementsOf(realm, list);
            JSCell* cell = callFromNativeCode(program, operationAOTNewArrayOfList, list, capacity << 1 | namesValues);
            std::optional<IndexingType> expected = namesValues ? given.whereValuesAreNamed : given.byWhatItHolds;
            AOT_TEST_CHECK_FOR_SOURCE(!!cell == !!expected && elementsOf(realm, list) == elements, given.list);
            if (!cell)
                continue;
            auto* born = dynamicDowncast<JSArray>(cell);
            AOT_TEST_CHECK_FOR_SOURCE(born && born != list && born->indexingType() == *expected && born->isPristine(), given.list);
            AOT_TEST_CHECK_FOR_SOURCE(born->length() == elements.size() && born->butterfly()->vectorLength() == capacity && elementsOf(realm, born) == elements, given.list);
            for (uint32_t index = 0; index < capacity; ++index)
                AOT_TEST_CHECK_FOR_SOURCE(born->canGetIndexQuickly(index) == (index < elements.size()), given.list);
            born->validatePristine(program.vm());
        }
    }
    JSArray* list = realms.array(realm, "[1, 2, 3]"_s);
    AOT_TEST_CHECK(!callFromNativeCode(program, operationAOTNewArrayOfList, list, 1u << 1));
    AOT_TEST_CHECK(!callFromNativeCode(program, operationAOTNewArrayOfList, list, (maxHoleArrayCapacity + 2) << 1));
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
    AOT_TEST_CHECK(JSValue::decode(callFromNativeCode(program, operationAOTArrayPush, array, JSValue::encode(jsNumber(1.5)))) == jsNumber(4));
    AOT_TEST_CHECK(elementsOf(realm, array) == Vector<EncodedJSValue>({ JSValue::encode(jsNumber(10)), JSValue::encode(jsNumber(11)), JSValue::encode(jsUndefined()), JSValue::encode(jsNumber(1.5)) }));
    AOT_TEST_CHECK(!hasAnyArrayStorage(array->indexingType()));
    array->validatePristine(program.vm());
}

static JSString* joinFromNativeCode(LoadedProgram& program, JSString* left, JSString* right)
{
    return uncheckedDowncast<JSString>(callFromNativeCode(program, operationAOTConcat, static_cast<JSCell*>(left), static_cast<JSCell*>(right)));
}

static String charactersOf(JSString* string)
{
    if (string->is8Bit()) {
        WTF::Vector<Latin1Character> characters(string->length());
        string->resolveToBuffer(characters.mutableSpan());
        return String(characters.span());
    }
    WTF::Vector<char16_t> characters(string->length());
    string->resolveToBuffer(characters.mutableSpan());
    return String(characters.span());
}

static String repeated(ASCIILiteral piece, unsigned times)
{
    StringBuilder text;
    for (unsigned i = 0; i < times; ++i)
        text.append(piece);
    return text.toString();
}

static void checkTextThatIsAddedToGrowsInBuffer(LoadedProgram& program)
{
    constexpr unsigned numberOfPieces = 10 * minAdditionsToGrowingText;
    constexpr unsigned numberOfPiecesOfOlderText = 4 * minAdditionsToGrowingText;
    constexpr unsigned maxNumberOfBuffers = 4;
    VM& vm = program.vm();
    Instance& instance = program.instance();
    JSString* piece = jsNontrivialString(vm, "ab"_s);

    JSString* text = piece;
    JSString* older = nullptr;
    JSString* lastBuffer = nullptr;
    unsigned numberOfBuffers = 0;
    for (unsigned count = 2; count <= numberOfPieces; ++count) {
        text = joinFromNativeCode(program, text, piece);
        AOT_TEST_CHECK(canGrowInPlace(instance, text) == (count - 2 >= minAdditionsToGrowingText));
        if (count == numberOfPiecesOfOlderText)
            older = text;
        if (!canGrowInPlace(instance, text))
            continue;
        JSString* buffer = uncheckedDowncast<JSRopeString>(text)->substringBase();
        numberOfBuffers += buffer != lastBuffer;
        lastBuffer = buffer;
    }
    AOT_TEST_CHECK(numberOfBuffers == maxNumberOfBuffers);
    AOT_TEST_CHECK(charactersOf(text) == repeated("ab"_s, numberOfPieces));

    for (unsigned timesOfMinLength : { 10u, 100u }) {
        JSString* longPiece = jsNontrivialString(vm, repeated("0123456789"_s, timesOfMinLength * minLengthOfGrowingText));
        JSString* longText = piece;
        for (unsigned additions = 0; additions < minAdditionsToGrowingText; ++additions) {
            longText = joinFromNativeCode(program, longText, additions == 1 ? longPiece : piece);
            AOT_TEST_CHECK(longText->isNonSubstringRope());
        }
        bool additionsAreShort = timesOfMinLength == 10;
        JSString* longer = joinFromNativeCode(program, longText, piece);
        AOT_TEST_CHECK(canGrowInPlace(instance, longer) == additionsAreShort && longer->length() == longText->length() + piece->length());
    }
    JSString* sharedPiece = jsNontrivialString(vm, repeated("0123456789"_s, 100 * minLengthOfGrowingText));
    JSString* textOfLongPieces = text;
    for (unsigned additions = 0; additions < 4; ++additions)
        textOfLongPieces = joinFromNativeCode(program, textOfLongPieces, sharedPiece);
    AOT_TEST_CHECK(textOfLongPieces->isNonSubstringRope() && canGrowInPlace(instance, text));

    AOT_TEST_CHECK(!canGrowInPlace(instance, older) && charactersOf(older) == repeated("ab"_s, numberOfPiecesOfOlderText));
    JSString* other = joinFromNativeCode(program, older, jsNontrivialString(vm, "XY"_s));
    AOT_TEST_CHECK(charactersOf(other) == makeString(repeated("ab"_s, numberOfPiecesOfOlderText), "XY"_s));
    AOT_TEST_CHECK(charactersOf(text) == repeated("ab"_s, numberOfPieces) && canGrowInPlace(instance, text));

    AOT_TEST_CHECK(charactersOf(joinFromNativeCode(program, text, text)) == repeated("ab"_s, 2 * numberOfPieces) && canGrowInPlace(instance, text));

    constexpr unsigned numberOfPiecesOfPart = 1000;
    constexpr unsigned numberOfPiecesWithPart = numberOfPieces + numberOfPiecesOfPart;
    JSString* lengthened = joinFromNativeCode(program, text, jsSubstring(program.realm(), text, 0, 2 * numberOfPiecesOfPart));
    AOT_TEST_CHECK(charactersOf(lengthened) == repeated("ab"_s, numberOfPiecesWithPart) && canGrowInPlace(instance, lengthened));
    bool isInBufferOfText = uncheckedDowncast<JSRopeString>(lengthened)->substringBase() == uncheckedDowncast<JSRopeString>(text)->substringBase();
    AOT_TEST_CHECK(canGrowInPlace(instance, text) == !isInBufferOfText);

    const char16_t wideCharacters[] = { 0x20ac, 0x20ac };
    String wide { std::span<const char16_t> { wideCharacters } };
    JSString* widened = joinFromNativeCode(program, lengthened, jsNontrivialString(vm, wide));
    AOT_TEST_CHECK(!widened->is8Bit() && canGrowInPlace(instance, widened));
    JSString* longer = joinFromNativeCode(program, widened, piece);
    AOT_TEST_CHECK(uncheckedDowncast<JSRopeString>(longer)->substringBase() == uncheckedDowncast<JSRopeString>(widened)->substringBase());
    AOT_TEST_CHECK(charactersOf(longer) == makeString(repeated("ab"_s, numberOfPiecesWithPart), wide, "ab"_s));
    AOT_TEST_CHECK(charactersOf(lengthened) == repeated("ab"_s, numberOfPiecesWithPart));

    JSString* inBuffer = joinFromNativeCode(program, lengthened, piece);
    JSString* belowRopes = inBuffer;
    String expected = charactersOf(inBuffer);
    for (unsigned count = 0; count < 100; ++count) {
        belowRopes = jsString(program.realm(), belowRopes, jsString(program.realm(), jsString(program.realm(), piece, piece), jsNontrivialString(vm, "cd"_s)));
        expected = makeString(expected, "ababcd"_s);
    }
    AOT_TEST_CHECK(!canGrowInPlace(instance, belowRopes) && canGrowInPlace(instance, inBuffer));
    JSString* written = joinFromNativeCode(program, belowRopes, piece);
    AOT_TEST_CHECK(canGrowInPlace(instance, written) && !canGrowInPlace(instance, inBuffer));
    AOT_TEST_CHECK(uncheckedDowncast<JSRopeString>(written)->substringBase() == uncheckedDowncast<JSRopeString>(inBuffer)->substringBase());
    AOT_TEST_CHECK(charactersOf(written) == makeString(expected, "ab"_s) && charactersOf(belowRopes) == expected);

    AOT_TEST_CHECK(longer->value(program.realm()).data == charactersOf(joinFromNativeCode(program, widened, piece)) && !longer->isRope());

    constexpr unsigned numberOfPiecesBetweenCollections = 100000;
    constexpr size_t maxNumberOfCellsLeft = 100;
    text = lengthened;
    vm.heap.collectNow(Sync, CollectionScope::Full);
    size_t numberOfCellsBefore = vm.heap.objectCount();
    for (unsigned count = 0; count < numberOfPiecesBetweenCollections; ++count)
        text = joinFromNativeCode(program, text, piece);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(vm.heap.objectCount() < numberOfCellsBefore + maxNumberOfCellsLeft);
    AOT_TEST_CHECK(text->length() == 2 * (numberOfPiecesWithPart + numberOfPiecesBetweenCollections));
}

static void checkLongStringSharesBuffer(LoadedProgram& program)
{
    VM& vm = program.vm();
    Instance& instance = program.instance();
    JSGlobalObject* realm = program.realm();
    constexpr unsigned tensPerPiece = 1;
    JSString* piece = jsNontrivialString(vm, repeated("0123456789"_s, tensPerPiece));
    unsigned numberOfPieces = 1;
    JSString* text = piece;
    auto grow = [&](unsigned length) {
        for (; text->length() < length; ++numberOfPieces)
            text = joinFromNativeCode(program, text, piece);
    };

    grow(minLengthSharedWithBuffer / 2);
    AOT_TEST_CHECK(canGrowInPlace(instance, text));
    JSString* shortText = jsString(realm, text, jsNontrivialString(vm, "ab"_s));
    AOT_TEST_CHECK(!tryResolveLongText(instance, shortText) && shortText->isRope());

    grow(minLengthSharedWithBuffer);
    AOT_TEST_CHECK(canGrowInPlace(instance, text));
    String expected = repeated("0123456789"_s, tensPerPiece * numberOfPieces);
    const StringImpl* buffer = uncheckedDowncast<JSRopeString>(text)->substringBase()->tryGetValueImpl();
    JSString* pieceOfRopes = jsString(realm, jsNontrivialString(vm, "ab"_s), jsString(realm, jsNontrivialString(vm, "cd"_s), jsNontrivialString(vm, "ef"_s)));
    JSString* first = jsString(realm, jsString(realm, text, pieceOfRopes), jsNontrivialString(vm, "first"_s));
    JSString* second = jsString(realm, text, jsNontrivialString(vm, "second"_s));
    JSString* tooLong = jsString(realm, text, jsNontrivialString(vm, repeated("0123456789"_s, 4 * minLengthSharedWithBuffer)));
    AOT_TEST_CHECK(first->isNonSubstringRope() && second->isNonSubstringRope() && tooLong->isNonSubstringRope());
    AOT_TEST_CHECK(!tryResolveLongText(instance, text) && !tryResolveLongText(instance, tooLong) && tooLong->isRope());

    AOT_TEST_CHECK(tryResolveLongText(instance, first) && !first->isRope());
    const StringImpl* shared = first->tryGetValueImpl();
    AOT_TEST_CHECK(shared->span8().data() >= buffer->span8().data() && shared->span8().data() + shared->length() <= buffer->span8().data() + buffer->length());
    AOT_TEST_CHECK(charactersOf(first) == makeString(expected, "abcdeffirst"_s));
    AOT_TEST_CHECK(!canGrowInPlace(instance, text) && charactersOf(text) == expected);

    AOT_TEST_CHECK(!tryResolveLongText(instance, second) && second->value(realm).data == makeString(expected, "second"_s));
    AOT_TEST_CHECK(charactersOf(first) == makeString(expected, "abcdeffirst"_s));
    JSString* longer = joinFromNativeCode(program, text, jsNontrivialString(vm, "third"_s));
    AOT_TEST_CHECK(charactersOf(longer) == makeString(expected, "third"_s) && charactersOf(first) == makeString(expected, "abcdeffirst"_s));

    first = jsString(realm, first, jsNontrivialString(vm, "more"_s));
    AOT_TEST_CHECK(!tryResolveLongText(instance, first) && charactersOf(first) == makeString(expected, "abcdeffirstmore"_s));
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(first->value(realm).data == makeString(expected, "abcdeffirstmore"_s) && charactersOf(longer) == makeString(expected, "third"_s));
}

static String copyIn16Bits(const String& text)
{
    std::span<char16_t> characters;
    Ref<StringImpl> copy = StringImpl::createUninitialized(text.length(), characters);
    StringView(text).getCharacters(characters);
    return String(WTF::move(copy));
}

static void checkGrowingTextTakesPartsWhoseCharactersHaveChangedWidth(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    Instance& instance = program.instance();
    JSString* piece = jsNontrivialString(vm, "ab"_s);
    WTF::Vector<AtomString> names;
    unsigned numberOfTexts = 0;
    auto nextText = [&](unsigned length) {
        StringBuilder text;
        text.append('~', 10 + numberOfTexts++);
        while (text.length() < length)
            text.append('z');
        return text.toString().left(length);
    };
    auto makeNarrow = [&](unsigned length) {
        JSString* string = jsNontrivialString(vm, nextText(length));
        AOT_TEST_CHECK(string->is8Bit() && !string->isRope());
        return string;
    };
    auto makeWide = [&](unsigned length) {
        JSString* string = jsNontrivialString(vm, copyIn16Bits(nextText(length)));
        AOT_TEST_CHECK(!string->is8Bit() && !string->isRope());
        return string;
    };
    auto takeCharactersOfName = [&](JSString* string, const String& name) {
        names.append(AtomString(name));
        AOT_TEST_CHECK(names.last().is8Bit() == name.is8Bit());
        AOT_TEST_CHECK(string->toExistingAtomString(realm).data == names.last().impl());
        AOT_TEST_CHECK(!string->isRope() && string->is8Bit() == name.is8Bit());
    };
    auto turnWide = [&](JSString* string) {
        AOT_TEST_CHECK(string->is8Bit());
        takeCharactersOfName(string, copyIn16Bits(charactersOf(string)));
    };
    auto turnNarrow = [&](JSString* string) {
        AOT_TEST_CHECK(!string->is8Bit());
        takeCharactersOfName(string, String::make8Bit(charactersOf(string).span16()));
    };
    auto bufferOf = [&](JSString* text) {
        AOT_TEST_CHECK(canGrowInPlace(instance, text));
        return uncheckedDowncast<JSRopeString>(text)->substringBase();
    };
    auto append = [&](JSString* text, std::initializer_list<JSString*> pieces) {
        JSString* grown = tryAppendToGrowingText(instance, text, std::span<JSString* const> { pieces.begin(), pieces.size() });
        AOT_TEST_CHECK(grown);
        return grown;
    };

    JSString* text = piece;
    while (!canGrowInPlace(instance, text))
        text = joinFromNativeCode(program, text, piece);
    JSString* buffer = bufferOf(text);
    String expected = charactersOf(text);
    AOT_TEST_CHECK(text->is8Bit());

    for (unsigned length : { 2u, 3u, 8u, 16u, 17u, 40u }) {
        JSString* part = makeNarrow(length);
        JSString* joined = jsString(realm, text, part);
        expected = makeString(expected, charactersOf(part), "ab"_s);
        turnWide(part);
        AOT_TEST_CHECK(joined->isNonSubstringRope() && joined->is8Bit());
        text = append(joined, { piece });
        AOT_TEST_CHECK(text->is8Bit() && bufferOf(text) == buffer && charactersOf(text) == expected);
    }

    {
        JSString* first = makeNarrow(17);
        JSString* second = makeNarrow(40);
        JSString* third = makeNarrow(5);
        JSString* added = jsString(realm, jsString(realm, first, piece), jsString(realm, second, third, first));
        JSString* appendedRope = jsString(realm, jsString(realm, third, second), piece);
        JSString* joined = jsString(realm, text, added);
        expected = makeString(expected, charactersOf(added), charactersOf(appendedRope));
        for (JSString* leaf : { first, second, third })
            turnWide(leaf);
        AOT_TEST_CHECK(joined->is8Bit() && appendedRope->is8Bit() && appendedRope->isNonSubstringRope());
        text = append(joined, { appendedRope });
        AOT_TEST_CHECK(text->is8Bit() && bufferOf(text) == buffer && charactersOf(text) == expected);
        text = append(text, { third });
        AOT_TEST_CHECK(!text->is8Bit() && bufferOf(text) != buffer && charactersOf(text) == makeString(expected, charactersOf(third)));
    }

    {
        text = joinFromNativeCode(program, piece, piece);
        while (!canGrowInPlace(instance, text))
            text = joinFromNativeCode(program, text, piece);
        buffer = bufferOf(text);
        expected = charactersOf(text);
        JSString* whole = makeNarrow(40);
        JSString* cut = jsSubstring(realm, whole, 3, 30);
        JSString* other = jsSubstring(realm, whole, 20, 19);
        JSString* joined = jsString(realm, text, cut);
        expected = makeString(expected, charactersOf(cut), charactersOf(other), charactersOf(cut));
        turnWide(whole);
        AOT_TEST_CHECK(cut->isSubstring() && cut->is8Bit() && other->isSubstring() && other->is8Bit());
        text = append(joined, { other, cut });
        AOT_TEST_CHECK(text->is8Bit() && bufferOf(text) == buffer && charactersOf(text) == expected);
    }

    {
        JSString* rope = makeNarrow(17);
        expected = charactersOf(rope);
        for (unsigned additions = 0; additions < minAdditionsToGrowingText; ++additions) {
            JSString* part = makeNarrow(17 + additions % 3);
            rope = jsString(realm, rope, part);
            expected = makeString(expected, charactersOf(part));
            turnWide(part);
        }
        AOT_TEST_CHECK(rope->is8Bit() && !canGrowInPlace(instance, rope));
        text = append(rope, { piece });
        AOT_TEST_CHECK(text->is8Bit() && canGrowInPlace(instance, text) && charactersOf(text) == makeString(expected, "ab"_s));
    }

    {
        const char16_t beyondLatin1[] = { 0x2713, 0x2713 };
        text = joinFromNativeCode(program, text, jsNontrivialString(vm, String { std::span<const char16_t> { beyondLatin1 } }));
        buffer = bufferOf(text);
        expected = charactersOf(text);
        AOT_TEST_CHECK(!text->is8Bit());
        JSString* narrow = makeNarrow(17);
        JSString* turnsWide = makeNarrow(18);
        JSString* turnsNarrow = makeWide(19);
        JSString* cut = jsSubstring(realm, turnsNarrow, 2, 15);
        JSString* added = jsString(realm, jsString(realm, narrow, turnsWide), jsString(realm, turnsNarrow, cut));
        JSString* joined = jsString(realm, text, added);
        expected = makeString(expected, charactersOf(added), charactersOf(cut), charactersOf(turnsNarrow));
        turnWide(turnsWide);
        turnNarrow(turnsNarrow);
        AOT_TEST_CHECK(cut->isSubstring() && !cut->is8Bit());
        text = append(joined, { cut, turnsNarrow });
        AOT_TEST_CHECK(!text->is8Bit() && bufferOf(text) == buffer && charactersOf(text) == expected);
    }
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

#if AOT_LOOP_POLLS_FAULT && OS(LINUX)

static constexpr ASCIILiteral programThatSpins =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "    v0 = NewClosure function=f1\n"
    "    Return v0\n"
    "}\n"
    "function f1 name=\"spins\" results=[{type=anything}] {\n"
    "    Jump -> next b1()\n"
    "  b1():\n"
    "    LoopHint\n"
    "    Jump -> next b1()\n"
    "}\n"_s;

static constexpr uint64_t maskOfCellsAtPoll = 0x5555555555555555ULL;
static jmp_buf s_placeToComeBackToFromPoll;
static uint64_t s_maskedCellsOfRegistersAtPoll[32];
static uint64_t s_registersWhoseCellsWereFound;

NEVER_INLINE static void makeCellForEveryRegisterAtPoll(JSGlobalObject* realm)
{
    for (GPRReg reg : allocatableGPRs)
        s_maskedCellsOfRegistersAtPoll[static_cast<unsigned>(reg)] = std::bit_cast<uint64_t>(constructEmptyObject(realm)) ^ maskOfCellsAtPoll;
}

NEVER_INLINE static void clearStackBelowCaller()
{
    volatile uint64_t words[8192];
    for (volatile uint64_t& word : words)
        word = 0;
}

NEVER_INLINE static bool lookForCellsWithEyesOfCollector(JSGlobalObject* realm, void*, void*)
{
    clearStackBelowCaller();
    Heap& heap = realm->vm().heap;
    heap.collectNow(Sync, CollectionScope::Full);
    for (GPRReg reg : allocatableGPRs) {
        if (heap.isMarked(std::bit_cast<const void*>(s_maskedCellsOfRegistersAtPoll[static_cast<unsigned>(reg)] ^ maskOfCellsAtPoll)))
            s_registersWhoseCellsWereFound |= uint64_t { 1 } << static_cast<unsigned>(reg);
    }
    longjmp(s_placeToComeBackToFromPoll, 1);
}

[[noreturn]] NEVER_INLINE static void enterPollWithCellInEveryRegister(Instance* instance, const uint8_t* poll)
{
#if CPU(X86_64)
    static_assert(instanceGPR == X86Registers::r13 && MacroAssembler::s_scratchRegister == X86Registers::r11);
    asm volatile(
        "movq %0, %%r13\n"
        "movq %1, %%r11\n"
        "andq $-16, %%rsp\n"
        "subq $8, %%rsp\n"
        "movabsq $0x5555555555555555, %%r14\n"
        "movq 0(%%rdx), %%rax\n"
        "xorq %%r14, %%rax\n"
        "movq 8(%%rdx), %%rcx\n"
        "xorq %%r14, %%rcx\n"
        "movq 24(%%rdx), %%rbx\n"
        "xorq %%r14, %%rbx\n"
        "movq 48(%%rdx), %%rsi\n"
        "xorq %%r14, %%rsi\n"
        "movq 56(%%rdx), %%rdi\n"
        "xorq %%r14, %%rdi\n"
        "movq 64(%%rdx), %%r8\n"
        "xorq %%r14, %%r8\n"
        "movq 72(%%rdx), %%r9\n"
        "xorq %%r14, %%r9\n"
        "movq 80(%%rdx), %%r10\n"
        "xorq %%r14, %%r10\n"
        "movq 96(%%rdx), %%r12\n"
        "xorq %%r14, %%r12\n"
        "movq 120(%%rdx), %%r15\n"
        "xorq %%r14, %%r15\n"
        "movq 16(%%rdx), %%rdx\n"
        "xorq %%r14, %%rdx\n"
        "xorl %%r14d, %%r14d\n"
        "jmpq *%%r11\n"
        :
        : "D"(instance), "S"(poll), "d"(s_maskedCellsOfRegistersAtPoll)
        : "memory");
#elif CPU(ARM64)
    static_assert(instanceGPR == ARM64Registers::x26);
    asm volatile(
        "mov x26, %0\n"
        "mov x17, %1\n"
        "mov x16, %2\n"
        "mov x27, sp\n"
        "and x27, x27, #-16\n"
        "mov sp, x27\n"
        "mov x27, #0x5555555555555555\n"
        "ldr x0, [x16, #0]\n"
        "eor x0, x0, x27\n"
        "ldr x1, [x16, #8]\n"
        "eor x1, x1, x27\n"
        "ldr x2, [x16, #16]\n"
        "eor x2, x2, x27\n"
        "ldr x3, [x16, #24]\n"
        "eor x3, x3, x27\n"
        "ldr x4, [x16, #32]\n"
        "eor x4, x4, x27\n"
        "ldr x5, [x16, #40]\n"
        "eor x5, x5, x27\n"
        "ldr x6, [x16, #48]\n"
        "eor x6, x6, x27\n"
        "ldr x7, [x16, #56]\n"
        "eor x7, x7, x27\n"
        "ldr x8, [x16, #64]\n"
        "eor x8, x8, x27\n"
        "ldr x9, [x16, #72]\n"
        "eor x9, x9, x27\n"
        "ldr x10, [x16, #80]\n"
        "eor x10, x10, x27\n"
        "ldr x11, [x16, #88]\n"
        "eor x11, x11, x27\n"
        "ldr x12, [x16, #96]\n"
        "eor x12, x12, x27\n"
        "ldr x13, [x16, #104]\n"
        "eor x13, x13, x27\n"
        "ldr x14, [x16, #112]\n"
        "eor x14, x14, x27\n"
        "ldr x15, [x16, #120]\n"
        "eor x15, x15, x27\n"
        "ldr x19, [x16, #152]\n"
        "eor x19, x19, x27\n"
        "ldr x20, [x16, #160]\n"
        "eor x20, x20, x27\n"
        "ldr x21, [x16, #168]\n"
        "eor x21, x21, x27\n"
        "ldr x22, [x16, #176]\n"
        "eor x22, x22, x27\n"
        "ldr x23, [x16, #184]\n"
        "eor x23, x23, x27\n"
        "ldr x24, [x16, #192]\n"
        "eor x24, x24, x27\n"
        "mov x16, xzr\n"
        "mov x27, xzr\n"
        "br x17\n"
        :
        : "r"(instance), "r"(poll), "r"(s_maskedCellsOfRegistersAtPoll)
        : "memory", "x16", "x17", "x26", "x27");
#endif
    __builtin_unreachable();
}

static void checkPollThatFaultsShowsEveryRegisterToCollector(LoadedProgram& program)
{
    const Image* image = Image::current();
    const uint8_t* poll = nullptr;
    for (const uint8_t& byte : image->code()) {
        std::optional<FaultSite> site = image->faultSiteAt(&byte);
        if (!site || site->kind != FaultSiteKind::LoopPoll)
            continue;
        AOT_TEST_CHECK(!poll && site->hasFrame == isARM64());
        poll = &byte;
    }
    AOT_TEST_CHECK(poll);

    VM& vm = program.vm();
    Instance& instance = program.instance();
    Watchdog& watchdog = vm.ensureWatchdog();
    watchdog.setTimeLimit(0_s, lookForCellsWithEyesOfCollector);
    VMEntryScope entryScope(vm, program.realm());
    SetForScope topCallFrame(vm.topCallFrame, vm.topCallFrame);
    SetForScope topEntryFrame(vm.topEntryFrame, static_cast<EntryFrame*>(Thread::currentSingleton().stack().origin()));
    if (!watchdog.isActive())
        watchdog.enteredVM();
    vm.notifyNeedWatchdogCheck();
    makeCellForEveryRegisterAtPoll(program.realm());
    clearStackBelowCaller();
    setPollsEnabled(instance, true);
    s_registersWhoseCellsWereFound = 0;
    if (!setjmp(s_placeToComeBackToFromPoll))
        enterPollWithCellInEveryRegister(&instance, poll);
    setPollsEnabled(instance, false);
    watchdog.setTimeLimit(Watchdog::noTimeLimit);

    uint64_t registersThatHeldCells = 0;
    for (GPRReg reg : allocatableGPRs)
        registersThatHeldCells |= uint64_t { 1 } << static_cast<unsigned>(reg);
    if (s_registersWhoseCellsWereFound != registersThatHeldCells)
        dataLogLn("registers whose cells were marked: ", RawHex(s_registersWhoseCellsWereFound), " of ", RawHex(registersThatHeldCells));
    AOT_TEST_CHECK(s_registersWhoseCellsWereFound == registersThatHeldCells);
}

static void pollThatFaultsShowsEveryRegisterToCollector()
{
    runWithLoadedProgram(pollShowsRegistersTest, programThatSpins, checkPollThatFaultsShowsEveryRegisterToCollector);
}

#endif

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

static void checkCallsAreCounted(LoadedProgram& program)
{
    const RuntimeCounts* counts = program.vm().aotState().counts.get();
    AOT_TEST_CHECK(counts);
    AOT_TEST_CHECK(countsCallsOf(Entry::operationAOTValueAdd) && countsCallsOf(Entry::operationAOTValueSub));
    AOT_TEST_CHECK(!countsCallsOf(Entry::HandleException));
    uint64_t additions = counts->numberOfCalls(Entry::operationAOTValueAdd);
    uint64_t subtractions = counts->numberOfCalls(Entry::operationAOTValueSub);
    checkCallsThroughRuntimeTable(program);
    AOT_TEST_CHECK(counts->numberOfCalls(Entry::operationAOTValueAdd) == additions + 2);
    AOT_TEST_CHECK(counts->numberOfCalls(Entry::operationAOTValueSub) == subtractions);
}

static void checkCallsAreNotCounted(LoadedProgram& program)
{
    AOT_TEST_CHECK(!Options::aotCountsPath());
    AOT_TEST_CHECK(!program.vm().aotState().counts);
    auto add = untagCFunctionPtr<decltype(&operationAOTValueAdd), OperationPtrTag>(entryIn(program.vm(), Entry::operationAOTValueAdd));
    AOT_TEST_CHECK(add == &operationAOTValueAdd);
}

static void checkVMWithoutImageCounts(LoadedProgram&)
{
    Thread::create("VM without an image"_s, [] {
        Image::disableOnThisThread();
        RefPtr<VM> vm = VM::create();
        JSLockHolder locker(*vm);
        const RuntimeCounts* counts = vm->aotState().counts.get();
        AOT_TEST_CHECK(!vm->hasCompiledCode() && counts);
        JSGlobalObject* realm = JSGlobalObject::create(*vm, JSGlobalObject::createStructure(*vm, jsNull()));
        uint64_t functions = counts->valueOf(RuntimeCounterID::InterpretedFunctionCode);
        NakedPtr<Exception> exception;
        evaluate(realm, makeSource("(function () { return 1; })()"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted), JSValue(), exception);
        AOT_TEST_CHECK(!exception);
        AOT_TEST_CHECK(counts->valueOf(RuntimeCounterID::InterpretedGlobalCode) == 1);
        AOT_TEST_CHECK(counts->valueOf(RuntimeCounterID::InterpretedFunctionCode) > functions);
        vm = nullptr;
    })->waitForCompletion();
}

static String textOfCounts(const String& path)
{
    auto bytes = FileSystem::readEntireFile(path);
    return bytes ? String::fromUTF8(bytes->span()) : String();
}

static unsigned numberOfLinesThatEndWith(const String& text, ASCIILiteral end)
{
    unsigned number = 0;
    for (StringView line : StringView(text).split('\n'))
        number += line.endsWith(end);
    return number;
}

static String runChildThatCounts(ASCIILiteral test, const String& suffixOfCountsPath = emptyString())
{
    TemporaryFile image(".image"_s, compileProgramText(programText).span());
    TemporaryFile counts(".counts"_s, { });
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments {
        String(nativeSubcommandName), String(singleProcessFlag), String(test),
        makeString("--aotImagePath="_s, image.path()), makeString("--aotCountsPath="_s, counts.path(), suffixOfCountsPath),
    };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process that counts ", result.description());
        failNativeTest(__FILE__, __LINE__, test.characters());
    }
    return textOfCounts(counts.path());
}

static void callsAreCountedUnderCountsPath()
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(callsAreCountedTest, programText, checkCallsAreCounted);
        return;
    }
    String text = runChildThatCounts(callsAreCountedTest);
    AOT_TEST_CHECK(numberOfLinesThatEndWith(text, "\toperation:operationAOTValueAdd"_s) == 1);
    AOT_TEST_CHECK(numberOfLinesThatEndWith(text, "0\toperation:operationAOTValueSub"_s) == 1);
    AOT_TEST_CHECK(!numberOfLinesThatEndWith(text, "\toperation:HandleException"_s));
    AOT_TEST_CHECK(numberOfLinesThatEndWith(text, "\tcounter:InterpretedFunctionCode"_s) == 1);
}

static void callsAreNotCountedByDefault()
{
    runWithLoadedProgram(callsAreNotCountedTest, programText, checkCallsAreNotCounted);
}

static void countsAreWrittenOnceWhenProcessExits()
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(countsAtExitTest, programText, [](LoadedProgram&) {
            AOT_TEST_CHECK(textOfCounts(String::fromUTF8(byteCast<char>(Options::aotCountsPath()))).isEmpty());
            exit(EXIT_SUCCESS);
        });
        return;
    }
    AOT_TEST_CHECK(numberOfLinesThatEndWith(runChildThatCounts(countsAtExitTest), "\tcounter:InterpretedFunctionCode"_s) == 1);
}

static void countsPathNamesProcess()
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(countsPathNamesProcessTest, programText, [](LoadedProgram&) { });
        String written = makeStringByReplacingAll(String::fromUTF8(byteCast<char>(Options::aotCountsPath())), "%p"_s, String::number(getCurrentProcessID()));
        AOT_TEST_CHECK(numberOfLinesThatEndWith(textOfCounts(written), "\tcounter:InterpretedFunctionCode"_s) == 1);
        AOT_TEST_CHECK(FileSystem::deleteFile(written));
        return;
    }
    AOT_TEST_CHECK(runChildThatCounts(countsPathNamesProcessTest, ".%p"_s).isEmpty());
}

static void vmWithoutImageCountsWhatItInterprets()
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(vmWithoutImageCountsTest, programText, checkVMWithoutImageCounts);
        return;
    }
    AOT_TEST_CHECK(numberOfLinesThatEndWith(runChildThatCounts(vmWithoutImageCountsTest), "\tcounter:InterpretedGlobalCode"_s) == 2);
}

static void unusedEntriesTrap()
{
    runWithLoadedProgram(unusedEntriesTrapTest, programText, checkUnusedEntriesTrap);
}

static void unmappedArgumentsComeFromFrameAndRest()
{
    runWithLoadedProgram(unmappedArgumentsTest, programText, checkUnmappedArgumentsOfEveryCount);
}

static void appendElements()
{
    runWithLoadedProgram(appendElementsTest, programText, checkAppendElements);
}

#if OS(UNIX)
static void checkRadixOutsideOfItsRangeStopsProcess(LoadedProgram& program)
{
    auto outcomeWith = [&](int32_t radix, bool takesInt32) {
        return runInChild([&] {
            if (takesInt32)
                callFromNativeCode(program, operationAOTInt32ToStringWithRadix, 255, radix);
            else
                callFromNativeCode(program, operationAOTNumberToStringWithRadix, 255.5, radix);
        });
    };
    for (bool takesInt32 : { true, false }) {
        for (int32_t radix : { 2, 16, 36 })
            AOT_TEST_CHECK(outcomeWith(radix, takesInt32) == ChildOutcome::Finished);
        for (int32_t radix : { 1, 0, 37, -1 })
            AOT_TEST_CHECK(outcomeWith(radix, takesInt32) == ChildOutcome::Crashed);
    }
}

static void radixOutsideOfItsRangeStopsProcess()
{
    runWithLoadedProgram(radixOutsideOfItsRangeTest, programText, checkRadixOutsideOfItsRangeStopsProcess);
}
#endif

static void arrayLiteralsArePristineAndContiguous()
{
    runWithLoadedProgram(arrayLiteralsTest, programText, checkArrayLiterals);
}

static void realmWithImageThatBeginsToHaveABadTimeStops()
{
    runWithLoadedProgramThatStops(badTimeStopsTest, programText, haveABadTime, "internal AOT error: array prototype check failed"_s);
}

static void literalsAreBornHoldingLists()
{
    runWithLoadedProgram(literalBornHoldingListTest, programText, checkLiteralsBornHoldingLists);
}

static void pushesAndHoles()
{
    runWithLoadedProgram(pushesAndHolesTest, programText, checkPushesAndHoles);
}

static void textThatIsAddedToGrowsInBuffer()
{
    runWithLoadedProgram(growingTextTest, programText, checkTextThatIsAddedToGrowsInBuffer);
}

static void longStringSharesBuffer()
{
    runWithLoadedProgram(longStringSharesBufferTest, programText, checkLongStringSharesBuffer);
}

static void growingTextTakesPartsWhoseCharactersHaveChangedWidth()
{
    runWithLoadedProgram(partsOfOtherWidthTest, programText, checkGrowingTextTakesPartsWhoseCharactersHaveChangedWidth);
}

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

static void operationsAreCalledThroughRuntimeTable()
{
    runWithLoadedProgram(callsThroughRuntimeTableTest, programText, checkCallsThroughRuntimeTable);
}

std::span<const NativeTest> runtimeOperationTests()
{
    static constexpr NativeTest tests[] = {
        { "runtimeColumnNamesOperation"_s, runtimeColumnNamesOperation },
        { "operationMayShareNameWithRuntimeOperation"_s, operationMayShareNameWithRuntimeOperation },
        { "entryWithoutOperationHasNone"_s, entryWithoutOperationHasNone },
        { "collectingOperationNeedsCollectingEntry"_s, collectingOperationNeedsCollectingEntry },
        { "runtime-operations/entry-names-are-distinct"_s, entryNamesAreDistinct },
        { unusedEntriesTrapTest, unusedEntriesTrap },
        { callsThroughRuntimeTableTest, operationsAreCalledThroughRuntimeTable },
        { unmappedArgumentsTest, unmappedArgumentsComeFromFrameAndRest },
        { appendElementsTest, appendElements },
#if OS(UNIX)
        { radixOutsideOfItsRangeTest, radixOutsideOfItsRangeStopsProcess },
#endif
        { arrayLiteralsTest, arrayLiteralsArePristineAndContiguous },
        { badTimeStopsTest, realmWithImageThatBeginsToHaveABadTimeStops },
        { pushesAndHolesTest, pushesAndHoles },
        { literalBornHoldingListTest, literalsAreBornHoldingLists },
        { growingTextTest, textThatIsAddedToGrowsInBuffer },
        { longStringSharesBufferTest, longStringSharesBuffer },
        { partsOfOtherWidthTest, growingTextTakesPartsWhoseCharactersHaveChangedWidth },
        { deferredTerminationTest, pollThrowsDeferredTerminationAfterTheDeferral },
        { pollInCallStubTest, pollInCallStubLeavesPendingOutOfMemoryError },
#if AOT_LOOP_POLLS_FAULT && OS(LINUX)
        { pollShowsRegistersTest, pollThatFaultsShowsEveryRegisterToCollector },
#endif
        { callsAreCountedTest, callsAreCountedUnderCountsPath },
        { callsAreNotCountedTest, callsAreNotCountedByDefault },
        { countsAtExitTest, countsAreWrittenOnceWhenProcessExits },
        { countsPathNamesProcessTest, countsPathNamesProcess },
        { vmWithoutImageCountsTest, vmWithoutImageCountsWhatItInterprets },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
