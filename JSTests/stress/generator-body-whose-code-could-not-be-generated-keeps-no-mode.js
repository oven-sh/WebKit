//@ runDefault("--useDollarVM=1")
//@ runDefault("--useDollarVM=1", "--useJIT=0")

// The body of a generator keeps the code it first ran in (ScriptExecutable::newCodeBlockFor), because a suspended body
// only resumes in the code that suspended it. A body whose code could not be generated has not run in any code. Here
// the parser runs out of stack for the body: an expression nested 400 deep, asked for at the edge of the stack, so
// next() throws and there is no code. A debugger attaches after that. The body then gets code with the debug hooks,
// like every function that had no code when the debugger attached. A body that was tied to what was asked for when
// its generation failed would have no debug hooks for as long as it lives.

const depth = 400;
const gen = (0, eval)("(function* gen() { yield " + "(".repeat(depth) + "$vm.wasCompiledWithDebuggerHooks($vm.codeBlockForFrame(0))" + ")".repeat(depth) + "; })");
// Every generator of gen runs the one linked body. This one is started after the debugger attached.
const probe = gen();

let failedGenerations = 0;
let hasRun = false;
function atTheStackEdge() {
    try {
        atTheStackEdge();
    } catch (error) {
        if (failedGenerations < 3 && !hasRun) {
            let generator;
            try {
                generator = gen();
            } catch {
                throw error;
            }
            try {
                generator.next();
                hasRun = true;
            } catch {
                failedGenerations++;
            }
        }
        throw error;
    }
}
try {
    atTheStackEdge();
} catch { }

// The body needs far more stack than a call of gen() does, so its generation fails in every frame that is tried.
if (failedGenerations !== 3 || hasRun)
    throw new Error("the generation of the body failed " + failedGenerations + " times at the stack edge, and the body " + (hasRun ? "ran" : "did not run"));

$vm.enableDebuggerModeWhenIdle(); // attaches a debugger to this realm, and deletes all code

setTimeout(() => {
    const hasDebugHooks = probe.next().value;
    if (hasDebugHooks !== true)
        throw new Error("the body has debug hooks: " + hasDebugHooks);
}, 0);
