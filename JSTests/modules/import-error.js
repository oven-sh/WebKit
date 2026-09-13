import { shouldBe } from "./resources/assert.js"

// This fork's messages name the module by its URL; compare with the directory taken off.
function stripDirectory(error) {
    return String(error).replace(/file:\/\/.*\//, "");
}

Promise.all([
    import('./import-error/import-not-found.js')
        .then($vm.abort, function (error) {
            shouldBe(stripDirectory(error), `SyntaxError: Export named 'B' not found in module 'export-not-found.js'.`);
        }).catch($vm.abort),
    import('./import-error/import-ambiguous.js')
        .then($vm.abort, function (error) {
            shouldBe(stripDirectory(error), `SyntaxError: Export named 'B' cannot be resolved due to ambiguous multiple bindings in module 'export-ambiguous.js'.`);
        }).catch($vm.abort),
    import('./import-error/import-default-from-star.js')
        .then($vm.abort, function (error) {
            shouldBe(stripDirectory(error), `SyntaxError: Missing 'default' export in module 'export-default-from-star.js'.`);
        }).catch($vm.abort),
]).catch($vm.abort);
