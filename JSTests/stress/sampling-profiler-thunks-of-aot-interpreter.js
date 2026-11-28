//@ skip if !$aot
//@ runDefault("--useAOT=1")

// The thunk stands in for the host function, and calls a C function that cannot tell the VM which frame called it.
if (platformSupportsSamplingProfiler()) {
    load("./sampling-profiler/samplesWithoutFrames.js", "caller relative");

    const text = ["a".repeat(50000), "b"].join("");
    function callIndexOf() { return text.indexOf("b"); }

    // The conversion is made by a function that does tell, so that this is the last frame to be known.
    const object = { };
    function caller() {
        String(object);
        for (let i = 0; i < testLoopCount / 100; i++)
            callIndexOf();
    }
    caller();

    startSamplingProfiler({ intervalInMicroseconds: 50, recordsSamplesWithoutFrames: true });
    expectMostStacksToInclude(["indexOf", "callIndexOf", "caller"], sampleOnly(() => {
        for (const end = preciseTime() + 0.002; preciseTime() < end;)
            caller();
    }));
}
