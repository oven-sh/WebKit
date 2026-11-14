function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`bad value: ${message}`);
}

// CreateDynamicFunction takes the [[Prototype]] of the function from new.target, also when new.target is %Function%
// and the constructor that is called is GeneratorFunction, AsyncFunction or AsyncGeneratorFunction.
function constructorsOf(global) {
    return ["function () { }", "function* () { }", "async function () { }", "async function* () { }"].map(source => global.eval(`(${source})`).constructor);
}
const constructors = [...constructorsOf(globalThis), ...constructorsOf(createGlobalObject())];
const reflectConstruct = Reflect.construct;

function test() {
    for (const [calleeIndex, callee] of constructors.entries()) {
        for (const [newTargetIndex, newTarget] of constructors.entries()) {
            const message = `callee ${calleeIndex}, new.target ${newTargetIndex}`;
            shouldBe(Object.getPrototypeOf(Reflect.construct(callee, ["return 1"], newTarget)), newTarget.prototype, message);
            shouldBe(Object.getPrototypeOf(reflectConstruct(callee, ["return 1"], newTarget)), newTarget.prototype, `${message}, aliased`);
        }
    }
}
noInline(test);

for (let i = 0; i < testLoopCount; i += 100)
    test();

for (const [index, constructor] of constructors.entries()) {
    // The callee is recognized as new.target because its "prototype" cannot change.
    const descriptor = Object.getOwnPropertyDescriptor(constructor, "prototype");
    shouldBe(descriptor.writable || descriptor.configurable, false, `descriptor ${index}`);

    // A new.target that is the callee needs no lookup: the function has the structure of the call form.
    const structure = $vm.getStructureTransitionList(constructor("return 1"))[0];
    shouldBe($vm.getStructureTransitionList(new constructor("return 1"))[0], structure, `structure of new ${index}`);
    shouldBe($vm.getStructureTransitionList(Reflect.construct(constructor, ["return 1"], constructor))[0], structure, `structure of Reflect.construct ${index}`);
}
