var hasBasicBlockExecuted = $vm.hasBasicBlockExecuted;
var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

// An if statement whose body is a lone break or continue is normally folded into the condition's
// jump. The basic block recorded for the body must still describe the body: executed once per time
// the condition held, and never when the statement after the if ran instead.

function continueTaken(n) {
    var fallThrough = 0;
    for (var i = 0; i < n; i++) {
        if (i >= 0) {
            continue;
        }
        fallThrough++;
    }
    return fallThrough;
}

function continueSkipped(n) {
    var fallThrough = 0;
    for (var i = 0; i < n; i++) {
        if (i < 0) {
            continue;
        }
        fallThrough++;
    }
    return fallThrough;
}

function breakTaken() {
    var fallThrough = 0;
    while (true) {
        if (fallThrough >= 0) {
            break;
        }
        fallThrough++;
    }
    return fallThrough;
}

function breakSkipped(n) {
    var fallThrough = 0;
    for (let i = 0; i < n; i++) {
        if (i < 0) {
            break;
        }
        fallThrough++;
    }
    return fallThrough;
}

function bracelessBreakTaken(n) {
    var fallThrough = 0;
    for (var i = 0; i < n; i++) {
        if (i >= 0)
            break;
        fallThrough++;
    }
    return fallThrough;
}

function bracelessContinueSkipped(n) {
    var fallThrough = 0;
    for (var i = 0; i < n; i++) {
        if (i < 0)
            continue;
        fallThrough++;
    }
    return fallThrough;
}

function breakElseTaken(n) {
    var elseCount = 0;
    for (var i = 0; i < n; i++) {
        if (i >= 0) {
            break;
        } else {
            elseCount++;
        }
    }
    return elseCount;
}

function breakElseSkipped(n) {
    var elseCount = 0;
    for (var i = 0; i < n; i++) {
        if (i < 0) {
            break;
        } else {
            elseCount++;
        }
    }
    return elseCount;
}

function labeledContinueTaken(n) {
    var fallThrough = 0;
    var i = 0;
    outer: while (i < n) {
        i++;
        while (true) {
            if (i > 0) {
                continue outer;
            }
            fallThrough++;
        }
    }
    return fallThrough;
}

function labeledBreakSkipped(n) {
    var fallThrough = 0;
    outer: for (var i = 0; i < n; i++) {
        for (var j = 0; j < 1; j++) {
            if (i < 0) {
                break outer;
            }
            fallThrough++;
        }
    }
    return fallThrough;
}

function switchBreakTaken(v) {
    var fallThrough = 0;
    switch (v) {
    case 1:
        if (v === 1) {
            break;
        }
        fallThrough++;
    }
    return fallThrough;
}

function switchBreakSkipped(v) {
    var fallThrough = 0;
    switch (v) {
    case 1:
        if (v !== 1) {
            break;
        }
        fallThrough++;
    }
    return fallThrough;
}

function doWhileContinueTaken(n) {
    var fallThrough = 0;
    var i = 0;
    do {
        i++;
        if (i > 0) {
            continue;
        }
        fallThrough++;
    } while (i < n);
    return fallThrough;
}

// Constant conditions take a different path through emitBytecodeInConditionContext: the jump is
// emitted unconditionally or not at all.
function constantTrueBreak() {
    var fallThrough = 0;
    while (true) {
        if (true) {
            break;
        }
        fallThrough++;
    }
    return fallThrough;
}

function constantFalseContinue(n) {
    var fallThrough = 0;
    for (var i = 0; i < n; i++) {
        if (false) {
            continue;
        }
        fallThrough++;
    }
    return fallThrough;
}

function checkBlock(func, text, expectedCount)
{
    var count = basicBlockExecutionCount(func, text);
    assert(count === expectedCount, func.name + ": '" + text + "' executed " + count + " times, expected " + expectedCount + ".");
    assert(hasBasicBlockExecuted(func, text) === (expectedCount > 0), func.name + ": '" + text + "' should " + (expectedCount > 0 ? "" : "not ") + "have executed.");
}

assert(!hasBasicBlockExecuted(continueTaken, "continue;"), "should not have executed yet.");
assert(continueTaken(3) === 0);
checkBlock(continueTaken, "continue;", 3);
checkBlock(continueTaken, "fallThrough++", 0);

assert(!hasBasicBlockExecuted(continueSkipped, "continue;"), "should not have executed yet.");
assert(continueSkipped(3) === 3);
checkBlock(continueSkipped, "continue;", 0);
checkBlock(continueSkipped, "fallThrough++", 3);

assert(!hasBasicBlockExecuted(breakTaken, "break;"), "should not have executed yet.");
assert(breakTaken() === 0);
checkBlock(breakTaken, "break;", 1);
checkBlock(breakTaken, "fallThrough++", 0);

assert(!hasBasicBlockExecuted(breakSkipped, "break;"), "should not have executed yet.");
assert(breakSkipped(3) === 3);
checkBlock(breakSkipped, "break;", 0);
checkBlock(breakSkipped, "fallThrough++", 3);

assert(bracelessBreakTaken(3) === 0);
checkBlock(bracelessBreakTaken, "break;", 1);
checkBlock(bracelessBreakTaken, "fallThrough++", 0);

assert(bracelessContinueSkipped(3) === 3);
checkBlock(bracelessContinueSkipped, "continue;", 0);
checkBlock(bracelessContinueSkipped, "fallThrough++", 3);

assert(breakElseTaken(3) === 0);
checkBlock(breakElseTaken, "break;", 1);
checkBlock(breakElseTaken, "elseCount++", 0);

assert(breakElseSkipped(3) === 3);
checkBlock(breakElseSkipped, "break;", 0);
checkBlock(breakElseSkipped, "elseCount++", 3);

assert(labeledContinueTaken(3) === 0);
checkBlock(labeledContinueTaken, "continue outer;", 3);
checkBlock(labeledContinueTaken, "fallThrough++", 0);

assert(labeledBreakSkipped(3) === 3);
checkBlock(labeledBreakSkipped, "break outer;", 0);
checkBlock(labeledBreakSkipped, "fallThrough++", 3);

assert(switchBreakTaken(1) === 0);
checkBlock(switchBreakTaken, "break;", 1);
checkBlock(switchBreakTaken, "fallThrough++", 0);

assert(switchBreakSkipped(1) === 1);
checkBlock(switchBreakSkipped, "break;", 0);
checkBlock(switchBreakSkipped, "fallThrough++", 1);

assert(doWhileContinueTaken(3) === 0);
checkBlock(doWhileContinueTaken, "continue;", 3);
checkBlock(doWhileContinueTaken, "fallThrough++", 0);

assert(constantTrueBreak() === 0);
checkBlock(constantTrueBreak, "break;", 1);
checkBlock(constantTrueBreak, "fallThrough++", 0);

assert(constantFalseContinue(3) === 3);
checkBlock(constantFalseContinue, "continue;", 0);
checkBlock(constantFalseContinue, "fallThrough++", 3);
