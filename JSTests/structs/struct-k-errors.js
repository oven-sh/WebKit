function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
let threw = false;
try { struct Bad extends Object { } } catch (e) { threw = e instanceof TypeError; }
assert(threw, "extends Object");

// A shared struct has no instance private names and no instance methods or accessors. A struct can have both.
for (const source of [
    "shared struct S { get #v() { return 1; } }",
    "shared struct S { #p; }",
    "shared struct S { #m() { } }",
    "shared struct S { m() { } }",
    "shared struct S { get g() { return 1; } }",
    "shared struct S { set g(v) { } }",
    "shared struct S { *g() { } }",
    "shared struct S { async g() { } }",
    "shared struct S { [Symbol.iterator]() { } }",
    "struct S { constructor; }",
    "struct S { constructor() { } constructor() { } }",
    "struct S { #a; #a; }",
    "struct S { static prototype; }",
])
    try { (0, eval)("if (false) { " + source + " }"); throw new Error("expected a SyntaxError for " + source); } catch (e) { if (!(e instanceof SyntaxError)) throw e; }
for (const source of [
    "struct S { get #v() { return 1; } }",
    "struct S { set #v(x) {} }",
    "struct S { 0; }",
    "struct S { 'a\u0000b'; }",
    "shared struct S { static #p; static #m() { } static get #a() { return 1; } static f() { } }",
])
    (0, eval)("if (false) { " + source + " }");

// A function whose prototype is a struct's prototype is not a struct to extend.
{
    struct Real { a; }
    function Fake() { }
    Fake.prototype = Real.prototype;
    let rejected = false;
    try { (0, eval)("(function (Fake) { struct Child extends Fake { b; } return Child; })").call(undefined, Fake); } catch (e) { rejected = e instanceof TypeError; }
    assert(rejected, "extends a function that borrows a struct prototype");
}

// The value that a constructor returns is ignored: the result of new is always the instance.
{
    const other = { already: 1 };
    struct Parent { x; constructor() { return other; } }
    struct Kid extends Parent { extra; }
    const parent = new Parent();
    assert(parent !== other && Object.isSealed(parent) && "x" in parent, "a returned object is dropped");
    const kid = new Kid();
    assert(Object.isSealed(kid) && Object.keys(kid).join() === "x,extra", "a struct whose parent returns an object");
    assert(Object.isExtensible(other) && !("x" in other), "the returned object is untouched");
    struct Primitive { constructor() { return 1; } }
    assert(Object.isSealed(new Primitive()), "a returned primitive is dropped, not a TypeError");
}
