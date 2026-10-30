//@ requireOptions("--useLazyModuleFunctionDeclarations=0")
// The same checks where the environment of a module makes the functions of its declarations at once,
// and not when a binding is first read.
import "./export-default-function-name-for-display.js";
