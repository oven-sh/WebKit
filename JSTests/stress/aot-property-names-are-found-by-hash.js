//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=1", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=1", "--useAOTInlineGuessedPlacesEverywhere=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=0")

function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
function atLeast(actual, expected, what) {
    if (!(actual >= expected))
        throw new Error(what + ": " + actual + ", fewer than " + expected);
}
function join(a, b) { return a + b; }
function nameOf(k, j) { return join(join("n", k), join("x", j)); }

function make0(v) { return { n0x0: v + 0, n0x1: v + 1, n0x2: v + 2, n0x3: v + 3, n0x4: v + 4, n0x5: v + 5, n0x6: v + 6, n0x7: v + 7, n0x8: v + 8, n0x9: v + 9, n0x10: v + 10, n0x11: v + 11, n0x12: v + 12, n0x13: v + 13, n0x14: v + 14, n0x15: v + 15, n0x16: v + 16, n0x17: v + 17, n0x18: v + 18, n0x19: v + 19 }; }
function make1(v) { return { n1x0: v + 0, n1x1: v + 1, n1x2: v + 2, n1x3: v + 3, n1x4: v + 4, n1x5: v + 5, n1x6: v + 6, n1x7: v + 7, n1x8: v + 8, n1x9: v + 9, n1x10: v + 10, n1x11: v + 11, n1x12: v + 12, n1x13: v + 13, n1x14: v + 14, n1x15: v + 15, n1x16: v + 16, n1x17: v + 17, n1x18: v + 18, n1x19: v + 19 }; }
function make2(v) { return { n2x0: v + 0, n2x1: v + 1, n2x2: v + 2, n2x3: v + 3, n2x4: v + 4, n2x5: v + 5, n2x6: v + 6, n2x7: v + 7, n2x8: v + 8, n2x9: v + 9, n2x10: v + 10, n2x11: v + 11, n2x12: v + 12, n2x13: v + 13, n2x14: v + 14, n2x15: v + 15, n2x16: v + 16, n2x17: v + 17, n2x18: v + 18, n2x19: v + 19 }; }
function make3(v) { return { n3x0: v + 0, n3x1: v + 1, n3x2: v + 2, n3x3: v + 3, n3x4: v + 4, n3x5: v + 5, n3x6: v + 6, n3x7: v + 7, n3x8: v + 8, n3x9: v + 9, n3x10: v + 10, n3x11: v + 11, n3x12: v + 12, n3x13: v + 13, n3x14: v + 14, n3x15: v + 15, n3x16: v + 16, n3x17: v + 17, n3x18: v + 18, n3x19: v + 19 }; }
function make4(v) { return { n4x0: v + 0, n4x1: v + 1, n4x2: v + 2, n4x3: v + 3, n4x4: v + 4, n4x5: v + 5, n4x6: v + 6, n4x7: v + 7, n4x8: v + 8, n4x9: v + 9, n4x10: v + 10, n4x11: v + 11, n4x12: v + 12, n4x13: v + 13, n4x14: v + 14, n4x15: v + 15, n4x16: v + 16, n4x17: v + 17, n4x18: v + 18, n4x19: v + 19 }; }
function make5(v) { return { n5x0: v + 0, n5x1: v + 1, n5x2: v + 2, n5x3: v + 3, n5x4: v + 4, n5x5: v + 5, n5x6: v + 6, n5x7: v + 7, n5x8: v + 8, n5x9: v + 9, n5x10: v + 10, n5x11: v + 11, n5x12: v + 12, n5x13: v + 13, n5x14: v + 14, n5x15: v + 15, n5x16: v + 16, n5x17: v + 17, n5x18: v + 18, n5x19: v + 19 }; }
function make6(v) { return { n6x0: v + 0, n6x1: v + 1, n6x2: v + 2, n6x3: v + 3, n6x4: v + 4, n6x5: v + 5, n6x6: v + 6, n6x7: v + 7, n6x8: v + 8, n6x9: v + 9, n6x10: v + 10, n6x11: v + 11, n6x12: v + 12, n6x13: v + 13, n6x14: v + 14, n6x15: v + 15, n6x16: v + 16, n6x17: v + 17, n6x18: v + 18, n6x19: v + 19 }; }
function make7(v) { return { n7x0: v + 0, n7x1: v + 1, n7x2: v + 2, n7x3: v + 3, n7x4: v + 4, n7x5: v + 5, n7x6: v + 6, n7x7: v + 7, n7x8: v + 8, n7x9: v + 9, n7x10: v + 10, n7x11: v + 11, n7x12: v + 12, n7x13: v + 13, n7x14: v + 14, n7x15: v + 15, n7x16: v + 16, n7x17: v + 17, n7x18: v + 18, n7x19: v + 19 }; }
function make8(v) { return { n8x0: v + 0, n8x1: v + 1, n8x2: v + 2, n8x3: v + 3, n8x4: v + 4, n8x5: v + 5, n8x6: v + 6, n8x7: v + 7, n8x8: v + 8, n8x9: v + 9, n8x10: v + 10, n8x11: v + 11, n8x12: v + 12, n8x13: v + 13, n8x14: v + 14, n8x15: v + 15, n8x16: v + 16, n8x17: v + 17, n8x18: v + 18, n8x19: v + 19 }; }
function make9(v) { return { n9x0: v + 0, n9x1: v + 1, n9x2: v + 2, n9x3: v + 3, n9x4: v + 4, n9x5: v + 5, n9x6: v + 6, n9x7: v + 7, n9x8: v + 8, n9x9: v + 9, n9x10: v + 10, n9x11: v + 11, n9x12: v + 12, n9x13: v + 13, n9x14: v + 14, n9x15: v + 15, n9x16: v + 16, n9x17: v + 17, n9x18: v + 18, n9x19: v + 19 }; }
function make10(v) { return { n10x0: v + 0, n10x1: v + 1, n10x2: v + 2, n10x3: v + 3, n10x4: v + 4, n10x5: v + 5, n10x6: v + 6, n10x7: v + 7, n10x8: v + 8, n10x9: v + 9, n10x10: v + 10, n10x11: v + 11, n10x12: v + 12, n10x13: v + 13, n10x14: v + 14, n10x15: v + 15, n10x16: v + 16, n10x17: v + 17, n10x18: v + 18, n10x19: v + 19 }; }
function make11(v) { return { n11x0: v + 0, n11x1: v + 1, n11x2: v + 2, n11x3: v + 3, n11x4: v + 4, n11x5: v + 5, n11x6: v + 6, n11x7: v + 7, n11x8: v + 8, n11x9: v + 9, n11x10: v + 10, n11x11: v + 11, n11x12: v + 12, n11x13: v + 13, n11x14: v + 14, n11x15: v + 15, n11x16: v + 16, n11x17: v + 17, n11x18: v + 18, n11x19: v + 19 }; }
function make12(v) { return { n12x0: v + 0, n12x1: v + 1, n12x2: v + 2, n12x3: v + 3, n12x4: v + 4, n12x5: v + 5, n12x6: v + 6, n12x7: v + 7, n12x8: v + 8, n12x9: v + 9, n12x10: v + 10, n12x11: v + 11, n12x12: v + 12, n12x13: v + 13, n12x14: v + 14, n12x15: v + 15, n12x16: v + 16, n12x17: v + 17, n12x18: v + 18, n12x19: v + 19 }; }
function make13(v) { return { n13x0: v + 0, n13x1: v + 1, n13x2: v + 2, n13x3: v + 3, n13x4: v + 4, n13x5: v + 5, n13x6: v + 6, n13x7: v + 7, n13x8: v + 8, n13x9: v + 9, n13x10: v + 10, n13x11: v + 11, n13x12: v + 12, n13x13: v + 13, n13x14: v + 14, n13x15: v + 15, n13x16: v + 16, n13x17: v + 17, n13x18: v + 18, n13x19: v + 19 }; }
function make14(v) { return { n14x0: v + 0, n14x1: v + 1, n14x2: v + 2, n14x3: v + 3, n14x4: v + 4, n14x5: v + 5, n14x6: v + 6, n14x7: v + 7, n14x8: v + 8, n14x9: v + 9, n14x10: v + 10, n14x11: v + 11, n14x12: v + 12, n14x13: v + 13, n14x14: v + 14, n14x15: v + 15, n14x16: v + 16, n14x17: v + 17, n14x18: v + 18, n14x19: v + 19 }; }
function make15(v) { return { n15x0: v + 0, n15x1: v + 1, n15x2: v + 2, n15x3: v + 3, n15x4: v + 4, n15x5: v + 5, n15x6: v + 6, n15x7: v + 7, n15x8: v + 8, n15x9: v + 9, n15x10: v + 10, n15x11: v + 11, n15x12: v + 12, n15x13: v + 13, n15x14: v + 14, n15x15: v + 15, n15x16: v + 16, n15x17: v + 17, n15x18: v + 18, n15x19: v + 19 }; }
const makers = [make0, make1, make2, make3, make4, make5, make6, make7, make8, make9, make10, make11, make12, make13, make14, make15];
function read0(o) { return o.n0x3; }
function read1(o) { return o.n1x3; }
function read2(o) { return o.n2x3; }
function read3(o) { return o.n3x3; }
function read4(o) { return o.n4x3; }
function read5(o) { return o.n5x3; }
function read6(o) { return o.n6x3; }
function read7(o) { return o.n7x3; }
function read8(o) { return o.n8x3; }
function read9(o) { return o.n9x3; }
function read10(o) { return o.n10x3; }
function read11(o) { return o.n11x3; }
function read12(o) { return o.n12x3; }
function read13(o) { return o.n13x3; }
function read14(o) { return o.n14x3; }
function read15(o) { return o.n15x3; }
const readers = [read0, read1, read2, read3, read4, read5, read6, read7, read8, read9, read10, read11, read12, read13, read14, read15];
const numberOfLiterals = 16;
const namesInLiteral = 20;
const namesInGroup = 5;

