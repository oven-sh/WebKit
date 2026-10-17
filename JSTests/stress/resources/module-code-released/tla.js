export let stage = "start";
export function later() { return stage; }
globalThis.moduleCodeReleasedStarted = true;
await globalThis.moduleCodeReleasedGate;
stage = "resumed";
const values = [];
for (let i = 0; i < 4; ++i)
    values.push(await Promise.resolve(i));
stage = "done:" + values.join("");
