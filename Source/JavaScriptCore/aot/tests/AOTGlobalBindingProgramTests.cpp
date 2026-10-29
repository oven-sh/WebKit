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
#include "AOTPlacement.h"
#include "AOTRuntimeValidation.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> globalBindingProgramTests();

static_assert(GlobalBindings::seesScriptBindingsFlag == Typed::GlobalBindingTable::seesScriptBindingsFlag);
static_assert(GlobalBindings::isStoredByNameFlag == Typed::GlobalBindingTable::isStoredByNameFlag);
static_assert(GlobalBindings::isForTypeOfFlag == Typed::GlobalBindingTable::isForTypeOfFlag);
static_assert(GlobalBindings::identifierMask == Typed::GlobalBindingTable::identifierMask);

// The program stores to the name, so no proof says what a read of it finds.
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

// The program stores to the name, so no proof says whether the name is there.
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

// The read sees script bindings and the store does not, so each of the two records of the name is found by that.
static constexpr ASCIILiteral programForManyInstances = R"IR(variable g0 name="worker" declared={type=anything}
module m0 path="main.js" displayPath="main.js" body=f0 exports=[{name="worker" variable=g0}]
entry hostReads=[{exportName="worker"}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=Worker
    PutModuleVariable v0 variable=g0
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="shared" [isFreeName]
    PropSet v2, v3 key="shared" mode=Sloppy
    Return
}
)IR"_s;

// A hundred accesses in a row call the run time at most twice where a record answers: a miss files, or flattens the Structure so that the next one files.
#define HELPERS R"JS(
const operations = ["GetGlobalBinding", "GetGlobalObjectBinding", "SetGlobalBinding", "SetGlobalBindingSloppy", "SetGlobalObjectBinding", "SetGlobalObjectBindingSloppy"];
const calls = () => {
    let sum = 0;
    for (const name of operations) sum += Number(aotOperationCount("operationAOT" + name));
    return sum;
};
const repeat = (what, access) => {
    let result;
    for (let i = 0; i < 100; i++) result = access();
    print(what + ": " + result);
};
const measure = (what, access) => {
    const callsBefore = calls();
    let result;
    for (let i = 0; i < 100; i++) result = access();
    const callsMade = calls() - callsBefore;
    const served = callsMade <= 2 ? "inline" : callsMade >= 100 ? "by operation" : callsMade + " calls";
    print(what + ": " + result + (globalThis.printsCalls ? ", " + served : ""));
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

#define LINE(what, served) what served "\n"

static void checkInEveryBuild(ASCIILiteral programText, ASCIILiteral statements, ASCIILiteral statementsThatPrintCalls, ASCIILiteral output, ASCIILiteral outputWithCalls)
{
    checkOutputOfProgramInShell(programText, outputWithCalls, { "heat=EverythingIsHot"_s }, statementsThatPrintCalls, { "--aotCountsPath=/dev/null"_s });
    checkOutputOfProgramInShell(programText, output, { }, statements, { "--aotCountsPath=/dev/null"_s });
    for (ASCIILiteral word : { "validate"_s, "heat=EverythingIsCold"_s })
        checkOutputOfProgramInShell(programText, output, { word }, statements, { "--aotCountsPath=/dev/null"_s });
}

#define CHECK_IN_EVERY_BUILD(programText, STATEMENTS, OUTPUT) \
    checkInEveryBuild(programText, HELPERS STATEMENTS ""_s, "globalThis.printsCalls = true;" HELPERS STATEMENTS ""_s, OUTPUT("", "") ""_s, OUTPUT(", inline", ", by operation") ""_s)

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
    print("the run time was asked: " + (calls() > 0));
};
)JS"

#define OUTPUT_OF_STORES_AFTER_A_CONSTANT(inlined, byOperation) \
    LINE("first value: 7", inlined) \
    LINE("first value, of the global object: 7", inlined) \
    LINE("stored by other code: 8", inlined) \
    LINE("stored by other code, of the global object: 8", inlined) \
    LINE("stored through the binding: 9", inlined) \
    LINE("stores through the binding: 10", inlined) \
    LINE("sloppy stores: 11", inlined) \
    LINE("stores to the global object: 12", inlined) \
    LINE("seen as a free name: 12", inlined) \
    "the run time was asked: true\n"

