import { shouldBe } from "./resources/assert.js";

{
    let error = null;
    try {
        await import("./resources/import-attributes-unsupported-1.js");
    } catch (e) {
        error = e;
    }
    shouldBe(String(error), `SyntaxError: Unexpected keyword 'true'. Expected an attribute value.`);
}
{
    let error = null;
    try {
        await import("./resources/import-attributes-unsupported-2.js");
    } catch (e) {
        error = e;
    }
    // Bun: every attribute is kept for the host's loaders (ScriptFetchParameters::attributes()), so a key JSC does not
    // know is not an error; upstream throws `SyntaxError: Import attribute "unsupported" is not supported`.
    shouldBe(error, null);
}
{
    let error = null;
    try {
        await import("./resources/import-attributes-unsupported-3.js");
    } catch (e) {
        error = e;
    }
    shouldBe(String(error), `SyntaxError: Unexpected number '42'. Expected an attribute key.`);
}
