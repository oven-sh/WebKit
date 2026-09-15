export function beforeAwait() { return helper() + 1; }
function helper() { return 41; }
export let awaited = await Promise.resolve(beforeAwait());
export function afterAwait() { return awaited + 1; }
