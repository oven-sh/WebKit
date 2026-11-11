/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "TopExceptionScope.h"
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> compactStorageTests();

static constexpr ASCIILiteral compactStorageProgramText =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

static constexpr ASCIILiteral narrowReferencesWord = "references=narrow"_s;

static bool hasCompactStorage(JSValue collection)
{
    if (auto* set = dynamicDowncast<JSSet>(collection))
        return set->hasCompactStorage();
    if (auto* map = dynamicDowncast<JSMap>(collection))
        return map->hasCompactStorage();
    failNativeTest(__FILE__, __LINE__, "the value is a Set or a Map");
}

static JSValue callInPlainRealm(LoadedProgram& program, ASCIILiteral functionSource)
{
    VM& vm = program.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSGlobalObject* realm = program.realms().plain();
    JSValue function = program.realms().evaluate(realm, functionSource);
    MarkedArgumentBuffer arguments;
    JSValue result = call(realm, function, arguments, "the source is a function"_s);
    if (scope.exception())
        dataLogLn("threw ", scope.exception()->value().toWTFString(realm));
    AOT_TEST_CHECK_FOR_SOURCE(!scope.exception(), functionSource);
    return result;
}

static void checkReaders(LoadedProgram& program, ASCIILiteral functionSource, ASCIILiteral expectedText)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realms().plain();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSObject* result = asObject(callInPlainRealm(program, functionSource));
    String text = result->get(realm, Identifier::fromString(vm, "text"_s)).toWTFString(realm);
    if (text != expectedText)
        dataLogLn("the readers gave\n", text, "\nexpected\n", expectedText);
    AOT_TEST_CHECK_FOR_SOURCE(text == expectedText, functionSource);

    auto* collections = uncheckedDowncast<JSArray>(result->get(realm, Identifier::fromString(vm, "collections"_s)));
    AOT_TEST_CHECK(collections->length());
    for (unsigned index = 0; index < collections->length(); ++index)
        AOT_TEST_CHECK_FOR_SOURCE(hasCompactStorage(collections->get(realm, index)), functionSource);
    AOT_TEST_CHECK(!scope.exception());
}

static void testEveryCollectionOfTheVMIsCompact(LoadedProgram& program)
{
    Realms& realms = program.realms();
    for (JSGlobalObject* realm : { realms.plain(), realms.locked() }) {
        AOT_TEST_CHECK(hasCompactStorage(realms.evaluate(realm, "(s => (s.add('a'), s))(new Set)"_s)));
        AOT_TEST_CHECK(hasCompactStorage(realms.evaluate(realm, "(m => (m.set('a', 1), m))(new Map)"_s)));
        AOT_TEST_CHECK(hasCompactStorage(realms.evaluate(realm, "(s => (s.add(1.5), s.add({ }), s.add(Symbol()), s))(new Set)"_s)));
        AOT_TEST_CHECK(!uncheckedDowncast<JSSet>(realms.evaluate(realm, "new Set"_s))->storage());
    }
}

static void testForEach(LoadedProgram& program)
{
    checkReaders(program, R"js((function () {
        let set = new Set;
        let map = new Map;
        for (let i = 0; i < 12; ++i) {
            set.add("k" + i);
            map.set("k" + i, i * 2);
        }
        set.delete("k3");
        map.delete("k4");
        let text = "";
        set.forEach((value, key, owner) => { text += value + (key === value && owner === set ? "," : "?"); });
        text += " ";
        map.forEach((value, key, owner) => { text += key + "=" + value + (owner === map ? "," : "?"); });
        return { text, collections: [set, map] };
    }))js"_s, "k0,k1,k2,k4,k5,k6,k7,k8,k9,k10,k11, k0=0,k1=2,k2=4,k3=6,k5=10,k6=12,k7=14,k8=16,k9=18,k10=20,k11=22,"_s);
}

