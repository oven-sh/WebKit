/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTGlobalBindings.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> globalBindingProgramTests();

static constexpr ASCIILiteral accessesOfOneName = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = NewClosure function=f5
    v7 = Call v1, -, v2, v3, v4, v5, v6
    Return
}
function f1 name="read" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="shared" [isFreeName]
    Return v1
}
function f2 name="store" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    PropSet v1, v0 key="shared" mode=Strict [isFreeName]
    Return
}
function f3 name="storeSloppy" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    PropSet v1, v0 key="shared" mode=Sloppy [isFreeName]
    Return
}
function f4 name="readOfGlobalObject" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="shared"
    Return v1
}
function f5 name="storeToGlobalObject" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    PropSet v1, v0 key="shared" mode=Strict
    Return
}
)IR"_s;

static constexpr ASCIILiteral questionsAboutOneName = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = Call v1, -, v2, v3, v4, v5
    Return
}
function f1 name="typeOfName" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="shared" [isFreeName, isForTypeOf]
    v2 = TypeOf v1
    Return v2
}
function f2 name="read" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="shared" [isFreeName]
    Return v1
}
function f3 name="readOfGlobalObject" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="shared"
    Return v1
}
function f4 name="storeSloppy" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    PropSet v1, v0 key="shared" mode=Sloppy [isFreeName]
    Return
}
)IR"_s;

static constexpr ASCIILiteral accessesOfLazyGlobal = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = Call v1, -, v2, v3
    Return
}
function f1 name="readLazy" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="Intl" [isFreeName]
    Return v1
}
function f2 name="storeLazy" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    PropSet v1, v0 key="Intl" mode=Sloppy [isFreeName]
    Return
}
)IR"_s;

static constexpr unsigned numberOfNamesOfLargeProgram = 1500;

static ASCIICString accessesOfManyNames()
{
    StringBuilder text;
    text.append(R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = Call v1, -, v2, v3
    Return
}
function f1 name="readAll" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
)IR"_s);
    for (unsigned index = 0; index < numberOfNamesOfLargeProgram; ++index) {
        unsigned read = 2 * index + 2;
        text.append("    v"_s, read, " = PropGet v1 key=\"name"_s, index, "\" [isFreeName]\n"_s);
        text.append("    v"_s, read + 1, " = Call v0, -, v"_s, read, '\n');
    }
    text.append(R"IR(    Return
}
function f2 name="storeAll" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
)IR"_s);
    for (unsigned index = 0; index < numberOfNamesOfLargeProgram; ++index)
        text.append("    PropSet v1, v0 key=\"name"_s, index, "\" mode=Sloppy [isFreeName]\n"_s);
    text.append("    Return\n}\n"_s);
    return ASCIICString(byteCast<char>(text.toString().utf8().span()));
}

#define HELPERS COUNTED_CALLS(R"("GetGlobalBinding", "GetGlobalObjectBinding", "SetGlobalBinding", "SetGlobalBindingSloppy", "SetGlobalObjectBinding", "SetGlobalObjectBindingSloppy", "GetFreeName", "GetFreeNameForTypeOf", "SetFreeName", "SetFreeNameSloppy")") R"JS(
const repeat = (what, access) => {
    let result;
    for (let i = 0; i < 100; i++) result = access();
    print(what + ": " + result);
};
const measure = (what, access) => {
    const before = countedCalls();
    let result;
    for (let i = 0; i < 100; i++) result = access();
    print(what + ": " + result + servedBy(before, 100, 2));
};
const describeError = (what, action) => {
    try {
        action();
        print(what + ": no error");
    } catch (error) {
        print(what + ": " + error.constructor.name);
    }
};
)JS"

#define CHECK_IN_EVERY_BUILD(programText, STATEMENTS, OUTPUT, LETTERS) do { \
        checkOutputOfProgramInShell(programText, OUTPUT ""_s, { }, HELPERS STATEMENTS ""_s); \
        checkServicesInShell(__FILE__, __LINE__, programText, OUTPUT ""_s, HELPERS STATEMENTS ""_s, wordsOfCaches, LETTERS); \
    } while (false)

