// CodeBlock::finishCreation remembers what a variable read resolved to while it links one block. The same name read at
// different scope depths, through a `with`, under a sloppy eval's var injection, as a global property, a global lexical
// and a module-like closure variable must still each resolve to their own binding.
function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

var x = "global var";
let y = "global lexical";
globalThis.z = "global property";

function sameNameAtTwoDepths() {
    let x = "outer";
    function inner() {
        let a = x; // closure variable, one hop
        {
            let x = "block";
            let f = () => x; // a different binding under the same name, captured
            a += "/" + f() + "/" + x;
        }
        return a + "/" + x + "/" + x;
    }
    return inner();
}
shouldBe(sameNameAtTwoDepths(), "outer/block/block/outer/outer");

function readsGlobalsRepeatedly() {
    return [x, y, z, x, y, z, x + y + z].join("|");
}
shouldBe(readsGlobalsRepeatedly(), "global var|global lexical|global property|global var|global lexical|global property|global varglobal lexicalglobal property");

function throughWith(o) {
    let r = [];
    r.push(z);
    with (o) {
        r.push(z, z);
    }
    r.push(z);
    return r.join("|");
}
shouldBe(throughWith({ z: "with" }), "global property|with|with|global property");
shouldBe(throughWith({}), "global property|global property|global property|global property");

function varInjection(code) {
    let before = z;
    eval(code);
    return before + "|" + z + "|" + z;
}
shouldBe(varInjection(""), "global property|global property|global property");
shouldBe(varInjection("var z = 'injected'"), "global property|injected|injected");

function readThenWrite() {
    let v = "one";
    function f() {
        let a = v;
        v = "two";
        return a + v + v;
    }
    return f();
}
shouldBe(readThenWrite(), "onetwotwo");

// A global lexical binding that appears after the function first linked.
function lateGlobalLexical() { return typeof lateBinding === "undefined" ? "none" : lateBinding + lateBinding; }
shouldBe(lateGlobalLexical(), "none");
loadString("let lateBinding = 'late';");
shouldBe(lateGlobalLexical(), "latelate");

for (let i = 0; i < 1e4; ++i) {
    shouldBe(sameNameAtTwoDepths(), "outer/block/block/outer/outer");
    shouldBe(throughWith({ z: "with" }), "global property|with|with|global property");
    shouldBe(varInjection("var z = 'injected'"), "global property|injected|injected");
}
