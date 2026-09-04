//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--verifyConcurrentButterfly=1", "--useJIT=0")
// The LLInt's get_by_val slow path reads a segmented array. See the resource file.
load("../resources/segmented-out-of-bounds-read.js", "caller relative");
