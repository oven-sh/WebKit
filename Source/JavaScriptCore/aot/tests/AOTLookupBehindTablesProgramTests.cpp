/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTPropertyCachesWords.h"
#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

// What no table holds the objects themselves answer, in the stubs GetById, InById, GetByValCached, InByValCached and PutByValCached. Every question here meets a Structure and a name for the first few times.
static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 fields=[{key="kept" declared={type=anything} flags=[IsRequired]}, {key="count" declared={type=anything} flags=[IsRequired]}]
layout L1 kind=Instance name="WithMethod" constructor=f12
layout L2 kind=Instance name="WithGetter" constructor=f13
layout L3 kind=Instance name="WithNeither" constructor=f14
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = NewClosure function=f5
    v7 = NewClosure function=f6
    v8 = NewClosure function=f7
    v9 = NewClosure function=f8
    v10 = NewClosure function=f9
    v11 = NewClosure function=f10
    v12 = CallDirect function=f11
    v13 = Builtin builtin=Proxy
    v14 = Call v1, -, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13
    Return
}
function f1 name="read" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="shared"
    Return v1
}
function f2 name="has" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropHas v0 key="shared"
    Return v1
}
function f3 name="hasOwn" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="Object" [isFreeName]
    v3 = PropGet v2 key="hasOwn"
    v4 = String string="shared"
    v5 = Call v3, v2, v0, v4 calleeKey="hasOwn"
    Return v5
}
function f4 name="readByKey" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = ElemGet v0, v1
    Return v2
}
function f5 name="hasByKey" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = ElemHas v0, v1
    Return v2
}
function f6 name="hasOwnByKey" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="Object" [isFreeName]
    v4 = PropGet v3 key="hasOwn"
    v5 = Call v4, v3, v0, v1 calleeKey="hasOwn"
    Return v5
}
function f7 name="storeByKey" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    ElemSet v0, v1, v2 mode=Strict
    Return
}
function f8 name="storeByKeySloppy" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    ElemSet v0, v1, v2 mode=Sloppy
    Return
}
function f9 name="makeRecord" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = NewObject -, v0, v1 layout=L0 fields=[0, 1]
    v3 = FinishObject v2
    Return v3
}
function f10 name="construct" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Construct v0
    Return v1
}
function f11 name="defineClasses" results=[{type=anything}] {
    v0 = NewClosure function=f12
    v1 = NewClass v0 layout=L1
    v2 = NewClosure function=f15
    DefineMethod v1, v2 layout=L1 key="shared"
    v4 = FinishClass v1 layout=L1
    v5 = NewClosure function=f13
    v6 = NewClass v5 layout=L2
    v7 = NewClosure function=f16
    DefineMethod v6, v7 layout=L2 methodKind=Getter key="shared"
    v9 = FinishClass v6 layout=L2
    v10 = NewClosure function=f14
    v11 = NewClass v10 layout=L3
    v12 = FinishClass v11 layout=L3
    v13 = NewArray v4, v9, v12 contents=anything
    Return v13
}
function f12 name="WithMethod" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1
    Return v2
}
function f13 name="WithGetter" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L2
    v2 = NewObject v1 layout=L2
    Return v2
}
function f14 name="WithNeither" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L3
    v2 = NewObject v1 layout=L3
    Return v2
}
function f15 name="shared" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    Return v0
}
function f16 name="shared" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    Return v0
}
)IR"_s;

// Fewer questions about one Structure and one name than the stubs answer before they leave for the operation that files.
static_assert(MissesOfPairs::missesBetweenFilings > 12);

#define ASK_QUESTIONS R"JS(
const operations = [
    "GetById", "GetByIdAndCache", "GetGeneric", "GetByVal", "GetByValGeneric", "GetByValAndCache", "GetWithProxyTrap", "HasWithProxyTrap",
    "InByIdAndCache", "InByValGeneric", "InByValAndCache", "HasOwnByIdAndCache", "HasOwnByValAndCache",
    "PutByValGeneric", "PutByValAndCache", "PutByValAndCacheSloppy",
];
const calls = () => {
    let sum = 0;
    for (const name of operations) sum += Number(aotOperationCount("operationAOT" + name));
    return sum;
};
// ask(n) says whether the nth answer is right.
const verdict = (ask, count = 12) => {
    const callsBefore = calls();
    let wrong = 0;
    for (let n = 0; n < count; n++) wrong += ask(n) ? 0 : 1;
    const callsMade = calls() - callsBefore;
    if (wrong) return "WRONG " + wrong + " times";
    if (!globalThis.printsCalls) return "right";
    return !callsMade ? "objects" : callsMade >= count ? "operations" : "a few operations";
};
const printAnswers = (kind, answers) => {
    let line = "";
    for (const question in answers) line += (line ? " | " : kind + ": ") + question + " " + answers[question];
    print(line);
};
let numberOfObjects = 0;
// An object of a Structure that nothing has been asked about.
const fresh = (prototype = Object.prototype) => {
    const object = Object.create(prototype);
    object["own" + numberOfObjects++] = 0;
    return object;
};
const withManyNames = (object, howMany) => {
    for (let k = 0; k < howMany; k++) object["filler" + k] = k;
    return object;
};
)JS"