#define STATEMENTS_THAT_STORE_AFTER_A_CONSTANT R"JS(
globalThis.publish = (read, store, storeSloppy, readOfGlobalObject, storeToGlobalObject) => {
    globalThis.shared = 7;
    measure("first value", read);
    measure("first value, of the global object", readOfGlobalObject);
    globalThis.shared = 8;
    measure("stored by other code", read);
    measure("stored by other code, of the global object", readOfGlobalObject);
    store(9);
    measure("stored through the binding", read);
    measure("stores through the binding", () => { store(10); return read(); });
    measure("sloppy stores", () => { storeSloppy(11); return read(); });
    measure("stores to the global object", () => { storeToGlobalObject(12); return readOfGlobalObject(); });
    measure("seen as a free name", read);
};
)JS"

#define OUTPUT_OF_STORES_AFTER_A_CONSTANT \
    "first value: 7\n" \
    "first value, of the global object: 7\n" \
    "stored by other code: 8\n" \
    "stored by other code, of the global object: 8\n" \
    "stored through the binding: 9\n" \
    "stores through the binding: 10\n" \
    "sloppy stores: 11\n" \
    "stores to the global object: 12\n" \
    "seen as a free name: 12\n"

static void testStoreAfterConstantIsSeen()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_THAT_STORE_AFTER_A_CONSTANT, OUTPUT_OF_STORES_AFTER_A_CONSTANT, "ccccccccc"_s);
}

#define STATEMENTS_THAT_DELETE R"JS(
globalThis.publish = (read, store, storeSloppy, readOfGlobalObject) => {
    globalThis.shared = 7;
    measure("before", read);
    measure("before, of the global object", readOfGlobalObject);
    delete globalThis.shared;
    describeError("read after the delete", read);
    print("of the global object: " + readOfGlobalObject());
    describeError("strict store after the delete", () => store(1));
    storeSloppy(8);
    repeat("created by a sloppy store", read);
    delete globalThis.shared;
    describeError("read after the second delete", read);
    print("of the global object: " + readOfGlobalObject());
};
)JS"

#define OUTPUT_OF_DELETES \
    "before: 7\n" \
    "before, of the global object: 7\n" \
    "read after the delete: ReferenceError\n" \
    "of the global object: undefined\n" \
    "strict store after the delete: ReferenceError\n" \
    "created by a sloppy store: 8\n" \
    "read after the second delete: ReferenceError\n" \
    "of the global object: undefined\n"

static void testDeleteThenReadThrows()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_THAT_DELETE, OUTPUT_OF_DELETES, "cc......"_s);
}

#define STATEMENTS_THAT_DEFINE_ACCESSORS R"JS(
globalThis.publish = (read, store, storeSloppy, readOfGlobalObject) => {
    globalThis.shared = 7;
    measure("value", read);
    let reads = 0;
    Object.defineProperty(globalThis, "shared", { get() { return ++reads; }, configurable: true });
    measure("getter", read);
    measure("getter, of the global object", readOfGlobalObject);
    let stored = 0;
    Object.defineProperty(globalThis, "shared", { get() { return stored; }, set(value) { stored = value * 2; }, configurable: true });
    measure("setter", () => { store(4); return read(); });
    Object.defineProperty(globalThis, "shared", { value: 5, writable: true, configurable: true });
    repeat("value again", read);
    Object.defineProperty(globalThis, "shared", { get() { Object.defineProperty(globalThis, "shared", { value: 10, writable: true, configurable: true }); return -10; }, configurable: true });
    print("getter that replaces itself: " + read());
    repeat("value of that getter", read);
};
)JS"

#define OUTPUT_OF_ACCESSORS \
    "value: 7\n" \
    "getter: 100\n" \
    "getter, of the global object: 200\n" \
    "setter: 8\n" \
    "value again: 5\n" \
    "getter that replaces itself: -10\n" \
    "value of that getter: 10\n"

