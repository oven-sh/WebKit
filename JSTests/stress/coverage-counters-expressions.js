load("./resources/coverage-counters.js", "caller relative");

// && || ?? ?:
test(`function (a, b) { return a && @2 b; }`, [0, 0], [1, 0], [1, 1]);
test(`function (a, b) { return a || @1 b; }`, [0, 0], [1, 0], [1, 1]);
test(`function (a, b) { return a ?? @2 b; }`, [0, 0], [null, 0], [undefined, 1]);
test(`function (a, b) { a && @1 b(); a || @1 b(); a ?? @0 b(); }`, [0, () => {}], [1, () => {}]);
test(`function (a, b, c) { if (a && @2 b || @3 c) @2 @!2 return 1; }`, [0, 0, 0], [0, 0, 1], [1, 0, 0], [1, 1, 0]);
test(`function (a, b, c) { if (a || @3 b && @2 c) @2 @!2 return 1; }`, [1, 0, 0], [0, 0, 0], [0, 1, 0], [0, 1, 1]);
test(`function (a, b) { if (!(a && @1 b)) @1 @!1 return 1; }`, [0, 0], [1, 1]);
test(`function (a, b) { if (a ?? @2 b) @2 @!2 return 1; }`, [0, 0], [1, 0], [null, 0], [null, 1]);
test(`function (a, b) { return a@2 ?.b ?? @2 b; }`, [null, 0], [{}, 0], [{ b: 1 }, 0]);
test(`function (a, b) { if (a@2 ?.b ?? @2 b) @1 @!2 return 1; }`, [null, 0], [{}, 0], [{ b: 1 }, 0]);
test(`function (a) { return a ? @1 1 : @2 2; }`, [0], [1], [""]);
test(`function (a, b, c) { if (a ? @1 b : @1 c) @1 @!1 return 1; }`, [0, 0, 0], [1, 1, 0]);
test(`function (a, b) { return a && @1 (@1 (@1 b)); }`, [0, 0], [1, 0]);
test(`function (a, b) { return 1 + (@2 a || b); }`, [0, 0], [1, 0]);
test(`function () { return [0 && @0 1, 1 && @1 1, null ?? @1 1]; }`, []);
test(`function (a) { "use strict"; return a && @1 a(); }`, [null], [() => 1]);

// &&= ||= ??=
test(`function (a) { a &&= @1 2; return a; }`, [0], [1]);
test(`function (a) { (() => a); a ||= @1 2; return a; }`, [0], [1]);
test(`function (b) { const a = { b }; a.b ??= @1 2; a["c"] ??= @2 3; return a; }`, [null], [1]);
test(`function (b) { const a = b; a &&= @1 2; }`, [0], [1]);

// ?.
test(`function (a) { return a@2 ?.b@1 ?.c; }`, [null], [{}], [{ b: { c: 1 } }]);
test(`function (a) { return a /* b */ @1 ?.[0]; }`, [null], [[1]]);
test(`function (a) { return a @1 ?.(); }`, [null], [() => 1]);
test(
    `function (a) { return a@2 ?.b@1 ?.(); }`,
    [null],
    [{}],
    [
        {
            b() {
                return this.c;
            },
            c: 1,
        },
    ],
);
test(`function (a) { return [a@1 ?.call(1), a@1 ?.apply(1, []), a@1 ?.hasOwnProperty("b")]; }`, [null], [() => 1]);
test(`function (a) { return [delete a@1 ?.b, delete a@1 ?.["b"]]; }`, [null], [{}]);
test(
    `function (a) { return (a@1 ?.b)(); }`,
    [null],
    [
        {
            b() {
                return this.c;
            },
            c: 1,
        },
    ],
);
test(`function (a) { if (a@2 ?.b) @1 @!2 return 1; }`, [null], [{}], [{ b: 1 }]);
test(`function (a) { return (a@2 ?.b)@1 ?.c; }`, [null], [{}], [{ b: { c: 1 } }]);
test(`function (a) { return [/a/@1 ?.test(a), arguments@1 ?.length]; }`, ["a"]);

// Default values.
test(`function (a = @1 1) @2 { return a; }`, [], [2]);
test(`function (a, b = @1 a()) @0 { }`, [null]);
test(`function ({ a = @1 1 }, [b = @1 2]) { return a + b; }`, [{}, [1]], [{ a: 1 }, []]);
test(`function (c) { let a, b; ({ a = @1 1 } = c); [b = @2 2] = []; return a + b; }`, [{}], [{ a: 1 }]);
test(`(a = @1 1) => @2 a`, [], [2]);
