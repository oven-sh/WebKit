import { count, increment, set, constant, object } from "./values.js";
import * as values from "./values.js";
export const arrow = () => count;
export function* generator() { yield count; increment(); yield count; }
export async function asynchronous() { await null; return count; }
export async function* asyncGenerator() { yield count; }
export function nested() { return (() => (() => count)())(); }
export function viaEval() { return eval("count"); }
export function viaIndirectEval() { return (0, eval)("typeof count"); }
export function viaFunctionConstructor() { return new Function("return typeof count")(); }
export function defaultParameter(value = count) { return value; }
export function withTypeof() { return [typeof count, typeof constant, typeof object, typeof values].join(); }
export class WithStatic { static captured = count; static read() { return count; } get accessor() { return count; } }
export const objectLiteral = { get accessor() { return count; }, method() { return count; }, [`computed${constant}`]: count };
export function destructure() { const { count: local } = values; return [local, count].join(); }
export function inTryFinally() { try { return count; } finally { increment(); } }
export function labelled() { outer: for (;;) { for (;;) { if (count >= 0) break outer; } } return count; }
export function templated() { return `${count}:${constant}`; }
export function optional() { return [object?.tag, values?.object?.tag].join(); }
export { increment, set };
