/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = Call v1, -, v2, v3
    Return
}
function f1 name="read" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="shared"
    Return v1
}
function f2 name="store" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    PropSet v0, v1 key="shared" mode=Strict
    Return
}
)IR"_s;

#define MEASURE_ACCESSES R"JS(
const operations = ["GetByIdAndCache", "GetById", "PutByIdAndCache", "PutById"];
const calls = () => {
    let sum = 0;
    for (const name of operations) sum += Number(aotOperationCount("operationAOT" + name));
    return sum;
};
const measure = (what, access) => {
    const callsBefore = calls();
    let result;
    for (let i = 0; i < 100; i++) result = access(i);
    const callsMade = calls() - callsBefore;
    const served = callsMade <= 3 ? "cached" : callsMade <= 8 ? "the object answers" : callsMade >= 100 ? "not cached" : callsMade + " calls";
    print(what + ": " + result + (globalThis.printsCalls ? ", " + served : ""));
};
const create = () => {
    const object = {};
    object["other"] = 1;
    object["shared"] = 2;
    return object;
};
const flattenByReading = (read, object) => {
    $vm.toUncacheableDictionary(object);
    read(object);
    read(object);
};
)JS"

// A cacheable dictionary gets another Structure when a property is deleted or redefined, and keeps its Structure when one is added.
#define STATEMENTS_WITH_CACHEABLE_DICTIONARIES R"JS(
globalThis.publish = (read, store) => {
    const dictionary = create();
    $vm.toCacheableDictionary(dictionary);
    measure("held", () => read(dictionary));
    dictionary["shared"] = 3;
    measure("replaced", () => read(dictionary));
    dictionary["later"] = 4;
    measure("another name added", () => read(dictionary));
    Object.defineProperty(dictionary, "shared", { get() { return 5; }, configurable: true });
    measure("getter", () => read(dictionary));

    const again = create();
    flattenByReading(read, again);
    $vm.toCacheableDictionary(again);
    measure("held, flattened before", () => read(again));
    measure("stored, flattened before", i => { store(again, i); return again["shared"]; });
    measure("read after the stores", () => read(again));
    delete again["shared"];
    measure("deleted", () => read(again));
    again["shared"] = 6;
    measure("added under the same Structure", () => read(again));
    Object.defineProperty(again, "shared", { value: 7, writable: false, configurable: true });
    measure("read-only", i => { try { store(again, i); } catch { } return read(again); });
};
)JS"

#define OUTPUT_LINE(what, served) what served "\n"

// What a read, or a store that replaces a value, is served by where no cache may answer, and a read of a name that the object has not.
#if CPU(X86_64)
#define READ_WITHOUT_CACHE(cached, notCached, objectAnswers) objectAnswers
#define READ_OF_ABSENT_NAME(cached, notCached) cached
#else
#define READ_WITHOUT_CACHE(cached, notCached, objectAnswers) notCached
#define READ_OF_ABSENT_NAME(cached, notCached) notCached
#endif

#define OUTPUT_OF_CACHEABLE_DICTIONARIES(cached, notCached) \
    OUTPUT_LINE("held: 2", cached) \
    OUTPUT_LINE("replaced: 3", cached) \
    OUTPUT_LINE("another name added: 3", cached) \
    OUTPUT_LINE("getter: 5", cached) \
    OUTPUT_LINE("held, flattened before: 2", cached) \
    OUTPUT_LINE("stored, flattened before: 99", cached) \
    OUTPUT_LINE("read after the stores: 99", cached) \
    OUTPUT_LINE("deleted: undefined", READ_OF_ABSENT_NAME(cached, notCached)) \
    OUTPUT_LINE("added under the same Structure: 6", cached) \
    OUTPUT_LINE("read-only: 7", notCached)