const counts = typeof aotOperationCount === "function" && !!jscOptions().useAOTOperationCounters && !!jscOptions().useAOTGuessedPlaces && isAOTCompiled(make0);
const countsHits = counts && !!jscOptions().useAOTDataStubs;
function count(what) { return counts ? aotOperationCount(what) || 0 : 0; }
function known() { return count("propertyNameIDIfKnown:known"); }
function unknown() { return count("propertyNameIDIfKnown:unknown"); }
function hits() { return count("operationAOTCountGuessedPlace:hit"); }

const parsed = [];
for (let k = 0; k < numberOfLiterals; ++k) {
    parsed.push([]);
    for (let first = 0; first < namesInLiteral; first += namesInGroup) {
        let text = "{";
        for (let j = first; j < first + namesInGroup; ++j)
            text += join(j > first ? ',"' : '"', nameOf(k, j)) + '":' + (100 * k + j);
        const knownBefore = known();
        const unknownBefore = unknown();
        parsed[k].push(JSON.parse(text + "}"));
        if (counts)
            atLeast(known() - knownBefore, namesInGroup - 1, "names of the program that it has not used yet are known, literal " + k + " from " + first);
        check(unknown() - unknownBefore, 0, "names of the program that are not known, literal " + k + " from " + first);
    }
}
for (let k = 0; k < numberOfLiterals; ++k) {
    const hitsBefore = hits();
    check(readers[k](parsed[k][0]), 100 * k + 3, "a parsed object with the name in the slot of the literal, " + k);
    if (countsHits)
        check(hits() - hitsBefore, 1, "the number recorded for a name that came from elsewhere is the number in the check, " + k);
    for (let group = 1; group < parsed[k].length; ++group)
        check(readers[k](parsed[k][group]), undefined, "a parsed object with other names of the literal, " + k);
    for (let other = 0; other < numberOfLiterals; ++other) {
        if (other !== k)
            check(readers[other](parsed[k][0]), undefined, "a parsed object with the names of literal " + k + " read by " + other);
    }
    for (let group = 0; group < parsed[k].length; ++group) {
        for (let j = 0; j < namesInGroup; ++j)
            check(parsed[k][group][nameOf(k, group * namesInGroup + j)], 100 * k + group * namesInGroup + j, nameOf(k, group * namesInGroup + j));
    }
}

