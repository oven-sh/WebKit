/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTDataBuilder.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTOperationData.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTPropertyWalk.h"
#include "AOTTemporaryFile.h"
#include "JSArrayInlines.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "Options.h"
#include "TopExceptionScope.h"
#include <wtf/DataLog.h>
#include <wtf/HashSet.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> dataBuilderTests();

static constexpr ASCIILiteral collectionInTheMiddleTest = "data-builder/collection-in-the-middle-loses-nothing"_s;

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Outer" fields=[{key="mixed" declared={type=anything} flags=[IsRequired]}, {key="doubles" declared={type=anything} flags=[IsRequired]}, {key="integers" declared={type=anything} flags=[IsRequired]}, {key="inner" declared={type=anything} flags=[IsRequired]}, {key="big" declared={type=anything} flags=[IsRequired]}, {key="text" declared={type=anything} flags=[IsRequired]}, {key="nested" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Inner" fields=[{key="zero" declared={type=number} flags=[IsRequired]}, {key="nan" declared={type=number} flags=[IsRequired]}, {key="nothing" declared={type=anything} flags=[IsRequired]}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = Data data=d0 @10
    v3 = Call v1, -, v2
    v4 = Data data=d1 @20
    v5 = Call v1, -, v4
    v6 = Data data=d2 @30
    v7 = Call v1, -, v6
    v8 = Data data=d3 @40
    v9 = Call v1, -, v8
    Return
}
data d0 root=Object{layout=L0 members=[{key="mixed" value=Array{contents=anything elements=[Number{value=1}, Hole, String{value="pear"}, Boolean{value=true}, Undefined, Null, Boolean{}]}}, {key="doubles" value=Array{contents=number elements=[Number{value=1.5}, Hole, Number{value=2}, Number{value=-0}]}}, {key="integers" value=Array{contents=number elements=[Number{value=1}, Number{value=2}]}}, {key="inner" value=Object{layout=L1 members=[{key="zero" value=Number{value=-0}}, {key="nan" value=Number{value=NaN}}, {key="nothing" value=Null}]}}, {key="big" value=BigInt{digits="12345678901234567890"}}, {key="text" value=String{value="plum"}}, {key="nested" value=Array{contents=anything elements=[Array{contents=anything elements=[Object{layout=L1 members=[{key="zero" value=Number{value=7}}, {key="nan" value=Number{value=8}}, {key="nothing" value=Undefined}]}]}, Array{contents=anything}]}}]} module=m0
data d1 root=Array{contents=number elements=[Number{value=1}, Number{value=2}, Number{value=3}, Number{value=4}, Number{value=5}, Number{value=6}, Number{value=7}, Number{value=8}, Number{value=9}, Number{value=10}, Number{value=11}, Number{value=12}, Number{value=13}, Number{value=14}, Number{value=15}, Number{value=16}, Number{value=17}]} module=m0
data d2 root=Array{contents=number elements=[Number{value=0.5}, Number{value=2}, Number{value=3}, Number{value=4}, Number{value=5}, Number{value=6}, Number{value=7}, Number{value=8}, Number{value=9}, Number{value=10}, Number{value=11}, Number{value=12}, Number{value=13}, Number{value=14}, Number{value=15}, Number{value=16}, Number{value=-0}]} module=m0
data d3 root=Array{contents=anything elements=[String{value="a0"}, Number{value=2}, Undefined, Null, Boolean{value=true}, String{value="a5"}, Number{value=7}, Number{value=8}, Number{value=9}, Number{value=10}, Number{value=11}, Number{value=12}, Number{value=13}, Number{value=14}, Number{value=15}, Number{value=16}, String{value="a16"}]} module=m0
)IR"_s;

