//@ requireOptions("--useLazyModuleFunctionDeclarations=1")
import * as lib from "./lazy-function-declarations-dfg/lib.js";
import { callPrivate, readWatched, overwriteWatched, overwriteNeverInstantiated, readNeverInstantiated, e0, e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27, e28, e29, e30, e31, e32, e33, e34, e35, e36, e37, e38, e39, e40, e41, e42, e43, e44, e45, e46, e47, e48, e49, e50, e51, e52, e53, e54, e55, e56, e57, e58, e59 } from "./lazy-function-declarations-dfg/lib.js";
import { shouldBe } from "./resources/assert.js";

const N = 60;

// Optimized code that reaches functions nobody has read yet, one after the other.
function callExported(i, x) {
    switch (i) {
    case 0: return e0(x);
    case 1: return e1(x);
    case 2: return e2(x);
    case 3: return e3(x);
    case 4: return e4(x);
    case 5: return e5(x);
    case 6: return e6(x);
    case 7: return e7(x);
    case 8: return e8(x);
    case 9: return e9(x);
    case 10: return e10(x);
    case 11: return e11(x);
    case 12: return e12(x);
    case 13: return e13(x);
    case 14: return e14(x);
    case 15: return e15(x);
    case 16: return e16(x);
    case 17: return e17(x);
    case 18: return e18(x);
    case 19: return e19(x);
    case 20: return e20(x);
    case 21: return e21(x);
    case 22: return e22(x);
    case 23: return e23(x);
    case 24: return e24(x);
    case 25: return e25(x);
    case 26: return e26(x);
    case 27: return e27(x);
    case 28: return e28(x);
    case 29: return e29(x);
    case 30: return e30(x);
    case 31: return e31(x);
    case 32: return e32(x);
    case 33: return e33(x);
    case 34: return e34(x);
    case 35: return e35(x);
    case 36: return e36(x);
    case 37: return e37(x);
    case 38: return e38(x);
    case 39: return e39(x);
    case 40: return e40(x);
    case 41: return e41(x);
    case 42: return e42(x);
    case 43: return e43(x);
    case 44: return e44(x);
    case 45: return e45(x);
    case 46: return e46(x);
    case 47: return e47(x);
    case 48: return e48(x);
    case 49: return e49(x);
    case 50: return e50(x);
    case 51: return e51(x);
    case 52: return e52(x);
    case 53: return e53(x);
    case 54: return e54(x);
    case 55: return e55(x);
    case 56: return e56(x);
    case 57: return e57(x);
    case 58: return e58(x);
    case 59: return e59(x);
    }
    return 0;
}
noInline(callExported);
noInline(callPrivate);

for (let i = 0; i < 100000; ++i) {
    shouldBe(callExported(0, i), i);
    shouldBe(callPrivate(0, i), i * 2);
}
for (let round = 0; round < 3; ++round) {
    for (let k = 1; k < N; ++k) {
        for (let i = 0; i < 200; ++i) {
            shouldBe(callExported(k, i), i + k);
            shouldBe(callPrivate(k, i), i * 2 + k);
        }
    }
}
for (let k = 0; k < N; ++k)
    shouldBe(lib["e" + k], eval("e" + k));

// A binding with a watchpoint set (the module assigns to it): reads in optimized code follow every change.
function readIt() { return readWatched(); }
noInline(readIt);
let first = readIt();
shouldBe(first(), "watched");
for (let i = 0; i < 100000; ++i)
    shouldBe(readIt(), first);
overwriteWatched(1);
for (let i = 0; i < 100000; ++i)
    shouldBe(readIt(), 1);
shouldBe(lib.watched, 1);
overwriteWatched(2);
shouldBe(readIt(), 2);
shouldBe(lib.watched, 2);

// Overwritten before anyone read it.
function readOther() { return readNeverInstantiated(); }
noInline(readOther);
overwriteNeverInstantiated("first store");
for (let i = 0; i < 100000; ++i)
    shouldBe(readOther(), "first store");
overwriteNeverInstantiated("second store");
for (let i = 0; i < 100000; ++i)
    shouldBe(readOther(), "second store");
shouldBe(lib.neverInstantiatedButOverwritten, "second store");
