//@ runBytecodeCache("--useUnlinkedCodeBlockJettisoning=1", "--forceCodeBlockToJettisonDueToOldAge=1")
//@ runBytecodeCache("--useJIT=0", "--forceCodeBlockToJettisonDueToOldAge=1")

// As bytecode-cache-leaf-key-reuse.js, with functions of every kind the cache tells apart: the ones JSC makes up (the
// body of a generator or of an async function that awaits, the initializer of a class's fields, a default constructor)
// and the ones that have more than one executable at a time (a function that is called and constructed has its nested
// functions once for each, a function in the condition of a loop is generated twice). The body of `asyncArrow` starts
// where the function in it does.

function check(actual, expected)
{
    if (actual !== expected)
        throw new Error("expected " + expected + " but got " + actual);
}

function dropUnlinkedCode()
{
    // The collector drops the unlinked code of a function that has not run for a number of collections
    // (UnlinkedCodeBlock::maxAge). In the second run the code is from the cache, and stays.
    let unlinkedFunctions = () => $vm.codeBlockCensus().unlinkedFunction;
    let before = unlinkedFunctions();
    for (let i = 0; i < 20 && unlinkedFunctions() * 2 > before; ++i)
        fullGC();
    if (!jscOptions().forceDiskCache && unlinkedFunctions() * 2 > before)
        throw new Error("the collector kept the unlinked code: " + unlinkedFunctions() + " of " + before + " code blocks");
}

var units = { };

function sourceOf(name)
{
    return `
units.${name} = (function () {
    function* generator() { function nested() { return "${name} generator"; } yield nested; }
    async function* asyncGenerator() { yield () => "${name} async generator"; }
    async function asyncFunction() { await 0; return () => "${name} async function"; }
    async function asyncFunctionWithoutAwait() { return () => "${name} async function without await"; }
    let asyncArrow = async () => function () { return "${name} async arrow"; }() + await "";
    class Fields {
        field = () => "${name} field";
        static staticField = () => "${name} static field";
        method() { return () => "${name} method"; }
        get accessor() { return () => "${name} accessor"; }
    }
    class Base { }
    class Derived extends Base { inherited() { return () => "${name} inherited"; } }
    function calledAndConstructed() { let nested = () => "${name} called and constructed"; if (new.target) this.nested = nested; else return nested; }
    function loop() { let seen = []; for (let i = 0; seen.push(() => "${name} loop") < 3; ++i) { } return seen; }
    return { generator, asyncGenerator, asyncFunction, asyncFunctionWithoutAwait, asyncArrow, Fields, Base, Derived, calledAndConstructed, loop };
});
`;
}

const count = 40;
let source = "";
for (let i = 0; i < count; ++i)
    source += sourceOf("first" + i) + sourceOf("second" + i);
loadString(source);

function run(name)
{
    let unit = units[name]();

    check(unit.generator().next().value(), name + " generator");

    let fromAsyncGenerator;
    unit.asyncGenerator().next().then((result) => { fromAsyncGenerator = result.value(); });
    let fromAsyncFunction;
    unit.asyncFunction().then((result) => { fromAsyncFunction = result(); });
    let fromAsyncFunctionWithoutAwait;
    unit.asyncFunctionWithoutAwait().then((result) => { fromAsyncFunctionWithoutAwait = result(); });
    let fromAsyncArrow;
    unit.asyncArrow().then((result) => { fromAsyncArrow = result; });
    drainMicrotasks();
    check(fromAsyncGenerator, name + " async generator");
    check(fromAsyncFunction, name + " async function");
    check(fromAsyncFunctionWithoutAwait, name + " async function without await");
    check(fromAsyncArrow, name + " async arrow");

    let fields = new unit.Fields;
    check(fields.field(), name + " field");
    check(unit.Fields.staticField(), name + " static field");
    check(fields.method()(), name + " method");
    check(fields.accessor(), name + " accessor");

    new unit.Base;
    check(new unit.Derived().inherited()(), name + " inherited");

    check(unit.calledAndConstructed()(), name + " called and constructed");
    check(new unit.calledAndConstructed().nested(), name + " called and constructed");

    for (let closure of unit.loop())
        check(closure(), name + " loop");
}

for (let i = 0; i < count; ++i)
    run("first" + i);

dropUnlinkedCode();

for (let i = 0; i < count; ++i)
    run("second" + i);
