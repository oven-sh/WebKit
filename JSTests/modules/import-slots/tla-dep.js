export let ready = "pending";
await Promise.resolve();
ready = "ready";
export const afterAwait = "after";
