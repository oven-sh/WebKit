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
  b0():
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
    v13 = Call v1, -, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12
    Return
}
function f1 name="read" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="shared"
    Return v1
}
function f2 name="has" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropHas v0 key="shared"
    Return v1
}
function f3 name="hasOwn" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="Object" [isFreeName]
    v3 = PropGet v2 key="hasOwn"
    v4 = String string="shared"
    v5 = Call v3, v2, v0, v4 calleeKey="hasOwn"
    Return v5
}
function f4 name="readByKey" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = ElemGet v0, v1
    Return v2
}
function f5 name="hasByKey" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = ElemHas v0, v1
    Return v2
}
function f6 name="hasOwnByKey" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="Object" [isFreeName]
    v4 = PropGet v3 key="hasOwn"
    v5 = Call v4, v3, v0, v1 calleeKey="hasOwn"
    Return v5
}
function f7 name="storeByKey" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    ElemSet v0, v1, v2 mode=Strict
    Return
}
function f8 name="storeByKeySloppy" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    ElemSet v0, v1, v2 mode=Sloppy
    Return
}
function f9 name="makeRecord" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = NewObject -, v0, v1 layout=L0 fields=[0, 1]
    v3 = FinishObject v2
    Return v3
}
function f10 name="construct" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Construct v0
    Return v1
}
function f11 name="defineClasses" results=[{type=anything}] {
  b0():
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
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1
    Return v2
}
function f13 name="WithGetter" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L2
    v2 = NewObject v1 layout=L2
    Return v2
}
function f14 name="WithNeither" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L3
    v2 = NewObject v1 layout=L3
    Return v2
}
function f15 name="shared" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    Return v0
}
function f16 name="shared" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    Return v0
}
)IR"_s;

// Fewer questions about one Structure and one name than the stubs answer before they leave for the operation that files.
static_assert(MissesOfPairs::missesBetweenFilings > 12);

#define ASK_QUESTIONS R"JS(
const operations = [
    "GetById", "GetByIdAndCache", "GetGeneric", "GetByVal", "GetByValGeneric", "GetByValAndCache",
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

std::span<const NativeTest> lookupBehindTablesProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "lookup-behind-tables/programs/ordinary-objects-answer"_s, testOrdinaryObjectsAnswer },
        { "lookup-behind-tables/programs/texts-that-are-no-atoms-are-names"_s, testTextsThatAreNoAtomsAreNames },
        { "lookup-behind-tables/programs/symbols-are-names"_s, testSymbolsAreNames },
        { "lookup-behind-tables/programs/stores-replace-own-values"_s, testStoresReplaceOwnValues },
        { "lookup-behind-tables/programs/compiled-objects-answer"_s, testCompiledObjectsAnswer },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