static void testStoreAfterConstantIsSeen()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_THAT_STORE_AFTER_A_CONSTANT, OUTPUT_OF_STORES_AFTER_A_CONSTANT);
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

#define OUTPUT_OF_DELETES(inlined, byOperation) \
    LINE("before: 7", inlined) \
    LINE("before, of the global object: 7", inlined) \
    "read after the delete: ReferenceError\n" \
    "of the global object: undefined\n" \
    "strict store after the delete: ReferenceError\n" \
    "created by a sloppy store: 8\n" \
    "read after the second delete: ReferenceError\n" \
    "of the global object: undefined\n"

static void testDeleteThenReadThrows()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_THAT_DELETE, OUTPUT_OF_DELETES);
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

#define OUTPUT_OF_ACCESSORS(inlined, byOperation) \
    LINE("value: 7", inlined) \
    LINE("getter: 100", byOperation) \
    LINE("getter, of the global object: 200", byOperation) \
    LINE("setter: 8", byOperation) \
    "value again: 5\n" \
    "getter that replaces itself: -10\n" \
    "value of that getter: 10\n"

static void testRedefinedAsGetterRuns()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_THAT_DEFINE_ACCESSORS, OUTPUT_OF_ACCESSORS);
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

#define OUTPUT_WITH_A_LET_OF_A_SCRIPT(inlined, byOperation) \
    LINE("property: 7", inlined) \
    LINE("property, of the global object: 7", inlined) \
    LINE("let of a script: 11", inlined) \
    LINE("of the global object: 7", inlined) \
    LINE("stores to the let: 12", byOperation) \
    "the property keeps its value: 7\n" \
    LINE("the let after a store to the global object: 12", inlined) \
    LINE("the property after it: 13", inlined)

static void testScriptLetShadowsBoundGlobal()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_WITH_A_LET_OF_A_SCRIPT, OUTPUT_WITH_A_LET_OF_A_SCRIPT);
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

#define OUTPUT_IN_THE_DEAD_ZONE(inlined, byOperation) \
    LINE("property: 7", inlined) \
    "read in the dead zone: ReferenceError\n" \
    "store in the dead zone: ReferenceError\n" \
    LINE("after the dead zone: 11", inlined)

static void testDeadZoneOfScriptLetThrows()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_IN_THE_DEAD_ZONE, OUTPUT_IN_THE_DEAD_ZONE);
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

#define OUTPUT_WITH_A_CONST_OF_A_SCRIPT(inlined, byOperation) \
    LINE("const of a script: 21", inlined) \
    "strict store to the const: TypeError\n" \
    "sloppy store to the const: TypeError\n" \
    LINE("after the refused stores: 21", inlined) \
    "of the global object: undefined\n" \
    LINE("the const over a property of its name: 21", inlined) \
    LINE("the property: 22", inlined)

static void testConstOfScriptIsConstant()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_WITH_A_CONST_OF_A_SCRIPT, OUTPUT_WITH_A_CONST_OF_A_SCRIPT);
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
    print("the run time was asked: " + (calls() > 0));
};
)JS"

#define OUTPUT_ABOUT_AN_ABSENT_NAME(inlined, byOperation) \
    LINE("absent: undefined", inlined) \
    LINE("absent, of the global object: undefined", inlined) \
    "read of the absent name: ReferenceError\n" \
    LINE("defined by other code: number", inlined) \
    LINE("defined by other code, of the global object: 7", inlined) \
    LINE("deleted: undefined", inlined) \
    LINE("deleted, of the global object: undefined", inlined) \
    "read of the deleted name: ReferenceError\n" \
    LINE("created by a sloppy store: string", inlined) \
    LINE("deleted again: undefined", inlined) \
    "defined as a getter: bigint\n" \
    LINE("absent at the end: undefined", inlined) \
    "the run time was asked: true\n"