for (let round = 0; round < 3; ++round) {
    for (let k = 0; k < numberOfLiterals; ++k) {
        const unknownBefore = unknown();
        const hitsBefore = hits();
        const made = makers[k](round);
        check(unknown() - unknownBefore, 0, "names of a literal that are not known, " + k);
        check(readers[k](made), round + 3, "a literal, " + k);
        if (countsHits)
            check(hits() - hitsBefore, 1, "the read of a literal hits, " + k);
        for (let j = 0; j < namesInLiteral; ++j)
            check(made[nameOf(k, j)], round + j, nameOf(k, j) + " of the literal");
        check(readers[(k + 1) % numberOfLiterals](made), undefined, "a literal read by the reader of the next, " + k);
    }
}

for (let k = 0; k < numberOfLiterals; ++k) {
    const knownBefore = known();
    const unknownBefore = unknown();
    const backwards = {};
    for (let j = namesInGroup - 1; j >= 0; --j)
        backwards[nameOf(k, j)] = j;
    if (counts)
        atLeast(known() - knownBefore, namesInGroup - 1, "names seen before are known, " + k);
    check(unknown() - unknownBefore, 0, "names seen before that are not known, " + k);
    check(readers[k](backwards), 3, "the name in another slot, " + k);
}

for (let k = 0; k < numberOfLiterals; ++k) {
    for (let j = 0; j < namesInLiteral; ++j) {
        for (const near of [join(nameOf(k, j), "_"), join("_", nameOf(k, j)), nameOf(k, j).toUpperCase(), nameOf(k, j + namesInLiteral), join(nameOf(k, j), "\u0101")]) {
            const knownBefore = known();
            const unknownBefore = unknown();
            const object = { [near]: j };
            check(known() - knownBefore, 0, "a name that only resembles one of the program is known: " + near);
            if (counts)
                check(unknown() - unknownBefore, 1, "a name that only resembles one of the program: " + near);
            check(object[near], j, near);
            check(readers[k](object), undefined, "n" + k + "x3 of an object with " + near);
        }
    }
}

const described = Symbol(nameOf(0, 3));
const knownBeforeSymbol = known();
const withSymbol = { [described]: 1 };
check(known() - knownBeforeSymbol, 0, "a symbol described as a name of the program is known");
check(read0(withSymbol), undefined, "an object with a symbol described as the name");
check(withSymbol[described], 1, "the symbol");
