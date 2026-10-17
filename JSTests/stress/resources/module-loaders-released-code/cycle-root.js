import { awaited } from "./cycle-awaits.js";
import { sibling } from "./cycle-sibling-throws.js";
export function describe() { return awaited + ":" + sibling; }
++globalThis.moduleLoadersReleasedCodeCycleRootStarted;
