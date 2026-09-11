import { x, shape } from "./dep.js"

export function read(n)
{
    let result;
    for (let i = 0; i < n; ++i)
        result = x;
    return [result, shape];
}
