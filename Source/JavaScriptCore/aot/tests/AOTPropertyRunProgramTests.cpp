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
    v3 = Call v1, -, v2
    Return
}
function f1 name="storeSix" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = TypeTest v0 testedType=object
    Branch v2 -> taken b1(| 0), notTaken b2()
  b1(v3):
    PropSet v3, v1 key="a" mode=Strict
    PropSet v3, v1 key="b" mode=Strict
    PropSet v3, v1 key="c" mode=Strict
    PropSet v3, v1 key="d" mode=Strict
    PropSet v3, v1 key="e" mode=Strict
    PropSet v3, v1 key="f" mode=Strict
    Return
  b2():
    Return
}
)IR"_s;

static constexpr ASCIILiteral statementsWithExistingProperties = R"JS(
globalThis.publish = storeSix => {
    const create = (before, names) => {
        const object = {};
        for (let i = 0; i < before; i++) object["before" + i] = 0;
        for (let i = 0; i < names.length; i++) object[names[i]] = 0;
        return object;
    };
    const storeAndPrint = (what, object) => {
        for (let i = 1; i <= 300; i++) storeSix(object, i);
        let numberOfKeys = 0;
        for (const key in object) numberOfKeys++;
        print(what + ": " + [object.a, object.b, object.c, object.d, object.e, object.f] + ", " + numberOfKeys + " keys");
    };
    storeAndPrint("in a row", create(0, "abcdef"));
    storeAndPrint("across the end of inline storage", create(4, "abcdef"));
    storeAndPrint("other order", create(0, "fedcba"));
    storeAndPrint("dictionary", create(200, "abcdef"));
};
)JS"_s;

static void testRunOverwritesExistingProperties()
{
    checkOutputOfProgramInShell(programText,
        "in a row: 300,300,300,300,300,300, 6 keys\n"
        "across the end of inline storage: 300,300,300,300,300,300, 10 keys\n"
        "other order: 300,300,300,300,300,300, 6 keys\n"
        "dictionary: 300,300,300,300,300,300, 206 keys\n"_s,
        { }, statementsWithExistingProperties);
}

static constexpr ASCIILiteral statementsWithNewObjects = R"JS(
globalThis.publish = storeSix => {
    const storeAndPrint = (what, create) => {
        let object;
        for (let i = 1; i <= 300; i++) {
            object = create();
            storeSix(object, i);
        }
        print(what + ": " + Object.keys(object) + ", " + [object.a, object.b, object.c, object.d, object.e, object.f]);
    };
    const prototype = { inherited: 0 };
    const below = Object.create(prototype);
    storeAndPrint("no prototype", () => Object.create(null));
    storeAndPrint("Object.prototype", () => ({}));
    storeAndPrint("other prototype", () => Object.create(prototype));
    storeAndPrint("two prototypes", () => Object.create(below));
    storeAndPrint("no room in the cell", () => { const object = Object.create(null); object.before = 0; return object; });
    storeAndPrint("array", () => []);
    storeAndPrint("function", () => function () { });
};
)JS"_s;

static void testRunAddsPropertiesToNewObjects()
{
    checkOutputOfProgramInShell(programText,
        "no prototype: a,b,c,d,e,f, 300,300,300,300,300,300\n"
        "Object.prototype: a,b,c,d,e,f, 300,300,300,300,300,300\n"
        "other prototype: a,b,c,d,e,f, 300,300,300,300,300,300\n"
        "two prototypes: a,b,c,d,e,f, 300,300,300,300,300,300\n"
        "no room in the cell: before,a,b,c,d,e,f, 300,300,300,300,300,300\n"
        "array: a,b,c,d,e,f, 300,300,300,300,300,300\n"
        "function: a,b,c,d,e,f, 300,300,300,300,300,300\n"_s,
        { }, statementsWithNewObjects);
}

