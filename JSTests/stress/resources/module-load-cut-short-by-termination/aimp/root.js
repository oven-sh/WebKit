(globalThis.bodies ??= new Map).set(import.meta.filename, (globalThis.bodies.get(import.meta.filename) ?? 0) + 1);
const dep = await import("./dep.js");
export const value = dep.value + 1;
