globalThis.evaluationsOfDependency = (globalThis.evaluationsOfDependency ?? 0) + 1;
export const evaluations = globalThis.evaluationsOfDependency;
