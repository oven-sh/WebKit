export let stage = "started";
export function describe() { return stage + ":" + later(); }
function later() { return "later"; }
const gate = globalThis.moduleLoadersReleasedCodeNextGate;
++globalThis.moduleLoadersReleasedCodeStarted;
await gate;
stage = "done";
