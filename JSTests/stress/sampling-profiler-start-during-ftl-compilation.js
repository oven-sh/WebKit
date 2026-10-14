//@ runFTLEager

// Only means something with concurrent compilation and low thresholds. The numbers are what keeps
// the FTL threads busy at the moment the profiler starts, and the process alive until they are done.
if (platformSupportsSamplingProfiler()) {
    const functions = [];
    for (let k = 0; k < 300; k++)
        functions.push(new Function("count", `let sum = ${k}; for (let i = 0; i < count; i++) { sum += (i ^ ${k}) * 3; sum = (sum % 1000003) | 0; } return sum;`));

    for (let round = 0; round < 30; round++) {
        if (round === 8)
            startSamplingProfiler();
        for (const f of functions)
            f(40);
    }
}
