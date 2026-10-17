export function plain(a, b) { return a + b; }
export async function asyncFn() { return "async"; }
export function* generatorFn() { yield 1; yield 2; }
export async function* asyncGeneratorFn() { yield "ag"; }
export default function namedDefault() { return "namedDefault"; }
function renamedLocally() { return "renamed"; }
export { renamedLocally as renamed, renamedLocally as renamedTwice };
export function reassigned() { return "original"; }
export function reassignedAfterRead() { return "original2"; }
export function neverRead() { return "neverRead"; }
export function callsPrivate(x) { return privateHelper(x) + 1; }
function privateHelper(x) { return x * 2; }
function privateNeverRead() { return 0; }
export function viaEval(name) { return eval(name); }
export function makeClosure() { return () => privateForClosure; }
function privateForClosure() { return "closure"; }
export function reassign() { reassigned = function replacement() { return "replaced"; }; }
export function reassignAfterRead() { reassignedAfterRead = 42; }
export function typeofPrivate() { return typeof privateTypeof; }
function privateTypeof() { }
export function readPrivateTwice() { return privateIdentity === privateIdentity && privateIdentity; }
function privateIdentity() { }
export function hoisted() { return "hoisted before body"; }
export let bodyRan = false;
export function withProps() { }
export function thrower() { throw new Error("from thrower"); }
export function usesArguments() { return arguments.length; }
export class NotLazy { static tag = "class"; }
export const constant = 7;
export let laterLet;
export function setLaterLet(v) { laterLet = v; }
bodyRan = true;
