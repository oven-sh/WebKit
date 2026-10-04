//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTSearchOfPropertyNameIDs=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTSearchOfPropertyNameIDs=1", "--useAOTInlineGuessedPlacesEverywhere=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTSearchOfPropertyNameIDs=1", "--useAOTSearchOfPropertyNameIDsInGetById=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTSearchOfPropertyNameIDsInGetById=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTSearchOfPropertyNameIDs=1", "--useAOTSearchOfPropertyNameIDsInGetById=1", "--useAOTInlining=0", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTSearchOfPropertyNameIDs=1", "--useAOTSearchOfPropertyNameIDsInGetById=1", "--useAOTDataStubs=0", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTSearchOfPropertyNameIDs=1", "--useAOTSearchOfPropertyNameIDsInGetById=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useDollarVM=1")
//@ runDefault("--useDollarVM=1")

const failures = [];

function check(actual, expected, what)
{
    if (!Object.is(actual, expected))
        failures.push(what + ": " + String(actual) + " instead of " + String(expected));
}

function read(o)
{
    const marker = o.marker;
    return marker === undefined ? o.wanted : o.wanted + marker;
}
noInline(read);

const births = [{ wanted: 1, marker: 0 }, { wanted: 2, marker: 0, other: 3 }, { wanted: 4, marker: 0, other: 5, third: 6 }];
function wide0() { return { wanted: 100, w0_1: 1, w0_2: 2, w0_3: 3, w0_4: 4, w0_5: 5, w0_6: 6, w0_7: 7, w0_8: 8, w0_9: 9, w0_10: 10, w0_11: 11, w0_12: 12, w0_13: 13, w0_14: 14, w0_15: 15, w0_16: 16, w0_17: 17, w0_18: 18, w0_19: 19, w0_20: 20, w0_21: 21, w0_22: 22, w0_23: 23 }; }
function wide1() { return { w1_0: 0, wanted: 101, w1_2: 2, w1_3: 3, w1_4: 4, w1_5: 5, w1_6: 6, w1_7: 7, w1_8: 8, w1_9: 9, w1_10: 10, w1_11: 11, w1_12: 12, w1_13: 13, w1_14: 14, w1_15: 15, w1_16: 16, w1_17: 17, w1_18: 18, w1_19: 19, w1_20: 20, w1_21: 21, w1_22: 22, w1_23: 23 }; }
function wide2() { return { w2_0: 0, w2_1: 1, wanted: 102, w2_3: 3, w2_4: 4, w2_5: 5, w2_6: 6, w2_7: 7, w2_8: 8, w2_9: 9, w2_10: 10, w2_11: 11, w2_12: 12, w2_13: 13, w2_14: 14, w2_15: 15, w2_16: 16, w2_17: 17, w2_18: 18, w2_19: 19, w2_20: 20, w2_21: 21, w2_22: 22, w2_23: 23 }; }
function wide3() { return { w3_0: 0, w3_1: 1, w3_2: 2, wanted: 103, w3_4: 4, w3_5: 5, w3_6: 6, w3_7: 7, w3_8: 8, w3_9: 9, w3_10: 10, w3_11: 11, w3_12: 12, w3_13: 13, w3_14: 14, w3_15: 15, w3_16: 16, w3_17: 17, w3_18: 18, w3_19: 19, w3_20: 20, w3_21: 21, w3_22: 22, w3_23: 23 }; }
function wide4() { return { w4_0: 0, w4_1: 1, w4_2: 2, w4_3: 3, wanted: 104, w4_5: 5, w4_6: 6, w4_7: 7, w4_8: 8, w4_9: 9, w4_10: 10, w4_11: 11, w4_12: 12, w4_13: 13, w4_14: 14, w4_15: 15, w4_16: 16, w4_17: 17, w4_18: 18, w4_19: 19, w4_20: 20, w4_21: 21, w4_22: 22, w4_23: 23 }; }
function wide5() { return { w5_0: 0, w5_1: 1, w5_2: 2, w5_3: 3, w5_4: 4, wanted: 105, w5_6: 6, w5_7: 7, w5_8: 8, w5_9: 9, w5_10: 10, w5_11: 11, w5_12: 12, w5_13: 13, w5_14: 14, w5_15: 15, w5_16: 16, w5_17: 17, w5_18: 18, w5_19: 19, w5_20: 20, w5_21: 21, w5_22: 22, w5_23: 23 }; }
function wide6() { return { w6_0: 0, w6_1: 1, w6_2: 2, w6_3: 3, w6_4: 4, w6_5: 5, wanted: 106, w6_7: 7, w6_8: 8, w6_9: 9, w6_10: 10, w6_11: 11, w6_12: 12, w6_13: 13, w6_14: 14, w6_15: 15, w6_16: 16, w6_17: 17, w6_18: 18, w6_19: 19, w6_20: 20, w6_21: 21, w6_22: 22, w6_23: 23 }; }
function wide7() { return { w7_0: 0, w7_1: 1, w7_2: 2, w7_3: 3, w7_4: 4, w7_5: 5, w7_6: 6, wanted: 107, w7_8: 8, w7_9: 9, w7_10: 10, w7_11: 11, w7_12: 12, w7_13: 13, w7_14: 14, w7_15: 15, w7_16: 16, w7_17: 17, w7_18: 18, w7_19: 19, w7_20: 20, w7_21: 21, w7_22: 22, w7_23: 23 }; }
function wide8() { return { w8_0: 0, w8_1: 1, w8_2: 2, w8_3: 3, w8_4: 4, w8_5: 5, w8_6: 6, w8_7: 7, wanted: 108, w8_9: 9, w8_10: 10, w8_11: 11, w8_12: 12, w8_13: 13, w8_14: 14, w8_15: 15, w8_16: 16, w8_17: 17, w8_18: 18, w8_19: 19, w8_20: 20, w8_21: 21, w8_22: 22, w8_23: 23 }; }
function wide9() { return { w9_0: 0, w9_1: 1, w9_2: 2, w9_3: 3, w9_4: 4, w9_5: 5, w9_6: 6, w9_7: 7, w9_8: 8, wanted: 109, w9_10: 10, w9_11: 11, w9_12: 12, w9_13: 13, w9_14: 14, w9_15: 15, w9_16: 16, w9_17: 17, w9_18: 18, w9_19: 19, w9_20: 20, w9_21: 21, w9_22: 22, w9_23: 23 }; }
function wide10() { return { w10_0: 0, w10_1: 1, w10_2: 2, w10_3: 3, w10_4: 4, w10_5: 5, w10_6: 6, w10_7: 7, w10_8: 8, w10_9: 9, wanted: 110, w10_11: 11, w10_12: 12, w10_13: 13, w10_14: 14, w10_15: 15, w10_16: 16, w10_17: 17, w10_18: 18, w10_19: 19, w10_20: 20, w10_21: 21, w10_22: 22, w10_23: 23 }; }
function wide11() { return { w11_0: 0, w11_1: 1, w11_2: 2, w11_3: 3, w11_4: 4, w11_5: 5, w11_6: 6, w11_7: 7, w11_8: 8, w11_9: 9, w11_10: 10, wanted: 111, w11_12: 12, w11_13: 13, w11_14: 14, w11_15: 15, w11_16: 16, w11_17: 17, w11_18: 18, w11_19: 19, w11_20: 20, w11_21: 21, w11_22: 22, w11_23: 23 }; }
function wide12() { return { w12_0: 0, w12_1: 1, w12_2: 2, w12_3: 3, w12_4: 4, w12_5: 5, w12_6: 6, w12_7: 7, w12_8: 8, w12_9: 9, w12_10: 10, w12_11: 11, wanted: 112, w12_13: 13, w12_14: 14, w12_15: 15, w12_16: 16, w12_17: 17, w12_18: 18, w12_19: 19, w12_20: 20, w12_21: 21, w12_22: 22, w12_23: 23 }; }
function wide13() { return { w13_0: 0, w13_1: 1, w13_2: 2, w13_3: 3, w13_4: 4, w13_5: 5, w13_6: 6, w13_7: 7, w13_8: 8, w13_9: 9, w13_10: 10, w13_11: 11, w13_12: 12, wanted: 113, w13_14: 14, w13_15: 15, w13_16: 16, w13_17: 17, w13_18: 18, w13_19: 19, w13_20: 20, w13_21: 21, w13_22: 22, w13_23: 23 }; }
function wide14() { return { w14_0: 0, w14_1: 1, w14_2: 2, w14_3: 3, w14_4: 4, w14_5: 5, w14_6: 6, w14_7: 7, w14_8: 8, w14_9: 9, w14_10: 10, w14_11: 11, w14_12: 12, w14_13: 13, wanted: 114, w14_15: 15, w14_16: 16, w14_17: 17, w14_18: 18, w14_19: 19, w14_20: 20, w14_21: 21, w14_22: 22, w14_23: 23 }; }
function wide15() { return { w15_0: 0, w15_1: 1, w15_2: 2, w15_3: 3, w15_4: 4, w15_5: 5, w15_6: 6, w15_7: 7, w15_8: 8, w15_9: 9, w15_10: 10, w15_11: 11, w15_12: 12, w15_13: 13, w15_14: 14, wanted: 115, w15_16: 16, w15_17: 17, w15_18: 18, w15_19: 19, w15_20: 20, w15_21: 21, w15_22: 22, w15_23: 23 }; }
function wide16() { return { w16_0: 0, w16_1: 1, w16_2: 2, w16_3: 3, w16_4: 4, w16_5: 5, w16_6: 6, w16_7: 7, w16_8: 8, w16_9: 9, w16_10: 10, w16_11: 11, w16_12: 12, w16_13: 13, w16_14: 14, w16_15: 15, wanted: 116, w16_17: 17, w16_18: 18, w16_19: 19, w16_20: 20, w16_21: 21, w16_22: 22, w16_23: 23 }; }
function wide17() { return { w17_0: 0, w17_1: 1, w17_2: 2, w17_3: 3, w17_4: 4, w17_5: 5, w17_6: 6, w17_7: 7, w17_8: 8, w17_9: 9, w17_10: 10, w17_11: 11, w17_12: 12, w17_13: 13, w17_14: 14, w17_15: 15, w17_16: 16, wanted: 117, w17_18: 18, w17_19: 19, w17_20: 20, w17_21: 21, w17_22: 22, w17_23: 23 }; }
function wide18() { return { w18_0: 0, w18_1: 1, w18_2: 2, w18_3: 3, w18_4: 4, w18_5: 5, w18_6: 6, w18_7: 7, w18_8: 8, w18_9: 9, w18_10: 10, w18_11: 11, w18_12: 12, w18_13: 13, w18_14: 14, w18_15: 15, w18_16: 16, w18_17: 17, wanted: 118, w18_19: 19, w18_20: 20, w18_21: 21, w18_22: 22, w18_23: 23 }; }
function wide19() { return { w19_0: 0, w19_1: 1, w19_2: 2, w19_3: 3, w19_4: 4, w19_5: 5, w19_6: 6, w19_7: 7, w19_8: 8, w19_9: 9, w19_10: 10, w19_11: 11, w19_12: 12, w19_13: 13, w19_14: 14, w19_15: 15, w19_16: 16, w19_17: 17, w19_18: 18, wanted: 119, w19_20: 20, w19_21: 21, w19_22: 22, w19_23: 23 }; }
function wide20() { return { w20_0: 0, w20_1: 1, w20_2: 2, w20_3: 3, w20_4: 4, w20_5: 5, w20_6: 6, w20_7: 7, w20_8: 8, w20_9: 9, w20_10: 10, w20_11: 11, w20_12: 12, w20_13: 13, w20_14: 14, w20_15: 15, w20_16: 16, w20_17: 17, w20_18: 18, w20_19: 19, wanted: 120, w20_21: 21, w20_22: 22, w20_23: 23 }; }
function wide21() { return { w21_0: 0, w21_1: 1, w21_2: 2, w21_3: 3, w21_4: 4, w21_5: 5, w21_6: 6, w21_7: 7, w21_8: 8, w21_9: 9, w21_10: 10, w21_11: 11, w21_12: 12, w21_13: 13, w21_14: 14, w21_15: 15, w21_16: 16, w21_17: 17, w21_18: 18, w21_19: 19, w21_20: 20, wanted: 121, w21_22: 22, w21_23: 23 }; }
function wide22() { return { w22_0: 0, w22_1: 1, w22_2: 2, w22_3: 3, w22_4: 4, w22_5: 5, w22_6: 6, w22_7: 7, w22_8: 8, w22_9: 9, w22_10: 10, w22_11: 11, w22_12: 12, w22_13: 13, w22_14: 14, w22_15: 15, w22_16: 16, w22_17: 17, w22_18: 18, w22_19: 19, w22_20: 20, w22_21: 21, wanted: 122, w22_23: 23 }; }
function wide23() { return { w23_0: 0, w23_1: 1, w23_2: 2, w23_3: 3, w23_4: 4, w23_5: 5, w23_6: 6, w23_7: 7, w23_8: 8, w23_9: 9, w23_10: 10, w23_11: 11, w23_12: 12, w23_13: 13, w23_14: 14, w23_15: 15, w23_16: 16, w23_17: 17, w23_18: 18, w23_19: 19, w23_20: 20, w23_21: 21, w23_22: 22, wanted: 123 }; }
const makers = [wide0, wide1, wide2, wide3, wide4, wide5, wide6, wide7, wide8, wide9, wide10, wide11, wide12, wide13, wide14, wide15, wide16, wide17, wide18, wide19, wide20, wide21, wide22, wide23];
for (const make of makers)
    noInline(make);

