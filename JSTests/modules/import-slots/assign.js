import { count, object, declared } from "./values.js";
import * as values from "./values.js";
import defaultFunction from "./values.js";
export function assign() { count = 1; }
export function increment() { count++; }
export function compound() { count += 1; }
export function logical() { count ??= 1; }
export function destructuring() { [count] = [1]; }
export function objectDestructuring() { ({ count } = { count: 1 }); }
export function forIn() { for (count in { a: 1 }); }
export function forOf() { for (count of [1]); }
export function assignFunction() { declared = null; }
export function assignDefault() { defaultFunction = null; }
export function assignNamespace() { values = null; }
export function assignNamespaceProperty() { values.count = 1; }
export function mutateObject() { object.mutated = true; return object.mutated; }
export function read() { return count; }
