//@ runDefault
//@ runDefault("--useConcurrentJIT=false", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20", "--thresholdForOptimizeSoon=20", "--thresholdForFTLOptimizeAfterWarmUp=50")
//@ runDefault("--useJIT=false")
//@ runDefault("--collectContinuously=true", "--useGenerationalGC=false")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

// The optimizing compilers fold reads of global variables that were written once, and find the variable's
// watchpoint set through the global object's symbol tables. Every write after that has to reach the folded code.
var globalVar = 10;
let globalLet = 20;
const globalConst = 30;
var neverWritten;
let lateLet;

function readVar() { return globalVar; }
function readLet() { return globalLet; }
function readConst() { return globalConst; }
function readNeverWritten() { return neverWritten; }
function readLate() { return lateLet; }
function readAll() { return globalVar + globalLet + globalConst; }
function writeVar(v) { globalVar = v; }
function writeLet(v) { globalLet = v; }
noInline(readVar);
noInline(readLet);
noInline(readConst);
noInline(readNeverWritten);
noInline(readLate);
noInline(readAll);
noInline(writeVar);
noInline(writeLet);

for (let i = 0; i < 20000; ++i) {
    shouldBe(readVar(), 10);
    shouldBe(readLet(), 20);
    shouldBe(readConst(), 30);
    shouldBe(readNeverWritten(), undefined);
    shouldBe(readLate(), undefined);
    shouldBe(readAll(), 60);
}

writeVar(11);
shouldBe(readVar(), 11);
shouldBe(readAll(), 61);
writeLet(21);
shouldBe(readLet(), 21);
shouldBe(readAll(), 62);
neverWritten = "written";
shouldBe(readNeverWritten(), "written");
lateLet = 1.5;
shouldBe(readLate(), 1.5);

for (let i = 0; i < 20000; ++i) {
    writeVar(i);
    writeLet(i + 1);
    shouldBe(readVar(), i);
    shouldBe(readLet(), i + 1);
    shouldBe(readAll(), 2 * i + 31);
}

// Variables that show up after the readers were linked, in another script of the same global object.
function readLater() { return typeof declaredLater === "undefined" ? -1 : declaredLater; }
function readLaterLexical() { try { return declaredLaterLexical; } catch (e) { return e instanceof ReferenceError ? -1 : -2; } }
noInline(readLater);
noInline(readLaterLexical);
for (let i = 0; i < 20000; ++i) {
    shouldBe(readLater(), -1);
    shouldBe(readLaterLexical(), -1);
}
(0, eval)("var declaredLater = 5;");
loadString("let declaredLaterLexical = 6;");
for (let i = 0; i < 20000; ++i) {
    shouldBe(readLater(), 5);
    shouldBe(readLaterLexical(), 6);
}
(0, eval)("declaredLater = 7; declaredLaterLexical = 8;");
shouldBe(readLater(), 7);
shouldBe(readLaterLexical(), 8);

// A global object property that becomes shadowed by a global lexical binding.
this.shadowed = 1;
function readShadowed() { return shadowed; }
noInline(readShadowed);
for (let i = 0; i < 20000; ++i)
    shouldBe(readShadowed(), 1);
loadString("let shadowed = 2;");
for (let i = 0; i < 20000; ++i)
    shouldBe(readShadowed(), 2);
loadString("shadowed = 3;");
shouldBe(readShadowed(), 3);
