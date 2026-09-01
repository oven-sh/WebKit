//@ requireOptions("--useJSThreads=1")
// Thread.restrict on a WebAssembly GC struct or array reference. Those cells
// have realmless structures (no global to compare the species-protected
// builtin slots against), so the receiver check used to dereference null.
// They must be refused with the 5.7 TypeError like every other receiver the
// enforcement hooks cannot cover; the restrict surface stays usable after.
load("../harness.js", "caller relative");

if (typeof WebAssembly !== "undefined") {
    // (module
    //   (type $s (struct (field i32)))
    //   (type $a (array (mut i32)))
    //   (func (export "mkStruct") (result anyref) (struct.new_default $s))
    //   (func (export "mkArray") (result anyref) (i32.const 1) (array.new_default $a)))
    const bytes = new Uint8Array([
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        // type section
        0x01, 0x0c, 0x03,
        0x5f, 0x01, 0x7f, 0x00,
        0x5e, 0x7f, 0x01,
        0x60, 0x00, 0x01, 0x6e,
        // function section
        0x03, 0x03, 0x02, 0x02, 0x02,
        // export section
        0x07, 0x16, 0x02,
        0x08, 0x6d, 0x6b, 0x53, 0x74, 0x72, 0x75, 0x63, 0x74, 0x00, 0x00,
        0x07, 0x6d, 0x6b, 0x41, 0x72, 0x72, 0x61, 0x79, 0x00, 0x01,
        // code section
        0x0a, 0x0f, 0x02,
        0x05, 0x00, 0xfb, 0x01, 0x00, 0x0b,
        0x07, 0x00, 0x41, 0x01, 0xfb, 0x07, 0x01, 0x0b,
    ]);
    shouldBeTrue(WebAssembly.validate(bytes), "GC-typed module validates");
    const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes));

    const struct = instance.exports.mkStruct();
    shouldBe(typeof struct, "object");
    shouldThrow(TypeError, () => Thread.restrict(struct), "cannot restrict this object");

    const array = instance.exports.mkArray();
    shouldBe(typeof array, "object");
    shouldThrow(TypeError, () => Thread.restrict(array), "cannot restrict this object");

    // Plain receivers are unaffected.
    const plain = Thread.restrict({ a: 1 });
    shouldBe(plain.a, 1);
}