static void testRedefinedAsGetterRuns()
{
    String output = outputOfProgramInShell(accessesOfOneName, { }, HELPERS STATEMENTS_THAT_DEFINE_ACCESSORS ""_s, { });
    AOT_TEST_CHECK(output == OUTPUT_OF_ACCESSORS ""_s);
    checkServicesInShell(__FILE__, __LINE__, accessesOfOneName, OUTPUT_OF_ACCESSORS ""_s, HELPERS STATEMENTS_THAT_DEFINE_ACCESSORS ""_s, wordsOfCaches, "cnnn..."_s);
}

#define STATEMENTS_WITH_A_LET_OF_A_SCRIPT R"JS(
globalThis.publish = (read, store, storeSloppy, readOfGlobalObject, storeToGlobalObject) => {
    globalThis.shared = 7;
    measure("property", read);
    measure("property, of the global object", readOfGlobalObject);
    loadString("let shared = 11;");
    measure("let of a script", read);
    measure("of the global object", readOfGlobalObject);
    measure("stores to the let", () => { store(12); return read(); });
    print("the property keeps its value: " + globalThis.shared);
    storeToGlobalObject(13);
    measure("the let after a store to the global object", read);
    measure("the property after it", readOfGlobalObject);
};
)JS"

#define OUTPUT_WITH_A_LET_OF_A_SCRIPT \
    "property: 7\n" \
    "property, of the global object: 7\n" \
    "let of a script: 11\n" \
    "of the global object: 7\n" \
    "stores to the let: 12\n" \
    "the property keeps its value: 7\n" \
    "the let after a store to the global object: 12\n" \
    "the property after it: 13\n"

static void testScriptLetShadowsBoundGlobal()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_WITH_A_LET_OF_A_SCRIPT, OUTPUT_WITH_A_LET_OF_A_SCRIPT, "ccccn.cc"_s);
}

#define STATEMENTS_IN_THE_DEAD_ZONE R"JS(
globalThis.publish = (read, store) => {
    globalThis.shared = 7;
    measure("property", read);
    globalThis.readShared = read;
    globalThis.storeShared = store;
    loadString(`
        describeError("read in the dead zone", readShared);
        describeError("store in the dead zone", () => storeShared(1));
        let shared = 11;
    `);
    measure("after the dead zone", read);
};
)JS"

#define OUTPUT_IN_THE_DEAD_ZONE \
    "property: 7\n" \
    "read in the dead zone: ReferenceError\n" \
    "store in the dead zone: ReferenceError\n" \
    "after the dead zone: 11\n"

static void testDeadZoneOfScriptLetThrows()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_IN_THE_DEAD_ZONE, OUTPUT_IN_THE_DEAD_ZONE, "c..c"_s);
}

#define STATEMENTS_WITH_A_CONST_OF_A_SCRIPT R"JS(
globalThis.publish = (read, store, storeSloppy, readOfGlobalObject, storeToGlobalObject) => {
    loadString("const shared = 21;");
    measure("const of a script", read);
    describeError("strict store to the const", () => store(1));
    describeError("sloppy store to the const", () => storeSloppy(1));
    measure("after the refused stores", read);
    print("of the global object: " + readOfGlobalObject());
    storeToGlobalObject(22);
    measure("the const over a property of its name", read);
    measure("the property", readOfGlobalObject);
};
)JS"

#define OUTPUT_WITH_A_CONST_OF_A_SCRIPT \
    "const of a script: 21\n" \
    "strict store to the const: TypeError\n" \
    "sloppy store to the const: TypeError\n" \
    "after the refused stores: 21\n" \
    "of the global object: undefined\n" \
    "the const over a property of its name: 21\n" \
    "the property: 22\n"

static void testConstOfScriptIsConstant()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_WITH_A_CONST_OF_A_SCRIPT, OUTPUT_WITH_A_CONST_OF_A_SCRIPT, "c..c.cc"_s);
}

