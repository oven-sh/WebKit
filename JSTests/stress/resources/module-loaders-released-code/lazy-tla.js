let calls = 0;
export function used(a) { ++calls; return a; }
export function never(a) { return ++calls; }
export function never2(a) { return inner(); }
function inner() { return ++calls; }
const gate = globalThis.moduleLoadersReleasedCodeNextGate;
++globalThis.moduleLoadersReleasedCodeStarted;
await gate;
export const marker = used(7);