static void testIterators(LoadedProgram& program)
{
    checkReaders(program, R"js((function () {
        let set = new Set(["a", "b"]);
        let map = new Map([["x", 1], ["y", 2]]);
        let show = result => JSON.stringify(result.value) + (result.done ? " done" : "");
        let parts = [];
        for (let iterator of [set.keys(), set.values(), set.entries(), set[Symbol.iterator](), map.keys(), map.values(), map.entries(), map[Symbol.iterator]()])
            parts.push(show(iterator.next()) + " " + show(iterator.next()) + " " + show(iterator.next()) + " " + show(iterator.next()));
        return { text: parts.join("\n"), collections: [set, map] };
    }))js"_s, R"("a" "b" undefined done undefined done
"a" "b" undefined done undefined done
["a","a"] ["b","b"] undefined done undefined done
"a" "b" undefined done undefined done
"x" "y" undefined done undefined done
1 2 undefined done undefined done
["x",1] ["y",2] undefined done undefined done
["x",1] ["y",2] undefined done undefined done)"_s);
}

static void testSpreadAndLoops(LoadedProgram& program)
{
    checkReaders(program, R"js((function () {
        let set = new Set;
        let map = new Map;
        for (let i = 0; i < 300; ++i) {
            set.add(i % 2 ? "k" + i : i);
            map.set("k" + i, i);
        }
        let parts = [[...set].length, [...set].slice(0, 4).join(), [...map].length, [...map].slice(298).join("|"), Math.max(...map.values())];
        let sum = 0;
        for (let value of set)
            sum += typeof value === "number" ? value : 1;
        for (let [key, value] of map)
            sum += value + key.length;
        parts.push(sum);
        let [first, second, ...rest] = set;
        parts.push(first, second, rest.length);
        return { text: parts.join(" "), collections: [set, map] };
    }))js"_s, "300 0,k1,2,k3 300 k298,298|k299,299 299 68440 0 k1 298"_s);
}

static void testArrayFrom(LoadedProgram& program)
{
    checkReaders(program, R"js((function () {
        let set = new Set;
        let map = new Map;
        for (let i = 0; i < 20; ++i) {
            set.add("s" + i);
            map.set("k" + i, "v" + i);
        }
        set.delete("s0");
        map.delete("k19");
        let ends = array => array.length + ":" + array[0] + ".." + array[array.length - 1];
        let parts = [Array.from(set), Array.from(set.values()), Array.from(set.keys()), Array.from(map.keys()), Array.from(map.values()), Array.from(map), Array.from(set, value => value + "!"), Array.from(map.entries())];
        return { text: parts.map(ends).join(" "), collections: [set, map] };
    }))js"_s, "19:s1..s19 19:s1..s19 19:s1..s19 19:k0..k18 19:v0..v18 19:k0,v0..k18,v18 19:s1!..s19! 19:k0,v0..k18,v18"_s);
}

static void testSetMethods(LoadedProgram& program)
{
    checkReaders(program, R"js((function () {
        let a = new Set(["a", "b", "c", 1, 2]);
        let b = new Set(["c", "d", 2, 3]);
        let results = [a.union(b), a.intersection(b), a.difference(b), a.symmetricDifference(b)];
        let text = results.map(set => [...set].join()).join(" ");
        text += " " + [a.isSubsetOf(b), new Set(["c", 2]).isSubsetOf(b), a.isSupersetOf(new Set(["a", 1])), a.isSupersetOf(b), a.isDisjointFrom(b), a.isDisjointFrom(new Set(["z"]))].join();
        text += " " + [...a].join() + " " + [...b].join();
        return { text, collections: [a, b, ...results] };
    }))js"_s, "a,b,c,1,2,d,3 c,2 a,b,1 a,b,1,d,3 false,true,true,false,false,true a,b,c,1,2 c,d,2,3"_s);
}

static void testGetOrInsert(LoadedProgram& program)
{
    checkReaders(program, R"js((function () {
        let map = new Map([["a", 1]]);
        let parts = [map.getOrInsert("a", 5), map.getOrInsert("b", 6), map.getOrInsert("b", 7), map.getOrInsertComputed("c", key => key + "!"), map.getOrInsertComputed("c", key => "never"), map.getOrInsert(1.5, "number"), map.getOrInsert(-0, "zero"), map.get(0)];
        for (let i = 0; i < 100; ++i)
            map.getOrInsert("k" + i, i);
        parts.push(map.size, map.get("k99"), [...map.keys()].slice(0, 6).join());
        return { text: parts.join(" "), collections: [map] };
    }))js"_s, "1 6 6 c! c! number zero zero 105 99 a,b,c,1.5,0,k0"_s);
}

