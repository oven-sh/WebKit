//@ requireOptions("--useDecimal=1", "--useDecimalOperators=1")
// Operators are not in https://tc39.es/proposal-decimal/, where they throw, but for == and !=, which compare identity.
// decimal-operators-off.js has those tests.

function shouldBe(actual, expected, what = "") {
    if (!Object.is(actual, expected))
        throw new Error(`${what} bad value: ${String(actual)}, expected ${String(expected)}`);
}

function shouldBeDecimal(actual, expected, what) {
    if (!(actual instanceof Decimal))
        throw new Error(`${what} not a Decimal: ${String(actual)}`);
    shouldBe(actual.toString(), expected.toString(), what);
}

function shouldThrow(func, errorType, what = "") {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(`${what} expected ${errorType.name}, got ${String(error)}`);
}

const values = ["NaN", "Infinity", "-Infinity", "0", "-0", "1", "-1", "0.1", "0.2", "-2.5", "3", "1e6144", "1e-6176", "9999999999999999999999999999999999"].map(s => new Decimal(s));

// Every operator is the method of the same meaning.
const arithmetic = {
    add: (a, b) => a + b,
    subtract: (a, b) => a - b,
    multiply: (a, b) => a * b,
    divide: (a, b) => a / b,
    remainder: (a, b) => a % b,
};
const comparisons = {
    equals: (a, b) => a == b,
    lessThan: (a, b) => a < b,
    lessThanOrEqual: (a, b) => a <= b,
    greaterThan: (a, b) => a > b,
    greaterThanOrEqual: (a, b) => a >= b,
};
for (let x of values) {
    for (let y of values) {
        const operands = `${x} ${y}`;
        for (let name in arithmetic)
            shouldBeDecimal(arithmetic[name](x, y), x[name](y), `${name} ${operands}`);
        for (let name in comparisons)
            shouldBe(comparisons[name](x, y), x[name](y), `${name} ${operands}`);
        // The opposite of ==, unlike notEquals().
        shouldBe(x != y, !x.equals(y), `!= ${operands}`);
    }
    shouldBeDecimal(-x, x.negate(), `- ${x}`);
}

// Literals.
shouldBe(0.1m + 0.2m == 0.3m, true);
shouldBe(String(0.1m + 0.2m), "0.3");
shouldBe(String(-1.5m), "-1.5");
shouldBe(String(1m + 2m * 3m - 4m / 8m % 3m), "6.5");
shouldBe(String((1m + 2m) * -(3m)), "-9");

