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
            errorMessage = String(error);
        }
        shouldBe(errorMessage, `SyntaxError: Export named 'x' not found in module '${callerSourceOrigin().replace("re-execute-error-module.js", "resources/error-module.js")}'.`);
    }
    {
        let errorMessage = null;
        try {
            await import("./resources/error-module.js");
        } catch (error) {
            errorMessage = String(error);
        }
        shouldBe(errorMessage, `SyntaxError: Export named 'x' not found in module '${callerSourceOrigin().replace("re-execute-error-module.js", "resources/error-module.js")}'.`);
    }
}()).catch(abort);
