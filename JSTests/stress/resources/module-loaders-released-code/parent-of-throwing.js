import { value } from "./throwing-dependency.js";
export function late() { return "late:" + value; }
++globalThis.moduleLoadersReleasedCodeParentStarted;
