import "./gate.js";
import { id } from "./dep.js";
import * as dependency from "./dep.js";
let calls = 0;
export function used(a) { ++calls; return a; }
export function count() { return ++calls; }
export function countThroughInner() { return inner(); }
function inner() { return ++calls; }
export function importedId() { return id; }
export function importedIdThroughNamespace() { return dependency.id; }
export const countFromExpression = () => ++calls;
export const importedIdFromExpression = () => id;
export const marker = used(7);