// Each kind makes { object, value, has, hasOwn }: what a read of `shared` gives and what the two questions about it give.
#define STATEMENTS_ASKING_ORDINARY_OBJECTS R"JS(
globalThis.publish = (read, has, hasOwn, readByKey, hasByKey, hasOwnByKey) => {
    const own = (object, value) => { object.shared = value; return { object, value, has: true, hasOwn: true }; };
    const inherited = (prototype, value) => { prototype.shared = value; return { object: fresh(prototype), value, has: true, hasOwn: false }; };
    const absent = object => ({ object, value: undefined, has: false, hasOwn: false });
    const kinds = {
        "own value in the cell": n => own(fresh(), n),
        "own value outside the cell": n => own(withManyNames(fresh(), 10), n),
        "own value among 300 names": n => own(withManyNames(fresh(), 300), n),
        "own value without a prototype": n => own(fresh(null), n),
        "own value of a dictionary that gives names up": n => { const made = own(fresh(), n); $vm.toUncacheableDictionary(made.object); return made; },
        "value of the prototype": n => inherited(fresh(), n),
        "value of the prototype, outside its cell": n => inherited(withManyNames(fresh(), 10), n),
        "value of a prototype with 300 names": n => inherited(withManyNames(fresh(), 300), n),
        "value two objects above": n => { const made = inherited(fresh(), n); return { ...made, object: fresh(made.object) }; },
        "nothing": n => absent(fresh()),
        "nothing without a prototype": n => absent(fresh(null)),
        "nothing among 10 names": n => absent(withManyNames(fresh(), 10)),
        "nothing among 300 names": n => absent(withManyNames(fresh(), 300)),
        "nothing below a prototype with 300 names": n => absent(fresh(withManyNames(fresh(), 300))),
        "own getter": n => {
            const object = fresh();
            Object.defineProperty(object, "shared", { get() { return this === object ? n : "another receiver"; }, configurable: true });
            return { object, value: n, has: true, hasOwn: true };
        },
        "getter of the prototype": n => {
            const prototype = fresh();
            const object = fresh(prototype);
            Object.defineProperty(prototype, "shared", { get() { return this === object ? n : "another receiver"; }, configurable: true });
            return { object, value: n, has: true, hasOwn: false };
        },
        "setter alone": n => {
            const object = fresh();
            Object.defineProperty(object, "shared", { set(value) { }, configurable: true });
            return { object, value: undefined, has: true, hasOwn: true };
        },
        "own value that is undefined": n => own(fresh(), undefined),
        "own value beside an element": n => { const made = own(fresh(), n); made.object[0] = 1; return made; },
        "value behind a proxy": n => ({ ...own(fresh(), n), object: new Proxy(own(fresh(), n).object, { }) }),
    };
    for (const kind in kinds) {
        const make = kinds[kind];
        const answers = {
            read: verdict(n => { const made = make(n); return read(made.object) === made.value; }),
            in: verdict(n => { const made = make(n); return has(made.object) === made.has; }),
            hasOwn: verdict(n => { const made = make(n); return hasOwn(made.object) === made.hasOwn; }),
            "read by key": verdict(n => { const made = make(n); return readByKey(made.object, "shared") === made.value; }),
            "in by key": verdict(n => { const made = make(n); return hasByKey(made.object, "shared") === made.has; }),
            "hasOwn by key": verdict(n => { const made = make(n); return hasOwnByKey(made.object, "shared") === made.hasOwn; }),
            "read by key, twice": verdict(n => { const made = make(n); return readByKey(made.object, "shared") === made.value && readByKey(made.object, "shared") === made.value; }),
        };
        printAnswers(kind, answers);
    }
};
)JS"

#define LINE_OF_ANSWERS(kind, answer) \
    kind ": read " answer " | in " answer " | hasOwn " answer " | read by key " answer " | in by key " answer " | hasOwn by key " answer " | read by key, twice " answer "\n"

#define OUTPUT_OF_ORDINARY_OBJECTS(objects, operations) \
    LINE_OF_ANSWERS("own value in the cell", objects) \
    LINE_OF_ANSWERS("own value outside the cell", objects) \
    LINE_OF_ANSWERS("own value among 300 names", objects) \
    LINE_OF_ANSWERS("own value without a prototype", objects) \
    LINE_OF_ANSWERS("own value of a dictionary that gives names up", objects) \
    LINE_OF_ANSWERS("value of the prototype", objects) \
    LINE_OF_ANSWERS("value of the prototype, outside its cell", objects) \
    LINE_OF_ANSWERS("value of a prototype with 300 names", objects) \
    LINE_OF_ANSWERS("value two objects above", objects) \
    LINE_OF_ANSWERS("nothing", objects) \
    LINE_OF_ANSWERS("nothing without a prototype", objects) \
    LINE_OF_ANSWERS("nothing among 10 names", objects) \
    LINE_OF_ANSWERS("nothing among 300 names", objects) \
    LINE_OF_ANSWERS("nothing below a prototype with 300 names", objects) \
    LINE_OF_ANSWERS("own getter", objects) \
    LINE_OF_ANSWERS("getter of the prototype", objects) \
    LINE_OF_ANSWERS("setter alone", objects) \
    LINE_OF_ANSWERS("own value that is undefined", objects) \
    LINE_OF_ANSWERS("own value beside an element", operations) \
    LINE_OF_ANSWERS("value behind a proxy", operations)

static constexpr OptionWords shellOptions = { "--aotCountsPath=/dev/null"_s, "--useDollarVM=1"_s };