static void testCopies(LoadedProgram& program)
{
    checkReaders(program, R"js((function () {
        let set = new Set(["a", "b", "c"]);
        set.delete("b");
        let map = new Map([["x", 1], ["y", 2]]);
        let setCopy = new Set(set);
        let mapCopy = new Map(map);
        set.add("later");
        map.set("x", 9);
        setCopy.add("own");
        let grouped = Map.groupBy(["one", "two", "three"], text => text.length);
        let text = [[...setCopy].join(), [...mapCopy].join("|"), [...set].join(), [...map].join("|"), JSON.stringify(Object.fromEntries(map)), [...grouped].join("|")].join(" ");
        return { text, collections: [set, map, setCopy, mapCopy, grouped] };
    }))js"_s, "a,c,own x,1|y,2 a,c,later x,9|y,2 {\"x\":9,\"y\":2} 3,one,two|5,three"_s);
}

static void testIteratorAliveAcrossGrowth(LoadedProgram& program)
{
    checkReaders(program, R"js((function () {
        let set = new Set(["a", "b", "c"]);
        let map = new Map([["a", 0], ["b", 1], ["c", 2]]);
        let setIterator = set.values();
        let mapIterator = map.entries();
        let seen = [setIterator.next().value, mapIterator.next().value.join("=")];
        for (let i = 0; i < 1000; ++i) {
            set.add("k" + i);
            map.set("k" + i, i);
        }
        set.add(1.5);
        map.set(1.5, "wide");
        let restOfSet = [...setIterator];
        let restOfMap = [...mapIterator];
        seen.push(restOfSet.length, restOfSet[0], restOfSet[1], restOfSet[2], restOfSet[1001], restOfSet[1002]);
        seen.push(restOfMap.length, restOfMap[0].join("="), restOfMap[2].join("="), restOfMap[1002].join("="));
        seen.push(setIterator.next().done, mapIterator.next().done);
        return { text: seen.join(" "), collections: [set, map] };
    }))js"_s, "a a=0 1003 b c k0 k999 1.5 1003 b=1 k0=0 1.5=wide true true"_s);
}

static void testIteratorAliveAcrossDeletion(LoadedProgram& program)
{
    checkReaders(program, R"js((function () {
        let set = new Set;
        for (let i = 0; i < 10; ++i)
            set.add("k" + i);
        let iterator = set.values();
        let seen = [iterator.next().value, iterator.next().value, iterator.next().value];
        set.delete("k0");
        set.delete("k2");
        set.delete("k3");
        set.delete("k7");
        seen.push(iterator.next().value);
        for (let i = 10; i < 60; ++i)
            set.add("k" + i);
        set.delete("k5");
        set.add("k0");
        let rest = [...iterator];
        seen.push(rest.length, rest.slice(0, 4).join(), rest[rest.length - 1], set.size);
        return { text: seen.join(" "), collections: [set] };
    }))js"_s, "k0 k1 k2 k4 54 k6,k8,k9,k10 k0 56"_s);
}

static void testIteratorAliveAcrossClear(LoadedProgram& program)
{
    checkReaders(program, R"js((function () {
        let set = new Set(["a", "b", "c"]);
        let map = new Map([["a", 0], ["b", 1], ["c", 2]]);
        let setIterator = set.values();
        let mapIterator = map.keys();
        let finished = set.values();
        [...finished];
        let seen = [setIterator.next().value, setIterator.next().value, mapIterator.next().value];
        set.clear();
        map.clear();
        seen.push(set.size, map.size, set.has("a"), map.has("a"));
        set.add("x");
        set.add("a");
        map.set("y", 5);
        for (let i = 0; i < 40; ++i)
            set.add("k" + i);
        let restOfSet = [...setIterator];
        seen.push(restOfSet.length, restOfSet.slice(0, 3).join(), [...mapIterator].join(), finished.next().done, [...set].length, map.get("y"));
        set.clear();
        set.clear();
        seen.push(set.size, [...set].length);
        set.add("z");
        return { text: seen.join(" "), collections: [set, map] };
    }))js"_s, "a b a 0 0 false false 42 x,a,k0 y true 42 5 0 0"_s);
}

