// A bare call, a tagged template or a delete through a mapped parameter must not reach the
// caller's arguments object.
//
// A sloppy function with a simple parameter list that mentions `arguments` and captures no
// parameter keeps its parameters in a DirectArguments object. The bytecode generator gave
// the register that holds that object to its emitters as the "scope" of such a parameter.
// A call passed it as |this|, the way it passes a real scope for the sake of `with`, and a
// delete used it as the base of del_by_id. A JS callee turns a scope back into undefined
// with op_to_this, and a native with JSValue::toThis. Neither changes an arguments object,
// so the callee got a live reference to the parameters of its caller.

function show(value) {
    if (value === globalThis)
        return "globalThis";
    if (typeof value === "string")
        return JSON.stringify(value);
    if (typeof value === "object" && value !== null || typeof value === "function")
        return Object.prototype.toString.call(value);
    return String(value);
}

// |what| is a description, or the function under test.
function shouldBe(actual, expected, what, detail = "") {
    if (actual !== expected)
        throw new Error((typeof what === "function" ? what.name : what) + detail + ": expected " + show(expected) + " but got " + show(actual));
}

function shouldThrowTypeError(func, what) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof TypeError))
        throw new Error(what + ": expected a TypeError but got " + show(error));
    return error;
}

// Every callee records the receiver it got, for the callers that cannot return it.
let receiver;
function strictCallee() { "use strict"; receiver = this; return this; }
function sloppyCallee() { receiver = this; return this; }
const proxyCallee = new Proxy(function () { }, {
    apply(target, thisArgument) { receiver = thisArgument; return thisArgument; }
});

// The receiver. Each of these callers returns what the callee returns.
function bareCall(callee) { arguments; return callee(); }
function parenthesizedCall(callee) { arguments; return (callee)(); }
function optionalCall(callee) { arguments; return callee?.(); }
function callWithArguments(callee) { arguments; return callee(1, 2, 3); }
function spreadCall(callee) { arguments; return callee(...[1, 2]); }
function taggedTemplate(callee) { arguments; return callee`x`; }
function taggedTemplateWithSubstitution(callee) { arguments; return callee`x${1}y`; }
function parameterNamedObject(Object) { arguments; return Object(); }
function parameterNamedArray(Array) { arguments; return Array(); }
function duplicateParameters(callee, callee) { arguments; return callee(); }
function argumentsByIndex(callee) { return arguments[0] === callee ? callee() : "arguments[0] is not the parameter"; }
function argumentsInArrowFunction(callee) { const inner = () => arguments; inner; return callee(); }
function argumentsOverwritten(callee) { arguments = null; return callee(); }
function nestedCaller(callee) { arguments; return (function (callee) { arguments; return callee(); })(callee); }
const holder = {
    method(callee) { arguments; return callee(); },
    set setter(callee) { arguments; receiver = "not called"; callee(); },
};
const fromFunctionConstructor = new Function("callee", "arguments; return callee();");

const returningCallers = [
    bareCall, parenthesizedCall, optionalCall, callWithArguments, spreadCall, taggedTemplate, taggedTemplateWithSubstitution,
    parameterNamedObject, parameterNamedArray, argumentsByIndex, argumentsInArrowFunction, argumentsOverwritten, nestedCaller,
    fromFunctionConstructor,
    function duplicateParameterNames(callee) { return duplicateParameters(null, callee); },
    function objectLiteralMethod(callee) { return holder.method(callee); },
];

// Each of these callers ignores what the callee returns, so it returns the recorded receiver.
function statementCall(callee) { arguments; receiver = "not called"; callee(); return receiver; }
async function asyncWithoutAwait(callee) { arguments; receiver = "not called"; callee(); }

const recordingCallers = [
    statementCall,
    function objectLiteralSetter(callee) { holder.setter = callee; return receiver; },
    function asyncFunctionWithoutAwait(callee) { asyncWithoutAwait(callee); return receiver; },
];

function receivers() {
    for (const caller of returningCallers) {
        shouldBe(caller(strictCallee), undefined, caller, " with a strict callee");
        shouldBe(caller(sloppyCallee), globalThis, caller, " with a sloppy callee");
    }
    for (const caller of recordingCallers) {
        shouldBe(caller(strictCallee), undefined, caller, " with a strict callee");
        shouldBe(caller(sloppyCallee), globalThis, caller, " with a sloppy callee");
    }
    shouldBe(typeof parameterNamedObject(Object), "object", "a parameter named Object that holds Object");
    shouldBe(Array.isArray(parameterNamedArray(Array)), true, "a parameter named Array that holds Array");
}

