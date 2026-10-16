var hasBasicBlockExecuted = $vm.hasBasicBlockExecuted;
var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function taken() { return true; }
function skipped() { return "skipped"; }

// The right operand of a short-circuit operator is a basic block of its own.
// It starts right after the left operand, so the operator belongs to it.

function coalesce(value) {
    return value ?? skipped();
}
coalesce("present");
checkBasicBlock(coalesce, "value ??", ShouldHaveExecuted);
checkBasicBlock(coalesce, "?? skipped()", ShouldNotHaveExecuted);
checkBasicBlock(coalesce, "skipped()", ShouldNotHaveExecuted);
coalesce(null);
checkBasicBlock(coalesce, "?? skipped()", ShouldHaveExecuted);
checkBasicBlock(coalesce, "skipped()", ShouldHaveExecuted);

function logicalOr(value) {
    return value || skipped();
}
logicalOr(true);
checkBasicBlock(logicalOr, "|| skipped()", ShouldNotHaveExecuted);
logicalOr(false);
checkBasicBlock(logicalOr, "|| skipped()", ShouldHaveExecuted);

function logicalAnd(value) {
    return value && skipped();
}
logicalAnd(false);
checkBasicBlock(logicalAnd, "value &&", ShouldHaveExecuted);
checkBasicBlock(logicalAnd, "&& skipped()", ShouldNotHaveExecuted);
logicalAnd(true);
checkBasicBlock(logicalAnd, "&& skipped()", ShouldHaveExecuted);

// Statement context: the result is ignored.
function statement(value) {
    value && skipped();
    return 1;
}
statement(false);
checkBasicBlock(statement, "&& skipped()", ShouldNotHaveExecuted);
checkBasicBlock(statement, "return 1", ShouldHaveExecuted);
statement(true);
checkBasicBlock(statement, "&& skipped()", ShouldHaveExecuted);

// Condition context: the enclosing statement closes the block.
function condition(value) {
    if (value && skipped())
        return 1;
    return 2;
}
condition(false);
checkBasicBlock(condition, "&& skipped()", ShouldNotHaveExecuted);
checkBasicBlock(condition, "return 1", ShouldNotHaveExecuted);
checkBasicBlock(condition, "return 2", ShouldHaveExecuted);
condition(true);
checkBasicBlock(condition, "&& skipped()", ShouldHaveExecuted);
checkBasicBlock(condition, "return 1", ShouldHaveExecuted);

function coalesceCondition(value) {
    if (value ?? skipped())
        return 1;
    return 2;
}
coalesceCondition(true);
checkBasicBlock(coalesceCondition, "?? skipped()", ShouldNotHaveExecuted);
checkBasicBlock(coalesceCondition, "return 1", ShouldHaveExecuted);
coalesceCondition(undefined);
checkBasicBlock(coalesceCondition, "?? skipped()", ShouldHaveExecuted);

// Nested operators: each right operand is its own block.
function nested(a, b) {
    return a || (b && skipped());
}
nested(true, true);
checkBasicBlock(nested, "|| (b", ShouldNotHaveExecuted);
checkBasicBlock(nested, "&& skipped()", ShouldNotHaveExecuted);
nested(false, false);
checkBasicBlock(nested, "|| (b", ShouldHaveExecuted);
checkBasicBlock(nested, "&& skipped()", ShouldNotHaveExecuted);
nested(false, true);
checkBasicBlock(nested, "&& skipped()", ShouldHaveExecuted);

// The block after the expression counts every evaluation of the expression.
function after(value) {
    var result = value || skipped();
    return result;
}
after(true);
after(false);
assert(basicBlockExecutionCount(after, "|| skipped()") === 1, "right operand ran once");
assert(basicBlockExecutionCount(after, "return result") === 2, "statement after ran twice");

// A loop emits its condition twice. Both emissions count the same block, and
// the body keeps its own count.
function whileLoop(limit) {
    var i = 0;
    while (i < limit && taken())
        i++;
    return i;
}
whileLoop(3);
assert(basicBlockExecutionCount(whileLoop, "&& taken()") === 4, "condition right operand ran once per check");
assert(basicBlockExecutionCount(whileLoop, "i++") === 3, "body ran once per iteration");
assert(basicBlockExecutionCount(whileLoop, "return i") === 1, "statement after the loop ran once");

function forLoop(limit) {
    var sum = 0;
    for (var i = 0; i < limit && taken(); i++) {
        sum += i;
    }
    return sum;
}
forLoop(3);
assert(basicBlockExecutionCount(forLoop, "&& taken()") === 4, "condition right operand ran once per check");
assert(basicBlockExecutionCount(forLoop, "sum += i") === 3, "body ran once per iteration");
assert(basicBlockExecutionCount(forLoop, "return sum") === 1, "statement after the loop ran once");

function forLoopUpdate(limit) {
    var sum = 0;
    for (var i = 0; i < limit; i++, i < limit && taken()) {
        sum += i;
    }
    return sum;
}
forLoopUpdate(3);
assert(basicBlockExecutionCount(forLoopUpdate, "&& taken()") === 2, "update right operand ran once per iteration that is not the last");
assert(basicBlockExecutionCount(forLoopUpdate, "sum += i") === 3, "body ran once per iteration");
assert(basicBlockExecutionCount(forLoopUpdate, "return sum") === 1, "statement after the loop ran once");

function doWhileLoop(limit) {
    var i = 0;
    do {
        i++;
    } while (i < limit && taken());
    return i;
}
doWhileLoop(3);
assert(basicBlockExecutionCount(doWhileLoop, "&& taken()") === 2, "condition right operand ran once per check that reached it");
assert(basicBlockExecutionCount(doWhileLoop, "return i") === 1, "statement after the loop ran once");

function whileLoopSkipped(limit) {
    var i = 0;
    while (i < limit && skipped())
        i++;
    return i;
}
whileLoopSkipped(0);
checkBasicBlock(whileLoopSkipped, "&& skipped()", ShouldNotHaveExecuted);
checkBasicBlock(whileLoopSkipped, "i++", ShouldNotHaveExecuted);
checkBasicBlock(whileLoopSkipped, "return i", ShouldHaveExecuted);

// A ternary in a loop condition has blocks of its own too. They must not
// swallow the body block when the condition is emitted the second time.
function ternaryInLoop(limit) {
    var i = 0;
    while (i < limit ? taken() : false)
        i++;
    return i;
}
ternaryInLoop(3);
assert(basicBlockExecutionCount(ternaryInLoop, "taken()") === 3, "then arm ran once per check that took it");
assert(basicBlockExecutionCount(ternaryInLoop, "false") === 1, "else arm ran once");
assert(basicBlockExecutionCount(ternaryInLoop, "i++") === 3, "body ran once per iteration");