// With the calls counted, and then only for the answers: checked at run time, and with all code compiled as hot and as cold code.
#define CHECK_OUTPUT(statements, output) do { \
        checkOutputOfProgramInShell(programText, output("objects", "operations", "a few operations") ""_s, { }, "globalThis.printsCalls = true;" ASK_QUESTIONS statements ""_s, shellOptions); \
        for (ASCIILiteral word : { "validate"_s, "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s }) \
            checkOutputOfProgramInShell(programText, output("right", "right", "right") ""_s, { word }, ASK_QUESTIONS statements ""_s, shellOptions); \
    } while (false)

#define OUTPUT_OF_ORDINARY_OBJECTS_BY(objects, operations, fewOperations) OUTPUT_OF_ORDINARY_OBJECTS(objects, operations)

static void testOrdinaryObjectsAnswer()
{
    CHECK_OUTPUT(STATEMENTS_ASKING_ORDINARY_OBJECTS, OUTPUT_OF_ORDINARY_OBJECTS_BY);
}

// A key that is no atom: the stub finds the name of a text of NameOfText::minLength to maxLength 8-bit characters that an operation has seen once.
static_assert(NameOfText::minLength == 2 && NameOfText::maxLength == 16);

#define STATEMENTS_ASKING_BY_TEXTS R"JS(
globalThis.publish = (read, has, hasOwn, readByKey, hasByKey, hasOwnByKey, storeByKey) => {
    const makeOfCharacterCodes = text => {
        const codes = [];
        for (let k = 0; k < text.length; k++) codes[k] = text.charCodeAt(k);
        return String.fromCharCode.apply(null, codes);
    };
    const forms = {
        "made of character codes": makeOfCharacterCodes,
        "a piece of a longer string": text => makeOfCharacterCodes("<<" + text + ">>").substring(2, 2 + text.length),
    };
    const texts = [];
    for (let length = 1; length <= 18; length++) texts[length - 1] = "abcdefghijklmnopqrstuvwxyz".substring(0, length);
    texts[18] = "\u0436\u0437\u0438\u0439";
    for (const form in forms) {
        const makeKey = forms[form];
        for (let t = 0; t < texts.length; t++) {
            const text = texts[t];
            const make = n => { const object = fresh(); object[text] = n; return object; };
            // The operation that the first such key leaves for notes the name of the text.
            readByKey(make(0), makeKey(text));
            printAnswers((t == 18 ? "4 wide" : text.length) + " characters, " + form, {
                "read": verdict(n => readByKey(make(n), makeKey(text)) === n),
                "read of nothing": verdict(n => readByKey(fresh(), makeKey(text)) === undefined),
                "in": verdict(n => hasByKey(make(n), makeKey(text)) === true),
                "hasOwn": verdict(n => hasOwnByKey(fresh(make(n)), makeKey(text)) === false),
                "store": verdict(n => { const object = make(n); storeByKey(object, makeKey(text), n + 1); return object[text] === n + 1; }),
            });
        }
    }
};
)JS"

#define LINE_OF_TEXT(length, form, answer) length " characters, " form ": read " answer " | read of nothing " answer " | in " answer " | hasOwn " answer " | store " answer "\n"
#define LINES_OF_TEXTS(form, answerOfOneCharacter, objects, operations) \
    LINE_OF_TEXT("1", form, answerOfOneCharacter) \
    LINE_OF_TEXT("2", form, objects) LINE_OF_TEXT("3", form, objects) LINE_OF_TEXT("4", form, objects) LINE_OF_TEXT("5", form, objects) LINE_OF_TEXT("6", form, objects) \
    LINE_OF_TEXT("7", form, objects) LINE_OF_TEXT("8", form, objects) LINE_OF_TEXT("9", form, objects) LINE_OF_TEXT("10", form, objects) LINE_OF_TEXT("11", form, objects) \
    LINE_OF_TEXT("12", form, objects) LINE_OF_TEXT("13", form, objects) LINE_OF_TEXT("14", form, objects) LINE_OF_TEXT("15", form, objects) LINE_OF_TEXT("16", form, objects) \
    LINE_OF_TEXT("17", form, operations) LINE_OF_TEXT("18", form, operations) LINE_OF_TEXT("4 wide", form, operations)
// A string of one character is an atom that all such strings share.
#define OUTPUT_OF_TEXTS(objects, operations, fewOperations) \
    LINES_OF_TEXTS("made of character codes", objects, objects, operations) \
    LINES_OF_TEXTS("a piece of a longer string", objects, objects, operations)

static void testTextsThatAreNoAtomsAreNames()
{
    CHECK_OUTPUT(STATEMENTS_ASKING_BY_TEXTS, OUTPUT_OF_TEXTS);
}

// A search goes from word to word of the index of a table until it meets the name or an empty word. These tables are as full as a table gets, and their names lie all over the memory that the names asked for lie in.
#define STATEMENTS_ASKING_FULL_TABLES R"JS(
globalThis.publish = (read, has, hasOwn, readByKey, hasByKey, hasOwnByKey) => {
    const keeper = { };
    const pool = [];
    for (let k = 0; k < 6000; k++) keeper[pool[k] = "n" + (10000 + k)] = k;
    for (const [howMany, stride] of [[127, 31], [1023, 5]]) {
        const listed = [];
        const lacking = [];
        for (let k = 0; listed.length < howMany; k++) (k % 3 || lacking.length == 64 ? listed : lacking).push(pool[k * stride]);
        const full = fresh();
        for (let k = 0; k < howMany; k++) full[listed[k]] = k;
        printAnswers(howMany + 1 + " names", {
            "read": verdict(n => readByKey(fresh(full), listed[n]) === n, howMany),
            "read of nothing": verdict(n => readByKey(fresh(full), lacking[n]) === undefined, lacking.length),
            "in": verdict(n => hasByKey(fresh(full), lacking[n]) === false, lacking.length),
            "hasOwn": verdict(n => hasOwnByKey(full, lacking[n]) === false, lacking.length),
        });
    }
};
)JS"

#define LINE_OF_FULL_TABLE(howMany, answer) howMany " names: read " answer " | read of nothing " answer " | in " answer " | hasOwn " answer "\n"
#define OUTPUT_OF_FULL_TABLES(objects, operations, fewOperations) LINE_OF_FULL_TABLE("128", objects) LINE_OF_FULL_TABLE("1024", objects)

static void testFullTablesAnswer()
{
    CHECK_OUTPUT(STATEMENTS_ASKING_FULL_TABLES, OUTPUT_OF_FULL_TABLES);
}

#define STATEMENTS_ASKING_BY_SYMBOLS R"JS(
globalThis.publish = (read, has, hasOwn, readByKey, hasByKey, hasOwnByKey, storeByKey) => {
    const symbol = Symbol("shared");
    const kinds = {
        "own value": n => { const object = fresh(); object[symbol] = n; return { object, value: n, has: true, hasOwn: true }; },
        "own value among 300 names": n => { const object = withManyNames(fresh(), 300); object[symbol] = n; return { object, value: n, has: true, hasOwn: true }; },
        "value of the prototype": n => { const prototype = fresh(); prototype[symbol] = n; return { object: fresh(prototype), value: n, has: true, hasOwn: false }; },
        "nothing": n => ({ object: fresh(), value: undefined, has: false, hasOwn: false }),
    };
    for (const kind in kinds) {
        const make = kinds[kind];
        printAnswers(kind, {
            "read": verdict(n => { const made = make(n); return readByKey(made.object, symbol) === made.value; }),
            "in": verdict(n => { const made = make(n); return hasByKey(made.object, symbol) === made.has; }),
            "hasOwn": verdict(n => { const made = make(n); return hasOwnByKey(made.object, symbol) === made.hasOwn; }),
        });
    }
};
)JS"

#define LINE_OF_SYMBOL(kind, answer) kind ": read " answer " | in " answer " | hasOwn " answer "\n"
#define OUTPUT_OF_SYMBOLS(objects, operations, fewOperations) \
    LINE_OF_SYMBOL("own value", objects) \
    LINE_OF_SYMBOL("own value among 300 names", objects) \
    LINE_OF_SYMBOL("value of the prototype", objects) \
    LINE_OF_SYMBOL("nothing", objects)

static void testSymbolsAreNames()
{
    CHECK_OUTPUT(STATEMENTS_ASKING_BY_SYMBOLS, OUTPUT_OF_SYMBOLS);
}