#define STATEMENTS_ABOUT_AN_ABSENT_NAME R"JS(
globalThis.publish = (typeOfName, read, readOfGlobalObject, storeSloppy) => {
    measure("absent", typeOfName);
    measure("absent, of the global object", readOfGlobalObject);
    describeError("read of the absent name", read);
    globalThis.shared = 7;
    measure("defined by other code", typeOfName);
    measure("defined by other code, of the global object", readOfGlobalObject);
    delete globalThis.shared;
    measure("deleted", typeOfName);
    measure("deleted, of the global object", readOfGlobalObject);
    describeError("read of the deleted name", read);
    storeSloppy("text");
    measure("created by a sloppy store", typeOfName);
    delete globalThis.shared;
    measure("deleted again", typeOfName);
    Object.defineProperty(globalThis, "shared", { get() { return 1n; }, configurable: true });
    repeat("defined as a getter", typeOfName);
    delete globalThis.shared;
    measure("absent at the end", typeOfName);
};
)JS"

#define OUTPUT_ABOUT_AN_ABSENT_NAME \
    "absent: undefined\n" \
    "absent, of the global object: undefined\n" \
    "read of the absent name: ReferenceError\n" \
    "defined by other code: number\n" \
    "defined by other code, of the global object: 7\n" \
    "deleted: undefined\n" \
    "deleted, of the global object: undefined\n" \
    "read of the deleted name: ReferenceError\n" \
    "created by a sloppy store: string\n" \
    "deleted again: undefined\n" \
    "defined as a getter: bigint\n" \
    "absent at the end: undefined\n"

static void testTypeOfAbsentNameFollowsTheGlobalObject()
{
    checkOutputOfProgramInShell(questionsAboutOneName, OUTPUT_ABOUT_AN_ABSENT_NAME ""_s, { "admit=Inspector"_s }, HELPERS STATEMENTS_ABOUT_AN_ABSENT_NAME ""_s);
    checkServicesInShell(__FILE__, __LINE__, questionsAboutOneName, OUTPUT_ABOUT_AN_ABSENT_NAME ""_s, HELPERS STATEMENTS_ABOUT_AN_ABSENT_NAME ""_s, wordsOfCaches, "cc.cccc.cc.c"_s, { "admit=Inspector"_s });
}

#define STATEMENTS_ABOUT_AN_ABSENT_NAME_AND_A_SCRIPT R"JS(
globalThis.publish = (typeOfName, read, readOfGlobalObject) => {
    measure("absent", typeOfName);
    globalThis.typeOfShared = typeOfName;
    loadString(`
        describeError("typeof in the dead zone", typeOfShared);
        let shared = true;
    `);
    measure("let of a script", typeOfName);
    measure("the global object has no such property", readOfGlobalObject);
};
)JS"

#define OUTPUT_ABOUT_AN_ABSENT_NAME_AND_A_SCRIPT \
    "absent: undefined\n" \
    "typeof in the dead zone: ReferenceError\n" \
    "let of a script: boolean\n" \
    "the global object has no such property: undefined\n"

static void testTypeOfAbsentNameSeesLetOfScript()
{
    CHECK_IN_EVERY_BUILD(questionsAboutOneName, STATEMENTS_ABOUT_AN_ABSENT_NAME_AND_A_SCRIPT, OUTPUT_ABOUT_AN_ABSENT_NAME_AND_A_SCRIPT, "c.cc"_s);
}

#define STATEMENTS_THAT_GROW_THE_STORAGE R"JS(
globalThis.publish = (read, store) => {
    globalThis.shared = 1;
    read();
    globalThis.shared = 2;
    measure("before the growth", read);
    store(3);
    for (let i = 0; i < 300; i++) globalThis["filler" + i] = i;
    repeat("after the growth", read);
    repeat("stores after the growth", () => { store(4); return read(); });
    print("read by other code: " + globalThis.shared);
    print("a neighbor keeps its value: " + globalThis.filler299);
};
)JS"

#define OUTPUT_WHILE_THE_STORAGE_GROWS \
    "before the growth: 2\n" \
    "after the growth: 3\n" \
    "stores after the growth: 4\n" \
    "read by other code: 4\n" \
    "a neighbor keeps its value: 299\n"

