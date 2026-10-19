import * as root from "./adopted-root.js";
import { value } from "./adopted-dependency.js";
export let awaited = "pending";
export function rootOf() { return root; }
const throwsAfterTheAwait = globalThis.moduleLoadersReleasedCodeThrowsAfterTheAwait;
await globalThis.moduleLoadersReleasedCodeNextGate;
++globalThis.moduleLoadersReleasedCodeAdoptedAwaitsResumed;
if (throwsAfterTheAwait)
    throw new Error("thrown after the await");
awaited = "awaited:" + value;
++globalThis.moduleLoadersReleasedCodeAdoptedAwaitsFinished;
