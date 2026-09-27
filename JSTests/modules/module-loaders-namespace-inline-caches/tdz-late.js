// Runs before tdz-entry.js, whose readLate() reads this module's namespace object while `value` is uninitialized.
import { readLate } from "./tdz-entry.js";
export let early;
try {
    early = "read " + readLate();
} catch (error) {
    early = error instanceof ReferenceError ? "ReferenceError" : "threw " + error;
}
export let value = globalThis.moduleLoadersNamespaceInlineCachesIds;