// A store replaces the value of a plain own property in the stub. Each kind makes { object, after }: what a read gives behind a store of n + 100.
#define STATEMENTS_STORING_BY_KEY R"JS(
globalThis.publish = (read, has, hasOwn, readByKey, hasByKey, hasOwnByKey, storeByKey, storeByKeySloppy) => {
    const own = (object, n) => { object.shared = n; return { object, after: n + 100 }; };
    const kinds = {
        "own value in the cell": n => own(fresh(), n),
        "own value outside the cell": n => own(withManyNames(fresh(), 10), n),
        "own value among 300 names": n => own(withManyNames(fresh(), 300), n),
        "own value without a prototype": n => own(fresh(null), n),
        "own value of a dictionary that gives names up": n => { const made = own(fresh(), n); $vm.toUncacheableDictionary(made.object); return made; },
        "own value that cannot be written": n => {
            const object = fresh();
            Object.defineProperty(object, "shared", { value: n, writable: false, configurable: true });
            return { object, after: n, throws: true };
        },
        "own value of a frozen object": n => ({ object: Object.freeze(own(fresh(), n).object), after: n, throws: true }),
        "own setter": n => {
            const object = fresh();
            let stored = n;
            Object.defineProperty(object, "shared", { get() { return stored; }, set(value) { stored = value + 1; }, configurable: true });
            return { object, after: n + 101 };
        },
        "setter of the prototype": n => {
            const prototype = fresh();
            let stored = n;
            Object.defineProperty(prototype, "shared", { get() { return stored; }, set(value) { stored = value + 2; }, configurable: true });
            return { object: fresh(prototype), after: n + 102 };
        },
        "value of the prototype": n => ({ object: fresh(own(fresh(), n).object), after: n + 100 }),
        "nothing": n => ({ object: fresh(), after: n + 100 }),
    };
    const storesRightly = (store, isStrict) => n => {
        const made = kinds[kind](n);
        let threw = false;
        try { store(made.object, "shared", n + 100); } catch (error) { threw = error instanceof TypeError; }
        return made.object.shared === made.after && threw === (isStrict && !!made.throws);
    };
    var kind;
    for (kind in kinds)
        printAnswers(kind, { "strict": verdict(storesRightly(storeByKey, true)), "sloppy": verdict(storesRightly(storeByKeySloppy, false)) });
};
)JS"

#define LINE_OF_STORE(kind, answer) kind ": strict " answer " | sloppy " answer "\n"
#define OUTPUT_OF_STORES(objects, operations, fewOperations) \
    LINE_OF_STORE("own value in the cell", objects) \
    LINE_OF_STORE("own value outside the cell", objects) \
    LINE_OF_STORE("own value among 300 names", objects) \
    LINE_OF_STORE("own value without a prototype", objects) \
    LINE_OF_STORE("own value of a dictionary that gives names up", objects) \
    LINE_OF_STORE("own value that cannot be written", operations) \
    LINE_OF_STORE("own value of a frozen object", operations) \
    LINE_OF_STORE("own setter", operations) \
    LINE_OF_STORE("setter of the prototype", operations) \
    LINE_OF_STORE("value of the prototype", operations) \
    LINE_OF_STORE("nothing", operations)

static void testStoresReplaceOwnValues()
{
    CHECK_OUTPUT(STATEMENTS_STORING_BY_KEY, OUTPUT_OF_STORES);
}

// A store gives a dictionary a name that it lacks in the stub, where neither the table of its names nor its storage has to grow. Each kind makes { object, key, value, isRight, throws }.
#define STATEMENTS_ADDING_BY_KEY R"JS(
globalThis.publish = (read, has, hasOwn, readByKey, hasByKey, hasOwnByKey, storeByKey, storeByKeySloppy, makeRecord) => {
    const dictionary = (object, givesNamesUp) => {
        if (givesNamesUp) $vm.toUncacheableDictionary(object);
        else $vm.toCacheableDictionary(object);
        return object;
    };
    const namesOf = object => { const own = Object.keys(object); let names = ""; for (let k = 0; k < own.length; k++) names += "," + own[k]; return names; };
    const added = (object, key = "added") => {
        const namesBefore = namesOf(object);
        return { object, key, isRight: value => object[key] === value && Object.hasOwn(object, key) && namesOf(object) === namesBefore + "," + key };
    };
    const refused = object => {
        const namesBefore = namesOf(object);
        return { object, key: "added", throws: true, isRight: () => object.added === undefined && !("added" in object) && namesOf(object) === namesBefore };
    };
    const kinds = {
        "without a prototype": n => added(dictionary(fresh(null))),
        "below Object.prototype": n => added(dictionary(fresh())),
        "one that gives names up": n => added(dictionary(fresh(), true)),
        "room outside the cell": n => added(dictionary(withManyNames(fresh(), 6))),
        "among 100 names": n => added(dictionary(withManyNames(fresh(), 99))),
        "among 300 names": n => added(withManyNames(fresh(), 299)),
        "a plain value above": n => { const above = fresh(); above.added = -1; return added(dictionary(fresh(above))); },
        "a name of wide characters": n => added(dictionary(fresh()), "\u4e2d\u6587"),
        "behind a name that was deleted and another that took its place": n => {
            const object = dictionary(withManyNames(fresh(), 3));
            delete object.filler1;
            object.later = n;
            return added(object);
        },
        "no storage outside the cell yet": n => added(dictionary(withManyNames(fresh(), 5))),
        "a full table": n => added(dictionary(withManyNames(fresh(), 7))),
        "full storage": n => added(dictionary(withManyNames(fresh(), 9))),
        "the place of a deleted name is free": n => { const object = dictionary(withManyNames(fresh(), 3)); delete object.filler1; return added(object); },
        "the name was there and was deleted": n => { const object = dictionary(withManyNames(fresh(), 3)); object.added = -1; object.last = n; delete object.added; return added(object); },
        "a name that spells an index": n => {
            const object = dictionary(fresh());
            const namesBefore = namesOf(object);
            return { object, key: "7", isRight: value => object[7] === value && namesOf(object) === ",7" + namesBefore };
        },
        "a name that begins with a digit": n => added(dictionary(fresh()), "7th"),
        "a wide character whose low half is a digit": n => added(dictionary(fresh()), "\u0137th"),
        "the empty name": n => added(dictionary(fresh()), ""),
        "then": n => added(dictionary(fresh()), "then"),
        "__proto__ without a prototype": n => added(dictionary(fresh(null)), "__proto__"),
        "__proto__ below Object.prototype": n => {
            const object = dictionary(fresh());
            const value = { };
            return { object, key: "__proto__", value, isRight: () => Object.getPrototypeOf(object) === value && !Object.hasOwn(object, "__proto__") };
        },
        "a symbol": n => {
            const object = dictionary(fresh());
            const key = Symbol("added");
            return { object, key, isRight: value => object[key] === value && Object.getOwnPropertySymbols(object)[0] === key };
        },
        "not extensible": n => refused(Object.preventExtensions(dictionary(fresh()))),
        "sealed": n => refused(Object.seal(dictionary(fresh()))),
        "frozen": n => refused(Object.freeze(dictionary(fresh()))),
        "a setter above": n => {
            const above = fresh();
            let stored;
            Object.defineProperty(above, "added", { set(value) { stored = value; }, configurable: true });
            const object = dictionary(fresh(above));
            return { object, key: "added", isRight: value => stored === value && !Object.hasOwn(object, "added") };
        },
        "a value above that cannot be written": n => {
            const above = fresh();
            Object.defineProperty(above, "added", { value: undefined, writable: false, configurable: true });
            const object = dictionary(fresh(above));
            return { object, key: "added", throws: true, isRight: () => !Object.hasOwn(object, "added") };
        },
        "a proxy above": n => {
            let stored;
            const object = dictionary(fresh(new Proxy({ }, { set(target, key, value) { stored = value; return true; } })));
            return { object, key: "added", isRight: value => stored === value && !Object.hasOwn(object, "added") };
        },
        "a prototype": n => {
            const object = dictionary(fresh());
            const below = fresh(object);
            const wasAbsent = read(below) === undefined && readByKey(below, "shared") === undefined;
            return { object, key: "shared", isRight: value => wasAbsent && read(below) === value && readByKey(below, "shared") === value && hasByKey(below, "shared") };
        },
        "no dictionary": n => added(fresh()),
        "a record with 200 names beside its fields": n => {
            const object = withManyNames(makeRecord(n, n), 200);
            return { object, key: "added", isRight: value => object.added === value && Object.hasOwn(object, "added") };
        },
    };
    const storesRightly = (store, isStrict) => n => {
        const made = kinds[kind](n);
        const value = "value" in made ? made.value : n + 100;
        let threw = false;
        try { store(made.object, made.key, value); } catch (error) { threw = error instanceof TypeError; }
        return made.isRight(value) && threw === (isStrict && !!made.throws);
    };
    var kind;
    for (kind in kinds)
        printAnswers(kind, { "strict": verdict(storesRightly(storeByKey, true)), "sloppy": verdict(storesRightly(storeByKeySloppy, false)) });
};
)JS"

