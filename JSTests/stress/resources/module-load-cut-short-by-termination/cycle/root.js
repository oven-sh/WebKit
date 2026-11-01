import { one } from "./dep.js";
(globalThis.bodies ??= new Map).set(import.meta.filename, (globalThis.bodies.get(import.meta.filename) ?? 0) + 1);
export function forty() { return 40; }
export const value = forty() + one();
