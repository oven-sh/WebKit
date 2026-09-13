import { shouldBe } from "./resources/assert.js"

const directory = import.meta.url.slice(0, import.meta.url.lastIndexOf("/") + 1);

Promise.all([
    import('./import-error/import-not-found.js')
        .then($vm.abort, function (error) {
            shouldBe(String(error), `SyntaxError: Export named 'B' not found in module '${directory}import-error/export-not-found.js'.`);
        }).catch($vm.abort),
    import('./import-error/import-ambiguous.js')
        .then($vm.abort, function (error) {
            shouldBe(String(error), `SyntaxError: Export named 'B' cannot be resolved due to ambiguous multiple bindings in module '${directory}import-error/export-ambiguous.js'.`);
        }).catch($vm.abort),
    import('./import-error/import-default-from-star.js')
        .then($vm.abort, function (error) {
            shouldBe(String(error), `SyntaxError: Missing 'default' export in module '${directory}import-error/export-default-from-star.js'.`);
        }).catch($vm.abort),
]).catch($vm.abort);
