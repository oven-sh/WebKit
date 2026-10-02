//@ skip if $architecture != "arm64"
//@ requireOptions("--compileMainScriptAheadOfTime=1")
//@ defaultRun
//@ run("gc-stress", "--slowPathAllocsBetweenGCs=20")

function tag(strings) { return strings; }
function site() { return tag`a${1}b`; }
function Both() { return tag`x${2}y`; }
noInline(site);
noInline(Both);

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`got ${actual}, expected ${expected}`);
}
if (!isAOTCompiled(site) || !isAOTCompiled(Both))
    throw new Error("not compiled");

const first = site();
shouldBe(first.join(), "a,b");
shouldBe(first.raw.join(), "a,b");
shouldBe(Object.isFrozen(first), true);
fullGC();
for (let i = 0; i < 100; i++)
    shouldBe(site(), first);
// One for each place in the text, whether the function is called or constructed.
shouldBe(new Both(), Both());
shouldBe(tag`a${1}b` === first, false);

// Another realm has its own.
const other = createGlobalObject();
shouldBe(first instanceof Array, true);
shouldBe(first instanceof other.Array, false);