static void testTypeOfAbsentNameFollowsTheGlobalObject()
{
    CHECK_IN_EVERY_BUILD(questionsAboutOneName, STATEMENTS_ABOUT_AN_ABSENT_NAME, OUTPUT_ABOUT_AN_ABSENT_NAME);
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

#define OUTPUT_ABOUT_AN_ABSENT_NAME_AND_A_SCRIPT(inlined, byOperation) \
    LINE("absent: undefined", inlined) \
    "typeof in the dead zone: ReferenceError\n" \
    LINE("let of a script: boolean", inlined) \
    LINE("the global object has no such property: undefined", inlined)

static void testTypeOfAbsentNameSeesLetOfScript()
{
    CHECK_IN_EVERY_BUILD(questionsAboutOneName, STATEMENTS_ABOUT_AN_ABSENT_NAME_AND_A_SCRIPT, OUTPUT_ABOUT_AN_ABSENT_NAME_AND_A_SCRIPT);
}

// The first read files a constant, and the store by other code ends it. What is filed then is where the property lies.
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

#define OUTPUT_WHILE_THE_STORAGE_GROWS(inlined, byOperation) \
    LINE("before the growth: 2", inlined) \
    "after the growth: 3\n" \
    "stores after the growth: 4\n" \
    "read by other code: 4\n" \
    "a neighbor keeps its value: 299\n"

static void testStorageGrowthKeepsLocation()
{
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_THAT_GROW_THE_STORAGE, OUTPUT_WHILE_THE_STORAGE_GROWS);
}

#define STATEMENTS_WITH_A_LAZY_GLOBAL R"JS(
globalThis.publish = readLazy => {
    const first = readLazy();
    measure("created once", () => readLazy() === first);
    print("the same for other code: " + (globalThis.Intl === first));
    print("what it holds: " + typeof first.DateTimeFormat);
};
)JS"

#define OUTPUT_WITH_A_LAZY_GLOBAL(inlined, byOperation) \
    LINE("created once: true", inlined) \
    "the same for other code: true\n" \
    "what it holds: function\n"

static void testLazyGlobalIsCreatedOnce()
{
    CHECK_IN_EVERY_BUILD(accessesOfLazyGlobal, STATEMENTS_WITH_A_LAZY_GLOBAL, OUTPUT_WITH_A_LAZY_GLOBAL);
}

// The second of two stores in a row is the inline one.
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

#define OUTPUT_OF_READS_OF_WHAT_A_BINDING_STORED(inlined, byOperation) \
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
    CHECK_IN_EVERY_BUILD(accessesOfOneName, STATEMENTS_THAT_READ_WHAT_A_BINDING_STORED, OUTPUT_OF_READS_OF_WHAT_A_BINDING_STORED);
}

static uint32_t recordOf(LoadedProgram& program, ASCIILiteral name, bool seesScriptBindings)
{
    GlobalBindings& bindings = *program.state().globalBindings;
    Identifier wanted = Identifier::fromString(program.vm(), name);
    std::optional<uint32_t> found;
    for (uint32_t number = 0; number < program.state().tables.globalBindings().size(); ++number) {
        if (bindings.nameOf(number) != wanted || bindings.seesScriptBindings(number) != seesScriptBindings)
            continue;
        AOT_TEST_CHECK(!found);
        found = number;
    }
    AOT_TEST_CHECK(found);
    return *found;
}

static void defineShared(LoadedProgram& program, JSValue value)
{
    program.realm()->putDirect(program.vm(), Identifier::fromString(program.vm(), "shared"_s), value);
}

