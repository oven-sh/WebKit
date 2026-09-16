import { count, increment } from "./values.js";
export let ready = "pending";
export const countBefore = count;
globalThis.importSlotsGate.started++;
await globalThis.importSlotsGate.next();
increment();
export const countAfter = count;
ready = "ready";