#define OUTPUT_OF_ADDITIONS(objects, operations, fewOperations) \
    LINE_OF_STORE("without a prototype", objects) \
    LINE_OF_STORE("below Object.prototype", objects) \
    LINE_OF_STORE("one that gives names up", objects) \
    LINE_OF_STORE("room outside the cell", objects) \
    LINE_OF_STORE("among 100 names", objects) \
    LINE_OF_STORE("among 300 names", objects) \
    LINE_OF_STORE("a plain value above", objects) \
    LINE_OF_STORE("a name of wide characters", objects) \
    LINE_OF_STORE("behind a name that was deleted and another that took its place", objects) \
    LINE_OF_STORE("no storage outside the cell yet", operations) \
    LINE_OF_STORE("a full table", operations) \
    LINE_OF_STORE("full storage", operations) \
    LINE_OF_STORE("the place of a deleted name is free", operations) \
    LINE_OF_STORE("the name was there and was deleted", operations) \
    LINE_OF_STORE("a name that spells an index", operations) \
    LINE_OF_STORE("a name that begins with a digit", operations) \
    LINE_OF_STORE("a wide character whose low half is a digit", operations) \
    LINE_OF_STORE("the empty name", operations) \
    LINE_OF_STORE("then", operations) \
    LINE_OF_STORE("__proto__ without a prototype", operations) \
    LINE_OF_STORE("__proto__ below Object.prototype", operations) \
    LINE_OF_STORE("a symbol", operations) \
    LINE_OF_STORE("not extensible", operations) \
    LINE_OF_STORE("sealed", operations) \
    LINE_OF_STORE("frozen", operations) \
    LINE_OF_STORE("a setter above", operations) \
    LINE_OF_STORE("a value above that cannot be written", operations) \
    LINE_OF_STORE("a proxy above", operations) \
    LINE_OF_STORE("a prototype", operations) \
    LINE_OF_STORE("no dictionary", operations) \
    LINE_OF_STORE("a record with 200 names beside its fields", operations)

static void testStoresAddToDictionaries()
{
    CHECK_OUTPUT(STATEMENTS_ADDING_BY_KEY, OUTPUT_OF_ADDITIONS);
}

// What the stub has written the engine reads: the order of the names, each value, and every way in which an object changes later. The names are made at run time, and nothing but the dictionary keeps them.
#define STATEMENTS_GROWING_DICTIONARIES R"JS(
globalThis.publish = (read, has, hasOwn, readByKey, hasByKey, hasOwnByKey, storeByKey, storeByKeySloppy) => {
    const nameAt = k => "name" + k * 7 + "x";
    // A text becomes a name where it is first stored under. The object that it is stored in here dies.
    const asName = text => { ({ })[text] = 0; return text; };
    const grow = (object, howMany, keyAt = k => asName(nameAt(k))) => {
        const callsBefore = calls();
        for (let k = 0; k < howMany; k++) storeByKey(object, keyAt(k), { k });
        return calls() - callsBefore;
    };
    const isWhole = (object, howMany, first = 0) => {
        let k = first;
        for (const name in object) {
            if (name !== nameAt(k) || object[name].k !== k || readByKey(object, name).k !== k || !hasOwnByKey(object, name)) return false;
            k++;
        }
        return k === howMany && !hasByKey(object, nameAt(howMany));
    };
    const report = (what, isRight, callsMade, most) => print(what + ": " + (!isRight ? "WRONG" : !globalThis.printsCalls ? "right" : callsMade <= most ? "a few operations" : callsMade + " operations"));
    for (const howMany of [5, 40, 128, 129, 700]) {
        for (const prototype of [null, Object.prototype]) {
            const object = Object.create(prototype);
            $vm.toCacheableDictionary(object);
            const callsMade = grow(object, howMany);
            fullGC();
            report(howMany + " names " + (prototype ? "below Object.prototype" : "without a prototype"), isWhole(object, howMany), callsMade, 12 + howMany / 8);
        }
    }
    {
        const object = Object.create(null);
        $vm.toCacheableDictionary(object);
        grow(object, 40, nameAt);
        fullGC();
        report("40 texts that were no names", isWhole(object, 40), 0, 0);
    }
    const make = () => { const object = Object.create(null); $vm.toCacheableDictionary(object); grow(object, 40); return object; };
    const check = (what, isRight) => report(what, isRight, 0, 0);

    let object = make();
    for (let k = 0; k < 10; k++) delete object[nameAt(k)];
    check("ten names deleted", isWhole(object, 40, 10));
    for (let k = 40; k < 60; k++) storeByKey(object, asName(nameAt(k)), { k });
    fullGC();
    check("twenty names behind them", isWhole(object, 60, 10));

    object = make();
    Object.defineProperty(object, nameAt(39), { get() { return { k: 39 }; }, enumerable: true, configurable: true });
    check("a getter in the place of the last value", isWhole(object, 40));
    storeByKey(object, nameAt(40), { k: 40 });
    check("a name behind the getter", isWhole(object, 41));

    object = make();
    check("written as JSON", JSON.stringify(object).length === JSON.stringify(Object.assign({ }, object)).length && isWhole(JSON.parse(JSON.stringify(object)), 40));
    check("copied", isWhole({ ...object }, 40) && isWhole(Object.assign(Object.create(null), object), 40));
    check("its names listed", Object.keys(object).length === 40 && Object.getOwnPropertyNames(object)[39] === nameAt(39) && Object.entries(object)[39][1].k === 39);
    Object.freeze(object);
    let threw = false;
    try { storeByKey(object, nameAt(40), { k: 40 }); } catch (error) { threw = error instanceof TypeError; }
    check("frozen", threw && Object.isFrozen(object) && isWhole(object, 40));

    object = make();
    $vm.flattenDictionaryObject(object);
    check("no dictionary any more", isWhole(object, 40));
    storeByKey(object, nameAt(40), { k: 40 });
    check("a name behind that", isWhole(object, 41));

    object = make();
    const below = Object.create(object);
    check("a prototype", readByKey(below, nameAt(39)).k === 39 && readByKey(below, nameAt(40)) === undefined);
    storeByKey(object, nameAt(40), { k: 40 });
    check("a prototype given a name", readByKey(below, nameAt(40)).k === 40 && isWhole(object, 41));

    // Many dictionaries die with their names, and the names are made anew.
    let wrong = 0;
    for (let round = 0; round < 30; round++) {
        wrong += isWhole(make(), 40) ? 0 : 1;
        if (round % 10 === 9) fullGC();
    }
    check("thirty that die", !wrong);
};
)JS"

