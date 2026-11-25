load("./resources/coverage-counters.js", "caller relative");

// Functions and classes.
test(`function () { const a = @1 () => @0 1, b = @1 function () { }, c = @1 class { }, d = @1 (@1 () => { }); return [a.name, b.name, c.name, d.name]; }`, []);
test(`function () { return (() => @1 ({ a: 1 }))(); }`, []);
test(`function () { function* a() @1 { @1 yield 1; @0 yield 2; } a(); return a().next(); }`, []);
test(`function () { async function* a() @1 { @1 yield 1; @0 yield 2; } a(); a().next(); }`, []);
test(`async function () @1 { }`, []);
test(`async function (a) @2 { @2 await a; @1 return 1; }`, [1], [{ then() {} }]);
test(`async (a) => @1 await a`, [1]);
test(`async (a) => @1 a`, [1]);
test(`function* (a = @1 1) @0 { }`, []);
test(`function () { class A { a = @2 1; b = @2 (2); c = @2 (3, 4); #d = @2 5; ["e"] = @2 6; static f = @1 7; static { @1 this.g = 8; } } new A; return [new A, { ...A }]; }`, []);
test(`function () { class A { a = @1 () => @0 1; b = @1 (this.a || @0 2) && @1 3; } return new A().a.name; }`, []);
test(`function () { class A { constructor() @2 { } a() @1 { } get b() @1 { } set b(c) @0 { } static c() @0 { } #d() @0 { } } class B extends A { } new B; const a = new A; a.a(); a.b; }`, []);
test(`function () { @1 class A extends Object { constructor() @1 { @1 try { super(); return; } catch @0 { } } } new A; }`, []);
test(`function () { function inlined(a) @6 { return a ? @3 1 : @3 2; } let sum = 0; for (let i = 0; i < 6; i++) sum += inlined(i & 1); return sum; }`, []);
test(`function f(a) @4 { if (a) @3 @!1 f(a - 1); @4 return a; }`, [3]);
test(`function () { const [a, b] = @1 [1, 2], { c } = @1 { c: 3 }; return a + b + c; }`, []);

// The code does what it does wherever the places are.
{
    const source = `(function (o) {
        var local = 1;
        (o.self)().called = (eval)("local");
        (o.a) = typeof (undeclared);
        [(o.b), (o.c) = 2] = [1];
        delete (o.self);
        return o;
    })`;
    const places = [];
    for (let offset = 0; offset < source.length + 2; offset++) places.push(offset, 0, offset, 1 + 2 ** 30);
    const counted = $vm.evaluateWithCoverage(source, new Uint32Array(places), 2);
    shouldBe(
        counted({
            self() {
                return this;
            },
        }),
        { called: 1, a: "undefined", b: 1, c: 2 },
        "everywhere",
    );
}

// A counter that does not fit a narrow or a 16 bit operand.
for (const counter of [127, 128, 255, 256, 32768, 65536]) {
    const counted = $vm.evaluateWithCoverage(`(function () { })`, new Uint32Array([13, counter]), counter + 1);
    counted();
    shouldBe($vm.coverageCounts(counted).indexOf(1), counter, "wide");
}

// The completion value of a program.
for (const marked of [`@1 1; @1 ;`, `@1 1; @1 if (false) @0 @!1 2;`, `@1 1; @1 do @1 { @1 2; @1 break; } while (true);`]) {
    const { source, places, expected } = parse(marked);
    shouldBe($vm.evaluateWithCoverage(source, places, expected.length), (0, eval)(source), marked);
}

// A program that could run without code.
{
    let caller;
    Object.defineProperty(globalThis, "jsonp", { set() { caller = new Error().stack.split("\n")[1].split("@")[0]; } });
    $vm.evaluateWithCoverage(`jsonp = {}`, new Uint32Array([0, 0]), 1);
    shouldBe(caller, "global code", "JSONP");
}

// Sources with the same text share code only if they have the same places, and never counts.
{
    const evaluate = (marked) => {
        const { source, places, expected } = parse(marked);
        return $vm.evaluateWithCoverage(source, places, expected.length);
    };
    const run = (f) => {
        for (let i = 0; i < testLoopCount; i++) f(i & 1);
        return f;
    };
    const even = Math.ceil(testLoopCount / 2);
    run(loadString(`(function (a) { return a ? 1 : 2; })`));
    const first = run(evaluate(`(function (a) { return a ? @0 1 : @0 2; })`));
    const second = run(evaluate(`(function (a) { return a ? @0 1 : @0 2; })`));
    const third = run(evaluate(`(function (a) { return a ? 1 : @0 2; })`));
    run(loadString(`(function (a) { return a ? 1 : 2; })`));
    shouldBe($vm.coverageCounts(first), [testLoopCount - even, even], "first");
    shouldBe($vm.coverageCounts(second), [testLoopCount - even, even], "second");
    shouldBe($vm.coverageCounts(third), [even], "third");
}

// A loop that is entered in one tier and left in another.
{
    const { source, places, expected } = parse(`(function (n) { let sum = 0; for (let i = 0; i < n; i++) @0 { if (i & 1) @0 @!0 do @0 sum++; while (false); } return sum; })`);
    const counted = $vm.evaluateWithCoverage(source, places, expected.length);
    const half = testLoopCount * 5;
    counted(2 * half);
    shouldBe($vm.coverageCounts(counted), [2 * half, half, half, half], "OSR entry");
}
