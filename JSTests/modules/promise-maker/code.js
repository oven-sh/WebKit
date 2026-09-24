// Loaded by several loaders. Every function makes a promise in some way and hands it, and what rejects it, out.
const error = () => new Error("rejected");

export function withResolvers() { const made = Promise.withResolvers(); return made; }
export function byConstructor() { let reject; const promise = new Promise((_, r) => { reject = r; }); return { promise, reject }; }
export function constructedThatThrows() { const promise = new Promise(() => { throw error(); }); return promise; }
export function constructedResolvedWith(promise) { const made = new Promise(resolve => { resolve(promise); }); return made; }
// The executor is made one and two scopes down from the module's, and is not made where the promise is.
export function constructedOneScopeDown() { const captured = error(); const promise = new Promise((_, reject) => { reject(captured); }); return promise; }
export function constructedTwoScopesDown() {
    let count = 0;
    const make = () => { const captured = error(); count++; return new Promise((_, reject) => { reject(captured); }); };
    return make();
}
export const rejectingExecutor = (_, reject) => { reject(error()); };
export function constructedWithGiven() { const promise = new Promise(rejectingExecutor); return promise; }
export function constructedWithBound() { const promise = new Promise(rejectingExecutor.bind(null)); return promise; }
class Derived extends Promise { }
export function constructedByDerived() { const promise = new Derived((_, reject) => { reject(error()); }); return promise; }
export function constructedWith(executor) { const promise = new Promise(executor); return promise; }
export function pending() { const promise = new Promise(() => { }); return promise; }
export function fulfilled() { const promise = new Promise(resolve => { resolve(1); }); return promise; }
export function rejected() { const promise = Promise.reject(error()); return promise; }
export function asyncThatThrowsAfterAwait() { const promise = (async () => { await null; throw error(); })(); return promise; }
// The async function is this module's; what throws inside it, after an await, is `thrower`.
export function asyncCalling(thrower) { const promise = (async () => { await null; thrower(); })(); return promise; }
export function thrower() { throw error(); }
export function callsReject(reject) { reject(error()); }
// Promises derived from `promise` here: what rejects them is a job, with no script calling.
export function thenOf(promise) { const derived = promise.then(value => value); return derived; }
export function thenThatThrows(promise) { const derived = promise.then(() => { throw error(); }); return derived; }
export function thenReturningRejected(promise) { const derived = promise.then(() => Promise.reject(error())); return derived; }
export function finallyOf(promise) { const derived = promise.finally(() => { }); return derived; }
export function all(promise) { const derived = Promise.all([promise]); return derived; }
export function race(promise) { const derived = Promise.race([promise]); return derived; }
export function resolvedWith(promise) { const made = Promise.withResolvers(); made.resolve(promise); return made.promise; }
export function asyncAwaiting(promise) { const derived = (async () => { await promise; })(); return derived; }
export function asyncReturning(promise) { const derived = (async () => { await null; return promise; })(); return derived; }
export async function* generator() { await null; throw error(); }
export function generatorNext() { const promise = generator().next(); return promise; }
// A promise nothing has been done with, what rejects it, and something only its executor has.
export function constructedHolding() {
    let reject;
    const held = { held: true };
    const promise = new Promise((_, r) => { reject = r; held.executorRan = true; });
    return { promise, reject, held: new WeakRef(held) };
}
// The promise `finally` made, while it waits for what its handler returned, and something only the handler has.
export function finallyHolding(source) {
    const held = { held: true };
    const returned = Promise.withResolvers();
    const promise = source.finally(() => { held.handlerRan = true; return returned.promise; });
    return { promise, returned, held: new WeakRef(held) };
}