// Assignments, to everything that can be assigned to.
{
    let a = 1.5m;
    a += 1m; shouldBe(String(a), "2.5");
    a -= 0.5m; shouldBe(String(a), "2");
    a *= 3m; shouldBe(String(a), "6");
    a /= 4m; shouldBe(String(a), "1.5");
    a %= 1m; shouldBe(String(a), "0.5");
    shouldBe(String(a++), "0.5"); shouldBe(String(a), "1.5");
    shouldBe(String(++a), "2.5");
    shouldBe(String(a--), "2.5"); shouldBe(String(--a), "0.5");
    a = a++; shouldBe(String(a), "0.5");
    let captured = 1m;
    (() => { captured += 1m; captured++; })();
    shouldBe(String(captured), "3");
    let o = { p: 1m, 0: 2m, get g() { return 5m; }, set g(v) { this.set = v; } };
    shouldBe(String(o.p++), "1"); shouldBe(String(++o.p), "3"); shouldBe(String(o.p += 2m), "5");
    let key = 0;
    shouldBe(String(o[key]++), "2"); shouldBe(String(--o[key]), "2"); shouldBe(String(o[key] *= 2m), "4");
    shouldBe(String(o.g--), "5"); shouldBe(String(o.set), "4");
    class C {
        #f = 1m;
        static s = 1m + 1m;
        #v = 7m;
        get #g() { return this.#v; }
        set #g(v) { this.#v = v; }
        run() { let old = this.#f++; ++this.#f; this.#f -= 1m; this.#g++; return `${old} ${this.#f} ${this.#v}`; }
    }
    shouldBe(new C().run(), "1 2 8");
    shouldBe(String(C.s), "2");
    class D extends (class { get x() { return 1m; } }) { run() { return String(super.x + 1m); } }
    shouldBe(new D().run(), "2");
    globalThis.global = 1m;
    global += 1m; global++;
    shouldBe(String(global), "3");
    const constant = 1m;
    shouldThrow(() => { constant++; }, TypeError);
    shouldThrow(() => { constant += 1m; }, TypeError);
    shouldBe(String(constant), "1");
    shouldThrow(() => { tdz++; let tdz; }, ReferenceError);
}

// The operands are evaluated once each, left to right, and before the operation.
{
    let log = [];
    let f = (name, value) => { log.push(name); return value; };
    f("a", 1m) + f("b", 2m);
    f("c", 1m) == f("d", 2m);
    let a = 1m;
    shouldBe(String(a + (a = 5m)), "6");
    a = 1m;
    a += (a = 5m);
    shouldBe(String(a), "6");
    a = 2m;
    shouldBe(String(a++ * a), "6");
    shouldBe(log.join(), "a,b,c,d");
}

// Conditions and control flow.
{
    let count = 0;
    for (let i = 0m; i < 10m; i++)
        ++count;
    shouldBe(count, 10);
    for (let i = 10m; i >= 0.5m; i -= 0.5m)
        ++count;
    shouldBe(count, 30);
    let i = 0m;
    while (!(i == 5m)) i++;
    do { i--; } while (i != 0m);
    shouldBe(String(i), "0");
    shouldBe(1m < 2m && 2m > 1m || false, true);
    shouldBe(1m > 2m ? "a" : "b", "b");
    if (1m >= 2m) throw new Error();
    if (NaN_() == NaN_()) throw new Error();
    if (!(NaN_() != NaN_())) throw new Error();
    if (NaN_() < 1m) throw new Error();
    if (NaN_() >= 1m) throw new Error();
    function NaN_() { return new Decimal("NaN"); }
}

// Results in ignored positions.
{
    let a = 1m;
    a + a;
    a == a;
    -a;
    a < a;
    void (a + a);
}

// Generators, async functions, eval, Function and arguments.
{
    function* g(a) { a += 1m; yield a++; yield a + (yield -a); }
    let it = g(1m);
    shouldBe(String(it.next().value), "2");
    shouldBe(String(it.next().value), "-3");
    shouldBe(String(it.next(4m).value), "7");
    let result;
    (async a => { result = a + await 2m; })(1m);
    drainMicrotasks();
    shouldBe(String(result), "3");
    shouldBe(String(eval("1m + 2m")), "3");
    shouldBe(String(Function("a", "b = a + 1m", "return a + b")(1m)), "3");
    function f(a) { arguments[0]++; a += 1m; return arguments[0]; }
    shouldBe(String(f(1m)), "3");
    shouldBe(`${1m + 1m}`, "2");
    let { x = 1m + 1m } = {};
    shouldBe(String(x), "2");
}

// Identity and everything that uses it is not affected.
{
    const a = 1m;
    shouldBe(a === a, true);
    shouldBe(1m === 1m, false);
    shouldBe(1m !== 1m, true);
    shouldBe(Object.is(1m, 1m), false);
    shouldBe([1m].includes(1m), false);
    switch (1m) { case 1m: throw new Error(); }
}

// A Decimal and something else: as in the proposal.
for (let other of [1, 1.5, 1n, "a", "", true, undefined, null, Symbol(), {}, [], () => { }, Object(1), new Proxy({}, {})]) {
    for (let name in arithmetic) {
        shouldThrow(() => arithmetic[name](1m, other), TypeError, name);
        shouldThrow(() => arithmetic[name](other, 1m), TypeError, name);
    }
    for (let name in comparisons) {
        if (name == "equals")
            continue;
        shouldThrow(() => comparisons[name](1m, other), TypeError, name);
        shouldThrow(() => comparisons[name](other, 1m), TypeError, name);
    }
}
shouldBe(1m == null, false);
shouldBe(undefined == 1m, false);
shouldBe(1m == {}, false);
shouldBe({} != 1m, true);
shouldThrow(() => 1m == 1, TypeError);
shouldThrow(() => "1" != 1m, TypeError);
shouldThrow(() => new Proxy(1m, {}) + 1m, TypeError);
shouldThrow(() => Object.create(1m) + 1m, TypeError);
shouldThrow(() => Decimal.prototype + 1m, TypeError);

// No operator.
shouldThrow(() => +1m, TypeError);
for (let source of ["a ** b", "a & b", "a | b", "a ^ b", "a << b", "a >> b", "a >>> b", "~a"])
    shouldThrow(() => Function("a", "b", "return " + source)(1m, 1m), TypeError, source);

// Objects that are not Decimals are converted as ever, once each and in order.
{
    let log = [];
    let o = (name, value) => ({ [Symbol.toPrimitive](hint) { log.push(name + hint); return value; } });
    shouldBe(o("a", 1) + o("b", 2), 3);
    shouldBe(o("c", 3) - o("d", 2), 1);
    shouldBe(o("e", 3) < o("f", 2), false);
    shouldBe(-o("g", 3), -3);
    let x = o("h", 3);
    shouldBe(x++, 3);
    shouldBe(x, 4);
    x = o("i", 3n);
    shouldBe(--x, 2n);
    shouldBe(o("j", 1) == 1, true);
    shouldBe(log.join(), "adefault,bdefault,cnumber,dnumber,enumber,fnumber,gnumber,hnumber,inumber,jdefault");
    shouldThrow(() => o("k", 1) + 1m, TypeError);
    shouldBe(log.at(-1), "kdefault");
    const same = {};
    shouldBe(same == same, true);
    shouldBe([1] + [2], "12");
}

// Neither valueOf nor the properties of the prototype are looked at.
{
    class Sub extends Decimal {
        get valueOf() { throw new Error("valueOf"); }
        get [Symbol.toPrimitive]() { throw new Error("toPrimitive"); }
        add() { throw new Error("add"); }
    }
    const result = new Sub("1") + new Sub("2");
    shouldBe(result.constructor, Decimal);
    shouldBe(String(result), "3");
}

// Other realms: the result is of the realm of the code.
{
    const other = createGlobalObject();
    const foreign = new other.Decimal("2");
    const result = 1m + foreign;
    shouldBe(Object.getPrototypeOf(result), Decimal.prototype);
    shouldBe(String(result), "3");
    shouldBe(Object.getPrototypeOf(other.Function("a", "return a + a")(1m)), other.Decimal.prototype);
    shouldBe(foreign == 2m, true);
}

// Replacing the constructor changes nothing.
{
    const D = Decimal;
    globalThis.Decimal = null;
    shouldBe(Object.getPrototypeOf(1m + 1m), D.prototype);
    globalThis.Decimal = D;
}

// Every tier, and code that has been optimized for something else.
{
    function all(a, b) {
        let c = a + b;
        c -= b;
        c = c * b / b;
        let old = c++;
        --c;
        if (!(old == c) || c != old || !(c <= old) || !(c >= old) || c < old || c > old)
            throw new Error("bad comparison");
        return [c, -c, a % b];
    }
    noInline(all);
    const kinds = [
        [1, 2, "1,-1,1"],
        [1m, 2m, "1,-1,1"],
        [1n, 2n, "1,-1,1"],
        [2.5, 2, "2.5,-2.5,0.5"],
        [2.5m, 2m, "2.5,-2.5,0.5"],
        [{ valueOf() { return 2.5; } }, { valueOf() { return 2; } }, "2.5,-2.5,0.5"],
    ];
    for (let kind of [...kinds, ...kinds.toReversed()]) {
        for (let i = 0; i < testLoopCount / 10; ++i)
            shouldBe(String(all(kind[0], kind[1])), kind[2]);
    }
    for (let i = 0; i < testLoopCount; ++i) {
        const kind = kinds[i % kinds.length];
        shouldBe(String(all(kind[0], kind[1])), kind[2]);
    }

    // Nothing may conclude that the result of an operator is no object.
    function use(a, b) {
        const r = a * b;
        return (typeof r) + (r instanceof Object) + (r === r) + String(r);
    }
    noInline(use);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(use(i & 1 ? 2 : 2m, i & 1 ? 3 : 3m), i & 1 ? "numberfalsetrue6" : "objecttruetrue6");
    }

    function sum(n, zero, one) {
        let total = zero;
        for (let i = zero; i < n; i++)
            total += i * one;
        return total;
    }
    noInline(sum);
    for (let i = 0; i < testLoopCount / 200; ++i) {
        shouldBe(String(sum(100m, 0m, 1m)), "4950");
        shouldBe(sum(100, 0, 1), 4950);
    }
}