static constexpr ASCIILiteral describedTree = "{mixed:[1,hole,\"pear\",true,undefined,null,false],doubles:[1.5,hole,2,-0],integers:[1,2],inner:{zero:-0,nan:NaN,nothing:null},big:12345678901234567890n,text:\"plum\",nested:[[{zero:7,nan:8,nothing:undefined}],[]]}"_s;
static constexpr ASCIILiteral describedIntegers = "[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17]"_s;
static constexpr ASCIILiteral describedDoubles = "[0.5,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,-0]"_s;
static constexpr ASCIILiteral describedMixture = "[\"a0\",2,undefined,null,true,\"a5\",7,8,9,10,11,12,13,14,15,16,\"a16\"]"_s;

static constexpr ASCIILiteral describeSource = R"js((function describe(value) {
    if (Array.isArray(value)) {
        let parts = [];
        for (let i = 0; i < value.length; ++i)
            parts.push(i in value ? describe(value[i]) : "hole");
        return "[" + parts.join(",") + "]";
    }
    if (typeof value === "object" && value !== null)
        return "{" + Object.keys(value).map(key => key + ":" + describe(value[key])).join(",") + "}";
    if (typeof value === "string")
        return JSON.stringify(value);
    if (typeof value === "bigint")
        return value + "n";
    return Object.is(value, -0) ? "-0" : String(value);
}))js"_s;

enum DataNumber : unsigned { Tree, Integers, Doubles, Mixture, NumberOfData };

static void testDataWordsRoundTrip()
{
    const JSValue immediates[] = {
        jsUndefined(), jsNull(), jsBoolean(true), jsBoolean(false),
        jsNumber(0), jsNumber(-1), jsNumber(INT32_MAX), jsNumber(INT32_MIN),
        jsDoubleNumber(1.5), jsDoubleNumber(-0.0), jsDoubleNumber(PNaN), jsDoubleNumber(std::numeric_limits<double>::infinity()), jsDoubleNumber(5e-324),
    };
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> words;
    AOT_TEST_CHECK(DataWord::kindOf(DataWord::hole) == DataWord::Kind::Immediate);
    AOT_TEST_CHECK(DataWord::encodeImmediate(JSValue()) == DataWord::hole);
    AOT_TEST_CHECK(words.add(DataWord::hole).isNewEntry);
    for (JSValue immediate : immediates) {
        uint64_t word = DataWord::encodeImmediate(immediate);
        AOT_TEST_CHECK(DataWord::kindOf(word) == DataWord::Kind::Immediate);
        AOT_TEST_CHECK(JSValue::encode(JSValue::decode(static_cast<EncodedJSValue>(word))) == JSValue::encode(immediate));
        AOT_TEST_CHECK(words.add(word).isNewEntry);
    }

    for (uint32_t operand : { 0u, 1u, 7u, 8u, 65535u, 1u << 24, UINT32_MAX - 1 }) {
        struct Case {
            uint64_t word;
            DataWord::Kind kind;
        };
        const Case cases[] = {
            { DataWord::encodeStringConstant(operand), DataWord::Kind::StringConstant },
            { DataWord::encodeBigIntConstant(operand), DataWord::Kind::BigIntConstant },
            { DataWord::encodeNode(operand), DataWord::Kind::Node },
        };
        for (const Case& testCase : cases) {
            AOT_TEST_CHECK(DataWord::kindOf(testCase.word) == testCase.kind);
            AOT_TEST_CHECK(DataWord::operandOf(testCase.word) == operand);
            AOT_TEST_CHECK(words.add(testCase.word).isNewEntry);
        }
    }
}