const options = typeof jscOptions === "function" ? jscOptions() : { };
const isCompiled = typeof aotRemarks === "function" && !!aotRemarks("check");
const isCounting = isCompiled && typeof aotOperationCount === "function" && !!options.useAOTOperationCounters && !!options.useAOTDataStubs;
const guessesPlaces = isCompiled && !!options.useAOTDataStubs;
const searchesAtGuessedPlaces = guessesPlaces && !!options.useAOTSearchOfPropertyNameIDs;
const count = name => isCounting ? aotOperationCount(name) || 0 : 0;
const errorOf = run => { try { run(); } catch (error) { return error.constructor.name; } return "none"; };

let unique = 0;
function neverSeen(make)
{
    const o = make();
    o["x" + unique++] = 1;
    return o;
}

if (guessesPlaces)
    check(aotRemarks("read").some(remark => remark === "guessed-place-read:wanted" || remark === "guessed-place-read-through-stub:wanted"), true, "the read is at a guessed place");
for (const birth of births)
    check(read(birth), birth.wanted, "a birth");

for (let position = 0; position < 24; ++position) {
    for (let round = 0; round < 3; ++round) {
        check(read(makers[position]()), 100 + position, "slot " + position + ", as born");
        check(read(neverSeen(makers[position])), 100 + position, "slot " + position + ", a Structure never seen");
    }
    const changed = makers[position]();
    changed.wanted = "changed";
    check(read(changed), "changed", "slot " + position + ", after a store");
}

