import { count, increment, constant } from "./values.js";
import { live, bump } from "./second.js";
import * as namespace from "./third.js";
// `constant`, `live` and `namespace` are only read on a path that is first taken after the
// function is hot.
export function sometimes(flag) {
    if (flag)
        return [constant, live, namespace.third].join();
    return count;
}
export function loop(n, flag) {
    let result;
    for (let i = 0; i < n; ++i)
        result = sometimes(flag);
    return result;
}
export { increment, bump };
