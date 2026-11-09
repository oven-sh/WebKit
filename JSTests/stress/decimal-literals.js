//@ requireOptions("--useDecimal=1")
// 1.5m. An extension: the proposal has no literals.

function shouldBe(actual, expected, what = "") {
    if (!Object.is(actual, expected))
        throw new Error(`${what} bad value: ${String(actual)}, expected ${String(expected)}`);
}

function shouldThrow(func, errorType) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(`expected ${errorType.name} from ${func}, got ${String(error)}`);
}

// What it evaluates to.
shouldBe(typeof 1.5m, "object");
shouldBe(Object.getPrototypeOf(1.5m), Decimal.prototype);
shouldBe(1.5m.toString(), "1.5");
shouldBe(1m.add(2m).toString(), "3");
shouldBe(0.1m.add(0.2m).equals(0.3m), true);
shouldBe(1m !== 1m, true);
shouldBe(`${1.50m}`, "1.5");
shouldBe([1m, 2m,, 3m].join(), "1,2,,3");
// The semicolon of an expression statement may follow, and the value may be unused.
1m;
1.5m
.5m;
(() => 1m)();
void 1m, 2m;

for (const [source, expected] of [
    ["0m", "0"], ["0.0m", "0"], ["1m", "1"], ["12345678901m", "12345678901"], ["1.m", "1"], [".5m", "0.5"], ["0.5m", "0.5"], ["0e5m", "0"],
    ["1e3m", "1000"], ["1E3m", "1000"], ["1e+3m", "1000"], ["1e-3m", "0.001"], [".5e1m", "5"], ["1.5e-10m", "1.5e-10"], ["1.e2m", "100"],
    ["123.456_789m", "123.456789"], ["1_000m", "1000"], ["1_0.0_1e1_0m", "100100000000"], [".0_1m", "0.01"],
    ["1234567890m", "1234567890"], ["9007199254740993m", "9007199254740993"], ["1e6144m", "1e6144"], ["1e6145m", "Infinity"], ["1e-7000m", "0"],
    ["9999999999999999999999999999999999m", "9999999999999999999999999999999999"],
    // Rounded like the argument of the constructor.
    ["99999999999999999999999999999999995m", "1e35"],
    ["1m/* comment */", "1"], ["1m\n", "1"], ["(1m)", "1"], ["1m.add(.5m)", "1.5"], ["1m?.add(1m)", "2"], ["1m['add'](1m)", "2"],
    ["true ? 1m : 2m", "1"], ["1m, 2m", "2"], ["[1m][0]", "1"], ["{ 1m }", "1"], ["if (1m) 2m", "2"], ["new (class { x = 1.5m })().x", "1.5"],
]) {
    shouldBe(eval(source).toString(), expected, source);
    shouldBe((0, eval)(source).toString(), expected, source);
    if (!/^[{i]/.test(source))
        shouldBe(new Function("return " + source)().toString(), expected, source);
}

// The operators throw, the minus in front of a literal too.
for (const source of ["-1m", "-1.5m", "- -1m", "-0m", "+1m", "~1m", "-(1m, 1m)", "1m + 1m", "1m * 2", "1m < 2m", "1m == 1", "1m ** 2m", "x = 1m, x++", "Math.abs(1m)", "1m | 0"])
    shouldThrow(() => (0, eval)(`(() => { let x; ${source} })()`), TypeError);

for (const source of [
    "1M", "1mm", "1mx", "1m1", "1m_", "1_m", "1._5m", "1.5_m", "1e5_m", "1em", "1e+m", "1.5.5m", "_1m;1_", "0x1m", "0b1m", "0o1m", "0x1.5m", "1nm", "1mn", "1.5n", "0_1m",
    "00m", "01m", "010m", "010.5m", "08m", "08.5m", "09e1m", "0777777777777777777777777m", "1m\\u0061", "1\\u006d", "1m$", "1m 2", "1m ''", "-1m ** 2m",
    "1m = 2", "1m++", "++1m", "for (1m of []);", "let 1m", "({ 1m })", "({ get 1m: 2 })", "export 1m",
])
    shouldThrow(() => new Function(source), SyntaxError);
for (const source of ["00m", "010m", "08m"])
    shouldThrow(() => new Function("'use strict'; " + source), SyntaxError);
// These stay what they were.
shouldBe(eval("08.5"), 8.5);
shouldBe(eval("010"), 8);
shouldThrow(() => eval("1 m"), SyntaxError);
shouldBe(eval("1\nvar m = 2; m"), 2);
shouldBe(JSON.parse("[1.5]")[0], 1.5);
shouldThrow(() => JSON.parse("1.5m"), SyntaxError);

// %Decimal% is used, whatever the program does to the global and the constructor.
{
    const original = globalThis.Decimal;
    const calls = [];
    globalThis.Decimal = function () { calls.push("called"); };
    const value = eval("1.5m");
    delete globalThis.Decimal;
    const other = eval("2.5m");
    globalThis.Decimal = original;
    shouldBe(Object.getPrototypeOf(value), original.prototype);
    shouldBe(other.toString(), "2.5");
    shouldBe(calls.length, 0);
    // Not enumerable, as before.
    delete globalThis.Decimal;
    Object.defineProperty(globalThis, "Decimal", { value: original, writable: true, configurable: true });
}
{
    const other = createGlobalObject();
    shouldBe(Object.getPrototypeOf(other.eval("1m")), other.Decimal.prototype);
}

// Each evaluation makes an object.
{
    const literal = () => 1m;
    const first = literal();
    first.property = 1;
    shouldBe(literal().property, undefined);
    shouldBe(first !== literal(), true);
}

// As the name of a property it is what it is as a key.
{
    const object = { 1.50m: "a", 1e34m: "b", 2m() { return "c"; }, get 3.0m() { return "d"; }, set 3.0m(value) { }, async 4m() { }, *5m() { }, 0m: "e", [6.5m]: "f" };
    shouldBe(Object.keys(object).join(), "0,2,3,4,5,1.5,1e34,6.5");
    shouldBe(object[1.5m], "a");
    shouldBe(object["1.5"], "a");
    shouldBe(object[1e34m], "b");
    shouldBe(object[2](), "c");
    shouldBe(object[2].name, "2");
    shouldBe(object[3.0m], "d");
    const { 1.5m: x, 2m: y, 0m: z = 1, ...rest } = object;
    shouldBe(x, "a");
    shouldBe(y, object[2]);
    shouldBe(z, "e");
    shouldBe("1.5" in rest, false);
    let assigned;
    ({ 1.5m: assigned } = object);
    shouldBe(assigned, "a");
    class C {
        1.5m = "field";
        static 2.5m = "static";
        3.5m() { return "method"; }
        get 4.5m() { return "getter"; }
        static async *5.5m() { }
    }
    shouldBe(new C()["1.5"], "field");
    shouldBe(C["2.5"], "static");
    shouldBe(new C()["3.5"](), "method");
    shouldBe(new C()["4.5"], "getter");
    shouldBe(C["5.5"].name, "5.5");
    shouldBe([..."ab"][1m], "b");
}

// Source text.
shouldBe((() => 1_0.50m).toString(), "() => 1_0.50m");

// Positions where a literal is copied, moved or suspended over.
{
    const f = (a = 1.5m, { b = 2.5m.negate() } = {}, [c = 3m] = []) => a.add(b).add(c).toString();
    shouldBe(f(), "2");
    function* g() { const x = 1m; yield x.add(2m); yield 3m; }
    shouldBe([...g()].join(), "3,3");
    let result;
    (async () => { result = (await 1m).add(await 2m); })();
    drainMicrotasks();
    shouldBe(result.toString(), "3");
    switch (true) {
    case 1m.equals(1.0m):
        break;
    default:
        throw new Error("switch");
    }
    label: for (let i = 0m; i.lessThan(3m); i = i.add(1m))
        continue label;
    shouldBe(((x, y) => x ?? y)(null, 2m).toString(), "2");
    shouldBe(String.raw`${1.5m}`, "1.5");
    shouldBe((x => x)`${1m}`.length, 2);
}

// In every tier.
{
    function sum(count) {
        let result = 0m;
        for (let i = 0; i < count; ++i)
            result = result.add(0.1m);
        return result;
    }
    noInline(sum);
    for (let i = 0; i < testLoopCount / 100; ++i)
        shouldBe(sum(10).equals(1m), true);
}

// A 16-bit source.
shouldBe(String(eval("/*\u4e00*/1_0.5e1m")), "105");
shouldThrow(() => eval("/*\u4e00*/1mx"), SyntaxError);
shouldThrow(() => eval("/*\u4e00*/01m"), SyntaxError);

// Names of members.
{
    let value;
    const object = new class { set 1m(v) { value = v; } async 2m() { } static get 3m() { return 4m; } };
    object["1"] = 5m;
    shouldBe(String(value), "5");
    shouldBe(object["2"]() instanceof Promise, true);
    shouldBe(String(object.constructor["3"]), "4");
}
