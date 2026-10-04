//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useMiniVMModeWithoutJIT=0", "--useConcurrentGC=1", "--collectContinuously=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function addsC(o, v) { o.c = v; }
function addsD(o, v) { o.d = v; }
function addsE(o, v) { o.e = v; }
function addsF(o, v) { o.f = v; }
function addsG(o, v) { o.g = v; }
function addsSeventh(o, v) { o.p7 = v; }
function addsToFunction(o, v) { o.added = v; }
function addsToArray(o, v) { o.added = v; }

const growing = "operationAOTPutByIdReallocating";
const countsOperations = typeof aotOperationCount === "function" && aotOperationCount(growing) !== null && isAOTCompiled(addsC) && (aotRemarks("addsC") || []).includes("calls:PutById");
function operationsDuring(f) {
    if (!countsOperations) {
        f();
        return -1;
    }
    const before = aotOperationCount(growing) + aotOperationCount("operationAOTPutById");
    f();
    return aotOperationCount(growing) + aotOperationCount("operationAOTPutById") - before;
}
function checkOperations(actual, atLeast, atMost, what) {
    if (actual >= 0 && (actual < atLeast || actual > atMost))
        throw new Error(what + ": " + actual + " calls of operations, not " + atLeast + " to " + atMost);
}
function two(i) { return { a: i, b: -i }; }
function six(i) {
    const result = {};
    result.p1 = i; result.p2 = i; result.p3 = i; result.p4 = i; result.p5 = i; result.p6 = i;
    return result;
}
const warmUp = 60;
const rounds = 100000;
const fewRefills = rounds / 10;

for (let i = 0; i < warmUp; i++) {
    const o = two(i);
    addsC(o, i); addsD(o, i); addsE(o, i); addsF(o, i); addsG(o, i);
    addsSeventh(six(i), i);
    addsToFunction(function () { }, i);
    addsToArray([i], i);
}

{
    const kept = [];
    checkOperations(operationsDuring(() => {
        for (let i = 0; i < rounds; i++) {
            const o = two(i);
            addsC(o, i);
            kept.push(o);
        }
    }), 0, fewRefills, "the first property added to a literal without room");
    fullGC();
    for (let i = 0; i < rounds; i++) {
        check(Object.keys(kept[i]).join(), "a,b,c", "the keys of a literal that got a property");
        check(kept[i].a + "," + kept[i].b + "," + kept[i].c, i + "," + -i + "," + i, "the properties of a literal that got a property");
        check(kept[i].d, undefined, "a property that was not added");
    }
    checkOperations(operationsDuring(() => {
        for (let i = 0; i < rounds; i++) {
            addsD(kept[i], "d" + i);
            addsE(kept[i], i + 0.5);
            addsF(kept[i], kept[i]);
        }
    }), 0, 6, "properties that fit the storage that was allocated");
    edenGC();
    for (let i = 0; i < rounds; i++) {
        const o = kept[i];
        check(Object.keys(o).join(), "a,b,c,d,e,f", "the keys once the storage is full");
        check(o.c + "," + o.d + "," + o.e + "," + (o.f === o), i + ",d" + i + "," + (i + 0.5) + ",true", "the properties once the storage is full");
    }
    checkOperations(operationsDuring(() => {
        for (let i = 0; i < rounds; i++)
            addsG(kept[i], i);
    }), rounds, rounds, "a property that needs more than the initial storage");
    for (let i = 0; i < rounds; i++) {
        const o = kept[i];
        check(Object.keys(o).join(), "a,b,c,d,e,f,g", "the keys once the storage has grown");
        check(o.c + "," + o.d + "," + o.e + "," + (o.f === o) + "," + o.g, i + ",d" + i + "," + (i + 0.5) + ",true," + i, "the properties once the storage has grown");
    }
}

{
    const kept = [];
    checkOperations(operationsDuring(() => {
        for (let i = 0; i < rounds; i++) {
            const o = six(i);
            addsSeventh(o, { i });
            kept.push(o);
        }
    }), 0, fewRefills, "the seventh property of an object that started empty");
    fullGC();
    for (let i = 0; i < rounds; i++)
        check(Object.keys(kept[i]).length + "," + kept[i].p6 + "," + kept[i].p7.i, "7," + i + "," + i, "an object with seven properties");
}

{
    const functions = [];
    const arrays = [];
    for (let i = 0; i < rounds; i++) {
        functions.push(function () { });
        arrays.push([i]);
    }
    checkOperations(operationsDuring(() => {
        for (let i = 0; i < rounds; i++)
            addsToFunction(functions[i], i);
    }), rounds, rounds, "the first property added to a function");
    checkOperations(operationsDuring(() => {
        for (let i = 0; i < rounds; i++)
            addsToArray(arrays[i], i);
    }), rounds, rounds, "the first property added to an array");
    for (let i = 0; i < rounds; i++) {
        check(functions[i].added, i, "a property added to a function");
        check(arrays[i].added + "," + arrays[i][0] + "," + arrays[i].length, i + "," + i + ",1", "a property added to an array");
    }
}

{
    const frozen = Object.preventExtensions(two(1));
    addsC(frozen, 1);
    check("c" in frozen, false, "a property added to an object that is not extensible");
    let received = 0;
    const inherits = Object.create({ set c(v) { received += v; } });
    inherits.a = 1;
    inherits.b = 2;
    for (let i = 0; i < 10; i++)
        addsC(inherits, 1);
    check(received + "," + Object.keys(inherits).join(), "10,a,b", "a setter at a site that usually allocates storage");
}
