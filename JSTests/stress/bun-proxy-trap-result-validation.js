function shouldBe(actual, expected) {
    if (!Object.is(actual, expected))
        throw new Error(`expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrow(operation, errorConstructor) {
    let error;
    try {
        operation();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorConstructor))
        throw new Error(`expected a ${errorConstructor.name} but got ${String(error)}: ${operation}`);
}

function repeat(check) {
    noInline(check);
    for (let i = 0; i < testLoopCount; ++i)
        check();
}

let answers = { };
const handler = { get: (target, key) => answers[key], set: () => true };

function shouldAcceptAnswer(proxy, key, answer, read) {
    answers = { [key]: answer };
    noInline(read);
    repeat(() => {
        shouldBe(read(proxy), answer);
        shouldBe(Reflect.get(proxy, key), answer);
    });
}

function shouldRejectAnswer(proxy, key, answer, read) {
    answers = { [key]: answer };
    noInline(read);
    repeat(() => {
        shouldThrow(() => read(proxy), TypeError);
        shouldThrow(() => Reflect.get(proxy, key), TypeError);
    });
}

function shouldAcceptStore(proxy, key, value, store) {
    noInline(store);
    repeat(() => {
        store(proxy, value);
        shouldBe(Reflect.set(proxy, key, value), true);
    });
}

function shouldRejectStore(proxy, key, value, store) {
    noInline(store);
    repeat(() => {
        shouldThrow(() => store(proxy, value), TypeError);
        shouldThrow(() => Reflect.set(proxy, key, value), TypeError);
    });
}

const symbol = Symbol.for("s");
const target = { free: 1 };
Object.defineProperty(target, "fixed", { value: 2 });
Object.defineProperty(target, "writable", { value: 3, writable: true });
Object.defineProperty(target, "nan", { value: NaN });
Object.defineProperty(target, "zero", { value: 0 });
Object.defineProperty(target, "text", { value: "ab" });
Object.defineProperty(target, "noGetter", { set() { } });
Object.defineProperty(target, "getter", { get() { return 1; } });
Object.defineProperty(target, symbol, { value: 4 });
Object.defineProperty(target, 7, { value: 5 });
const proxy = new Proxy(target, handler);

for (const answer of [undefined, -0, "text", { }]) {
    shouldAcceptAnswer(proxy, "free", answer, p => p.free);
    shouldAcceptAnswer(proxy, "absent", answer, p => p.absent);
    shouldAcceptAnswer(proxy, "writable", answer, p => p.writable);
    shouldAcceptAnswer(proxy, "getter", answer, p => p.getter);
}
shouldAcceptAnswer(proxy, "fixed", 2, p => p.fixed);
shouldAcceptAnswer(proxy, "fixed", Math.sqrt(4), p => p.fixed);
shouldAcceptAnswer(proxy, "nan", NaN, p => p.nan);
shouldAcceptAnswer(proxy, "zero", 0, p => p.zero);
shouldAcceptAnswer(proxy, "text", ["a", "b"].join(""), p => p.text);
shouldAcceptAnswer(proxy, "noGetter", undefined, p => p.noGetter);
shouldAcceptAnswer(proxy, symbol, 4, p => p[symbol]);
shouldAcceptAnswer(proxy, 7, 5, p => p[7]);

shouldRejectAnswer(proxy, "fixed", 3, p => p.fixed);
shouldRejectAnswer(proxy, "nan", undefined, p => p.nan);
shouldRejectAnswer(proxy, "zero", -0, p => p.zero);
shouldRejectAnswer(proxy, "text", "abc", p => p.text);
shouldRejectAnswer(proxy, "noGetter", 1, p => p.noGetter);
shouldRejectAnswer(proxy, symbol, 5, p => p[symbol]);
shouldRejectAnswer(proxy, 7, 6, p => p[7]);

shouldAcceptStore(proxy, "free", 9, (p, value) => { p.free = value; });
shouldAcceptStore(proxy, "absent", 9, (p, value) => { p.absent = value; });
shouldAcceptStore(proxy, "writable", 9, (p, value) => { p.writable = value; });
shouldAcceptStore(proxy, "fixed", 2, (p, value) => { p.fixed = value; });
shouldAcceptStore(proxy, "noGetter", 1, (p, value) => { p.noGetter = value; });
shouldAcceptStore(proxy, 7, 5, (p, value) => { p[7] = value; });
shouldRejectStore(proxy, "fixed", 9, (p, value) => { p.fixed = value; });
shouldRejectStore(proxy, "getter", 1, (p, value) => { p.getter = value; });
shouldRejectStore(proxy, symbol, 5, (p, value) => { p[symbol] = value; });
shouldRejectStore(proxy, 7, 6, (p, value) => { p[7] = value; });

const math = new Proxy(Math, handler);
shouldAcceptAnswer(math, "PI", Math.PI, p => p.PI);
shouldRejectAnswer(math, "PI", 3, p => p.PI);
shouldAcceptAnswer(math, "max", 1, p => p.max);

const symbolConstructor = new Proxy(Symbol, handler);
shouldAcceptAnswer(symbolConstructor, "iterator", Symbol.iterator, p => p.iterator);
shouldRejectAnswer(symbolConstructor, "iterator", Symbol.hasInstance, p => p.iterator);
shouldAcceptAnswer(symbolConstructor, "keyFor", 1, p => p.keyFor);

class Empty { }
const classProxy = new Proxy(Empty, handler);
shouldAcceptAnswer(classProxy, "prototype", Empty.prototype, p => p.prototype);
shouldRejectAnswer(classProxy, "prototype", { }, p => p.prototype);

const array = [1];
const arrayProxy = new Proxy(array, handler);
const readLength = p => p.length;
shouldAcceptAnswer(arrayProxy, "length", 5, readLength);
Object.freeze(array);
shouldAcceptAnswer(arrayProxy, "length", 1, readLength);
shouldRejectAnswer(arrayProxy, "length", 5, readLength);

const createPair = () => ({ a: 1, b: 2 });
const readA = p => p.a;
const pairProxy = new Proxy(createPair(), handler);
shouldAcceptAnswer(pairProxy, "a", 5, readA);
const otherPair = createPair();
const otherPairProxy = new Proxy(otherPair, handler);
shouldAcceptAnswer(otherPairProxy, "a", 5, readA);
Object.defineProperty(otherPair, "a", { writable: false, configurable: false });
shouldRejectAnswer(otherPairProxy, "a", 5, readA);
shouldAcceptAnswer(otherPairProxy, "a", 1, readA);
shouldAcceptAnswer(otherPairProxy, "b", 5, p => p.b);
shouldAcceptAnswer(pairProxy, "a", 5, readA);