static constexpr ASCIILiteral statementsWithTwoStructuresInTurn = R"JS(
globalThis.publish = storeSix => {
    const prototypes = [{ inherited: 0 }, { inherited: 1 }];
    let wrong = 0;
    for (let i = 1; i <= 1000; i++) {
        const object = Object.create(prototypes[i & 1]);
        storeSix(object, i);
        if (Object.keys(object).join() !== "a,b,c,d,e,f" || object.a !== i || object.f !== i || object.inherited !== (i & 1))
            wrong++;
    }
    print(wrong + " wrong");
};
)JS"_s;

static void testTwoStructuresInTurn()
{
    checkOutputOfProgramInShell(programText, "0 wrong\n"_s, { }, statementsWithTwoStructuresInTurn);
}

static constexpr ASCIILiteral statementsChangingThePrototypeChain = R"JS(
globalThis.publish = storeSix => {
    const base = { inherited: 0 };
    const kept = [];
    let seen = 0;
    const storeAndPrint = what => {
        let object;
        for (let i = 1; i <= 300; i++) {
            object = Object.create(base);
            try { storeSix(object, i); } catch (error) { if (!(error instanceof TypeError)) print(error); }
        }
        kept[kept.length] = object;
        print(what + ": " + Object.keys(object) + ", " + [object.a, object.b, object.c, object.d, object.e, object.f] + ", seen " + seen);
    };
    storeAndPrint("nothing inherited");
    base.other = 0;
    storeAndPrint("other name on the prototype");
    Object.defineProperty(base, "c", { set(value) { seen += value; }, configurable: true });
    storeAndPrint("inherited setter");
    delete base.c;
    storeAndPrint("setter deleted");
    Object.defineProperty(base, "e", { value: -1, writable: false, configurable: true });
    storeAndPrint("inherited read-only property");
    Object.defineProperty(base, "e", { value: -2, writable: true });
    storeAndPrint("inherited writable property");
    Object.setPrototypeOf(base, { set f(value) { seen = -value; } });
    storeAndPrint("setter above the prototype");
    Object.setPrototypeOf(base, null);
    storeAndPrint("nothing above the prototype");
};
)JS"_s;

static constexpr ASCIILiteral outputWhileThePrototypeChainChanges =
    "nothing inherited: a,b,c,d,e,f, 300,300,300,300,300,300, seen 0\n"
    "other name on the prototype: a,b,c,d,e,f, 300,300,300,300,300,300, seen 0\n"
    "inherited setter: a,b,d,e,f, 300,300,,300,300,300, seen 45150\n"
    "setter deleted: a,b,c,d,e,f, 300,300,300,300,300,300, seen 45150\n"
    "inherited read-only property: a,b,c,d, 300,300,300,300,-1,, seen 45150\n"
    "inherited writable property: a,b,c,d,e,f, 300,300,300,300,300,300, seen 45150\n"
    "setter above the prototype: a,b,c,d,e, 300,300,300,300,300,, seen -300\n"
    "nothing above the prototype: a,b,c,d,e,f, 300,300,300,300,300,300, seen -300\n"_s;

static void testRunFollowsChangesToPrototypeChain()
{
    checkOutputOfProgramInShell(programText, outputWhileThePrototypeChainChanges, { "references=wide"_s }, statementsChangingThePrototypeChain);
    checkOutputOfProgramInShell(programText, outputWhileThePrototypeChainChanges, { "references=narrow"_s }, statementsChangingThePrototypeChain);
}

std::span<const NativeTest> propertyRunProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "property-run/programs/run-overwrites-existing-properties"_s, testRunOverwritesExistingProperties },
        { "property-run/programs/run-adds-properties-to-new-objects"_s, testRunAddsPropertiesToNewObjects },
        { "property-run/programs/run-follows-changes-to-the-prototype-chain"_s, testRunFollowsChangesToPrototypeChain },
        { "property-run/programs/two-structures-in-turn"_s, testTwoStructuresInTurn },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
