//@ runDefault
//@ runDefault("--useLazyValueProfilePredictions=false")
//@ runDefault("--useJIT=false")
//@ runDefault("--useConcurrentJIT=false", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20", "--thresholdForOptimizeSoon=20", "--thresholdForFTLOptimizeAfterWarmUp=50")
//@ runDefault("--useConcurrentJIT=true", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20", "--thresholdForOptimizeSoon=20", "--thresholdForFTLOptimizeAfterWarmUp=50")
//@ runDefault("--collectContinuously=true", "--useGenerationalGC=false")
//@ runDefault("--useEagerCodeBlockJettisonTiming=true")
//@ runDefault("--forceEagerCompilation=true")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

// The profiles show up in the middle of the first (and only) call: the loop crosses the threshold, later
// it enters Baseline and DFG code through OSR, and the types it sees change on the way.
function onlyCall(n) {
    let o = { a: 1, b: 2.5, c: "s" };
    let sum = 0;
    let text = "";
    for (let i = 0; i < n; ++i) {
        sum += o.a;
        if (i > n / 2) {
            sum += o.b;
            o.a = i % 3 ? 1 : 1.5;
        }
        if (!(i % 1000))
            text += o.c;
    }
    return text + sum;
}
shouldBe(typeof onlyCall(200000), "string");

// A callee that ran once gets called from code that is already hot, and its results change type.
function coldCallee(x) { return x.value; }
function hotCaller(o, callCold) {
    let result = o.value;
    if (callCold)
        result = coldCallee(o);
    return result;
}
noInline(hotCaller);
for (let i = 0; i < 50000; ++i)
    shouldBe(hotCaller({ value: i }, false), i);
shouldBe(hotCaller({ value: "string" }, true), "string");
for (let i = 0; i < 50000; ++i)
    shouldBe(hotCaller({ value: i + 0.5 }, true), i + 0.5);

// Argument profiles: the first calls pass integers, later ones doubles, strings and objects.
function takesArguments(a, b, c) { return a + b + c; }
noInline(takesArguments);
shouldBe(takesArguments(1, 2, 3), 6);
shouldBe(takesArguments(1, 2, 3), 6);
for (let i = 0; i < 20000; ++i)
    shouldBe(takesArguments(i, 1, 2), i + 3);
for (let i = 0; i < 20000; ++i)
    shouldBe(takesArguments(i + 0.5, 1, 2), i + 3.5);
shouldBe(takesArguments("a", "b", "c"), "abc");
shouldBe(takesArguments({ valueOf() { return 1; } }, 2, 3), 6);

// Many functions that run once or twice, with a collection in between.
{
    let functions = [];
    for (let i = 0; i < 2000; ++i)
        functions.push(new Function("o", "x", `let r = o.p + x; if (x > 1) r += o.q.length + [x].length; return r + ${i};`));
    let o = { p: 1, q: "abc" };
    for (let i = 0; i < functions.length; ++i)
        shouldBe(functions[i](o, 1), 2 + i);
    fullGC();
    for (let i = 0; i < functions.length; i += 2)
        shouldBe(functions[i](o, 2), 3 + 3 + 1 + i);
    edenGC();
    for (let round = 0; round < 40; ++round) {
        for (let i = 0; i < functions.length; i += 100)
            shouldBe(functions[i](o, 2), 7 + i);
    }
}

// A function with a lot of value profiles.
{
    let body = "let r = 0;";
    for (let i = 0; i < 9000; ++i)
        body += `r += o.p${i % 7};`;
    body += "return r;";
    let big = new Function("o", body);
    let o = { p0: 1, p1: 1, p2: 1, p3: 1, p4: 1, p5: 1, p6: 1 };
    shouldBe(big(o), 9000);
    shouldBe(big(o), 9000);
    o.p3 = 1.5;
    shouldBe(big(o), 9000 + 0.5 * Math.floor((9000 + 3) / 7));
}

// The same code linked twice: the second CodeBlock is created after the first one warmed up.
function makeClosure() { return function (o) { return o.x + o.y; }; }
{
    let first = makeClosure();
    for (let i = 0; i < 1000; ++i)
        shouldBe(first({ x: i, y: 1 }), i + 1);
    let source = "(function (o) { return o.x + o.y; })";
    let a = eval(source);
    for (let i = 0; i < 1000; ++i)
        shouldBe(a({ x: i, y: 1 }), i + 1);
    let b = eval(source);
    shouldBe(b({ x: "a", y: "b" }), "ab");
    for (let i = 0; i < 1000; ++i)
        shouldBe(b({ x: i, y: 1.5 }), i + 1.5);
}

// Generators and async functions re-enter their code at many points.
function* generator(n) { for (let i = 0; i < n; ++i) yield { v: i }.v; }
{
    let sum = 0;
    for (let v of generator(3))
        sum += v;
    shouldBe(sum, 3);
    for (let round = 0; round < 200; ++round) {
        for (let v of generator(100))
            sum += v;
    }
    shouldBe(sum, 3 + 200 * 4950);
}

// The profiles show up while activations of the same code are on the stack, and while a generator of it is suspended.
function recursive(n, o) {
    if (!n)
        return o.leaf;
    let result = recursive(n - 1, o);
    return result + o.step;
}
shouldBe(recursive(30, { leaf: 1, step: 2 }), 61);
shouldBe(recursive(30, { leaf: 0.5, step: 2 }), 60.5);
shouldBe(recursive(300, { leaf: "a", step: "b" }).length, 301);
function* suspended(o) { let a = o.x; yield a; let b = o.y; yield a + b; return o.z; }
{
    let first = suspended({ x: 1, y: 2, z: 3 });
    shouldBe(first.next().value, 1);
    for (let i = 0; i < 100; ++i) {
        let other = suspended({ x: i, y: 0.5, z: "z" });
        shouldBe(other.next().value, i);
        shouldBe(other.next().value, i + 0.5);
        shouldBe(other.next().value, "z");
    }
    shouldBe(first.next().value, 3);
    shouldBe(first.next().value, 3);
}

// A getter re-enters the function that is reading it until that function is warm.
{
    let depth = 0;
    let o = { get p() { return depth++ < 20 ? reader(o) + 1 : 0; } };
    function reader(x) { return x.p; }
    shouldBe(reader(o), 20);
    depth = 0;
    shouldBe(reader(o), 20);
}