// A callee that is not a JS function gets the same receiver.
function otherCallees() {
    for (const caller of [bareCall, optionalCall, spreadCall, taggedTemplate]) {
        shouldBe(caller(proxyCallee), undefined, caller, " with a Proxy callee");
        shouldBe(caller(Object.prototype.toString), "[object Undefined]", caller, " with a native callee");
    }
    shouldBe(statementCall(proxyCallee), undefined, statementCall, " with a Proxy callee");
}

// |this| does not give the callee the parameters of its caller. A sloppy callee can still read
// a copy of them through caller.arguments, as in V8.
function writeVictim(callback, secret) { arguments; callback(); return secret; }
function writer() {
    if (this !== undefined && this !== globalThis)
        this[1] = "overwritten by the callee";
}
function readVictim(callback, secret) { arguments; return callback(); }
function reader() { "use strict"; return this === undefined ? undefined : this[1]; }

function parametersThroughThis() {
    shouldBe(writeVictim(writer, "the caller's own parameter"), "the caller's own parameter", "a callee wrote the caller's parameter through |this|");
    shouldBe(readVictim(reader, "a value the callee was never given"), undefined, "a callee read the caller's parameter through |this|");
}

// What must not change.
function returnsOne() { return 1; }
function returnsTwo() { return 2; }

// The callee is the current value of the parameter, read before the arguments are evaluated.
function reassigned(callee, other) { arguments; callee = other; return callee(); }
function aliased(callee, other) { arguments[0] = other; return callee(); }
function unmapped(callee, other) { delete arguments[0]; arguments[0] = other; return callee(); }
function assignedInArgument(callee, other) { arguments; return callee(callee = other); }
function aliasedInArgument(callee, other) { return callee(arguments[0] = other); }
function assignedInSubstitution(callee, other) { arguments; return callee`${callee = other}`; }

// An explicit receiver is kept.
function methodCall(object) { arguments; return object.method(); }
function methodCallByValue(object, name) { arguments; return object[name](); }
function optionalMethodCall(object) { arguments; return object?.method(); }
function taggedMethod(object) { arguments; return object.method`x`; }
function callWithReceiver(callee, thisValue) { arguments; return callee.call(thisValue); }
function methodOfArguments(callee) { return arguments[0]() === arguments; }
function methodOfEscapedArguments(callee) { const escaped = arguments; return escaped[0]() === escaped; }
const object = { method: sloppyCallee, strictMethod: strictCallee };

// These callers do not keep their parameters in a DirectArguments object.
function noArguments(callee) { return callee(); }
function capturedParameter(callee) { arguments; (function () { callee; }); return callee(); }
function strictCaller(callee) { "use strict"; arguments; return callee(); }
function defaultParameter(callee, other = 1) { arguments; return callee(); }
function restParameter(callee, ...rest) { arguments; return callee(); }
function parameterNamedArguments(arguments) { return arguments(); }
function* generatorCaller(callee) { arguments; yield callee(); }
// A function that calls a name spelled eval captures all of its parameters.
function parameterNamedEval(eval) { arguments; return eval(); }
function optionalParameterNamedEval(eval) { arguments; return eval?.(); }

const otherCallers = [
    noArguments, capturedParameter, strictCaller, defaultParameter, restParameter, parameterNamedArguments,
    parameterNamedEval, optionalParameterNamedEval,
    function generatorFunction(callee) { return generatorCaller(callee).next().value; },
];

// `with` is what the scope in the this register is for.
function withStatement(callee, object) { arguments; with (object) { return callee(); } }
const withObject = { callee: strictCallee };

function unchanged() {
    shouldBe(reassigned(returnsOne, returnsTwo), 2, reassigned);
    shouldBe(aliased(returnsOne, returnsTwo), 2, aliased);
    shouldBe(unmapped(returnsOne, returnsTwo), 1, unmapped);
    shouldBe(assignedInArgument(returnsOne, returnsTwo), 1, assignedInArgument);
    shouldBe(aliasedInArgument(returnsOne, returnsTwo), 1, aliasedInArgument);
    shouldBe(assignedInSubstitution(returnsOne, returnsTwo), 1, assignedInSubstitution);

    shouldBe(methodCall(object), object, methodCall);
    shouldBe(methodCallByValue(object, "strictMethod"), object, methodCallByValue);
    shouldBe(optionalMethodCall(object), object, optionalMethodCall);
    shouldBe(taggedMethod(object), object, taggedMethod);
    shouldBe(callWithReceiver(strictCallee, object), object, callWithReceiver);
    shouldBe(methodOfArguments(sloppyCallee), true, methodOfArguments);
    shouldBe(methodOfEscapedArguments(strictCallee), true, methodOfEscapedArguments);

    for (const caller of otherCallers) {
        shouldBe(caller(strictCallee), undefined, caller, " with a strict callee");
        shouldBe(caller(sloppyCallee), globalThis, caller, " with a sloppy callee");
    }

    shouldBe(withStatement(strictCallee, { }), undefined, "with, name not in the object, strict callee");
    shouldBe(withStatement(sloppyCallee, { }), globalThis, "with, name not in the object, sloppy callee");
    shouldBe(withStatement(sloppyCallee, withObject), withObject, "with, name in the object");
}

