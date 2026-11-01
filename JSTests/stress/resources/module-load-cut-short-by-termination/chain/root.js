import { value as mid } from "./mid.js";
(globalThis.bodies ??= new Map).set(import.meta.filename, (globalThis.bodies.get(import.meta.filename) ?? 0) + 1);
export const value = mid + 1;
