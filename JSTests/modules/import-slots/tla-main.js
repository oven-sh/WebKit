import { ready, afterAwait } from "./tla-dep.js";
import { count, increment } from "./values.js";
export const seenAtStart = [ready, afterAwait];
increment();
await null;
export const seenAfterAwait = [ready, afterAwait, count];
export function read() { return [ready, count]; }
