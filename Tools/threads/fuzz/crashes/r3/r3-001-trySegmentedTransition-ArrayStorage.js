try {
    [3221225471,3221225471];
    const v4 = 7 - 7;
    Int32Array.e = Int32Array;
    const v6 = new Int32Array(7);
    try { v6.reduce(v4); } catch (e) {}
    function f10() {
        return 2036021036;
    }
    const v12 = Symbol.iterator;
    const v21 = {
        [v12]() {
            let v14 = 10;
            const v20 = {
                next() {
                    v14--;
                    const v18 = v14 == 0;
                    return { done: v18, value: v14 };
                },
            };
            return v20;
        },
    };
    v21.a = v21;
    v21.b = v21;
    128 >= 128;
    const v25 = 128 - 128;
    v25 >> v25;
    const v28 = new Uint32Array(128);
    ArrayBuffer.name;
    let v31 = 3221225469;
    v31++;
    const v33 = { maxByteLength: v31 };
    v33.b = v33;
    v33.toLocaleString().charAt(Uint32Array);
    const v37 = new ArrayBuffer(3701, v33);
    const v39 = new Int32Array(v37);
    const t37 = v39.constructor;
    new t37(v39, v33, v33);
    v39[645];
    new Set();
    new Set();
    const v47 = /zq(?: foo )/dsgm;
    v47.toString();
    const v49 = v47.toString();
    const v50 = [];
    const v66 = {
        set d(a52) {
            try {
                let v56;
                try { v56 = new ThreadLocal(); } catch (e) {}
                try { v56.value = v56; } catch (e) {}
                function f57(a58) {
                    a58?.value;
                    try { a58.value = a58; } catch (e) {}
                    a58?.value;
                }
                let v61;
                try { v61 = new v31(f57, v56); } catch (e) {}
                v56?.value;
                try { v61.join(); } catch (e) {}
                super[67](a52, a52);
            } catch(e65) {
            }
        },
    };
    const v68 = Atomics.store(v50);
    const v69 = v68 ?? v68;
    v69 ?? v69;
    async function f71(a72, a73) {
        a72 >>> a72;
        try { a73.wait(a73, Set, a72, a73); } catch (e) {}
        a72 / a72;
        for await (const v77 of v39) {
            v77 - v77;
            v77 != v77;
        }
        const v81 = await v50;
        try { v81.some(a73); } catch (e) {}
        return v47;
    }
    f71(v28, v33);
    f71(3701, Atomics).finally(v49);
    const v97 = {
        3340997507: f10,
        [v12]() {
            let v87 = 10;
            const v93 = {
                next() {
                    v87--;
                    const v91 = v87 == 0;
                    return { done: v91, value: v87 };
                },
            };
            return v93;
        },
        set [v39](a95) {
            super.d;
        },
    };
    v97.e = v97;
} catch(e98) {
}
gc();
// CRASH INFO
// ==========
// TERMSIG: 6
// STDERR:
// 
// STDOUT:
// 
// FUZZER ARGS: /root/fuzzilli/.build/release/FuzzilliCli --profile=jscthreads --storagePath=/root/WebKit/WebKitBuild/Fuzz/fuzzilli-storage --resume --timeout=1000 --jobs=4 --resume /root/WebKit/WebKitBuild/Fuzz/bin/jsc
// TARGET ARGS: /root/WebKit/WebKitBuild/Fuzz/bin/jsc --validateOptions=true --thresholdForJITSoon=10 --thresholdForJITAfterWarmUp=10 --thresholdForOptimizeAfterWarmUp=100 --thresholdForOptimizeAfterLongWarmUp=100 --thresholdForOptimizeSoon=100 --thresholdForFTLOptimizeAfterWarmUp=1000 --thresholdForFTLOptimizeSoon=1000 --validateBCE=true --useJSThreads=true --reprl
// CONTRIBUTORS: 
// EXECUTION TIME: 48ms
