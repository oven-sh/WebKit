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
        shouldBe(errorMessage, `SyntaxError: Export named 'x' not found in module '${callerSourceOrigin().slice(0, callerSourceOrigin().lastIndexOf("/") + 1)}resources/error-module.js'.`);
    }
    {
        let errorMessage = null;
        try {
            await import("./resources/error-module.js");
        } catch (error) {
            errorMessage = String(error);
        }
        shouldBe(errorMessage, `SyntaxError: Export named 'x' not found in module '${callerSourceOrigin().slice(0, callerSourceOrigin().lastIndexOf("/") + 1)}resources/error-module.js'.`);
    }
}()).catch(abort);