// A callee that is not callable is found after the arguments are evaluated, and the message
// names the parameter.
let sideEffects = 0;
function sideEffect() { return ++sideEffects; }
function missingCallee(callee) { arguments; return callee(sideEffect()); }
function missingOptionalCallee(callee) { arguments; return callee?.(sideEffect()); }
function notAFunction(cb) { arguments; return cb(); }

function errors() {
    sideEffects = 0;
    shouldThrowTypeError(function () { missingCallee(); }, "missingCallee");
    shouldBe(sideEffects, 1, "missingCallee evaluates its arguments");
    shouldBe(missingOptionalCallee(), undefined, missingOptionalCallee);
    shouldBe(sideEffects, 1, "missingOptionalCallee does not evaluate its arguments");
    shouldBe(String(shouldThrowTypeError(function () { notAFunction(1); }, "notAFunction")), "TypeError: cb is not a function. (In 'cb()', 'cb' is 1)", notAFunction);
    shouldThrowTypeError(function () { bareCall(Object.prototype.valueOf); }, "bareCall with Object.prototype.valueOf");
    shouldThrowTypeError(function () { taggedTemplate(Object.prototype.valueOf); }, "taggedTemplate with Object.prototype.valueOf");
}

// A mapped parameter is a binding that cannot be deleted. The delete must not remove the
// property of that name from the arguments object.
function deleteParameter(parameter) { arguments; return delete parameter; }
function deleteIgnoredResult(parameter) { arguments; delete parameter; return parameter; }
function deleteLength(length) { arguments; return [delete length, length, arguments.length]; }
function deleteCallee(callee) { arguments; return [delete callee, callee, typeof arguments.callee]; }
function deleteOwnProperty(expando) { arguments.expando = "own property"; return [delete expando, expando, arguments.expando]; }
function deleteArgumentsLength(length) { return [delete arguments.length, length, arguments.length]; }

function deletes() {
    shouldBe(deleteParameter(1), false, deleteParameter);
    shouldBe(deleteIgnoredResult(1), 1, deleteIgnoredResult);
    shouldBe(deleteLength(7).join(), "false,7,1", deleteLength);
    shouldBe(deleteCallee(7).join(), "false,7,function", deleteCallee);
    shouldBe(deleteOwnProperty(7).join(), "false,7,own property", deleteOwnProperty);
    shouldBe(deleteArgumentsLength(7).join(), "true,7,", deleteArgumentsLength);
}

// Every other way to name a mapped parameter still reads and writes the arguments object.
function everyOtherUse(parameter) {
    arguments;
    shouldBe(parameter, 1, "read");
    parameter = 2;
    shouldBe(arguments[0], 2, "=");
    parameter += 3;
    shouldBe(arguments[0], 5, "+=");
    shouldBe(parameter++, 5, "postfix ++");
    shouldBe(++parameter, 7, "prefix ++");
    shouldBe(parameter--, 7, "postfix --");
    shouldBe(--parameter, 5, "prefix --");
    parameter ||= 100;
    parameter &&= 6;
    parameter ??= 100;
    shouldBe(arguments[0], 6, "logical assignment");
    shouldBe(typeof parameter, "number", "typeof");
    for (parameter in { key: 1 }) { }
    shouldBe(arguments[0], "key", "for-in");
    for (parameter of [8]) { }
    shouldBe(arguments[0], 8, "for-of");
    [parameter] = [9];
    shouldBe(arguments[0], 9, "array destructuring assignment");
    ({ parameter } = { parameter: 10 });
    shouldBe(arguments[0], 10, "object destructuring assignment");
    for ([parameter] of [[11]]) { }
    shouldBe(arguments[0], 11, "destructuring for-of");
    var [parameter] = [12];
    shouldBe(arguments[0], 12, "var destructuring");
    var parameter;
    shouldBe(parameter, 12, "var");
    arguments[0] = 13;
    return parameter;
}
// A function declaration with the name of a parameter. The generator has to accept both shapes.
function declarationNamedLikeParameter(parameter) { arguments; function parameter() { } return typeof parameter; }
function blockDeclarationNamedLikeParameter(parameter) { arguments; { function parameter() { } } return typeof parameter; }