static void testNumberOfWords()
{
    using Kind = DataNodeHeader::Kind;
    struct Case {
        uint32_t count;
        uint32_t wordsOfArray;
        uint32_t wordsOfObject;
    };
    static constexpr Case cases[] = {
        { 0, 1, 1 }, { 1, 2, 3 }, { 3, 4, 5 }, { 4, 5, 6 }, { 5, 6, 8 }, { 8, 9, 11 }, { 9, 10, 13 }, { 1000, 1001, 1251 },
    };
    for (const Case& testCase : cases) {
        AOT_TEST_CHECK(DataNodeHeader::numberOfWords(Kind::Array, testCase.count) == testCase.wordsOfArray);
        AOT_TEST_CHECK(DataNodeHeader::numberOfWords(Kind::Object, testCase.count) == testCase.wordsOfObject);
    }

    for (Kind kind : { Kind::Array, Kind::Object }) {
        DataNodeHeader header { 12, static_cast<uint32_t>(kind), 345 };
        DataNodeHeader decoded = DataNodeHeader::decode(header.encoded());
        AOT_TEST_CHECK(decoded.count == 12 && decoded.kind() == kind && decoded.operand == 345);
    }
}

static const DataRequest& requestFor(LoadedProgram& program, DataNumber number)
{
    std::span<const uint32_t> requests = program.state().tables.requests();
    AOT_TEST_CHECK(requests.size() == 1 + NumberOfData * sizeof(DataRequest) / sizeof(uint32_t));
    return reinterpret_cast<const DataRequest*>(requests.data() + 1)[number];
}

static JSObject* build(LoadedProgram& program, DataNumber number)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSObject* result = tryBuildData(program.realm(), program.state(), requestFor(program, number));
    AOT_TEST_CHECK(!scope.exception() && result);
    return result;
}

static JSValue callInPlainRealm(LoadedProgram& program, ASCIILiteral functionSource, std::initializer_list<JSValue> values)
{
    VM& vm = program.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSGlobalObject* realm = program.realms().plain();
    JSValue function = program.realms().evaluate(realm, functionSource);
    MarkedArgumentBuffer arguments;
    for (JSValue value : values)
        arguments.append(value);
    JSValue result = call(realm, function, arguments, "the source is a function"_s);
    AOT_TEST_CHECK_FOR_SOURCE(!scope.exception(), functionSource);
    return result;
}

static String describe(LoadedProgram& program, JSValue value)
{
    return callInPlainRealm(program, describeSource, { value }).toWTFString(program.realms().plain());
}

static bool isDescribedAs(LoadedProgram& program, JSValue value, ASCIILiteral expected)
{
    String description = describe(program, value);
    if (description == expected)
        return true;
    dataLogLn("built    ", description, "\nexpected ", expected);
    return false;
}

static JSValue get(LoadedProgram& program, JSValue base, ASCIILiteral name)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSValue result = base.get(program.realm(), Identifier::fromString(program.vm(), name));
    AOT_TEST_CHECK(!scope.exception() && result);
    return result;
}

static JSValue getIndex(LoadedProgram& program, JSValue base, unsigned index)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSValue result = base.get(program.realm(), index);
    AOT_TEST_CHECK(!scope.exception() && result);
    return result;
}

static void testRequestsOfTheProgram(LoadedProgram& program)
{
    std::span<const uint64_t> data = program.state().tables.data();
    std::span<const StaticsRange> roots = program.state().tables.staticsRoots();
    AOT_TEST_CHECK(requestFor(program, Tree).sharedStorageOffset == DataRequest::noSharedStorage);
    AOT_TEST_CHECK(DataNodeHeader::decode(data[requestFor(program, Tree).root]).kind() == DataNodeHeader::Kind::Object);
    for (DataNumber number : { Integers, Doubles, Mixture }) {
        const DataRequest& request = requestFor(program, number);
        AOT_TEST_CHECK(request.sharedStorageOffset != DataRequest::noSharedStorage);
        AOT_TEST_CHECK(std::ranges::any_of(roots, [&](const StaticsRange& range) {
            return range.contains(request.sharedStorageOffset);
        }));
        DataNodeHeader header = DataNodeHeader::decode(data[request.root]);
        AOT_TEST_CHECK(header.kind() == DataNodeHeader::Kind::Array && header.count == 17);
    }
    AOT_TEST_CHECK(DataNodeHeader::decode(data[requestFor(program, Integers).root]).operand == ArrayWithInt32);
    AOT_TEST_CHECK(DataNodeHeader::decode(data[requestFor(program, Doubles).root]).operand == ArrayWithDouble);
    AOT_TEST_CHECK(DataNodeHeader::decode(data[requestFor(program, Mixture).root]).operand == ArrayWithContiguous);
}

