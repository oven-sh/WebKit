import { tag } from "./dep.js"

export function describe(n)
{
    let result;
    for (let i = 0; i < n; ++i)
        result = [who, tag, typeof marker, globalThis === realGlobal].join();
    return result;
}

export function evaluated() { return new Function("return typeof who")(); }
export const lazy = () => import("./dep.js");
