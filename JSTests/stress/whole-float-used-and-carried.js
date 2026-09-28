//@ requireOptions("--useDollarVM=1")
load("./resources/tagged-arithmetic.js", "caller relative");

// A number that is only carried is left alone by the compilers. It is one that is also looked at as a number that they
// have a reason to unbox, and then it has to be boxed again as what it was.

const add = $vm.tagged.add, list = $vm.tagged.newArray;
const F0 = float(0), F2 = float(2), F4 = float(4);
let sink = 0;
let closed = 0.5;
var globalVar = 0.5;
let globalLexical = 0.5;
const holder = { field: 0.5 };
const results = { first: 0.5, last: 0.5 };
class A { } A.prototype.constant = int(1);
class B { } B.prototype.constant = float(0.5);
class C { } C.prototype.constant = float(2);

// [source of a function of (x, i), or of two: one that puts x somewhere and one that gets it back]. They are compiled apart,
// so that what is put cannot be handed straight to what gets it.
const routes = {
    "a local that is compared": ["{ let y = x; for (let k = 0; k < 3; ++k) { if (y < k * 0.25) sink = k; if (k === i) y = x; } return y; }"],
    "a local that is multiplied": ["{ let y = x; for (let k = 0; k < 3; ++k) { sink = y * 1.5 + y * 2.5; if (k === i) y = x; } return y; }"],
    "a local that is x or a float, and is multiplied": ["{ let y = x; if (i & 1) y = 0.5; sink = y * 1.5 + y * 2.5; sink += y * 3.5; return i & 1 ? x : y; }"],
    "a local of an inlined function": ["{ return ((y) => { for (let k = 0; k < 3; ++k) { if (y < k * 0.25) sink = k; } return y; })(x); }"],
    "a property": ["{ holder.field = x; }", "{ return holder.field; }"],
    "a property, compared before": ["{ if (x < 0.25) sink = i; holder.field = x; }", "{ return holder.field; }"],
    "a property, compared after": ["{ holder.field = x; }", "{ let y = holder.field; if (y < 0.25) sink = i; return y; }"],
    "a property, multiplied after": ["{ holder.field = x; }", "{ let y = holder.field; sink = y * 1.5; return y; }"],
    "a property, read in a loop": ["{ holder.field = x; }", "{ let y; for (let k = 0; k < 3; ++k) { y = holder.field; sink = y * 1.5; } return y; }"],
    "a closure variable": ["{ closed = x; }", "{ return closed; }"],
    "a closure variable, compared": ["{ if (x < 0.25) sink = i; closed = x; }", "{ let y = closed; if (y < 0.25) sink = i; return y; }"],
    "a global lexical variable": ["{ globalLexical = x; }", "{ let y = globalLexical; if (y < 0.25) sink = i; return y; }"],
    "a global variable": ["{ globalVar = x; }", "{ let y = globalVar; if (y < 0.25) sink = i; return y; }"],
    "an element, compared": ["{ kept[i & 3] = x; }", "{ let y = kept[i & 3]; if (y < 0.25) sink = i; return y; }"],
};
const kept = list(0.5, 0.5, 0.5, 0.5);

const kinds = { int: [1, 7, -3].map(int), integral: [2, 5, -4].map(float), fractional: [0.5, -1.5, 2.25].map(float) };

for (let name in routes) {
    for (let trained in kinds) {
        for (let surprise in kinds) {
            if (trained === surprise)
                continue;
            let functions = routes[name].map(body => eval(freshSource("x, i", body)));
            functions.forEach(noInline);
            let run = (values, i) => {
                let x = values[i % values.length];
                let wanted = show(x);
                let got;
                for (let f of functions)
                    got = f(x, i);
                if (show(got) !== wanted)
                    throw new Error(`${name}, compiled for ${trained} and given ${surprise}: ${wanted} arrived as ${show(got)}, the ${i}th time`);
            };
            for (let i = 0; i < testLoopCount; ++i)
                run(kinds[trained], i);
            for (let i = 0; i < 20; ++i)
                run(kinds[surprise], i);
            for (let i = 0; i < testLoopCount; ++i)
                run(i % 5 < 2 ? kinds[surprise] : kinds[trained], i);
        }
    }
}