// A typeof in front of the operator, of another statement or of an operand.
{
    const tests = [
        (a, b, x) => { let kind = typeof x; return a != b; },
        (a, b, x) => { let kind = typeof x; return !(a == b); },
        (a, b, x) => { let kind = typeof x; if (a != b) return true; return false; },
        (a, b, x) => { typeof x; return a != b; },
        (a, b, x) => { let kind = typeof x; return a < b; },
        (a, b, x) => { let kind = typeof x; return a > b; },
    ];
    for (const test of tests) {
        for (let i = 0; i < testLoopCount / 20; ++i) {
            shouldBe(test(1.5m, 1.5m, 0), false, String(test));
            shouldBe(test(1, 1, 0), false, String(test));
        }
    }
    shouldBe(tests[0](1m, 2m, 0), true);
    shouldBe(tests[0]("a", "b", 0), true);
    const o = 1m;
    shouldBe(typeof o != "object", false);
    shouldBe(typeof o != "string", true);
    shouldBe("object" != typeof o, false);
    shouldBe(typeof o !== "object", false);
}

// Code that has seen one kind of operands meets another, and a realm that has only seen the Decimals of another.
for (const [operator, method] of [["+", "add"], ["-", "subtract"], ["*", "multiply"], ["/", "divide"], ["%", "remainder"], ["==", "equals"], ["<", "lessThan"], ["<=", "lessThanOrEqual"], [">", "greaterThan"], [">=", "greaterThanOrEqual"]]) {
    const object = { valueOf() { return 7; } }, number = { valueOf() { return 2; } };
    const ordinary = operator === "==" ? false : eval(`7 ${operator} 2`);
    for (const order of [0, 1]) {
        const f = new Function("a", "b", `return a ${operator} b; // ${order}`);
        noInline(f);
        for (let turn = 0; turn < 2; ++turn) {
            for (let i = 0; i < testLoopCount; ++i) {
                if (turn === order)
                    shouldBe(String(f(7m, 2m)), String(7m[method](2m)), operator);
                else
                    shouldBe(f(object, number), ordinary, operator);
            }
        }
        for (const [a, b] of [[7m, number], [object, 2m]]) {
            let error;
            try {
                shouldBe(f(a, b), false, operator);
            } catch (e) {
                error = e;
            }
            shouldBe(error instanceof TypeError, operator !== "==", operator);
        }
    }

    const other = createGlobalObject();
    const g = other.eval(`(function (a, b, rarely) { if (rarely) return -a; return a ${operator} b; })`);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(String(g(7m, 2m)), String(7m[method](2m)), operator);
    shouldBe(Object.getPrototypeOf(g(7m, 2m, true)), other.Decimal.prototype, operator);
}
{
    function notEqual(a, b) { return a != b; }
    noInline(notEqual);
    const nan = new Decimal(NaN);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(notEqual(nan, nan), true);
        shouldBe(notEqual(0m, -0m), false);
    }
}
