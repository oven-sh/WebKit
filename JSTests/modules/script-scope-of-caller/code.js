// Loaded by several loaders. Every function asks the engine whose script is calling.
export const atTopLevel = $vm.ownerOfCaller();
export function plain() { const owner = $vm.ownerOfCaller(); return owner; }
export const arrow = () => { const owner = $vm.ownerOfCaller(); return owner; };
export function nested() { let a = 1; return (() => { let b = a; return (() => { const owner = $vm.ownerOfCaller(); return [owner, a + b][0]; })(); })(); }
export class Thing {
    get owner() { const owner = $vm.ownerOfCaller(); return owner; }
    method() { const owner = $vm.ownerOfCaller(); return owner; }
    static make() { const owner = $vm.ownerOfCaller(); return owner; }
}
export function* generator() { yield $vm.ownerOfCaller(); yield $vm.ownerOfCaller(); }
export async function afterAwait() { await null; const owner = $vm.ownerOfCaller(); return owner; }
export function thenCallback() { return Promise.resolve().then(() => { const owner = $vm.ownerOfCaller(); return owner; }); }
export function throughBuiltin() { return [0].map(() => { const owner = $vm.ownerOfCaller(); return owner; })[0]; }
// The builtin calls the host function: the script that called the builtin is calling.
export function hostFunctionCalledByBuiltin() { return [0].map($vm.ownerOfCaller)[0]; }
export function throughHostFunction() { return JSON.parse("[0]", function () { const owner = $vm.ownerOfCaller(); return owner; }); }
export function calls(f) { const owner = f(); return owner; }
// Small enough to be inlined into its caller, which may be another loader's script.
export function small() { const owner = $vm.ownerOfCaller(); return owner; }
export function bound() { return plain.bind(null); }
export function directEval() { const owner = eval("$vm.ownerOfCaller()"); return owner; }
