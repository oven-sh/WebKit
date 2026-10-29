import * as dep from "./dep.js";
import * as barrel from "./barrel.js";
export function readId() { return dep.id; }
export function readTag() { return dep.tag; }
export function readDefault() { return dep.default; }
export function readDeclared() { return dep.declared; }
export function readMissing() { return dep.missing; }
export function readByVal(key) { return dep[key]; }
export function readThroughBarrel() { return barrel.id; }
export function readRenamed() { return barrel.renamed; }
export function readWhole() { return barrel.whole.id; }
export function bump() { return dep.bump(); }
export function sum(n)
{
    let result = 0;
    for (let i = 0; i < n; ++i)
        result += dep.id + barrel.renamed;
    return result;
}
