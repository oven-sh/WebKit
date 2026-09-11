//@ requireOptions("--useLazyModuleFunctionDeclarations=1")
import * as lib from "./lazy-function-declarations/lib.js";
import namedDefault, { plain, renamed, renamedTwice, reassigned, reassign, reassignedAfterRead, reassignAfterRead, callsPrivate, hoisted, bodyRan, withProps, thrower, NotLazy, constant, laterLet, setLaterLet, usesArguments } from "./lazy-function-declarations/lib.js";
import * as reexport from "./lazy-function-declarations/reexport.js";
import { plainAgain, renamedAgain } from "./lazy-function-declarations/reexport.js";
import anonymousDefault from "./lazy-function-declarations/anonymous-default.js";
import * as anonymousDefaultNamespace from "./lazy-function-declarations/anonymous-default.js";
import { results as cycleAResults, bResults as cycleBResults } from "./lazy-function-declarations/cycle-a.js";
import * as tla from "./lazy-function-declarations/tla.js";
import { results as tdzResults } from "./lazy-function-declarations/tdz.js";
import { shouldBe, shouldThrow } from "./resources/assert.js";

const lazy = !!$vm.uninstantiatedFunctionDeclarations && $vm.uninstantiatedFunctionDeclarations(lib) !== 0;

// Cycles: a function is callable before the body of its module ran.
shouldBe(cycleBResults.join(), "function,a!");
shouldBe(cycleAResults.join(), "function,b?,a!");
shouldBe(tdzResults.join("|"), "ReferenceError: Cannot access 'letBinding' before initialization.|ReferenceError: Cannot access 'ClassBinding' before initialization.|function");

// Identity: one function object per binding, however it is reached first.
shouldBe(typeof plain, "function");
shouldBe(plain, lib.plain);
shouldBe(plain, plainAgain);
shouldBe(plain, reexport.plain);
shouldBe(reexport.plainAgain, lib.plain);
shouldBe(plain(1, 2), 3);
shouldBe(lib.renamed, lib.renamedTwice);
shouldBe(renamed, renamedTwice);
shouldBe(renamed, renamedAgain);
shouldBe(renamed(), "renamed");
shouldBe(renamed.name, "renamedLocally");
shouldBe(namedDefault, lib.default);
shouldBe(namedDefault(), "namedDefault");
shouldBe(namedDefault.name, "namedDefault");
shouldBe(anonymousDefault, anonymousDefaultNamespace.default);
shouldBe(anonymousDefault(), "anonymous");
shouldBe(anonymousDefault.name, "default");
shouldBe(reexport.ownFunction(), "own");

// First read through the namespace object, then through an import in another module.
shouldBe(lib.hoisted(), "hoisted before body");
shouldBe(hoisted, lib.hoisted);
shouldBe(bodyRan, true);

// Kinds.
shouldBe(Object.getPrototypeOf(lib.asyncFn), Object.getPrototypeOf(async function () { }));
shouldBe(Object.getPrototypeOf(lib.generatorFn), Object.getPrototypeOf(function* () { }));
shouldBe(Object.getPrototypeOf(lib.asyncGeneratorFn), Object.getPrototypeOf(async function* () { }));
shouldBe([...lib.generatorFn()].join(), "1,2");
shouldBe(typeof lib.generatorFn.prototype, "object");
shouldBe(lib.asyncFn.hasOwnProperty("prototype"), false);
shouldBe(new plain(1, 2) instanceof plain, true);
shouldBe(plain.length, 2);
shouldBe(plain.toString(), "function plain(a, b) { return a + b; }");
shouldBe(usesArguments(1, 2, 3), 3);

// Private functions: read from other functions, closures and eval.
shouldBe(callsPrivate(20), 41);
shouldBe(lib.typeofPrivate(), "function");
shouldBe(typeof lib.readPrivateTwice(), "function");
shouldBe(lib.readPrivateTwice(), lib.readPrivateTwice());
shouldBe(lib.makeClosure()()(), "closure");
shouldBe(lib.makeClosure()(), lib.makeClosure()());
shouldBe(lib.viaEval("privateNeverRead")(), 0);
shouldBe(lib.viaEval("privateNeverRead"), lib.viaEval("(() => privateNeverRead)()"));
shouldBe(lib.viaEval("plain"), plain);
shouldBe(lib.viaEval("typeof privateHelper"), "function");
shouldThrow(() => lib.viaEval("doesNotExist"), "ReferenceError: doesNotExist is not defined");

// Assignment before anything read the binding: the declaration is never instantiated.
reassign();
shouldBe(reassigned(), "replaced");
shouldBe(lib.reassigned.name, "replacement");
// Assignment after.
const before = reassignedAfterRead;
shouldBe(before(), "original2");
reassignAfterRead();
shouldBe(reassignedAfterRead, 42);
shouldBe(lib.reassignedAfterRead, 42);
shouldBe(before(), "original2");

// Properties survive: it is the same object.
withProps.tag = 1;
shouldBe(lib.withProps.tag, 1);

// Namespace object.
shouldBe(Object.keys(lib).join(), "NotLazy,asyncFn,asyncGeneratorFn,bodyRan,callsPrivate,constant,default,generatorFn,hoisted,laterLet,makeClosure,namedDefault,neverRead,plain,readPrivateTwice,reassign,reassignAfterRead,reassigned,reassignedAfterRead,renamed,renamedTwice,setLaterLet,thrower,typeofPrivate,usesArguments,viaEval,withProps".replace("namedDefault,", ""));
{
    let descriptor = Object.getOwnPropertyDescriptor(lib, "neverRead");
    shouldBe(typeof descriptor.value, "function");
    shouldBe(descriptor.writable, true);
    shouldBe(descriptor.enumerable, true);
    shouldBe(descriptor.configurable, false);
    shouldBe(descriptor.value, lib.neverRead);
    shouldBe(Reflect.has(lib, "thrower"), true);
    shouldBe("thrower" in lib, true);
    shouldThrow(() => { "use strict"; lib.plain = 1; }, "TypeError: Attempted to assign to readonly property.");
}
shouldThrow(thrower, "Error: from thrower");
try {
    thrower();
} catch (e) {
    shouldBe(e.stack.includes("thrower@"), true);
}

// Other kinds of bindings are untouched.
shouldBe(NotLazy.tag, "class");
shouldBe(constant, 7);
shouldBe(laterLet, undefined);
setLaterLet(5);
shouldBe(laterLet, 5);

// Top-level await.
shouldBe(tla.awaited, 42);
shouldBe(tla.afterAwait(), 43);

// Tier up while some functions are still uninstantiated, and instantiate them from optimized code.
function hot(i) {
    if (i === 199990)
        return lib.neverRead() + reexport.neverRead();
    return plain(i, 1) + callsPrivate(i);
}
noInline(hot);
let sum = 0;
for (let i = 0; i < 200000; ++i) {
    let result = hot(i);
    if (typeof result === "number")
        sum += result;
    else
        shouldBe(result, "neverReadneverRead");
}
shouldBe(sum, 3 * (199999 * 200000 / 2) + 2 * 200000 - (3 * 199990 + 2));

if (lazy)
    shouldBe($vm.uninstantiatedFunctionDeclarations(lib) < 10, true);
