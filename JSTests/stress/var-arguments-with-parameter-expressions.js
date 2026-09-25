//@ defaultRun; run("debugger-bytecode", "--forceDebuggerBytecodeGeneration=true"); run("profilers", "--useTypeProfiler=true", "--useControlFlowProfiler=true")
// When the parameters have expressions, the body's vars live in an environment of their own, below the one that holds the
// parameters and `arguments` (FunctionDeclarationInstantiation, step 28). A `var arguments` in the body is a binding of that
// environment. It starts as the arguments object, and a store to it does not reach the `arguments` that the parameter
// expressions and their closures read.
function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function show(value) {
    if (typeof value === "function")
        return "function";
    if (Array.isArray(value))
        return "[" + value.map(show).join(",") + "]";
    if (Object.prototype.toString.call(value) === "[object Arguments]")
        return "arguments(" + value.length + ")";
    return String(value);
}

// The closure in the parameters keeps the arguments object. The body reads its own var.
function assigned(a = () => arguments) { var arguments = 7; return [a(), arguments]; }
function assignedLater(a = () => arguments) { var arguments; var before = arguments; arguments = 7; return [a(), before, arguments, a() === before]; }
function assignedBeforeTheDeclaration(a = () => arguments) { arguments = 7; var arguments; return [a(), arguments]; }
function readInTheParameters(a = arguments) { var arguments = 7; return [a, arguments]; }
function lengthInTheParameters(a = arguments.length) { var arguments = 7; return [a, arguments]; }
function lengthInAClosure(a = () => arguments.length) { var arguments = 7; return [a(), arguments]; }
function elementInAClosure(a = 1, b = () => arguments[0]) { var arguments = 7; a = 5; return [b(), arguments, a]; }
function typeofInTheParameters(a = typeof arguments) { var arguments = 7; return [a, arguments]; }
function closureInTheBody(a = () => arguments) { var arguments = 7; return [a(), (() => arguments)(), (function () { return arguments.length; })(1, 2)]; }
function evalInTheParameters(a = eval("() => arguments")) { var arguments = 7; return [a(), arguments]; }
function evalInTheBody(a = () => arguments) { var arguments = 7; return [a(), arguments, eval("arguments")]; }
function withInTheBody(a = () => arguments) { var arguments = 7; with ({}) { return [a(), arguments]; } }
function destructuredDefault({ a = () => arguments }) { var arguments = 7; return [a(), arguments]; }
var fromComputedKey;
function computedKey({ [(fromComputedKey = () => arguments, "k")]: x }) { var arguments = 7; return [fromComputedKey(), arguments, x]; }
function forOf(a = () => arguments) { for (var arguments of [7]); return [a(), arguments]; }
function forIn(a = () => arguments) { for (var arguments in { k: 1 }); return [a(), arguments]; }
function objectPattern(a = () => arguments) { var { arguments } = { arguments: 7 }; return [a(), arguments]; }
function arrayPattern(a = () => arguments) { var [arguments] = [7]; return [a(), arguments]; }
function nestedBlock(a = () => arguments) { { var arguments = 7; } return [a(), arguments]; }

