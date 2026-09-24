//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// A C function can be given any name (`int f(void) __asm__("7");`). Whatever it is, the export is an own data
// property of the exports object under that name and calls that function.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const utf8 = text => unescape(encodeURIComponent(text));

const names = ["7", "0", "4294967294", "4294967295", "-1", "1.5", "", "__proto__", "constructor", "toString", "hasOwnProperty", "then", "length", "a b", "a\0b", "été", "\u{1f600}", "x".repeat(5000)];
const funcs = names.map((name, i) => ({ name: "function" + i, sig: 0, exported: true, blocks: [[["ConstI32", s(i + 100)], ["Ret", 0]]] }));
const m = $vm.cModule(assemble({
    sigs: [{ ret: T.i32, params: [] }],
    funcs,
    // The last two names are given twice: the later export is the one the name ends up with.
    exports: [...names.map((name, i) => ({ name: utf8(name), func: i, ret: FFI.i32, args: [] })), { name: "7", func: 1, ret: FFI.i32, args: [] }, { name: "then", func: 2, ret: FFI.i32, args: [] }],
}));
eq(Object.getPrototypeOf(m), Object.prototype, "the exports object is an ordinary object");
eq(Reflect.ownKeys(m).length, names.length, "one property for each name");
for (let i = 0; i < names.length; i++) {
    const name = names[i];
    const descriptor = Object.getOwnPropertyDescriptor(m, name);
    if (!descriptor || typeof descriptor.value !== "function")
        throw new Error(`no own function named ${JSON.stringify(name.slice(0, 20))}`);
    const expected = name === "7" ? 101 : name === "then" ? 102 : i + 100;
    eq(m[name](), expected, `calling ${JSON.stringify(name.slice(0, 20))}`);
}
eq(m[7](), 101, "an index reaches the export whose name reads as one");
eq(m[0](), 101, "index 0");
eq(Object.keys(m)[0], "0", "and it is an indexed property");
eq(({}).__proto__, Object.prototype, "Object.prototype is as it was");
eq(typeof ({}).constructor, "function", "and so is its constructor");
print("export names ok");
