import { shouldBe } from "./resources/assert.js";
import functionDeclaration from "./export-default-function-name-for-display/function-declaration.js";
import generatorDeclaration from "./export-default-function-name-for-display/generator-declaration.js";
import asyncFunctionDeclaration from "./export-default-function-name-for-display/async-function-declaration.js";
import asyncGeneratorDeclaration from "./export-default-function-name-for-display/async-generator-declaration.js";
import ClassDeclaration from "./export-default-function-name-for-display/class-declaration.js";
import DerivedClassDeclaration from "./export-default-function-name-for-display/derived-class-declaration.js";
import ClassWithDefaultConstructor from "./export-default-function-name-for-display/class-with-default-constructor.js";
import DerivedClassWithDefaultConstructor from "./export-default-function-name-for-display/derived-class-with-default-constructor.js";
import arrowFunction from "./export-default-function-name-for-display/arrow-function.js";
import functionExpression from "./export-default-function-name-for-display/function-expression.js";
import ClassExpression from "./export-default-function-name-for-display/class-expression.js";
import namedStarDefault from "./export-default-function-name-for-display/named-star-default.js";
import awaits from "./export-default-function-name-for-display/awaits.js";
import tailCall from "./export-default-function-name-for-display/tail-call.js";

// An anonymous default export is bound to the private name "*default*", whose text is "starDefault".
// Its name is "default", and that is what a stack, an error message, a code block and a heap snapshot show.
// https://tc39.es/ecma262/#sec-exports-runtime-semantics-evaluation

// A line of a stack is `name@url:line:column`.
function frames(stack) {
    return stack.replace(/@.*/g, "").split("\n").join(" < ");
}

function messageOf(run) {
    try {
        run();
    } catch (error) {
        return error.message;
    }
    return "no error";
}

const anonymous = [functionDeclaration, generatorDeclaration, asyncFunctionDeclaration, asyncGeneratorDeclaration, ClassDeclaration, DerivedClassDeclaration, ClassWithDefaultConstructor, DerivedClassWithDefaultConstructor, arrowFunction, functionExpression, ClassExpression, tailCall];

// A call in tail position that throws leaves no function object for its frame, and the frame has the name of its code.
// A caller that inlined the function has no such frame at all.
noInline(tailCall);

function checkFrames() {
    shouldBe(frames(functionDeclaration()), "default < checkFrames < module code");
    shouldBe(frames(generatorDeclaration().next().value), "default < checkFrames < module code");
    shouldBe(frames(new ClassDeclaration().stack), "default < checkFrames < module code");
    shouldBe(frames(arrowFunction()), "default < checkFrames < module code");
    shouldBe(frames(functionExpression()), "default < checkFrames < module code");
    shouldBe(frames(new ClassExpression().stack), "default < checkFrames < module code");
    // A function that is really named starDefault keeps its name.
    shouldBe(frames(namedStarDefault()), "starDefault < checkFrames < module code");

    let thrown;
    try {
        tailCall();
    } catch (error) {
        thrown = error;
    }
    shouldBe(frames(thrown.stack), "default < checkFrames < module code");
}

// Behind an await the function that waits is an async frame.
async function checkAsyncFrames() {
    shouldBe(frames(await asyncFunctionDeclaration()), "default < async checkAsyncFrames");
    shouldBe(frames((await asyncGeneratorDeclaration().next()).value), "default");
    let caught;
    try {
        await awaits();
    } catch (error) {
        caught = error;
    }
    shouldBe(frames(caught.stack), "thrower < async default < async checkAsyncFrames");
}

// The "name" property is made when it is first looked for. The frames are the same before and after.
// The loops take checkFrames() and what it calls through the tiers that the mode reaches in 300 calls: the DFG and
// the FTL with the thresholds of the eager modes, the baseline JIT with the others.
const loopCount = Math.min(testLoopCount, 300);
for (let i = 0; i < loopCount; ++i)
    checkFrames();
await checkAsyncFrames();
for (const target of anonymous) {
    shouldBe(target.name, "default");
    shouldBe(target.bind().name, "bound default");
}
shouldBe(namedStarDefault.name, "starDefault");
for (let i = 0; i < loopCount; ++i)
    checkFrames();
await checkAsyncFrames();

// These two messages have the name of a class declaration.
shouldBe(messageOf(() => ClassDeclaration()), "Cannot call a class constructor default without |new|");
shouldBe(messageOf(() => new DerivedClassDeclaration), "Cannot return a non-object type in the constructor of a derived class default.");

// What tools print for the code of a function: `name#hash`. A default constructor has its code once it ran.
new ClassWithDefaultConstructor;
new DerivedClassWithDefaultConstructor;
for (const target of anonymous)
    shouldBe($vm.codeBlockFor(target).split("#")[0], "default");
shouldBe($vm.codeBlockFor(namedStarDefault).split("#")[0], "starDefault");

// A heap snapshot names an object after its constructor and labels a function with its name.
const instances = [new ClassDeclaration, new ClassExpression];
const classNames = generateHeapSnapshot().nodeClassNames;
shouldBe(classNames.includes("default"), true);
shouldBe(classNames.includes("starDefault"), false);

// A node of this snapshot is seven values. The fifth one is the index of its label.
const snapshot = JSON.parse(generateHeapSnapshotForGCDebugging());
function countOfNodesWithLabel(label) {
    const index = snapshot.labels.indexOf(label);
    let count = 0;
    for (let i = 4; i < snapshot.nodes.length; i += 7) {
        if (snapshot.nodes[i] === index)
            ++count;
    }
    return count;
}
shouldBe(countOfNodesWithLabel("starDefault"), 1);
shouldBe(countOfNodesWithLabel("default") >= anonymous.length, true);
shouldBe(instances.length, 2);
