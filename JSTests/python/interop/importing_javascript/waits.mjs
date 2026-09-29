export const x = await new Promise(resolve => { globalThis.release = resolve; });
