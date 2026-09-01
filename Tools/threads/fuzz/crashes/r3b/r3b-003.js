// === r3b triage ===
// ID: r3b-003
// SIGNATURE: exit-3::
// KIND: exit-3
// EXIT: 3
// SOURCE: /root/WebKit/WebKitBuild/Fuzz/fuzzilli-storage/crashes/program_20260619060545_92F15D65-8DC0-4164-A370-4BF6121D7B8C_flaky.js
// ==================
const v2 = new Float32Array(0);
const v5 = [[0,1073741823,0,v2],v2,v2];
const v7 = { maxByteLength: 1000 };
new Uint8Array(2347);
function f12() {
    return 1073741823;
}
[f12,Float32Array,Float32Array,f12,0];
new Float64Array(127);
const v20 = ("7MjpB").normalize();
const v22 = ([123736.20085078245,312.40743345371584,-3.949428491341635,0.550616234362865,-1000.0]).constructor;
const v23 = { maxByteLength: 0 };
v5?.[1];
const v26 = SharedArrayBuffer.prototype;
try { v26.grow(); } catch (e) {}
v22(v20).entries().toArray();
function F31(a33, a34) {
    if (!new.target) { throw 'must be called with new'; }
    a33.big();
    function f36() {
        Uint8Array.of(191, 127, 223, 5, 144, 32, 129);
        f36();
    }
    f36();
    try { this.constructor(); } catch (e) {}
    a33[2].startsWith(v22);
    a34[0] = a34;
}
function F53(a55 = [-1.7976931348623157e+308,437.54278732677017,3.0,0.7564176354963756]) {
    if (!new.target) { throw 'must be called with new'; }
    const v56 = this.constructor;
    try { new v56(); } catch (e) {}
}
const v58 = new F31("NFC", "7MjpB");
try { v58(); } catch (e) {}
new F31("7MjpB", v20);
new F31(v20, "NFC");
const v62 = new F31("NFC", "NFC");
~9007199254740991n;
let v65 = 9007199254740991n ^ 9007199254740991n;
v65--;
const v68 = WeakMap.bind();
try { v68.apply(F31, v62); } catch (e) {}
const v70 = WeakMap.prototype;
v70.b = v70;
gc();
// CRASH INFO
// ==========
// TERMSIG: 6
// STDERR:
// In call from next#AYbUto:[0x7e755c2e8d40->0x7e755c506e00, BaselineFunctionCall, 114 (StrictMode)] <none> to arrayIteratorNextHelper#Cp8Z2u:[0x7e755c2e8f00->0x7e755c507ee0, BaselineFunctionCall, 130 (ShouldAlwaysBeInlined) (StrictMode)]: caller's DFG capability level is not set.
// STDOUT:
// 
// FUZZER ARGS: /root/fuzzilli/.build/release/FuzzilliCli --profile=jscthreads --storagePath=/root/WebKit/WebKitBuild/Fuzz/fuzzilli-storage --resume --timeout=1000 --jobs=4 --resume /root/WebKit/WebKitBuild/Fuzz/bin/jsc
// TARGET ARGS: /root/WebKit/WebKitBuild/Fuzz/bin/jsc --validateOptions=true --thresholdForJITSoon=10 --thresholdForJITAfterWarmUp=10 --thresholdForOptimizeAfterWarmUp=100 --thresholdForOptimizeAfterLongWarmUp=100 --thresholdForOptimizeSoon=100 --thresholdForFTLOptimizeAfterWarmUp=1000 --thresholdForFTLOptimizeSoon=1000 --validateBCE=true --useJSThreads=true --reprl
// CONTRIBUTORS: SpliceMutator, ArrayGenerator, BigIntGenerator, TypedArrayGenerator, TrivialFunctionGenerator, IntegerGenerator
// EXECUTION TIME: 8ms
