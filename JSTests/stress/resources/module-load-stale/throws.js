globalThis.evaluations = (globalThis.evaluations ?? 0) + 1;
if (globalThis.throwFromModule)
    throw new Error("thrown by the module");
export const evaluation = globalThis.evaluations;