#define LINE_OF_GROWTH(what, answer) what ": " answer "\n"
#define OUTPUT_OF_GROWTH(objects, operations, fewOperations) \
    LINE_OF_GROWTH("5 names without a prototype", fewOperations) \
    LINE_OF_GROWTH("5 names below Object.prototype", fewOperations) \
    LINE_OF_GROWTH("40 names without a prototype", fewOperations) \
    LINE_OF_GROWTH("40 names below Object.prototype", fewOperations) \
    LINE_OF_GROWTH("128 names without a prototype", fewOperations) \
    LINE_OF_GROWTH("128 names below Object.prototype", fewOperations) \
    LINE_OF_GROWTH("129 names without a prototype", fewOperations) \
    LINE_OF_GROWTH("129 names below Object.prototype", fewOperations) \
    LINE_OF_GROWTH("700 names without a prototype", fewOperations) \
    LINE_OF_GROWTH("700 names below Object.prototype", fewOperations) \
    LINE_OF_GROWTH("40 texts that were no names", fewOperations) \
    LINE_OF_GROWTH("ten names deleted", fewOperations) \
    LINE_OF_GROWTH("twenty names behind them", fewOperations) \
    LINE_OF_GROWTH("a getter in the place of the last value", fewOperations) \
    LINE_OF_GROWTH("a name behind the getter", fewOperations) \
    LINE_OF_GROWTH("written as JSON", fewOperations) \
    LINE_OF_GROWTH("copied", fewOperations) \
    LINE_OF_GROWTH("its names listed", fewOperations) \
    LINE_OF_GROWTH("frozen", fewOperations) \
    LINE_OF_GROWTH("no dictionary any more", fewOperations) \
    LINE_OF_GROWTH("a name behind that", fewOperations) \
    LINE_OF_GROWTH("a prototype", fewOperations) \
    LINE_OF_GROWTH("a prototype given a name", fewOperations) \
    LINE_OF_GROWTH("thirty that die", fewOperations)

static void testDictionariesGrownByStoresAreWhole()
{
    CHECK_OUTPUT(STATEMENTS_GROWING_DICTIONARIES, OUTPUT_OF_GROWTH);
}

// A Structure is made for what comes again. The first object that a store by key gives a name which no object of its Structure has been given becomes a dictionary.
// The next one that is given the same name there gets a Structure for it, and for each name behind it. A dictionary that is asked again and again for what it lacks ceases to be one.
#define STATEMENTS_GIVING_NAMES_AGAIN R"JS(
globalThis.publish = (read, has, hasOwn, readByKey, hasByKey, hasOwnByKey, storeByKey) => {
    const kindOf = object => $vm.value(object).indexOf(", Dictionary") >= 0 ? "a dictionary" : "no dictionary";
    const structureOf = object => { const text = $vm.value(object); return text.substring(text.indexOf("StructureID: ")); };
    const build = (names, prototype = null) => {
        const object = Object.create(prototype);
        for (let k = 0; k < names.length; k++) storeByKey(object, names[k], k);
        for (let k = 0; k < names.length; k++) if (object[names[k]] !== k) return "WRONG";
        return object;
    };
    const names = ["first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth", "ninth", "tenth"];
    print("names that come for the first time: " + kindOf(build(names)));
    const second = build(names);
    print("the same names again: " + kindOf(second));
    const third = build(names);
    print("and again: " + kindOf(third) + ", " + (structureOf(second) === structureOf(third) ? "of the same Structure" : "of another Structure"));
    print("the same names below Object.prototype: " + kindOf(build(names, Object.prototype)) + ", then " + kindOf(build(names, Object.prototype)));
    const others = ["first", "second", "third", "other", "another"];
    print("the same beginning and another end: " + kindOf(build(others)) + ", then " + kindOf(build(others)));
    print("one name: " + kindOf(build(["alone"])) + ", then " + kindOf(build(["alone"])));

    let object = build(["asked", "often"]);
    let found = 0;
    for (let k = 0; k < 400; k++) found += readByKey(object, "lacking") === undefined ? 0 : 1;
    print("a dictionary asked 400 times for what it lacks: " + kindOf(object) + ", found " + found + " times");
    storeByKey(object, "later", 2);
    print("given one more name: " + kindOf(object) + ", " + object.asked + object.often + object.later);
    object = build(["sought", "seldom"]);
    for (let k = 0; k < 400; k++) found += readByKey(object, "lacking" + k) === undefined ? 0 : 1;
    print("a dictionary asked for 400 names that it lacks: " + kindOf(object) + ", found " + found + " times");
};
)JS"