static void testStorageGrowthKeepsLocation()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_THAT_GROW_THE_STORAGE, OUTPUT_WHILE_THE_STORAGE_GROWS, "c...."_s);
}

#define STATEMENTS_WITH_A_LAZY_GLOBAL R"JS(
globalThis.publish = readLazy => {
    const first = readLazy();
    measure("created once", () => readLazy() === first);
    print("the same for other code: " + (globalThis.Intl === first));
    print("what it holds: " + typeof first.DateTimeFormat);
};
)JS"

#define OUTPUT_WITH_A_LAZY_GLOBAL \
    "created once: true\n" \
    "the same for other code: true\n" \
    "what it holds: function\n"

static void testLazyGlobalIsCreatedOnce()
{
    CHECK_IN_EVERY_BUILD(accessesOfLazyGlobal, STATEMENTS_WITH_A_LAZY_GLOBAL, OUTPUT_WITH_A_LAZY_GLOBAL, "c.."_s);
}

#define STATEMENTS_THAT_READ_WHAT_A_BINDING_STORED R"JS(
globalThis.publish = (read, store, storeSloppy, readOfGlobalObject, storeToGlobalObject) => {
    describeError("strict store to an absent name", () => store(1));
    storeSloppy(2);
    print("a sloppy store creates it: " + globalThis.shared);
    store(3);
    store(4);
    print("read by other code: " + globalThis.shared);
    print("its descriptor: " + JSON.stringify(Object.getOwnPropertyDescriptor(globalThis, "shared")));
    storeToGlobalObject(5);
    storeToGlobalObject(6);
    print("after stores to the global object: " + globalThis.shared);
    store({ kept: 0 });
    fullGC();
    store({ kept: 7 });
    edenGC();
    for (let i = 0; i < 10000; i++) ({ kept: i });
    print("kept across a collection: " + globalThis.shared.kept);
    Object.defineProperty(globalThis, "shared", { value: 30, writable: false, configurable: true });
    describeError("strict store to a read-only property", () => store(31));
    describeError("sloppy store to it", () => storeSloppy(32));
    describeError("strict store to it on the global object", () => storeToGlobalObject(33));
    print("it keeps its value: " + read());
};
)JS"

#define OUTPUT_OF_READS_OF_WHAT_A_BINDING_STORED \
    "strict store to an absent name: ReferenceError\n" \
    "a sloppy store creates it: 2\n" \
    "read by other code: 4\n" \
    "its descriptor: {\"value\":4,\"writable\":true,\"enumerable\":true,\"configurable\":true}\n" \
    "after stores to the global object: 6\n" \
    "kept across a collection: 7\n" \
    "strict store to a read-only property: TypeError\n" \
    "sloppy store to it: no error\n" \
    "strict store to it on the global object: TypeError\n" \
    "it keeps its value: 30\n"

static void testStoreThroughBindingThenNativeRead()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_THAT_READ_WHAT_A_BINDING_STORED, OUTPUT_OF_READS_OF_WHAT_A_BINDING_STORED, ".........."_s);
}

static_assert(numberOfNamesOfLargeProgram == 1500, "The statements and their output spell the number.");

#define STATEMENTS_THAT_USE_MANY_NAMES R"JS(
globalThis.publish = (readAll, storeAll) => {
    for (let i = 0; i < 1500; i++) globalThis["name" + i] = i;
    let sum = 0;
    const add = value => { sum += value; };
    for (let turn = 0; turn < 3; turn++) readAll(add);
    print("each name has its own value: " + sum);
    storeAll(2);
    storeAll(3);
    sum = 0;
    readAll(add);
    print("stored through the bindings: " + sum);
    print("read by other code: " + globalThis.name0 + " " + globalThis.name1499);
    globalThis.name1499 = 10;
    sum = 0;
    readAll(add);
    print("the last name stored by other code: " + sum);
};
)JS"

