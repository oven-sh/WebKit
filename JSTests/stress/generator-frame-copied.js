//@ requireOptions("--useGeneratorFrameWriteThrough=0")

load("./resources/generator-frame-tests.js", "caller relative");

runGeneratorFrameTests(testLoopCount / 100);
