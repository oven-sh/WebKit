//@ skip if !$aot
//@ runDefault("--useAOT=1", "--sizeClassProgression=1.3")

const sizeClassProgression = jscOptions().sizeClassProgression;
if (sizeClassProgression !== 1.3)
    throw new Error(`sizeClassProgression: expected 1.3 but got ${sizeClassProgression}`);
