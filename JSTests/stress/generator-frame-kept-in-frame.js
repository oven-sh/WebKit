//@ requireOptions("--forceGeneratorFrameWriteThrough=1")

load("./resources/generator-frame-tests.js", "caller relative");

runGeneratorFrameTests(testLoopCount / 100);
