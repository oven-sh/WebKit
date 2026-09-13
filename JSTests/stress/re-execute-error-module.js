var abort = $vm.abort;
var moduleURL = callerSourceOrigin().replace(/[^/]*$/, "resources/error-module.js");

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
            errorMessage = String(error);
        }
        shouldBe(errorMessage, `SyntaxError: Export named 'x' not found in module '${moduleURL}'.`);
    }
    {
        let errorMessage = null;
        try {
            await import("./resources/error-module.js");
        } catch (error) {
            errorMessage = String(error);
        }
        shouldBe(errorMessage, `SyntaxError: Export named 'x' not found in module '${moduleURL}'.`);
    }
}()).catch(abort);
