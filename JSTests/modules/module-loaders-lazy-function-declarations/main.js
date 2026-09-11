import { d0, d1, d2, d3, d4, d5, d6, d7, depCallCount } from "./dep.js";

let calls = 0;
function f0(x) { ++calls; return x * 10 + 0; }
function f1(x) { ++calls; return x * 10 + 1; }
function f2(x) { ++calls; return x * 10 + 2; }
function f3(x) { ++calls; return x * 10 + 3; }
function f4(x) { ++calls; return x * 10 + 4; }
function f5(x) { ++calls; return x * 10 + 5; }
function f6(x) { ++calls; return x * 10 + 6; }
function f7(x) { ++calls; return x * 10 + 7; }
function f8(x) { ++calls; return x * 10 + 8; }
function f9(x) { ++calls; return x * 10 + 9; }
function f10(x) { ++calls; return x * 10 + 10; }
function f11(x) { ++calls; return x * 10 + 11; }
function f12(x) { ++calls; return x * 10 + 12; }
function f13(x) { ++calls; return x * 10 + 13; }
function f14(x) { ++calls; return x * 10 + 14; }
function f15(x) { ++calls; return x * 10 + 15; }

// Reads one of this module's own function declarations, or an imported one, through code that all loaders share.
export function driver(i, x)
{
    switch (i) {
    case 0: return f0(x);
    case 1: return f1(x);
    case 2: return f2(x);
    case 3: return f3(x);
    case 4: return f4(x);
    case 5: return f5(x);
    case 6: return f6(x);
    case 7: return f7(x);
    case 8: return f8(x);
    case 9: return f9(x);
    case 10: return f10(x);
    case 11: return f11(x);
    case 12: return f12(x);
    case 13: return f13(x);
    case 14: return f14(x);
    case 15: return f15(x);
    case 16: return d0(x);
    case 17: return d1(x);
    case 18: return d2(x);
    case 19: return d3(x);
    case 20: return d4(x);
    case 21: return d5(x);
    case 22: return d6(x);
    case 23: return d7(x);
    }
    return -1;
}
export function callCounts() { return [calls, depCallCount()]; }
export function neverReadByTheTest() { return calls; }
export { f0, f15 };