static void testChangesDuringForEach(LoadedProgram& program)
{
    checkReaders(program, R"js((function () {
        let set = new Set(["a", "b", "c"]);
        let seen = [];
        set.forEach(value => {
            seen.push(value);
            if (value === "a") {
                set.delete("b");
                for (let i = 0; i < 20; ++i)
                    set.add("k" + i);
            }
            if (value === "k5")
                set.clear();
            if (value === "k5" || value === "after")
                set.add(value === "k5" ? "after" : "last");
        });
        return { text: seen.join() + " " + [...set].join(), collections: [set] };
    }))js"_s, "a,c,k0,k1,k2,k3,k4,k5,after,last after,last"_s);
}

static void testStructuredCloneInShell()
{
    TemporaryFile image(".image"_s, compileProgramText(compactStorageProgramText, { narrowReferencesWord }).span());
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        String(R"js(
let set = new Set;
let map = new Map;
for (let i = 0; i < 300; ++i) {
    set.add("k" + i);
    map.set("k" + i, { index: i });
}
set.add(1.5);
set.delete("k7");
map.set(map, set);
let setClone = structuredClone(set);
let mapClone = structuredClone(map);
print([setClone !== set, setClone instanceof Set, setClone.size, setClone.has("k0"), setClone.has("k299"), setClone.has(1.5), setClone.has("k7"), set.size].join(" "));
print([mapClone !== map, mapClone instanceof Map, mapClone.size, mapClone.get("k299").index, mapClone.get("k299") !== map.get("k299"), mapClone.has(map), mapClone.get(mapClone) instanceof Set, mapClone.get(mapClone).size].join(" "));
setClone.add("own");
print([setClone.size, set.has("own")].join(" "));
)js"_s),
    };
    ChildProcessResult result = ChildProcessLauncher(shellPath(), { }).run(arguments.span());
    String printed = makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
    constexpr ASCIILiteral expected = "true true 300 true true true false 300\ntrue true 301 299 true false true 300\n301 false\n"_s;
    if (!result.exitedWith(EXIT_SUCCESS) || printed != expected)
        dataLog("the shell ", result.description(), " and printed\n", printed, "expected\n", expected);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS) && printed == expected);
}

template<LoadedProgramTest body>
static void runNativeTestWithNarrowReferences()
{
    for (const NativeTest& test : compactStorageTests()) {
        if (test.run == &runNativeTestWithNarrowReferences<body>) {
            runWithLoadedProgram(test.name, compactStorageProgramText, body, { narrowReferencesWord });
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::span<const NativeTest> compactStorageTests()
{
    static constexpr NativeTest tests[] = {
        { "compact-storage/every-collection-of-the-vm-is-compact"_s, runNativeTestWithNarrowReferences<testEveryCollectionOfTheVMIsCompact> },
        { "compact-storage/for-each"_s, runNativeTestWithNarrowReferences<testForEach> },
        { "compact-storage/iterators"_s, runNativeTestWithNarrowReferences<testIterators> },
        { "compact-storage/spread-and-loops"_s, runNativeTestWithNarrowReferences<testSpreadAndLoops> },
        { "compact-storage/array-from"_s, runNativeTestWithNarrowReferences<testArrayFrom> },
        { "compact-storage/set-methods"_s, runNativeTestWithNarrowReferences<testSetMethods> },
        { "compact-storage/get-or-insert"_s, runNativeTestWithNarrowReferences<testGetOrInsert> },
        { "compact-storage/copies"_s, runNativeTestWithNarrowReferences<testCopies> },
        { "compact-storage/iterator-alive-across-growth"_s, runNativeTestWithNarrowReferences<testIteratorAliveAcrossGrowth> },
        { "compact-storage/iterator-alive-across-deletion"_s, runNativeTestWithNarrowReferences<testIteratorAliveAcrossDeletion> },
        { "compact-storage/iterator-alive-across-clear"_s, runNativeTestWithNarrowReferences<testIteratorAliveAcrossClear> },
        { "compact-storage/changes-during-for-each"_s, runNativeTestWithNarrowReferences<testChangesDuringForEach> },
        { "compact-storage/structured-clone-in-shell"_s, testStructuredCloneInShell },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