for (const position of [1, 7, 8, 15, 16, 23]) {
    const objects = [];
    for (let i = 0; i < 240; ++i)
        objects.push(neverSeen(makers[position]));
    const arrivals = count("operationAOTGetById"), elsewhere = count("operationAOTCountGuessedPlace:another-slot"), absent = count("operationAOTCountGuessedPlace:not-an-own-property");
    let sum = 0;
    for (const o of objects)
        sum += read(o);
    check(sum, 240 * (100 + position), "slot " + position + ": 240 Structures never seen");
    if (!isCounting)
        continue;
    check(count("operationAOTCountGuessedPlace:another-slot") - elsewhere, 240, "slot " + position + ": the name has its number in another slot");
    check(count("operationAOTCountGuessedPlace:not-an-own-property") - absent, 240, "slot " + position + ": the marker is not there");
    if (searchesAtGuessedPlaces)
        check(count("operationAOTGetById") - arrivals <= 264, true, "slot " + position + ": only the reads of the marker arrive in the operation (" + (count("operationAOTGetById") - arrivals) + " arrivals for 240 + 240 reads)");
}

{
    const make = makers[20];
    const withAccessor = make();
    let calls = 0;
    Object.defineProperty(withAccessor, "wanted", { get() { ++calls; return "got"; } });
    for (let i = 0; i < 5; ++i)
        check(read(withAccessor), "got", "an accessor in place of the property");
    check(calls, 5, "the getter is called each time");
    const readOnly = Object.defineProperty(make(), "wanted", { value: "fixed", writable: false });
    check(read(readOnly), "fixed", "a read-only property");
    check(read(Object.freeze(make())), 120, "a frozen object");
    check(read(Object.seal(make())), 120, "a sealed object");
    check(read(Object.preventExtensions(make())), 120, "an object that cannot be extended");
    const without = make();
    delete without.wanted;
    check(read(without), undefined, "after delete");
    without.wanted = "again";
    check(read(without), "again", "added again after delete");
    const lessOne = make();
    delete lessOne.w20_3;
    check(read(lessOne), 120, "after another property is deleted");
    const dictionary = make();
    $vm.toCacheableDictionary(dictionary);
    check(read(dictionary), 120, "a dictionary");
    dictionary.wanted = "d";
    check(read(dictionary), "d", "a dictionary, after a store");
    const uncacheable = make();
    $vm.toUncacheableDictionary(uncacheable);
    delete uncacheable.wanted;
    check(read(uncacheable), undefined, "an uncacheable dictionary, after delete");
    const holdsUndefined = make();
    holdsUndefined.wanted = undefined;
    check(read(holdsUndefined), undefined, "the property holds undefined");
    const outOfLine = { };
    for (let i = 0; i < 30; ++i)
        outOfLine["o" + i] = i;
    outOfLine["wan" + "ted".slice(0)] = "far";
    check(read(outOfLine), "far", "out of line");
    const inherits = Object.create(make());
    check(read(inherits), 120, "on the prototype");
    inherits.wanted = "own";
    check(read(inherits), "own", "own, over the prototype's");
    check(read(Object.create(null)), undefined, "no prototype, not there");
    check(read({ unrelated: 1 }), undefined, "not there");
    check(read([]), undefined, "an array");
    check(read(Object.assign([], { wanted: "a" })), "a", "an array with the property");
    check(read(Object.assign(function () { }, { wanted: "f" })), "f", "a function with the property");
    check(read(new Proxy({ }, { get(target, name) { return name === "wanted" ? "trapped" : undefined; } })), "trapped", "a proxy");
    check(read(5), undefined, "a number");
    check(read("s"), undefined, "a string");
    check(read(true), undefined, "a boolean");
    check(read(Symbol.iterator), undefined, "a symbol");
    check(read(5n), undefined, "a big integer");
    check(errorOf(() => read(null)), "TypeError", "null");
    check(errorOf(() => read(undefined)), "TypeError", "undefined");
    class Made { constructor() { this.first = 1; this.wanted = "made"; } }
    check(read(new Made), "made", "an instance of a class");
    const withMarker = make();
    withMarker.marker = 1000;
    check(read(withMarker), 1120, "the marker beyond the named slots");
}

