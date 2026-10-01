import { count, increment, object } from "./counter.js"
import * as counter from "./counter.js"

export function run(n)
{
    for (let i = 0; i < n; ++i)
        increment();
    return count;
}

export function read()
{
    return [count, counter.count, object === counter.object];
}

export const loadLazy = () => import("./lazy.js");
export const loadCounter = () => import("./counter.js");
export { counter };
