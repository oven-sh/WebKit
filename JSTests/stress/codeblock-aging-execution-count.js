//@ runDefault("--useEagerCodeBlockJettisonTiming=1", "--useExecutionCountForCodeBlockAging=1", "--useConcurrentJIT=0")

// Options::useExecutionCountForCodeBlockAging: an LLInt/Baseline CodeBlock whose
// execution counter has advanced since the last old-age check should have its TTL
// renewed instead of being jettisoned. This test exercises that path heavily with
// the eager 10ms TTL and verifies that actively-executed functions keep their
// CodeBlock across full GCs while an idle function is still discarded.

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error((msg || "") + " expected " + expected + " but got " + actual);
}

var N = 80;
var active = [];
for (var i = 0; i < N; i++)
    active.push(new Function("x", "return x + " + i + ";"));

var idle = new Function("x", "return x - 1;");
noInline(idle);

function runActive() {
    var s = 0;
    for (var i = 0; i < N; i++) s += active[i](1);
    return s;
}
var expected = N + (N * (N - 1)) / 2;

shouldBe(runActive(), expected, "warmup");
idle(0);

var activeJettisons = 0;
for (var iter = 0; iter < 30; iter++) {
    shouldBe(runActive(), expected, "iter " + iter);
    // Let the eager TTL elapse (InterpreterThunk = 10ms, BaselineJIT = 30ms).
    var start = preciseTime();
    while (preciseTime() - start < 0.060) { }
    fullGC();
    for (var i = 0; i < N; i++) {
        if ($vm.codeBlockFor(active[i]) === undefined)
            activeJettisons++;
    }
}

// Every active function was executed between every pair of GCs, so none of
// them should ever have been jettisoned for old age.
shouldBe(activeJettisons, 0, "active functions should not be jettisoned while executing");

// The idle function was never executed after warmup and had 30 chances to be
// discarded; the new policy must not have kept it alive.
if ($vm.codeBlockFor(idle) !== undefined)
    throw new Error("idle function was never jettisoned; policy is over-retaining");

shouldBe(runActive(), expected, "final");
