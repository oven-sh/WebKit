//@ runFTLEager

// Only means something with concurrent compilation and low thresholds. There are enough functions to keep the
// FTL threads busy at the moment the profiler starts, and enough calls after it for them to finish.
if (platformSupportsSamplingProfiler()) {
    const functions = [];
    for (let k = 0; k < 300; k++)
        functions.push(new Function("count", `let sum = ${k}; for (let i = 0; i < count; i++) { sum += (i ^ ${k}) * 3; sum = (sum % 1000003) | 0; } return sum;`));

    const startAt = testLoopCount >> 2;
    for (let i = 0; i < testLoopCount; i++) {
        if (i === startAt)
            startSamplingProfiler();
        functions[i % functions.length](40);
    }
}
