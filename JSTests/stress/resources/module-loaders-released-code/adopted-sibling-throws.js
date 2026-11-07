export const sibling = "adopted sibling";
if (globalThis.moduleLoadersReleasedCodeShouldThrow)
    throw new Error("the adopted sibling throws");