#define OUTPUT_OF_NAMES_GIVEN_AGAIN(objects, operations, fewOperations) \
    "names that come for the first time: a dictionary\n" \
    "the same names again: no dictionary\n" \
    "and again: no dictionary, of the same Structure\n" \
    "the same names below Object.prototype: a dictionary, then no dictionary\n" \
    "the same beginning and another end: a dictionary, then no dictionary\n" \
    "one name: a dictionary, then no dictionary\n" \
    "a dictionary asked 400 times for what it lacks: no dictionary, found 0 times\n" \
    "given one more name: a dictionary, 012\n" \
    "a dictionary asked for 400 names that it lacks: a dictionary, found 0 times\n"

static void testStructuresAreMadeForWhatComesAgain()
{
    CHECK_OUTPUT(STATEMENTS_GIVING_NAMES_AGAIN, OUTPUT_OF_NAMES_GIVEN_AGAIN);
}

// The objects of a layout share a Structure, so each pair of a Structure and a name is asked fewer times in all than the stubs answer before they leave.
#define STATEMENTS_ASKING_COMPILED_OBJECTS R"JS(
globalThis.publish = (read, has, hasOwn, readByKey, hasByKey, hasOwnByKey, storeByKey, storeByKeySloppy, makeRecord, construct, classes) => {
    const [WithMethod, WithGetter, WithNeither] = classes;
    const twice = ask => verdict(ask, 2);
    const values = [7, -1.5, "text", null, true, undefined, { }];
    let wrong = 0;
    const callsBefore = calls();
    for (let k = 0; k < values.length; k++) {
        const record = makeRecord(values[k], k);
        wrong += readByKey(record, "kept") === values[k] && readByKey(record, "count") === k ? 0 : 1;
    }
    print("fields of a record by key: " + (wrong ? "WRONG " + wrong + " times" : !globalThis.printsCalls ? "right" : calls() == callsBefore ? "objects" : "operations"));
    const method = WithMethod.prototype.shared;
    const kinds = {
        "a record without the name": n => ({ object: makeRecord(n, n), isRight: value => value === undefined, has: false, hasOwn: false }),
        "a method of the class": n => ({ object: construct(WithMethod), isRight: value => value === method, has: true, hasOwn: false }),
        "a getter of the class": n => { const object = construct(WithGetter); return { object, isRight: value => value === object, has: true, hasOwn: false }; },
        "a class without the name": n => ({ object: construct(WithNeither), isRight: value => value === undefined, has: false, hasOwn: false }),
        "a record given the name later": n => { const object = makeRecord(n, n); object.shared = n; return { object, isRight: value => value === n, has: true, hasOwn: true }; },
        "an ordinary object below a record": n => ({ object: fresh(makeRecord(n, n)), isRight: value => value === undefined, has: false, hasOwn: false }),
        "an ordinary object below an instance": n => ({ object: fresh(construct(WithMethod)), isRight: value => value === method, has: true, hasOwn: false }),
        "an ordinary object below an instance with a getter": n => { const object = fresh(construct(WithGetter)); return { object, isRight: value => value === object, has: true, hasOwn: false }; },
        "an ordinary object below an instance without the name": n => ({ object: fresh(construct(WithNeither)), isRight: value => value === undefined, has: false, hasOwn: false }),
    };
    // The first reads of an instance itself are filed at once.
    const filedAtOnce = { "a method of the class": true, "a getter of the class": true, "a class without the name": true };
    const printsCalls = globalThis.printsCalls;
    for (const kind in kinds) {
        const make = kinds[kind];
        globalThis.printsCalls = printsCalls && !filedAtOnce[kind];
        printAnswers(kind, {
            "read": twice(n => { const made = make(n); return made.isRight(read(made.object)); }),
            "in": twice(n => { const made = make(n); return has(made.object) === made.has; }),
            "hasOwn": twice(n => { const made = make(n); return hasOwn(made.object) === made.hasOwn; }),
            "read by key": twice(n => { const made = make(n); return made.isRight(readByKey(made.object, "shared")); }),
            "in by key": twice(n => { const made = make(n); return hasByKey(made.object, "shared") === made.has; }),
            "hasOwn by key": twice(n => { const made = make(n); return hasOwnByKey(made.object, "shared") === made.hasOwn; }),
        });
    }
    printAnswers("a record given the name later", {
        "store by key": twice(n => { const object = makeRecord(n, n); object.shared = n; storeByKey(object, "shared", n + 1); return object.shared === n + 1; }),
    });
};
)JS"

#define LINE_OF_COMPILED_OBJECT(kind, answer) kind ": read " answer " | in " answer " | hasOwn " answer " | read by key " answer " | in by key " answer " | hasOwn by key " answer "\n"
#define OUTPUT_OF_COMPILED_OBJECTS(objects, operations, fewOperations) \
    "fields of a record by key: " objects "\n" \
    LINE_OF_COMPILED_OBJECT("a record without the name", objects) \
    LINE_OF_COMPILED_OBJECT("a method of the class", "right") \
    LINE_OF_COMPILED_OBJECT("a getter of the class", "right") \
    LINE_OF_COMPILED_OBJECT("a class without the name", "right") \
    LINE_OF_COMPILED_OBJECT("a record given the name later", objects) \
    LINE_OF_COMPILED_OBJECT("an ordinary object below a record", objects) \
    LINE_OF_COMPILED_OBJECT("an ordinary object below an instance", objects) \
    LINE_OF_COMPILED_OBJECT("an ordinary object below an instance with a getter", objects) \
    LINE_OF_COMPILED_OBJECT("an ordinary object below an instance without the name", objects) \
    "a record given the name later: store by key " objects "\n"

static void testCompiledObjectsAnswer()
{
    CHECK_OUTPUT(STATEMENTS_ASKING_COMPILED_OBJECTS, OUTPUT_OF_COMPILED_OBJECTS);
}

// The key that a trap is handed for a name of the program is kept, so what is filed for a proxy is there behind a collection.
#define STATEMENTS_ASKING_A_PROXY_BEHIND_COLLECTIONS R"JS(
globalThis.publish = (read, has) => {
    const proxy = new Proxy({ shared: 1 }, { get(target, key) { return target[key]; }, has(target, key) { return key in target; } });
    for (let n = 0; n < 40; n++) { read(proxy); has(proxy); }
    printAnswers("a collection in front of each", {
        "read": verdict(n => { fullGC(); return read(proxy) === 1; }),
        "in": verdict(n => { fullGC(); return has(proxy); }),
    });
};
)JS"

#define OUTPUT_OF_A_PROXY_BEHIND_COLLECTIONS(objects, operations, fewOperations) "a collection in front of each: read " objects " | in " objects "\n"

static void testWhatIsFiledForAProxyOutlivesCollections()
{
    CHECK_OUTPUT(STATEMENTS_ASKING_A_PROXY_BEHIND_COLLECTIONS, OUTPUT_OF_A_PROXY_BEHIND_COLLECTIONS);
}

