import { value as left } from "./left.js";
import { value as right } from "./right.js";
(globalThis.bodies ??= new Map).set(import.meta.filename, (globalThis.bodies.get(import.meta.filename) ?? 0) + 1);
export const value = left + right;