static void testCacheableDictionaryFilesWhatItHolds()
{
    checkOutputOfProgramInShell(programText, OUTPUT_OF_CACHEABLE_DICTIONARIES(", cached", ", not cached") ""_s,
        { }, "globalThis.printsCalls = true;" MEASURE_ACCESSES STATEMENTS_WITH_CACHEABLE_DICTIONARIES ""_s, { "--aotCountsPath=/dev/null"_s, "--useDollarVM=1"_s });
    for (ASCIILiteral word : { "validate"_s, "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s })
        checkOutputOfProgramInShell(programText, OUTPUT_OF_CACHEABLE_DICTIONARIES("", "") ""_s, { word }, MEASURE_ACCESSES STATEMENTS_WITH_CACHEABLE_DICTIONARIES ""_s, { "--aotCountsPath=/dev/null"_s, "--useDollarVM=1"_s });
}

// An uncacheable dictionary changes in every way under one Structure, so nothing is filed under it once it cannot be flattened again.
#define STATEMENTS_WITH_UNCACHEABLE_DICTIONARIES R"JS(
globalThis.publish = (read, store) => {
    const dictionary = create();
    $vm.toUncacheableDictionary(dictionary);
    measure("flattened by the first read", () => read(dictionary));
    $vm.toUncacheableDictionary(dictionary);
    measure("held", () => read(dictionary));
    dictionary["shared"] = 3;
    measure("replaced", () => read(dictionary));
    measure("stored", i => { store(dictionary, i); return dictionary["shared"]; });
    delete dictionary["shared"];
    measure("deleted", () => read(dictionary));
    dictionary["shared"] = 4;
    measure("added", () => read(dictionary));
    delete dictionary["other"];
    dictionary["moved"] = 5;
    measure("another name in the place of a deleted one", () => read(dictionary));
    Object.defineProperty(dictionary, "shared", { get() { return 6; }, configurable: true });
    measure("getter", () => read(dictionary));
    delete dictionary["shared"];
    Object.setPrototypeOf(dictionary, { shared: 7 });
    measure("inherited", () => read(dictionary));
    Object.getPrototypeOf(dictionary).shared = 8;
    measure("inherited, replaced", () => read(dictionary));
    Object.setPrototypeOf(dictionary, { get shared() { return 9; } });
    measure("inherited getter", () => read(dictionary));
    Object.setPrototypeOf(dictionary, null);
    measure("no prototype", () => read(dictionary));
};
)JS"

#define OUTPUT_OF_UNCACHEABLE_DICTIONARIES(cached, notCached, objectAnswers) \
    OUTPUT_LINE("flattened by the first read: 2", cached) \
    OUTPUT_LINE("held: 2", READ_WITHOUT_CACHE(cached, notCached, objectAnswers)) \
    OUTPUT_LINE("replaced: 3", READ_WITHOUT_CACHE(cached, notCached, objectAnswers)) \
    OUTPUT_LINE("stored: 99", READ_WITHOUT_CACHE(cached, notCached, objectAnswers)) \
    OUTPUT_LINE("deleted: undefined", READ_WITHOUT_CACHE(cached, notCached, objectAnswers)) \
    OUTPUT_LINE("added: 4", READ_WITHOUT_CACHE(cached, notCached, objectAnswers)) \
    OUTPUT_LINE("another name in the place of a deleted one: 4", READ_WITHOUT_CACHE(cached, notCached, objectAnswers)) \
    OUTPUT_LINE("getter: 6", READ_WITHOUT_CACHE(cached, notCached, objectAnswers)) \
    OUTPUT_LINE("inherited: 7", READ_WITHOUT_CACHE(cached, notCached, objectAnswers)) \
    OUTPUT_LINE("inherited, replaced: 8", READ_WITHOUT_CACHE(cached, notCached, objectAnswers)) \
    OUTPUT_LINE("inherited getter: 9", READ_WITHOUT_CACHE(cached, notCached, objectAnswers)) \
    OUTPUT_LINE("no prototype: undefined", READ_WITHOUT_CACHE(cached, notCached, objectAnswers))

static void testUncacheableDictionaryFilesNothing()
{
    checkOutputOfProgramInShell(programText, OUTPUT_OF_UNCACHEABLE_DICTIONARIES(", cached", ", not cached", ", the object answers") ""_s,
        { }, "globalThis.printsCalls = true;" MEASURE_ACCESSES STATEMENTS_WITH_UNCACHEABLE_DICTIONARIES ""_s, { "--aotCountsPath=/dev/null"_s, "--useDollarVM=1"_s });
    for (ASCIILiteral word : { "validate"_s, "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s })
        checkOutputOfProgramInShell(programText, OUTPUT_OF_UNCACHEABLE_DICTIONARIES("", "", "") ""_s, { word }, MEASURE_ACCESSES STATEMENTS_WITH_UNCACHEABLE_DICTIONARIES ""_s, { "--aotCountsPath=/dev/null"_s, "--useDollarVM=1"_s });
}

std::span<const NativeTest> dictionaryAccessByNameProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "dictionary-access-by-name/programs/cacheable-dictionary-files-what-it-holds"_s, testCacheableDictionaryFilesWhatItHolds },
        { "dictionary-access-by-name/programs/uncacheable-dictionary-files-nothing"_s, testUncacheableDictionaryFilesNothing },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
