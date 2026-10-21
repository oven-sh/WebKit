//@ requireOptions("--useZombieMode=1", "--thresholdForStringReplaceCache=0")
// The same checks with every global regexp replace routed through the string replace cache path.
load("./string-replace-function-result-written-in-place.js", "caller relative");