function expect(what, actual, wanted) {
    if (show(actual) !== wanted)
        throw new Error(`${what}: ${show(actual)} and not ${wanted}`);
}

// Constants, which nothing checks.
function intConstantAmongFloats(i) { let y = 1; if (i & 1) y = 0.5; sink = y * 1.5 + y * 2.5; sink += y * 3.5; return y; }
noInline(intConstantAmongFloats);
function floatConstants(i) { let y = F2; for (let k = 0; k < 3; ++k) { if (y < k) sink = k; if (k === (i & 3)) y = F4; } return y; }
noInline(floatConstants);
function floatConstantAmongInts(i) { let y = F2; for (let k = 0; k < 3; ++k) { sink = (y + k) | 0; if (k === (i & 3)) y = 4; } return y; }
noInline(floatConstantAmongInts);
for (let i = 0; i < testLoopCount * 2; ++i) {
    expect("an int constant in a local that also holds floats", intConstantAmongFloats(i), i & 1 ? "float 0.5" : "int 1");
    expect("float constants with integral values", floatConstants(i), (i & 3) === 3 ? "float 2" : "float 4");
    expect("a float constant in a local that also holds ints", floatConstantAmongInts(i), (i & 3) === 3 ? "float 2" : "int 4");
}

// A float constant in a local that JavaScript's arithmetic also puts doubles in.
function floatConstantAmongDoubles(i) { let y = F2; for (let k = 0; k < 4; ++k) { sink = y * 1.5; if (k === (i & 7)) y = k * 0.25 + 0.5; } return y; }
noInline(floatConstantAmongDoubles);
for (let i = 0; i < testLoopCount * 2; ++i)
    expect("a float constant in a local that also holds doubles", floatConstantAmongDoubles(i), (i & 7) > 3 ? "float 2" : (i & 7) === 2 ? "int 1" : "float " + ((i & 7) * 0.25 + 0.5));

// The same, and it is only stored: to return it would be reason enough to keep it boxed.
function floatConstantAmongDoublesStored(i) { let y = F2; for (let k = 0; k < 4; ++k) { results.last = y; sink = y * 1.5; if (k === (i & 7)) y = (i & 7) * 0.25 + 0.5; } }
noInline(floatConstantAmongDoublesStored);
for (let i = 0; i < testLoopCount * 2; ++i) {
    floatConstantAmongDoublesStored(i);
    expect("a float constant in a local that also holds doubles, stored", results.last, (i & 7) > 2 ? "float 2" : (i & 7) === 2 ? "int 1" : "float " + ((i & 7) * 0.25 + 0.5));
}

// The same, of a constant that the compiler works out for itself.
function foldedFloatConstantAmongDoublesStored(i) { let y = add(2, -0); for (let k = 0; k < 4; ++k) { results.last = y; sink = y * 1.5; if (k === (i & 7)) y = (i & 7) * 0.25 + 0.5; } }
noInline(foldedFloatConstantAmongDoublesStored);
for (let i = 0; i < testLoopCount * 2; ++i) {
    foldedFloatConstantAmongDoublesStored(i);
    expect("a folded float constant in a local that also holds doubles, stored", results.last, (i & 7) > 2 ? "float 2" : (i & 7) === 2 ? "int 1" : "float " + ((i & 7) * 0.25 + 0.5));
}

// Optimized code that is left because of a surprise, and entered again at the head of the loop that was being run.
for (let [trained, surprise] of [list(float(0.5), int(1)), list(int(1), float(2)), list(float(2), int(1)), list(int(1), float(0.5)), list(float(0.5), float(2)), list(float(2), float(0.5)), list(intAsDouble(1), float(2))]) {
    let f = eval(freshSource("x, n", "{ let y = x; for (let k = 0; k < n; ++k) { if (y < k * 0.25) sink = k; } return y; }"));
    noInline(f);
    for (let i = 0; i < 4; ++i)
        expect("around a long loop", f(trained, 100000), show(trained));
    for (let i = 0; i < 4; ++i) {
        expect("around a long loop, the other kind", f(surprise, 100000), show(surprise));
        expect("around a long loop, the first kind again", f(trained, 100000), show(trained));
    }
}