static void testConstantOfDataWord(LoadedProgram& program)
{
    VM& vm = program.vm();
    State& state = program.state();
    AOT_TEST_CHECK(!constantOfDataWord(state, DataWord::hole));
    AOT_TEST_CHECK(constantOfDataWord(state, DataWord::encodeImmediate(jsUndefined())).isUndefined());
    AOT_TEST_CHECK(constantOfDataWord(state, DataWord::encodeImmediate(jsNull())).isNull());
    AOT_TEST_CHECK(constantOfDataWord(state, DataWord::encodeImmediate(jsBoolean(true))).isTrue());
    AOT_TEST_CHECK(constantOfDataWord(state, DataWord::encodeImmediate(jsNumber(-7))) == jsNumber(-7));
    JSValue negativeZero = constantOfDataWord(state, DataWord::encodeImmediate(jsDoubleNumber(-0.0)));
    AOT_TEST_CHECK(negativeZero.isDouble() && !negativeZero.asDouble() && std::signbit(negativeZero.asDouble()));

    uint32_t numberOfConstants = Image::current()->text().numberOfStringConstants();
    AOT_TEST_CHECK(numberOfConstants >= 5);
    for (uint32_t number = 0; number < numberOfConstants; ++number) {
        JSValue constant = constantOfDataWord(state, DataWord::encodeStringConstant(number));
        AOT_TEST_CHECK(constant.isString() && constant == JSValue(state.strings->constant(vm, number)));
        AOT_TEST_CHECK(constantOfDataWord(state, DataWord::encodeStringConstant(number)) == constant);
    }

    AOT_TEST_CHECK(state.tables.bigIntConstants().size() == 1);
    JSValue big = constantOfDataWord(state, DataWord::encodeBigIntConstant(0));
    AOT_TEST_CHECK(big.isBigInt() && big == state.strings->bigIntConstant(program.realm(), 0));
    AOT_TEST_CHECK(isDescribedAs(program, big, "12345678901234567890n"_s));
}

static void testNestedObjectsAndArrays(LoadedProgram& program)
{
    JSObject* tree = build(program, Tree);
    AOT_TEST_CHECK(isDescribedAs(program, tree, describedTree));

    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(tree));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(asObject(get(program, tree, "inner"_s))));
    AOT_TEST_CHECK(tree->realm() == program.realm());

    auto* mixed = uncheckedDowncast<JSArray>(get(program, tree, "mixed"_s));
    auto* doubles = uncheckedDowncast<JSArray>(get(program, tree, "doubles"_s));
    auto* integers = uncheckedDowncast<JSArray>(get(program, tree, "integers"_s));
    AOT_TEST_CHECK(mixed->indexingType() == ArrayWithContiguous && mixed->length() == 7);
    AOT_TEST_CHECK(doubles->indexingType() == ArrayWithDouble && doubles->length() == 4);
    AOT_TEST_CHECK(integers->indexingType() == ArrayWithInt32 && integers->length() == 2);
    AOT_TEST_CHECK(mixed->realm() == program.realm());
    AOT_TEST_CHECK(!mixed->canGetIndexQuickly(1u) && mixed->canGetIndexQuickly(4u) && mixed->getIndexQuickly(4).isUndefined());
    AOT_TEST_CHECK(!doubles->canGetIndexQuickly(1u) && doubles->canGetIndexQuickly(3u));
    for (JSArray* array : { mixed, doubles, integers })
        AOT_TEST_CHECK(!isCopyOnWrite(array->indexingMode()));
}

