var abort = $vm.abort;

function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}`);
}

(async function () {
    {
        let errorMessage = null;
        try {
            await import("./resources/error-module.js");
        } catch (error) {
            // This fork's message names the module by its URL; compare with the directory taken off.
            errorMessage = String(error).replace(/file:\/\/.*\//, "");
        }
        shouldBe(errorMessage, `SyntaxError: Export named 'x' not found in module 'error-module.js'.`);
    }
    {
        let errorMessage = null;
        try {
            await import("./resources/error-module.js");
        } catch (error) {
            // This fork's message names the module by its URL; compare with the directory taken off.
            errorMessage = String(error).replace(/file:\/\/.*\//, "");
        }
        shouldBe(errorMessage, `SyntaxError: Export named 'x' not found in module 'error-module.js'.`);
    }
}()).catch(abort);
