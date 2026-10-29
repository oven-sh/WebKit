export let id = globalThis.moduleLoadersNamespaceInlineCachesIds = (globalThis.moduleLoadersNamespaceInlineCachesIds | 0) + 1;
export const tag = "dep" + id;
export default "default" + id;
export function bump() { return ++id; }
export function declared() { return "declared" + id; }