static void testTwoExecutionsGiveDistinctGraphs(LoadedProgram& program)
{
    JSObject* first = build(program, Tree);
    JSObject* second = build(program, Tree);
    AOT_TEST_CHECK(first != second);
    for (ASCIILiteral name : { "mixed"_s, "doubles"_s, "integers"_s, "inner"_s, "nested"_s }) {
        JSValue inFirst = get(program, first, name);
        JSValue inSecond = get(program, second, name);
        AOT_TEST_CHECK(inFirst.isObject() && inSecond.isObject() && inFirst != inSecond);
        if (auto* array = dynamicDowncast<JSArray>(inFirst))
            AOT_TEST_CHECK(array->butterfly() != uncheckedDowncast<JSArray>(inSecond)->butterfly());
    }
    JSValue deepInFirst = getIndex(program, getIndex(program, get(program, first, "nested"_s), 0), 0);
    JSValue deepInSecond = getIndex(program, getIndex(program, get(program, second, "nested"_s), 0), 0);
    AOT_TEST_CHECK(deepInFirst.isObject() && deepInFirst != deepInSecond);
    AOT_TEST_CHECK(get(program, first, "text"_s) == get(program, second, "text"_s));
    AOT_TEST_CHECK(get(program, first, "big"_s) == get(program, second, "big"_s));

    callInPlainRealm(program, "(function (tree) { tree.mixed[0] = 'changed'; tree.doubles[1] = 4; tree.integers.push(3); tree.inner.zero = 1; tree.nested[0][0].nan = 2; tree.text = 'other'; })"_s, { first });
    AOT_TEST_CHECK(isDescribedAs(program, first, "{mixed:[\"changed\",hole,\"pear\",true,undefined,null,false],doubles:[1.5,4,2,-0],integers:[1,2,3],inner:{zero:1,nan:NaN,nothing:null},big:12345678901234567890n,text:\"other\",nested:[[{zero:7,nan:2,nothing:undefined}],[]]}"_s));
    AOT_TEST_CHECK(isDescribedAs(program, second, describedTree));
    AOT_TEST_CHECK(isDescribedAs(program, build(program, Tree), describedTree));
}

static void checkSharedStorage(LoadedProgram& program, DataNumber number, IndexingType expectedMode, ASCIILiteral described, ASCIILiteral describedAfterStore)
{
    auto& shared = program.state().statics->at<JSCellButterfly*>(requestFor(program, number).sharedStorageOffset);
    AOT_TEST_CHECK(!shared);

    auto* first = uncheckedDowncast<JSArray>(build(program, number));
    JSCellButterfly* storage = shared;
    AOT_TEST_CHECK(storage && storage->indexingMode() == expectedMode && storage->length() == 17);
    auto* second = uncheckedDowncast<JSArray>(build(program, number));
    AOT_TEST_CHECK(shared == storage);

    AOT_TEST_CHECK(first != second);
    for (JSArray* array : { first, second }) {
        AOT_TEST_CHECK(array->indexingMode() == expectedMode && isCopyOnWrite(array->indexingMode()));
        AOT_TEST_CHECK(array->butterfly() == storage->toButterfly());
        AOT_TEST_CHECK(array->realm() == program.realm());
        AOT_TEST_CHECK(isDescribedAs(program, array, described));
    }

    callInPlainRealm(program, "(function (array) { array[1] = 99; })"_s, { first });
    AOT_TEST_CHECK(!isCopyOnWrite(first->indexingMode()) && first->butterfly() != storage->toButterfly());
    AOT_TEST_CHECK(isDescribedAs(program, first, describedAfterStore));
    AOT_TEST_CHECK(second->butterfly() == storage->toButterfly() && isCopyOnWrite(second->indexingMode()));
    AOT_TEST_CHECK(isDescribedAs(program, second, described));

    auto* third = uncheckedDowncast<JSArray>(build(program, number));
    AOT_TEST_CHECK(shared == storage && third->butterfly() == storage->toButterfly());
    AOT_TEST_CHECK(isDescribedAs(program, third, described));
}