// Nothing in this program makes a proxy, so its code holds nothing that reads and calls traps.
static constexpr ASCIILiteral textOfProgramThatNamesNoProxy = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = NewClosure function=f5
    v7 = NewClosure function=f6
    v8 = Call v1, -, v2, v3, v4, v5, v6, v7
    Return
}
function f1 name="read" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="shared"
    Return v1
}
function f2 name="has" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropHas v0 key="shared"
    Return v1
}
function f3 name="store" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    PropSet v0, v1 key="shared"
    Return
}
function f4 name="readByKey" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = ElemGet v0, v1
    Return v2
}
function f5 name="hasByKey" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = ElemHas v0, v1
    Return v2
}
function f6 name="storeByKey" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    ElemSet v0, v1, v2 mode=Strict
    Return
}
)IR"_s;

#define STATEMENTS_HANDING_PROXIES_OVER R"JS(
globalThis.publish = (read, has, store, readByKey, hasByKey, storeByKey) => {
    const throws = ask => { try { ask(); } catch (error) { return error instanceof TypeError; } return false; };
    const kinds = {
        "with traps": () => {
            const seen = [];
            const target = { shared: 1 };
            const proxy = new Proxy(target, {
                get(object, key, receiver) { seen.push("get " + key + (receiver === proxy)); return object[key] + 1; },
                has(object, key) { seen.push("has " + key); return !(key in object); },
                set(object, key, value, receiver) { seen.push("set " + key + (receiver === proxy)); object[key] = value + 1; return true; },
            });
            return n => {
                seen.length = 0;
                target.shared = n;
                const answers = [read(proxy), has(proxy), readByKey(proxy, "shared"), hasByKey(proxy, "shared"), hasByKey(proxy, "other")];
                store(proxy, n + 10);
                answers.push(target.shared);
                storeByKey(proxy, "shared", n + 20);
                answers.push(target.shared);
                return answers.join() === [n + 1, false, n + 1, false, true, n + 11, n + 21].join()
                    && seen.join() === "get sharedtrue,has shared,get sharedtrue,has shared,has other,set sharedtrue,set sharedtrue";
            };
        },
        "without traps": () => {
            const target = { shared: 1 };
            const proxy = new Proxy(target, { });
            return n => {
                target.shared = n;
                const answers = [read(proxy), has(proxy), readByKey(proxy, "shared"), hasByKey(proxy, "shared"), hasByKey(proxy, "other"), readByKey(proxy, "other")];
                store(proxy, n + 10);
                answers.push(target.shared);
                storeByKey(proxy, "shared", n + 20);
                answers.push(target.shared);
                return answers.join() === [n, true, n, true, false, undefined, n + 10, n + 20].join();
            };
        },
        "above the receiver": () => {
            const heir = Object.create(new Proxy({ }, { get(object, key, receiver) { return receiver === heir ? key : "another receiver"; }, has() { return true; } }));
            return n => read(heir) === "shared" && has(heir) && readByKey(heir, "k" + n) === "k" + n && hasByKey(heir, "k" + n);
        },
        "whose trap answers against the target": () => {
            const proxy = new Proxy(Object.freeze({ shared: 1 }), { get() { return 2; }, has() { return false; }, set() { return true; } });
            return n => throws(() => read(proxy)) && throws(() => has(proxy)) && throws(() => store(proxy, n + 2)) && throws(() => readByKey(proxy, "shared")) && throws(() => hasByKey(proxy, "shared")) && throws(() => storeByKey(proxy, "shared", n + 2));
        },
        "revoked": () => {
            const { proxy, revoke } = Proxy.revocable({ shared: 1 }, { });
            const before = read(proxy) === 1 && has(proxy);
            revoke();
            return n => before && throws(() => read(proxy)) && throws(() => has(proxy)) && throws(() => store(proxy, n)) && throws(() => readByKey(proxy, "shared")) && throws(() => hasByKey(proxy, "shared")) && throws(() => storeByKey(proxy, "shared", n));
        },
    };
    for (const kind in kinds)
        print(kind + ": " + verdict(kinds[kind](), 40));
};
)JS"

#define OUTPUT_OF_PROXIES_HANDED_OVER(verdict) \
    "with traps: " verdict "\n" \
    "without traps: " verdict "\n" \
    "above the receiver: " verdict "\n" \
    "whose trap answers against the target: " verdict "\n" \
    "revoked: " verdict "\n"

static void testAProxyHandedToAProgramThatMakesNoneIsAnswered()
{
    checkOutputOfProgramInShell(textOfProgramThatNamesNoProxy, OUTPUT_OF_PROXIES_HANDED_OVER("operations") ""_s, { }, "globalThis.printsCalls = true;" ASK_QUESTIONS STATEMENTS_HANDING_PROXIES_OVER ""_s, shellOptions);
    for (ASCIILiteral word : { "validate"_s, "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s })
        checkOutputOfProgramInShell(textOfProgramThatNamesNoProxy, OUTPUT_OF_PROXIES_HANDED_OVER("right") ""_s, { word }, ASK_QUESTIONS STATEMENTS_HANDING_PROXIES_OVER ""_s, shellOptions);
}

std::span<const NativeTest> lookupBehindTablesProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "lookup-behind-tables/programs/ordinary-objects-answer"_s, testOrdinaryObjectsAnswer },
        { "lookup-behind-tables/programs/texts-that-are-no-atoms-are-names"_s, testTextsThatAreNoAtomsAreNames },
        { "lookup-behind-tables/programs/full-tables-answer"_s, testFullTablesAnswer },
        { "lookup-behind-tables/programs/symbols-are-names"_s, testSymbolsAreNames },
        { "lookup-behind-tables/programs/stores-replace-own-values"_s, testStoresReplaceOwnValues },
        { "lookup-behind-tables/programs/stores-add-to-dictionaries"_s, testStoresAddToDictionaries },
        { "lookup-behind-tables/programs/dictionaries-grown-by-stores-are-whole"_s, testDictionariesGrownByStoresAreWhole },
        { "lookup-behind-tables/programs/structures-are-made-for-what-comes-again"_s, testStructuresAreMadeForWhatComesAgain },
        { "lookup-behind-tables/programs/compiled-objects-answer"_s, testCompiledObjectsAnswer },
        { "lookup-behind-tables/programs/what-is-filed-for-a-proxy-outlives-collections"_s, testWhatIsFiledForAProxyOutlivesCollections },
        { "lookup-behind-tables/programs/a-proxy-handed-to-a-program-that-makes-none-is-answered"_s, testAProxyHandedToAProgramThatMakesNoneIsAnswered },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
