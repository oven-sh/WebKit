import "./missing.js";
(globalThis.bodies ??= new Map).set(import.meta.filename, (globalThis.bodies.get(import.meta.filename) ?? 0) + 1);
export const value = 1;