static void testCopyOnWriteArrayOfIntegers(LoadedProgram& program)
{
    checkSharedStorage(program, Integers, CopyOnWriteArrayWithInt32, describedIntegers, "[1,99,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17]"_s);
}

static void testCopyOnWriteArrayOfDoubles(LoadedProgram& program)
{
    checkSharedStorage(program, Doubles, CopyOnWriteArrayWithDouble, describedDoubles, "[0.5,99,3,4,5,6,7,8,9,10,11,12,13,14,15,16,-0]"_s);
}

static void testCopyOnWriteArrayOfAnything(LoadedProgram& program)
{
    checkSharedStorage(program, Mixture, CopyOnWriteArrayWithContiguous, describedMixture, "[\"a0\",99,undefined,null,true,\"a5\",7,8,9,10,11,12,13,14,15,16,\"a16\"]"_s);
}

static void testSharedStorageSurvivesCollections(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto& shared = program.state().statics->at<JSCellButterfly*>(requestFor(program, Mixture).sharedStorageOffset);
    build(program, Mixture);
    JSCellButterfly* storage = shared;
    AOT_TEST_CHECK(storage);
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full, CollectionScope::Eden }) {
        vm.heap.collectSync(scope);
        AOT_TEST_CHECK(shared == storage && vm.heap.isMarked(storage));
        AOT_TEST_CHECK(isDescribedAs(program, build(program, Mixture), describedMixture));
    }
}

static void checkNothingIsLost(LoadedProgram& program)
{
    AOT_TEST_CHECK(Options::collectContinuously());
    for (unsigned round = 0; round < 3000; ++round) {
        JSObject* tree = build(program, Tree);
        JSObject* mixture = build(program, Mixture);
        if (round % 100)
            continue;
        AOT_TEST_CHECK(isDescribedAs(program, tree, describedTree));
        AOT_TEST_CHECK(isDescribedAs(program, mixture, describedMixture));
        AOT_TEST_CHECK(isDescribedAs(program, build(program, Doubles), describedDoubles));
    }
}

static void testCollectionInTheMiddleLosesNothing()
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(collectionInTheMiddleTest, programText, checkNothingIsLost);
        return;
    }

    TemporaryFile image(".image"_s, compileProgramText(programText).span());
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments {
        String(nativeSubcommandName), String(singleProcessFlag), String(collectionInTheMiddleTest),
        makeString("--aotImagePath="_s, image.path()), "--collectContinuously=1"_s,
    };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process that collects continuously ", result.description());
        failNativeTest(__FILE__, __LINE__, collectionInTheMiddleTest.characters());
    }
}

template<LoadedProgramTest body>
static constexpr void (*withData)() = runNativeTestWithLoadedProgram<dataBuilderTests, programText, body>;

std::span<const NativeTest> dataBuilderTests()
{
    static constexpr NativeTest tests[] = {
        { "data-builder/data-words-round-trip"_s, testDataWordsRoundTrip },
        { "data-builder/number-of-words"_s, testNumberOfWords },
        { "data-builder/requests-of-the-program"_s, withData<testRequestsOfTheProgram> },
        { "data-builder/constant-of-data-word"_s, withData<testConstantOfDataWord> },
        { "data-builder/nested-objects-and-arrays"_s, withData<testNestedObjectsAndArrays> },
        { "data-builder/two-executions-give-distinct-graphs"_s, withData<testTwoExecutionsGiveDistinctGraphs> },
        { "data-builder/copy-on-write-array-of-integers"_s, withData<testCopyOnWriteArrayOfIntegers> },
        { "data-builder/copy-on-write-array-of-doubles"_s, withData<testCopyOnWriteArrayOfDoubles> },
        { "data-builder/copy-on-write-array-of-anything"_s, withData<testCopyOnWriteArrayOfAnything> },
        { "data-builder/shared-storage-survives-collections"_s, withData<testSharedStorageSurvivesCollections> },
        { collectionInTheMiddleTest, testCollectionInTheMiddleLosesNothing },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