function wideWithFar() { return { f0: 0, f1: 1, f2: 2, f3: 3, f4: 4, f5: 5, f6: 6, f7: 7, f8: 8, f9: 9, f10: 10, f11: 11, f12: 12, f13: 13, f14: 14, f15: 15, f16: 16, f17: 17, f18: 18, f19: 19, f20: 20, f21: 21, f22: 22, f23: 23, far: "far", last: 25 }; }
noInline(wideWithFar);
function readFar0(o) { return o.far; }
function readFar1(o) { return o.far; }
function readFar2(o) { return o.far; }
function readFar3(o) { return o.far; }
function readFar4(o) { return o.far; }
function readFar5(o) { return o.far; }
function readFar6(o) { return o.far; }
function readFar7(o) { return o.far; }
function readFar8(o) { return o.far; }
function readFar9(o) { return o.far; }
function readFar10(o) { return o.far; }
function readFar11(o) { return o.far; }
function readFar12(o) { return o.far; }
function readFar13(o) { return o.far; }
function readFar14(o) { return o.far; }
function readFar15(o) { return o.far; }
function readFar16(o) { return o.far; }
function readFar17(o) { return o.far; }
function readFar18(o) { return o.far; }
function readFar19(o) { return o.far; }
function readFar20(o) { return o.far; }
function readFar21(o) { return o.far; }
function readFar22(o) { return o.far; }
function readFar23(o) { return o.far; }
{
    const sites = [readFar0, readFar1, readFar2, readFar3, readFar4, readFar5, readFar6, readFar7, readFar8, readFar9, readFar10, readFar11, readFar12, readFar13, readFar14, readFar15, readFar16, readFar17, readFar18, readFar19, readFar20, readFar21, readFar22, readFar23];
    const noCells = [5, 1.5, true];
    const unlisted = i => ["z" + i + "a", "z" + i + "b", "z" + i + "c"];
    const others = [
        [i => { const k = unlisted(i); return { [k[0]]: "wrong0", [k[1]]: "wrong1", [k[2]]: "wrong2" }; }, undefined, "names without a number, the property not there"],
        [i => { const k = unlisted(i); const o = { [k[0]]: "wrong0", [k[1]]: "wrong1" }; for (let n = 0; n < 10; ++n) o[k[2] + n] = "wrong"; o["f" + "ar".slice(0)] = "there"; return o; }, "there", "names without a number, the property out of line"],
        [i => { const k = unlisted(i); const o = { [k[0]]: "wrong0", [k[1]]: "wrong1" }; o["f" + "ar".slice(0)] = "near"; return o; }, "near", "names without a number, then the property"],
        [i => { const k = unlisted(i); const o = Object.create(null); o[k[0]] = "wrong0"; o[k[1]] = "wrong1"; return o; }, undefined, "no prototype, names without a number"],
    ];
    let site = 0;
    for (const noCell of noCells) {
        for (const failedAttempts of [1, 3]) {
            for (const [make, expected, what] of others) {
                const readFar = sites[site];
                noInline(readFar);
                for (let i = 0; i < 4; ++i)
                    check(readFar(wideWithFar()), "far", "site " + site + ": beyond the named slots");
                for (let i = 0; i < failedAttempts; ++i)
                    check(readFar(noCell), undefined, "site " + site + ": from " + String(noCell));
                for (let i = 0; i < 3; ++i)
                    check(readFar(make(site * 10 + i)), expected, "site " + site + ", after " + failedAttempts + " reads from " + String(noCell) + ": " + what);
                check(readFar(wideWithFar()), "far", "site " + site + ": beyond the named slots, again");
                ++site;
            }
        }
    }
}

if (failures.length)
    throw new Error(failures.length + " failures:\n" + [...new Set(failures)].slice(0, 30).join("\n"));
