import { count, increment, set, object } from "./values.js";
import * as values from "./values.js";
import { live, bump } from "./second.js";
import thirdDefault from "./third.js";
export function run(n) {
    for (let i = 0; i < n; ++i) {
        increment();
        bump();
    }
    return [count, live].join();
}
export function read() { return [count, values.count, live, object === values.object, thirdDefault.tag].join(); }
export const loadValues = () => import("./values.js");
export { values, set };
