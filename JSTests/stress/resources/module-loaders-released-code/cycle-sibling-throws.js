export const sibling = "sibling";
if (globalThis.moduleLoadersReleasedCodeShouldThrow)
    throw new Error("the sibling throws");
