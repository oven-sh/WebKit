// What a-module-is-a-module.mjs gives to Python.
export let count = 0;
export function bump() { return ++count; }
export default function hello(name) { return "hello " + name; }
export class Thing { constructor(x) { this.x = x; } }
export const _private = "not for import *";
export const whatThisIs = function () { return this === undefined ? "undefined" : typeof this; };
export * as again from "./a-module-of-javascript.mjs";