shouldBe(show(assigned(undefined, 1, 1, 1)), "[arguments(4),7]");
shouldBe(show(assignedLater(undefined, 1, 1, 1)), "[arguments(4),arguments(4),7,true]");
shouldBe(show(assignedBeforeTheDeclaration(undefined, 1, 1, 1)), "[arguments(4),7]");
shouldBe(show(readInTheParameters(undefined, 1, 1, 1)), "[arguments(4),7]");
shouldBe(show(lengthInTheParameters(undefined, 1, 1, 1)), "[4,7]");
shouldBe(show(lengthInAClosure(undefined, 1, 1, 1)), "[4,7]");
shouldBe(show(elementInAClosure(3)), "[3,7,5]");
shouldBe(show(typeofInTheParameters(undefined, 1, 1, 1)), "[object,7]");
shouldBe(show(closureInTheBody(undefined, 1, 1, 1)), "[arguments(4),7,2]");
shouldBe(show(evalInTheParameters(undefined, 1, 1, 1)), "[arguments(4),7]");
shouldBe(show(evalInTheBody(undefined, 1, 1, 1)), "[arguments(4),7,7]");
shouldBe(show(withInTheBody(undefined, 1, 1, 1)), "[arguments(4),7]");
shouldBe(show(destructuredDefault({}, 1, 1, 1)), "[arguments(4),7]");
shouldBe(show(computedKey({ k: 5 }, 1, 1, 1)), "[arguments(4),7,5]");
shouldBe(show(forOf(undefined, 1, 1, 1)), "[arguments(4),7]");
shouldBe(show(forIn(undefined, 1, 1, 1)), "[arguments(4),k]");
shouldBe(show(objectPattern(undefined, 1, 1, 1)), "[arguments(4),7]");
shouldBe(show(arrayPattern(undefined, 1, 1, 1)), "[arguments(4),7]");
shouldBe(show(nestedBlock(undefined, 1, 1, 1)), "[arguments(4),7]");

// A direct eval in the body finds the body's var. It does not make another one.
function evalDeclaresItAgain(a = () => arguments) { var arguments; eval("var arguments = 9"); return [a(), arguments]; }
function evalDeclaresAFunction(a = () => arguments) { var arguments; eval("function arguments() { }"); return [a(), arguments]; }
function evalDeclaresItAgainWithARestParameter(...rest) { var arguments; eval("var arguments = 9"); return arguments; }
shouldBe(show(evalDeclaresItAgain(undefined, 1, 1, 1)), "[arguments(4),9]");
shouldBe(show(evalDeclaresAFunction(undefined, 1, 1, 1)), "[arguments(4),function]");
shouldBe(show(evalDeclaresItAgainWithARestParameter(1, 2, 3)), "9");

// The var starts as the value that `arguments` has when the parameters are done, and the two stay apart after that.
function storedInTheParameters(a = () => arguments, b = (arguments = 9)) { var arguments; return [a(), arguments]; }
function storedByAClosureLater(a = () => arguments, b = () => { arguments = 9; }) { var arguments = 7; b(); return [a(), arguments]; }
function neverAssigned(a = () => arguments) { if (false) { var arguments = 7; } return [a(), arguments, a() === arguments]; }
shouldBe(show(storedInTheParameters(undefined, undefined, 1)), "[9,9]");
shouldBe(show(storedByAClosureLater(undefined, undefined, 1)), "[9,7]");
shouldBe(show(neverAssigned(undefined, 1, 1, 1)), "[arguments(4),arguments(4),true]");

// A function declaration wins over the arguments object in the body only. In a block it replaces the var when the block runs.
function functionDeclaration(a = () => arguments) { function arguments() { } return [a(), arguments]; }
function functionDeclarationAndVar(a = () => arguments) { var arguments; function arguments() { } return [a(), arguments]; }
function functionInABlock(a = () => arguments) { var before = arguments; { function arguments() { } } return [a(), before, arguments]; }
function functionInABlockAndVar(a = () => arguments) { var arguments; var before = arguments; { function arguments() { } } return [a(), before, arguments]; }
function functionInABlockThatDoesNotRun(a = () => arguments) { if (false) { function arguments() { } } return [a(), arguments, a() === arguments]; }
shouldBe(show(functionDeclaration(undefined, 1, 1, 1)), "[arguments(4),function]");
shouldBe(show(functionDeclarationAndVar(undefined, 1, 1, 1)), "[arguments(4),function]");
shouldBe(show(functionInABlock(undefined, 1, 1, 1)), "[arguments(4),arguments(4),function]");
shouldBe(show(functionInABlockAndVar(undefined, 1, 1, 1)), "[arguments(4),arguments(4),function]");
shouldBe(show(functionInABlockThatDoesNotRun(undefined, 1, 1, 1)), "[arguments(4),arguments(4),true]");

