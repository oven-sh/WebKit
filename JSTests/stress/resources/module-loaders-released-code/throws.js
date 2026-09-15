export function late() { return "late"; }
++globalThis.moduleLoadersReleasedCodeThrowsStarted;
if (globalThis.moduleLoadersReleasedCodeShouldThrow)
    throw new Error("the body throws");
export const done = true;
