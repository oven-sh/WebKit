load("./resources/coverage-counters.js", "caller relative");

test(`function (a) @2 { @2 a.b; @1 return 1; @0 a.c; }`, [null], [{}]);
test(`function () { @1 "use strict"; @1 return typeof this; }`, []);
test(`function () { @1 ; @1 { @1 ; } @1 var a; @1 let b; @1 const c = @1 1; @1 class d { } @1 function e() @0 { } @1 debugger; }`, []);
test(`function (a) { @1 label: @1 { @1 break label; @0 a(); } @1 with (a) @1 return b; }`, [{ b: 1 }]);
test(`function (a) { @1 a(), @0 a(); }`, [null]);
test(`function (a) { @1 @1 return a; }`, [1]);

// if
test(`function (a) { if (a) @1 @!2 a++; return a; }`, [0], [1], [""]);
test(`function (a) { if (a) @1 @!2 { return 1; } else @2 { return 2; } }`, [0], [1], [""]);
test(`function (a) { if (a === 1) @1 @!3 ; else @3 if (a === 2) @1 @!2 ; else @2 if (a === 3) @1 @!1 ; }`, [1], [2], [3], [4]);
test(`function (a) { if (a.b) @0 @!0 return 1; }`, [null]);
test(`function () { if (true) @1 @!0 ; if (false) @0 @!1 ; if (0) @0 @!1 ; else @1 ; }`, []);
test(`function (a) { for (const b of a) { if (b === 1) @1 @!2 continue; if (b === 2) @1 @!1 { @1 break; } } }`, [[0, 1, 2, 3]]);
test(`function (a) { for (const b of a) { if (b) @1 @!1 { break; } } }`, [[0, 1]]);
test(`function (a) { if (a) @1 @!1 function b() { } return typeof b; }`, [0], [1]);
test(`function (a) { if (a) @!1 a++; if (a) @1 a++; return a; }`, [0], [1]);
test(`function (a) { while (a--) @!0 ; return a && @!0 a; }`, [0], [1]);

// Loops.
test(`function (a) { while (@5 a--) @3 ; }`, [0], [3]);
test(`function (a, b) { while (a-- && @3 b--) @2 { } }`, [1, 9], [9, 1]);
test(`function (a, b) { while (a-- > 0 || @2 b--) @2 { } }`, [1, 1]);
test(`function (a) { while (a) @1 break; }`, [0], [1]);
test(`function (a) { do @4 { } while (@4 --a); }`, [1], [3]);
test(`function (a, b) { do @4 { } while (a-- && b--); }`, [1, 9], [9, 1]);
test(`function (a) { do @2 { if (a) break; } while (false); }`, [0], [1]);
test(`function (a) { for (let i = @2 0; @5 i < a; @3 i++) @3 { } }`, [0], [3]);
test(`function (a, b) { for (; a-- && b--;) @2 { } }`, [1, 9], [9, 1]);
test(`function () { for (;;) @1 break; }`, []);
test(`function (a) { for (const b in a) @3 { if (b === "c") break; } }`, [null], [{}], [{ a: 1, b: 1 }], [{ c: 1 }]);
test(`function (a) { for (const b of a) @4 { if (b) break; } }`, [[]], [[0, 0]], [[1]], [new Set([0])], [null]);
test(`async function (a) @3 { for await (const b of a) @3 { if (b) break; } }`, [[]], [[0, 0]], [[1]]);
test(`function () { outer: for (const a of [1, 2]) @2 { for (const b of [1, 2]) @2 { continue outer; } } }`, []);

// switch
test(`function (a) { @4 switch (a) { @1 case 1: @2 case 2: break; @1 case 3: } }`, [1], [2], [3], [4]);
test(`function (a) { @3 switch (a) { @1 case "aa": @2 case "bb": } }`, ["aa"], ["bb"], ["cc"]);
test(`function (a) { @3 switch (a) { @1 case "a": @2 case "b": } }`, ["a"], ["b"], ["c"]);
test(`function (a, b) { switch (a) { @1 case b: @2 default: @3 case b + 1: } }`, [1, 1], [2, 1], [3, 1]);
test(`function (a, b) { @1 switch (a) { @0 case b.c: } }`, [1, null]);
test(`function (a) { @1 switch (a) { } }`, [1]);
test(`function (a) { if (a) @1 @!1 switch (a) { case 0: } }`, [0], [1]);

// try
test(`function (a) { @2 try { @2 a.b; @1 return 1; } catch @1 { @1 return 2; } finally @2 { } }`, [null], [{}]);
test(`function (a) { try { a.b; } catch (error) @1 { } }`, [null], [{}]);
test(`function (a) { try { throw a; } catch ({ b = @1 1 }) @1 { } }`, [null], [{}]);
test(`function (a) { try { return a && @1 a.b.c; } catch { return 1; } }`, [0], [{}]);
test(`function (a) { for (const b of a) { try { continue; } finally @2 { } } }`, [[1, 2]]);
