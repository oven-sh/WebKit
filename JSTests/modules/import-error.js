import { shouldBe } from "./resources/assert.js"

function moduleURL(name) {
    return import.meta.url.replace(/[^/]*$/, "import-error/" + name);
}

Promise.all([
    import('./import-error/import-not-found.js')
        .then($vm.abort, function (error) {
            shouldBe(String(error), `SyntaxError: Export named 'B' not found in module '${moduleURL("export-not-found.js")}'.`);
        }).catch($vm.abort),
    import('./import-error/import-ambiguous.js')
        .then($vm.abort, function (error) {
            shouldBe(String(error), `SyntaxError: Export named 'B' cannot be resolved due to ambiguous multiple bindings in module '${moduleURL("export-ambiguous.js")}'.`);
        }).catch($vm.abort),
    import('./import-error/import-default-from-star.js')
        .then($vm.abort, function (error) {
            shouldBe(String(error), `SyntaxError: Missing 'default' export in module '${moduleURL("export-default-from-star.js")}'.`);
        }).catch($vm.abort),
]).catch($vm.abort);
