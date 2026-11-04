function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function syntaxError(src, m) {
    try { (0, eval)(src); } catch (e) { assert(e instanceof SyntaxError, m + " threw " + e); return; }
    throw new Error("no SyntaxError: " + m);
}

// A struct body takes every element of a class body. These parse.
(0, eval)(`if (false) {
    const k = "k";
    struct S {
        a = 1; [k]; #p = 2; static s = 3; static { }
        a; a;
        #m() { } static #sm() { } get #g() { return 1; } set #g(v) { }
        static async *gen() { }
        static get sg() { return 1; }
        m() { } get g() { return 1; } set g(v) { } *it() { } async am() { }
        constructor(x) { super(x); }
    }
}`);

// Early errors that remain: those of classes, and the instance private names and methods of a shared struct.
syntaxError("struct S { constructor; }", "field named constructor");
syntaxError("struct S { constructor() { } 'constructor'() { } }", "two constructors");
syntaxError("struct S { #p; #p; }", "duplicate private name");
syntaxError("struct S { static prototype = 1; }", "static field named prototype");
syntaxError("shared struct S { #p; }", "shared struct private field");
syntaxError("shared struct S { #m() {} }", "shared struct private method");
syntaxError("shared struct S { m() {} }", "shared struct method");
syntaxError("shared struct S { get g() { return 1; } }", "shared struct getter");
syntaxError("shared struct S { static { } #p; }", "shared struct private field after a static block");
syntaxError("class C { #p = 1; static f() { shared struct S { x = C.#p; } } }", "an instance field of a shared struct that mentions a private name");
syntaxError("class C { #p = 1; static f() { shared struct S { ['k'] = this.#p; } } }", "a computed instance field of a shared struct that mentions a private name");
// A static element, or an unshared struct, can.
(0, eval)("class C { #p = 1; static f() { shared struct S { static x = C.#p; } struct T { x = C.#p; } } }");
