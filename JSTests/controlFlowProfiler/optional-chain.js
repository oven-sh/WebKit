var hasBasicBlockExecuted = $vm.hasBasicBlockExecuted;
var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

// Each `?.` link of an optional chain starts a basic block: the rest of the
// chain runs only when the link's base is not nullish.

function dot(o) {
    return o?.a;
}
dot(null);
checkBasicBlock(dot, "return o", ShouldHaveExecuted);
checkBasicBlock(dot, "?.a", ShouldNotHaveExecuted);
dot({ a: 1 });
checkBasicBlock(dot, "?.a", ShouldHaveExecuted);

function bracket(o) {
    return o?.["a"];
}
bracket(null);
checkBasicBlock(bracket, "[\"a\"]", ShouldNotHaveExecuted);
bracket({ a: 1 });
checkBasicBlock(bracket, "[\"a\"]", ShouldHaveExecuted);

function call(f) {
    return f?.();
}
call(null);
checkBasicBlock(call, "?.()", ShouldNotHaveExecuted);
call(function() { });
checkBasicBlock(call, "?.()", ShouldHaveExecuted);

function methodCall(o) {
    return o?.m();
}
methodCall(null);
checkBasicBlock(methodCall, "?.m()", ShouldNotHaveExecuted);
methodCall({ m() { } });
checkBasicBlock(methodCall, "?.m()", ShouldHaveExecuted);

function chain(o) {
    return o?.a.b?.c();
}
chain(null);
checkBasicBlock(chain, "?.a.b", ShouldNotHaveExecuted);
checkBasicBlock(chain, "?.c()", ShouldNotHaveExecuted);
chain({ a: { b: null } });
checkBasicBlock(chain, "?.a.b", ShouldHaveExecuted);
checkBasicBlock(chain, "?.c()", ShouldNotHaveExecuted);
chain({ a: { b: { c() { } } } });
checkBasicBlock(chain, "?.c()", ShouldHaveExecuted);

function deleteDot(o) {
    return delete o?.a;
}
deleteDot(null);
checkBasicBlock(deleteDot, "?.a", ShouldNotHaveExecuted);
deleteDot({ a: 1 });
checkBasicBlock(deleteDot, "?.a", ShouldHaveExecuted);

function coalesce(o) {
    return o?.a ?? "fallback";
}
coalesce(null);
checkBasicBlock(coalesce, "?.a", ShouldNotHaveExecuted);
checkBasicBlock(coalesce, "?? \"fallback\"", ShouldHaveExecuted);
coalesce({ a: 1 });
checkBasicBlock(coalesce, "?.a", ShouldHaveExecuted);
assert(basicBlockExecutionCount(coalesce, "?? \"fallback\"") === 1, "right operand ran once");

function condition(o) {
    if (o?.a)
        return 1;
    return 2;
}
condition(null);
checkBasicBlock(condition, "?.a", ShouldNotHaveExecuted);
checkBasicBlock(condition, "return 1", ShouldNotHaveExecuted);
checkBasicBlock(condition, "return 2", ShouldHaveExecuted);
condition({ a: 1 });
checkBasicBlock(condition, "?.a", ShouldHaveExecuted);
checkBasicBlock(condition, "return 1", ShouldHaveExecuted);

function calleeChain(o) {
    return (o?.m)();
}
calleeChain({ m() { } });
checkBasicBlock(calleeChain, "?.m", ShouldHaveExecuted);

// The block after the chain counts every evaluation of the statement.
function after(o) {
    var result = o?.a;
    return result;
}
after(null);
after({ a: 1 });
assert(basicBlockExecutionCount(after, "?.a") === 1, "link ran once");
assert(basicBlockExecutionCount(after, "return result") === 2, "statement after ran twice");