// The var for a function in a block exists from the start and starts as the arguments object, as in V8 and as
// https://github.com/tc39/ecma262/issues/991 proposes. The current text of Annex B.3.2.1 makes it when the block runs. Only a
// store before the block, or a `delete` after it, can tell the two apart.
function storeBeforeAFunctionInABlock(a = () => arguments) { arguments = 5; { function arguments() { } } return [a(), arguments]; }
function storeBeforeAFunctionInABlockThatDoesNotRun(a = () => arguments) { arguments = 5; if (false) { function arguments() { } } return [a(), arguments]; }
function storeFromTheParametersBeforeAFunctionInABlock(a = () => arguments, b = () => { arguments = 9; }) { b(); var before = arguments; { function arguments() { } } return [a(), before, arguments]; }
function deleteAfterAFunctionInABlock(a = () => arguments) { { function arguments() { } } return [delete arguments, a(), arguments]; }
shouldBe(show(storeBeforeAFunctionInABlock(undefined, 1, 1, 1)), "[arguments(4),function]");
shouldBe(show(storeBeforeAFunctionInABlockThatDoesNotRun(undefined, 1, 1, 1)), "[arguments(4),5]");
shouldBe(show(storeFromTheParametersBeforeAFunctionInABlock(undefined, undefined, 1)), "[9,arguments(3),function]");
shouldBe(show(deleteAfterAFunctionInABlock(undefined, 1, 1, 1)), "[false,arguments(4),function]");

// Every kind of function that makes its own arguments object.
var functionExpression = function (a = () => arguments) { var arguments = 7; return [a(), arguments]; };
var namedFunctionExpression = function named(a = () => arguments) { var arguments = 7; return [a(), arguments, typeof named]; };
var functionExpressionNamedArguments = function arguments(a = () => arguments) { var arguments = 7; return [a(), arguments]; };
var object = {
    method(a = () => arguments) { var arguments = 7; return [a(), arguments]; },
    set setter(a = () => arguments) { var arguments = 7; this.result = [a(), arguments]; },
    async asyncMethod(a = () => arguments) { var arguments = 7; object.result = [a(), arguments]; await 0; },
    *generatorMethod(a = () => arguments) { var arguments = 7; yield [a(), arguments]; },
};
var fromFunctionConstructor = new Function("a = () => arguments", "var arguments = 7; return [a(), arguments];");
function Constructor(a = () => arguments) { var arguments = 7; this.result = [a(), arguments]; }
async function asyncFunctionWithoutAwait(a = () => arguments) { var arguments = 7; object.result = [a(), arguments]; }
async function asyncFunction(a = () => arguments) { var arguments = 7; object.result = [a(), arguments]; await 0; }
function* generator(a = () => arguments) { var arguments = 7; yield 1; yield [a(), arguments]; }
async function* asyncGenerator(a = () => arguments) { var arguments = 7; object.result = [a(), arguments]; yield 1; }

shouldBe(show(functionExpression(undefined, 1, 1, 1)), "[arguments(4),7]");
shouldBe(show(namedFunctionExpression(undefined, 1, 1, 1)), "[arguments(4),7,function]");
shouldBe(show(functionExpressionNamedArguments(undefined, 1, 1, 1)), "[arguments(4),7]");
shouldBe(show(object.method(undefined, 1, 1, 1)), "[arguments(4),7]");
object.setter = undefined;
shouldBe(show(object.result), "[arguments(1),7]");
shouldBe(show(fromFunctionConstructor(undefined, 1, 1, 1)), "[arguments(4),7]");
shouldBe(show(new Constructor(undefined, 1, 1, 1).result), "[arguments(4),7]");
asyncFunctionWithoutAwait(undefined, 1, 1, 1);
shouldBe(show(object.result), "[arguments(4),7]");
asyncFunction(undefined, 1, 1);
shouldBe(show(object.result), "[arguments(3),7]");
object.asyncMethod(undefined, 1);
shouldBe(show(object.result), "[arguments(2),7]");
var iterator = generator(undefined, 1, 1, 1);
iterator.next();
shouldBe(show(iterator.next().value), "[arguments(4),7]");
shouldBe(show(object.generatorMethod(undefined, 1, 1).next().value), "[arguments(3),7]");
asyncGenerator(undefined, 1, 1, 1, 1).next();
shouldBe(show(object.result), "[arguments(5),7]");

