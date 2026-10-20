// A delete stub answers for every object of one Structure, and does not call the deleteProperty of the class of the
// object. So a site must get no stub for a delete in which the class did work that the Structure does not show.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": bad value: " + actual + " expected: " + expected);
}

// ClonedArguments makes `callee` when the delete asks for it: an accessor that is not configurable for the arguments
// of a strict function, an ordinary property for function.arguments of a sloppy one. The two kinds have one Structure
// until then, also after a delete of `length`. A site that saw the first kind must not answer for the second.
function strictArguments() { "use strict"; return arguments; }
function sloppyCallee(a, b) { return argumentsOfSloppyCallee(); }
function argumentsOfSloppyCallee() { return sloppyCallee.arguments; }
function deleteLength(o) { return delete o.length; }
noInline(deleteLength);
function deleteCalleeAfterLength(o) { return delete o.callee; }
noInline(deleteCalleeAfterLength);
function deleteCallee(o) { return delete o.callee; }
noInline(deleteCallee);

for (let i = 0; i < testLoopCount; ++i) {
    let strict = strictArguments(1, 2);
    shouldBe(deleteLength(strict), true, "length of strict arguments");
    shouldBe(deleteCalleeAfterLength(strict), false, "callee of strict arguments after length");
}
for (let i = 0; i < 100; ++i) {
    let sloppy = sloppyCallee(1, 2);
    shouldBe(deleteLength(sloppy), true, "length of function.arguments");
    shouldBe(deleteCalleeAfterLength(sloppy), true, "callee of function.arguments after length");
    shouldBe("callee" in sloppy, false, "callee of function.arguments is gone after length");
}

for (let i = 0; i < testLoopCount; ++i)
    shouldBe(deleteCallee(strictArguments(1, 2)), false, "callee of strict arguments");
for (let i = 0; i < 100; ++i) {
    let sloppy = sloppyCallee(1, 2);
    shouldBe(deleteCallee(sloppy), true, "callee of function.arguments");
    shouldBe("callee" in sloppy, false, "callee of function.arguments is gone");
}

// The global object answers for a `var` from its symbol table. A script of the JSONP shape declares one and leaves the
// Structure of the global object as it is.
let deleteGlobal = new Function("return delete declaredLater;");
noInline(deleteGlobal);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(deleteGlobal(), true, "a global that is not there");
loadString("var declaredLater = 1;");
shouldBe(deleteGlobal(), false, "a global var");
shouldBe(declaredLater, 1, "the global var stays");

// A function also records that its `name` is no longer the one it was made with. bind() reads that record.
function deleteName(f) { return delete f.name; }
noInline(deleteName);
for (let i = 0; i < testLoopCount; ++i) {
    let f = function named() { };
    f.name;
    shouldBe(deleteName(f), true, "name of a function");
    shouldBe(f.bind().name, "bound ", "name of a bound function");
}
