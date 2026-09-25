//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// A bytecode cache's deferred decoding (thin child executables, lazy
// SymbolTable constants) and prelinked module records' on-demand maps
// materialize on first use with one mutator in mind; with the GIL off several
// threads materialized them at once (a union settled twice, one Decoder's
// tables grown under each other, a record's flag set before its maps). The
// GIL off a Decoder does not defer into its payload and the activation
// checklist turns usePrelinkedModuleInfo off, so such a payload is decoded
// eagerly under the compilation lock; with the GIL on all three stay on.
// The races themselves need a bytecode cache that outlives a process and are
// measured by a two-process driver (PERF-RESULTS / LANDING-PLAN, ninth round).

const [thin, lazySymbolTables, prelinked] = $vm.lazyDecodingOptions();
if ($vm.useThreadGIL()) {
    if (!thin || !lazySymbolTables || !prelinked)
        throw new Error(`GIL on keeps the deferred decoding: ${thin} ${lazySymbolTables} ${prelinked}`);
} else if (thin || lazySymbolTables || prelinked)
    throw new Error(`GIL off decodes eagerly: ${thin} ${lazySymbolTables} ${prelinked}`);