#define OUTPUT_OF_MANY_NAMES \
    "each name has its own value: 3372750\n" \
    "stored through the bindings: 4500\n" \
    "read by other code: 3 3\n" \
    "the last name stored by other code: 4507\n"

static void testManyNamesHaveTheirOwnRecords()
{
    ASCIICString programText = accessesOfManyNames();
    CHECK_IN_EVERY_BUILD(ASCIILiteral::fromLiteralUnsafe(programText.data()), STATEMENTS_THAT_USE_MANY_NAMES, OUTPUT_OF_MANY_NAMES, "...."_s);
}

static constexpr ASCIILiteral roomForRecordsTest = "global-bindings/instance-has-room-for-every-record"_s;

static void checkRoomForRecords(LoadedProgram& program)
{
    const ProgramTables& tables = program.state().tables;
    InstanceSizes sizes = tables.instanceSizes;
    AOT_TEST_CHECK(sizes.numberOfGlobalBindings >= 2 * numberOfNamesOfLargeProgram);
    AOT_TEST_CHECK(sizes.numberOfGlobalBindings == tables.globalBindings().size());
    AOT_TEST_CHECK(InstanceLayout::offsetOfStringCell(sizes.numberOfStringCells) <= InstanceLayout::offsetOfGlobalBindings(sizes));
    AOT_TEST_CHECK(InstanceLayout::endOfGlobalBindings(sizes) <= InstanceLayout::bytesReservedFromInstance(sizes));

    GlobalBindings& bindings = *program.state().globalBindings;
    AOT_TEST_CHECK(&bindings.at(0) == program.instance().tableAt<GlobalBinding>(InstanceLayout::offsetOfGlobalBindings(sizes)));
    for (uint32_t number = 0; number < sizes.numberOfGlobalBindings; ++number) {
        const GlobalBinding& record = bindings.at(number);
        AOT_TEST_CHECK(!record.constant && !record.structureID && !record.location);
    }

    uint32_t last = sizes.numberOfGlobalBindings - 1;
    AOT_TEST_CHECK(bindings.fileConstant(program.vm(), last, nullptr, jsNumber(1)));
    AOT_TEST_CHECK(bindings.at(last).constant == JSValue::encode(jsNumber(1)) && !bindings.at(last - 1).constant);
}

static void testInstanceHasRoomForEveryRecord()
{
    ASCIICString programText = accessesOfManyNames();
    runWithLoadedProgram(roomForRecordsTest, ASCIILiteral::fromLiteralUnsafe(programText.data()), checkRoomForRecords);
}

std::span<const NativeTest> globalBindingProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "global-bindings/programs/store-after-constant-is-seen"_s, testStoreAfterConstantIsSeen },
        { "global-bindings/programs/delete-then-read-throws"_s, testDeleteThenReadThrows },
        { "global-bindings/programs/redefined-as-getter-runs"_s, testRedefinedAsGetterRuns },
        { "global-bindings/programs/script-let-shadows-bound-global"_s, testScriptLetShadowsBoundGlobal },
        { "global-bindings/programs/dead-zone-of-script-let-throws"_s, testDeadZoneOfScriptLetThrows },
        { "global-bindings/programs/const-of-script-is-constant"_s, testConstOfScriptIsConstant },
        { "global-bindings/programs/typeof-absent-name-follows-the-global-object"_s, testTypeOfAbsentNameFollowsTheGlobalObject },
        { "global-bindings/programs/typeof-absent-name-sees-let-of-script"_s, testTypeOfAbsentNameSeesLetOfScript },
        { "global-bindings/programs/storage-growth-keeps-location"_s, testStorageGrowthKeepsLocation },
        { "global-bindings/programs/lazy-global-is-created-once"_s, testLazyGlobalIsCreatedOnce },
        { "global-bindings/programs/store-through-binding-then-native-read"_s, testStoreThroughBindingThenNativeRead },
        { "global-bindings/programs/many-names-have-their-own-records"_s, testManyNamesHaveTheirOwnRecords },
        { roomForRecordsTest, testInstanceHasRoomForEveryRecord },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
