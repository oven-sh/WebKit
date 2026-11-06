//@ requireOptions("--useSharedArrayBuffer=0")
// Bun: upstream renamed the option useSharedArrayBuffer to alwaysUseSharedArrayBuffer (325936@main). The old name is an alias here, so that
// BUN_JSC_useSharedArrayBuffer=0 still takes the global away. The shell turns the option on before it reads its arguments.
if (typeof SharedArrayBuffer !== "undefined")
    throw new Error("--useSharedArrayBuffer=0 did not take the global SharedArrayBuffer away");
