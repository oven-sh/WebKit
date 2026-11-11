import { awaited } from "./adopted-awaits.js";
import { sibling } from "./adopted-sibling-throws.js";
export function describe() { return awaited + ":" + sibling; }
++globalThis.moduleLoadersReleasedCodeAdoptedRootStarted;
