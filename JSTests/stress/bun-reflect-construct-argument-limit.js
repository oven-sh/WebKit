function shouldBe(actual, expected, label) {
    if (actual !== expected)
        throw new Error(`${label}: expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrow(f, constructor, label) {
    let error;
    try {
        f();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof constructor))
        throw new Error(`${label}: expected a ${constructor.name} but got ${String(error)}`);
}

const thousandElements = Array.from({ length: 1000 }, (_, index) => index);

function Recorder(...constructorArguments) {
    this.constructorArguments = constructorArguments;
}

function testLengthOverLimit() {
    for (const length of [0x100001, 2 ** 32]) {
        let elementReadCount = 0;
        const argumentsList = { length, get 0() { ++elementReadCount; return "a"; } };
        shouldThrow(() => Reflect.construct(function () { }, argumentsList), RangeError, `length ${length}`);
        shouldBe(elementReadCount, 0, `elements read with length ${length}`);
    }
}

function testLengthReadOnce() {
    let lengthReadCount = 0;
    const instance = Reflect.construct(Recorder, { get length() { ++lengthReadCount; return 2; }, 0: "a", 1: "b" });
    shouldBe(instance.constructorArguments.join(), "a,b", "arguments with a length getter");
    shouldBe(lengthReadCount, 1, "reads of length");
}

function testElementOrder() {
    const reads = [];
    const elements = { length: 3, 0: "a", get 1() { reads.push("getter"); return "b"; }, 2: "c" };
    const argumentsList = new Proxy(elements, { get(target, key, receiver) { reads.push(key); return Reflect.get(target, key, receiver); } });
    shouldBe(Reflect.construct(Recorder, argumentsList).constructorArguments.join(), "a,b,c", "arguments");
    shouldBe(reads.join(), "length,0,1,getter,2", "reads");
}

function testThrowingElement() {
    let laterReadCount = 0;
    const argumentsList = { length: 3, 0: "a", get 1() { throw new EvalError("element"); }, get 2() { ++laterReadCount; return "c"; } };
    shouldThrow(() => Reflect.construct(Recorder, argumentsList), EvalError, "throwing element");
    shouldBe(laterReadCount, 0, "elements read after the throwing element");
}

function testThousandElements() {
    const { constructorArguments } = Reflect.construct(Recorder, thousandElements);
    shouldBe(constructorArguments.length, 1000, "argument count");
    shouldBe(constructorArguments[999], 999, "last argument");
}

const tests = [testLengthOverLimit, testLengthReadOnce, testElementOrder, testThrowingElement, testThousandElements];
for (const test of tests)
    noInline(test);
for (let i = 0; i < testLoopCount; ++i) {
    for (const test of tests)
        test();
}