static void testWorkerHasItsOwnRecords(LoadedProgram& program)
{
    VM& vm = program.vm();
    // No built-in of Bun's starts a second instance in the VM of the program, so the compiler writes the flag for no program. The run time keeps the instances of one VM apart for a host that has such a built-in.
    const ProgramTables& tables = Image::current()->programTables();
    AOT_TEST_CHECK(!(tables.flags & ProgramTables::mayRunInMultipleInstances));
    overwriteWordInImage(tables.flags, tables.flags | ProgramTables::mayRunInMultipleInstances);
    Instance* second = Instance::of(JSGlobalObject::createWithCustomMethodTable(vm, JSGlobalObject::createStructure(vm, jsNull()), program.realm()->globalObjectMethodTable()));
    AOT_TEST_CHECK(second && second->state && second->state->globalBindings);

    Instance& first = program.instance();
    AOT_TEST_CHECK(first.runtime.counters.globalBindings && second->runtime.counters.globalBindings);
    AOT_TEST_CHECK(first.runtime.counters.globalBindings != second->runtime.counters.globalBindings);
    // Compiled code reaches a word of these tables from the instance, without the address of the table.
    for (Instance* instance : { &first, second }) {
        AOT_TEST_CHECK(instance->runtime.counters.globalBindings == instance->tableAt<GlobalBinding>(InstanceLayout::offsetOfGlobalBindings));
        AOT_TEST_CHECK(instance->prefix().builtinCells == instance->tableAt<JSCell*>(InstanceLayout::offsetOfBuiltinCells));
        AOT_TEST_CHECK(instance->builtins.hostValues == instance->tableAt<EncodedJSValue>(InstanceLayout::offsetOfBuiltinValues));
        AOT_TEST_CHECK(instance->builtins.locationStates == instance->tableAt<uint8_t>(InstanceLayout::offsetOfBuiltinLocationStates));
    }

    uint32_t number = recordOf(program, "shared"_s, true);
    GlobalBinding& record = program.state().globalBindings->at(number);
    GlobalBinding& recordOfSecond = second->state->globalBindings->at(number);
    AOT_TEST_CHECK(&record == &first.runtime.counters.globalBindings[number] && &recordOfSecond == &second->runtime.counters.globalBindings[number]);
    AOT_TEST_CHECK(!record.constant && !record.structureID && !record.location);

    defineShared(program, jsNumber(1));
    AOT_TEST_CHECK(program.state().globalBindings->fileConstant(vm, number, program.realm(), jsNumber(1)));
    AOT_TEST_CHECK(record.constant == JSValue::encode(jsNumber(1)));
    AOT_TEST_CHECK(!recordOfSecond.constant && !recordOfSecond.structureID && !recordOfSecond.location);

    second->state->globalBindings->dropConstants();
    AOT_TEST_CHECK(record.constant == JSValue::encode(jsNumber(1)));

    defineShared(program, jsNumber(2));
    AOT_TEST_CHECK(!record.constant);
}

static constexpr ASCIILiteral staleConstantTest = "global-bindings/validating-build-aborts-on-stale-constant"_s;
static constexpr ASCIILiteral freshConstantPassed = "the constant that holds has passed"_s;

static void answerWithStaleConstant(LoadedProgram& program)
{
    uint32_t number = recordOf(program, "shared"_s, true);
    GlobalBinding& record = program.state().globalBindings->at(number);
    defineShared(program, jsNumber(1));
    AOT_TEST_CHECK(program.state().globalBindings->fileConstant(program.vm(), number, program.realm(), jsNumber(1)));
    operationAOTValidateGlobalBinding(&program.instance(), number, record.constant);
    dataLogLn(freshConstantPassed);

    record.constant = JSValue::encode(jsNumber(2));
    operationAOTValidateGlobalBinding(&program.instance(), number, record.constant);
}

static void testValidatingBuildAbortsOnStaleConstant()
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(staleConstantTest, programForManyInstances, answerWithStaleConstant);
        return;
    }

    TemporaryFile image(".image"_s, compileProgramText(programForManyInstances).span());
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(staleConstantTest), makeString("--aotImagePath="_s, image.path()) };
    ChildProcessResult result = launcher.run(arguments.span());
    AOT_TEST_CHECK(result.outcome == ChildProcessOutcome::Crashed);
    AOT_TEST_CHECK(result.output.contains(freshConstantPassed));
    AOT_TEST_CHECK(result.output.contains(makeString("AOT validation: "_s, nameOf(ValidationFailureKind::LookupDiffers))));
}

template<LoadedProgramTest body>
static constexpr void (*inManyInstances)() = runNativeTestWithLoadedProgram<globalBindingProgramTests, programForManyInstances, body>;

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
        { "global-bindings/worker-has-its-own-records"_s, inManyInstances<testWorkerHasItsOwnRecords> },
        { staleConstantTest, testValidatingBuildAbortsOnStaleConstant },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
