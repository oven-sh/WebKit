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
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = Call v1, -, v2, v3
    Return
}
function f1 name="read" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="shared"
    Return v1
}
function f2 name="store" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    PropSet v0, v1 key="shared" mode=Strict
    Return
}
)IR"_s;

#define ACCESS_DICTIONARIES COUNTED_CALLS(R"("GetByIdAndCache", "GetById", "GetGeneric", "PutByIdAndCache", "PutById")") R"JS(
const repeat = access => {
    let answer;
    for (let i = 0; i < 400; i++) answer = access(i);
    return answer;
};
const printLastAnswer = (what, access) => { const before = countedCalls(); print(what + ": " + repeat(access) + servedBy(before, 400)); };
const create = () => {
    const object = {};
    object["other"] = 1;
    object["shared"] = 2;
    return object;
};
// Filing a read flattens a dictionary, but only once per object: an object that becomes a dictionary again stays one.
const flattenByReading = (read, object) => {
    $vm.toUncacheableDictionary(object);
    repeat(() => read(object));
};
)JS"

#define STATEMENTS_WITH_CACHEABLE_DICTIONARIES R"JS(
globalThis.publish = (read, store) => {
    const dictionary = create();
    $vm.toCacheableDictionary(dictionary);
    printLastAnswer("held", () => read(dictionary));
    dictionary["shared"] = 3;
    printLastAnswer("replaced", () => read(dictionary));
    dictionary["later"] = 4;
    printLastAnswer("another name added", () => read(dictionary));
    Object.defineProperty(dictionary, "shared", { get() { return 5; }, configurable: true });
    printLastAnswer("getter", () => read(dictionary));

    const again = create();
    flattenByReading(read, again);
    $vm.toCacheableDictionary(again);
    printLastAnswer("held, flattened before", () => read(again));
    printLastAnswer("stored, flattened before", i => { store(again, i); return again["shared"]; });
    printLastAnswer("read after the stores", () => read(again));
    delete again["shared"];
    printLastAnswer("deleted", () => read(again));
    again["shared"] = 6;
    printLastAnswer("added under the same Structure", () => read(again));
    Object.defineProperty(again, "shared", { value: 7, writable: false, configurable: true });
    printLastAnswer("read-only", i => { try { store(again, i); } catch { } return read(again); });
};
)JS"

#define OUTPUT_OF_CACHEABLE_DICTIONARIES \
    "held: 2\n" \
    "replaced: 3\n" \
    "another name added: 3\n" \
    "getter: 5\n" \
    "held, flattened before: 2\n" \
    "stored, flattened before: 399\n" \
    "read after the stores: 399\n" \
    "deleted: undefined\n" \
    "added under the same Structure: 6\n" \
    "read-only: 7\n"

static void testCacheableDictionaryFilesWhatItHolds()
{
    checkOutputOfProgramInShell(programText, OUTPUT_OF_CACHEABLE_DICTIONARIES ""_s, { }, ACCESS_DICTIONARIES STATEMENTS_WITH_CACHEABLE_DICTIONARIES ""_s, { "--useDollarVM=1"_s });
    checkServicesInShell(__FILE__, __LINE__, programText, OUTPUT_OF_CACHEABLE_DICTIONARIES ""_s, ACCESS_DICTIONARIES STATEMENTS_WITH_CACHEABLE_DICTIONARIES ""_s, wordsOfCaches, "cccccccocn"_s, { }, { "--aotCountsPath=/dev/null"_s, "--useDollarVM=1"_s });
}

static void testCacheableDictionaryIsServedAsItWas()
{
    checkServicesInShell(__FILE__, __LINE__, programText, OUTPUT_OF_CACHEABLE_DICTIONARIES ""_s, ACCESS_DICTIONARIES STATEMENTS_WITH_CACHEABLE_DICTIONARIES ""_s, wordsOfCaches, "cccccccccn"_s, { }, { "--aotCountsPath=/dev/null"_s, "--useDollarVM=1"_s });
}

#define STATEMENTS_WITH_UNCACHEABLE_DICTIONARIES R"JS(
globalThis.publish = (read, store) => {
    const dictionary = create();
    $vm.toUncacheableDictionary(dictionary);
    printLastAnswer("flattened by reading", () => read(dictionary));
    $vm.toUncacheableDictionary(dictionary);
    printLastAnswer("held", () => read(dictionary));
    dictionary["shared"] = 3;
    printLastAnswer("replaced", () => read(dictionary));
    printLastAnswer("stored", i => { store(dictionary, i); return dictionary["shared"]; });
    delete dictionary["shared"];
    printLastAnswer("deleted", () => read(dictionary));
    dictionary["shared"] = 4;
    printLastAnswer("added", () => read(dictionary));
    delete dictionary["other"];
    dictionary["moved"] = 5;
    printLastAnswer("another name in the place of a deleted one", () => read(dictionary));
    Object.defineProperty(dictionary, "shared", { get() { return 6; }, configurable: true });
    printLastAnswer("getter", () => read(dictionary));
    delete dictionary["shared"];
    Object.setPrototypeOf(dictionary, { shared: 7 });
    printLastAnswer("inherited", () => read(dictionary));
    Object.getPrototypeOf(dictionary).shared = 8;
    printLastAnswer("inherited, replaced", () => read(dictionary));
    Object.setPrototypeOf(dictionary, { get shared() { return 9; } });
    printLastAnswer("inherited getter", () => read(dictionary));
    Object.setPrototypeOf(dictionary, null);
    printLastAnswer("no prototype", () => read(dictionary));
};
)JS"

#define OUTPUT_OF_UNCACHEABLE_DICTIONARIES \
    "flattened by reading: 2\n" \
    "held: 2\n" \
    "replaced: 3\n" \
    "stored: 399\n" \
    "deleted: undefined\n" \
    "added: 4\n" \
    "another name in the place of a deleted one: 4\n" \
    "getter: 6\n" \
    "inherited: 7\n" \
    "inherited, replaced: 8\n" \
    "inherited getter: 9\n" \
    "no prototype: undefined\n"

static void testUncacheableDictionaryFilesNothing()
{
    checkOutputOfProgramInShell(programText, OUTPUT_OF_UNCACHEABLE_DICTIONARIES ""_s, { }, ACCESS_DICTIONARIES STATEMENTS_WITH_UNCACHEABLE_DICTIONARIES ""_s, { "--useDollarVM=1"_s });
    checkServicesInShell(__FILE__, __LINE__, programText, OUTPUT_OF_UNCACHEABLE_DICTIONARIES ""_s, ACCESS_DICTIONARIES STATEMENTS_WITH_UNCACHEABLE_DICTIONARIES ""_s, wordsOfCaches, "cooooooooooo"_s, { }, { "--aotCountsPath=/dev/null"_s, "--useDollarVM=1"_s });
}

std::span<const NativeTest> dictionaryAccessByNameProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "dictionary-access-by-name/programs/cacheable-dictionary-files-what-it-holds"_s, testCacheableDictionaryFilesWhatItHolds },
        { "dictionary-access-by-name/programs/cacheable-dictionary-is-served-as-it-was"_s, testCacheableDictionaryIsServedAsItWas, "reads by name: a name deleted from a cacheable dictionary is asked of the object at every read; the compiler before this one filed that it is absent"_s },
        { "dictionary-access-by-name/programs/uncacheable-dictionary-files-nothing"_s, testUncacheableDictionaryFilesNothing },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
