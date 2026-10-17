import { ready, countBefore, countAfter } from "./gate-dep.js";
import { count, increment } from "./values.js";
import { live, bump } from "./second.js";
export const seenAtStart = [ready, countBefore, countAfter, count, live].join();
export function read() { return [ready, count, live].join(); }
export function loop(n) {
    let result;
    for (let i = 0; i < n; ++i)
        result = read();
    return result;
}
export { increment, bump };