// An arrow function has no arguments object of its own. Its `var arguments` hides the one of the function around it.
function aroundAnArrowFunction() { return ((a = () => arguments) => { var arguments = 7; return [a(), arguments]; })(); }
shouldBe(show(aroundAnArrowFunction(1, 2)), "[arguments(2),7]");

// One environment when the parameters have no expression: the var is the binding that holds the arguments object.
function simpleParameters(a) { var arguments = 7; return [arguments, a]; }
function simpleParametersNeverAssigned(a) { var arguments; return [arguments, a]; }
function restParameter(...rest) { var arguments = 7; return [arguments, rest.length]; }
function restParameterNeverAssigned(...rest) { var arguments; return [arguments, rest.length]; }
function patternWithoutExpressions({ x }) { var arguments = 7; return [arguments, x]; }
function patternWithoutExpressionsNeverAssigned({ x }) { var arguments; return [arguments, x]; }
shouldBe(show(simpleParameters(1, 2)), "[7,1]");
shouldBe(show(simpleParametersNeverAssigned(1, 2)), "[arguments(2),1]");
shouldBe(show(restParameter(1, 2, 3)), "[7,3]");
shouldBe(show(restParameterNeverAssigned(1, 2, 3)), "[arguments(3),3]");
shouldBe(show(patternWithoutExpressions({ x: 1 }, 2)), "[7,1]");
shouldBe(show(patternWithoutExpressionsNeverAssigned({ x: 1 }, 2)), "[arguments(2),1]");

// No `var arguments`: the body stores to the binding of the parameters.
function noDeclaration(a = () => arguments) { arguments = 7; return [a(), arguments]; }
function lexicalDeclaration(a = () => arguments) { let arguments = 7; return [a(), arguments]; }
shouldBe(show(noDeclaration(undefined, 1, 1, 1)), "[7,7]");
shouldBe(show(lexicalDeclaration(undefined, 1, 1, 1)), "[arguments(4),7]");

// A parameter named `arguments` is an ordinary parameter, and there is no arguments object. The var starts with its value.
function parameterNamedArguments(arguments, a = () => arguments) { var arguments = 7; return [a(), arguments]; }
function patternBindsArguments({ arguments }, a = () => arguments) { var arguments = 7; return [a(), arguments]; }
function restParameterNamedArguments(a = () => arguments, ...arguments) { var arguments = 7; return [a(), arguments]; }
function patternBindsArgumentsStoredLater({ arguments }, a = () => arguments, b = () => { arguments = 9; }) { var arguments; b(); return [a(), arguments]; }
shouldBe(show(parameterNamedArguments(5)), "[5,7]");
shouldBe(show(patternBindsArguments({ arguments: 5 })), "[5,7]");
shouldBe(show(restParameterNamedArguments(undefined, 1, 2)), "[[1,2],7]");
shouldBe(show(patternBindsArgumentsStoredLater({ arguments: 5 })), "[9,5]");

// Through the tiers, with the two bindings in registers and in environments.
function hot(a = arguments, b = arguments.length) { var arguments = b + 3; return a.length * 100 + arguments; }
function hotWithClosures(a = () => arguments, b = (value) => { arguments = value; }) {
    var arguments = 7;
    var fromTheBody = () => arguments;
    b(a().length + 30);
    return a() * 100 + fromTheBody();
}
function hotWithAFunctionInABlock(a = () => arguments) {
    var before = arguments;
    { function arguments() { return 5; } }
    return (before === a() ? 100 : 0) + a().length * 10 + arguments();
}
noInline(hot);
noInline(hotWithClosures);
noInline(hotWithAFunctionInABlock);
for (var i = 0; i < testLoopCount; i++) {
    shouldBe(hot(undefined, undefined, 1, 1), 407);
    shouldBe(hotWithClosures(undefined, undefined, 1), 3307);
    shouldBe(hotWithAFunctionInABlock(undefined, 1), 125);
}