// The same, of a number that is not an argument: those are looked at on the way in, before any local is.
const source = { v: 0.5 };
for (let comparison of ["y < k * 0.25", "y < k"]) {
    for (let [trained, surprise] of [list(float(0.5), int(1)), list(int(1), float(2)), list(float(2), int(1)), list(int(1), float(0.5)), list(float(0.5), float(2)), list(float(2), float(0.5)), list(intAsDouble(1), float(2))]) {
        let f = eval(freshSource("n", "{ let y = source.v; for (let k = 0; k < n; ++k) { if (" + comparison + ") sink = k; } return y; }"));
        noInline(f);
        for (let i = 0; i < 4; ++i) {
            source.v = trained;
            expect("loaded, and kept around a long loop", f(100000), show(trained));
        }
        for (let i = 0; i < 4; ++i) {
            source.v = surprise;
            expect("loaded, and kept around a long loop, the other kind", f(100000), show(surprise));
            source.v = trained;
            expect("loaded, and kept around a long loop, the first kind again", f(100000), show(trained));
        }
    }
}

// Copied from one property to another, and used as a number, by code that hands it to nothing else.
for (let [trained, surprise] of [list(float(0.5), int(1)), list(float(2), int(1)), list(float(0.5), float(2)), list(int(1), float(2)), list(float(2), float(0.5))]) {
    let copy = eval(freshSource("", "{ let y = source.v; results.last = y; sink = y * 1.5; }"));
    noInline(copy);
    for (let i = 0; i < testLoopCount * 2; ++i) {
        source.v = trained;
        copy();
        expect("copied", results.last, show(trained));
    }
    for (let i = 0; i < 50; ++i) {
        source.v = surprise;
        copy();
        expect("copied, the other kind", results.last, show(surprise));
    }
}

// One place that reads arrays of int32s and arrays of doubles.
function element(a, i) { return a[i]; }
noInline(element);
function elementCompared(a, i) { let y = a[i]; if (y < 0.25) sink = i; return y; }
noInline(elementCompared);
const int32s = [1, 2, 3], doubles = [0.5, 1.5, 2.5];
for (let i = 0; i < testLoopCount * 2; ++i) {
    for (let f of [element, elementCompared]) {
        expect("from an array of int32s", f(int32s, i % 3), "int " + (i % 3 + 1));
        expect("from an array of doubles", f(doubles, i % 3), "float " + (i % 3 + 0.5));
    }
}

// A loop variable that starts as an int constant, becomes a float, and is stored on the way.
function storedOnTheWay(n, step) { let s = 0; for (let k = 0; k < n; ++k) { if (!k) results.first = s; results.last = s; s = add(s, step); } return n; }
noInline(storedOnTheWay);
function storedOnTheWayByJavaScript(n, step) { let s = 0; for (let k = 0; k < n; ++k) { if (!k) results.first = s; results.last = s; s = s + step; } return n; }
noInline(storedOnTheWayByJavaScript);
for (let i = 0; i < testLoopCount; ++i) {
    for (let f of [storedOnTheWay, storedOnTheWayByJavaScript]) {
        f(5, float(0.5));
        expect("what the loop variable was at first", results.first, "int 0");
        // What JavaScript computes is what its value says it is.
        expect("what the loop variable was at last", results.last, f === storedOnTheWay ? "float 2" : "int 2");
    }
}

// A property that is a constant of the prototype: an int for one class and a float for another.
function constantOfThePrototype(o) { let y = o.constant; sink = y * 1.5; return y; }
noInline(constantOfThePrototype);
const instances = [new A, new B, new C];
for (let i = 0; i < testLoopCount * 2; ++i)
    expect("a constant of the prototype", constantOfThePrototype(instances[i % 3]), ["int 1", "float 0.5", "float 2"][i % 3]);

function constantOfThePrototypeStored(o) { let y = o.constant; results.last = y; sink = y * 1.5; }
noInline(constantOfThePrototypeStored);
for (let i = 0; i < testLoopCount * 2; ++i) {
    constantOfThePrototypeStored(instances[i % 3]);
    expect("a constant of the prototype, stored", results.last, ["int 1", "float 0.5", "float 2"][i % 3]);
}