// A function made while a lexical binding of an enclosing scope is in its dead zone checks the
// dead zone by name, also for its own parameter of that name.
let deadZone;
{
    function assigns(shadowed) { arguments; shadowed = 1; return shadowed; }
    function destructures(shadowed) { arguments; [shadowed] = [5]; return shadowed; }
    function loops(shadowed) { arguments; for ([shadowed] of [[7]]) { } return shadowed; }
    function modifies(shadowed) { arguments; shadowed += 2; shadowed++; ++shadowed; return typeof shadowed + shadowed; }
    function calls(shadowed) { arguments; return shadowed(); }
    function tags(shadowed) { arguments; return shadowed`x`; }
    function deletesIt(shadowed) { arguments; return delete shadowed; }
    deadZone = function () {
        shouldBe(assigns(0), 1, assigns);
        shouldBe(destructures(0), 5, destructures);
        shouldBe(loops(0), 7, loops);
        shouldBe(modifies(0), "number4", modifies);
        shouldBe(calls(strictCallee), undefined, calls);
        shouldBe(tags(strictCallee), undefined, tags);
        shouldBe(deletesIt(0), false, deletesIt);
    };
    let shadowed = 3;
}

function otherUses() {
    shouldBe(everyOtherUse(1), 13, everyOtherUse);
    shouldBe(declarationNamedLikeParameter(1), "function", declarationNamedLikeParameter);
    shouldBe(blockDeclarationNamedLikeParameter(1), "number", blockDeclarationNamedLikeParameter);
    deadZone();
}

// Optimized code. With the arguments object out of the call, the arguments elimination of
// the FTL can remove it. These check the receiver and the arguments in optimized code, and
// after an exit that comes after the call. The callees are not inlined, so that the call
// stays a call.
function strictNotInlined() { "use strict"; return this; }
function sloppyNotInlined() { return this; }
function writerNotInlined() {
    if (this !== undefined && this !== globalThis)
        this[1] = "overwritten by the callee";
}
function forwardedTo(first, second) {
    "use strict";
    return this === undefined && first === forwardedTo && arguments.length === 2 ? second : "the callee got the wrong receiver or arguments";
}
noInline(strictNotInlined);
noInline(sloppyNotInlined);
noInline(writerNotInlined);
noInline(forwardedTo);

function readsArgumentAfterCall(callee, value, expectedReceiver) {
    if (callee() !== expectedReceiver)
        return "the callee got the wrong receiver";
    return arguments[1];
}
function keepsParameterAfterCall(callback, secret) {
    if (arguments.length !== 2)
        return "the wrong number of arguments";
    callback();
    return arguments[1] === secret ? secret : "the callee wrote the parameter";
}
function exitsLate(callee, value, expectedReceiver) {
    const got = callee();
    const sum = value + 1;
    if (got !== expectedReceiver)
        return "the callee got the wrong receiver";
    if (arguments.length !== 3 || arguments[0] !== callee || arguments[1] !== value || arguments[2] !== expectedReceiver)
        return "the arguments object is wrong after the call";
    return sum;
}
function forwardsArguments(callee) { return callee(...arguments); }
noInline(readsArgumentAfterCall);
noInline(keepsParameterAfterCall);
noInline(exitsLate);
noInline(forwardsArguments);

function tiers(i) {
    shouldBe(bareCall(strictCallee), undefined, bareCall, " with a strict callee");
    shouldBe(bareCall(sloppyCallee), globalThis, bareCall, " with a sloppy callee");
    shouldBe(optionalCall(strictCallee), undefined, optionalCall, " with a strict callee");
    shouldBe(spreadCall(sloppyCallee), globalThis, spreadCall, " with a sloppy callee");
    shouldBe(taggedTemplate(strictCallee), undefined, taggedTemplate, " with a strict callee");
    shouldBe(statementCall(sloppyCallee), globalThis, statementCall, " with a sloppy callee");
    shouldBe(deleteParameter(i), false, deleteParameter);
    parametersThroughThis();

    shouldBe(readsArgumentAfterCall(strictNotInlined, i, undefined), i, readsArgumentAfterCall, " with a strict callee");
    shouldBe(readsArgumentAfterCall(sloppyNotInlined, i, globalThis), i, readsArgumentAfterCall, " with a sloppy callee");
    shouldBe(keepsParameterAfterCall(writerNotInlined, i), i, keepsParameterAfterCall);
    shouldBe(exitsLate(strictNotInlined, i, undefined), i + 1, exitsLate);
    shouldBe(forwardsArguments(forwardedTo, i), i, forwardsArguments);
}

receivers();
otherCallees();
unchanged();
errors();
deletes();
otherUses();

for (let i = 0; i < testLoopCount; i++)
    tiers(i);

// The callers are optimized code now.
otherCallees();

// The first value that is not an int32 makes the optimized code of exitsLate exit after its call.
shouldBe(exitsLate(strictNotInlined, "late", undefined), "late1", exitsLate, " after an exit, with a strict callee");
shouldBe(exitsLate(sloppyNotInlined, 0.5, globalThis), 1.5, exitsLate, " after an exit, with a sloppy callee");
