// === r47 triage ===
// ID: r47-002
// SIGNATURE: AddressSanitizer: SEGV::std::__atomic_base::fetch_add(unsigned int, std::memory_order)|WTF::DeferrableRefCountedBase::ref() const|WTF::DefaultRefDerefTraits::refIfNotNull(JSC::ArrayBuffer*)
// EXIT: 134
// SOURCE: /root/WebKit/WebKitBuild/Fuzz/fuzzilli-storage-r47/crashes/program_20260619094343_C6BCD3A0-2F58-499B-819C-DC2EDB37C10C_deterministic.js
// ==================
const v2 = { maxByteLength: 257104451 };
const v4 = new ArrayBuffer(5, v2);
const v6 = new Int16Array(v4);
function f9(a10) {
    for (let v11 = 0; v11 < 5; v11++) {
        Atomics.and(a10);
        a10.s1 = v2;
    }
    return Int16Array;
}
new Thread(f9, v6);
gc();
// CRASH INFO
// ==========
// TERMSIG: 11
// STDERR:
// 
// STDOUT:
// 
// FUZZER ARGS: /root/fuzzilli/.build/release/FuzzilliCli --profile=jscthreads --storagePath=/root/WebKit/WebKitBuild/Fuzz/fuzzilli-storage-r47 --timeout=1000 --jobs=4 /root/WebKit/WebKitBuild/Fuzz/bin/jsc
// TARGET ARGS: /root/WebKit/WebKitBuild/Fuzz/bin/jsc --validateOptions=true --thresholdForJITSoon=10 --thresholdForJITAfterWarmUp=10 --thresholdForOptimizeAfterWarmUp=100 --thresholdForOptimizeAfterLongWarmUp=100 --thresholdForOptimizeSoon=100 --thresholdForFTLOptimizeAfterWarmUp=1000 --thresholdForFTLOptimizeSoon=1000 --validateBCE=true --useJSThreads=true --reprl
// CONTRIBUTORS: NullGenerator, StringGenerator, IntArrayGenerator, PropertyAtomicsGenerator, RegExpGenerator, IntegerGenerator, ResizableArrayBufferGenerator
// EXECUTION TIME: 13ms
