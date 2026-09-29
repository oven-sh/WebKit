import { shouldBe } from "./resources/assert.js"


Promise.all([
    import('./indirect-export-error/indirect-export-not-found.js')
        .then($vm.abort, function (error) {
            shouldBe(String(error), `SyntaxError: export 'B' not found in './indirect-export-not-found-2.js'`);
        }).catch($vm.abort),
    import('./indirect-export-error/indirect-export-ambiguous.js')
        .then($vm.abort, function (error) {
            shouldBe(String(error), `SyntaxError: Cannot export 'B' multiple times in './indirect-export-ambiguous-2.js'`);
        }).catch($vm.abort),
    import('./indirect-export-error/indirect-export-default.js')
        .then($vm.abort, function (error) {
            shouldBe(String(error), `SyntaxError: export default cannot be used with export *`);
        }).catch($vm.abort),
    // exportEntries iterates in source order (OrderedHashMap), so the reported
    // name is always the first unresolvable export — not whichever the hash
    // table happens to place first.
    import('./indirect-export-error/indirect-export-source-order.js')
        .then($vm.abort, function (error) {
            shouldBe(String(error), `SyntaxError: export 'aaa' not found in './indirect-export-source-order-2.js'`);
        }).catch($vm.abort),
]).catch($vm.abort);
