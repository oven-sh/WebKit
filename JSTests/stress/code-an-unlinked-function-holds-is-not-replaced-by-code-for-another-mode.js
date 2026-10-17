//@ runDefault("--useDollarVM=1", "--forceCodeBlockToJettisonDueToOldAge=1")
//@ runDefault("--useDollarVM=1", "--useBytecodeOptimizer=1", "--forceCodeBlockToJettisonDueToOldAge=1")

// Two realms share the unlinked function of a generator body, and a debugger attaches to this realm. The other realm
// then runs the body first: its code is the code the unlinked function holds, and its generator suspends in it. This
// realm asks for the same body with the debug hooks and gets code generated for it.
//
// That code must not take the place of the held code. The other realm's CodeBlock is collected (it is old), and when
// its generator resumes, the new CodeBlock has to be made of the very code the generator is suspended in. Code that is
// generated again is not always the same code: the bytecode optimizer is handed the enclosing scopes only the first time
// (takeParentDeclaredNames), and what it generates without them keeps other registers across a yield.

const source = "let last = 0, prev = 0; return function outer() { let mid = 1; return function* gen(tag) { let n = 0; while (n < 500) { prev = last; last = yield tag + n + mid + prev; n += last; } return n; }; };";
const otherRealm = createGlobalObject();
// Made before the debugger attaches, neither started.
const debugged = new Function(source)()()("d");
const plain = new otherRealm.Function(source)()()("p");

$vm.enableDebuggerModeWhenIdle(); // attaches a debugger to this realm only, and deletes all code

setTimeout(() => {
    const values = [];
    values.push(plain.next().value); // the code the unlinked function holds from here on
    values.push(debugged.next().value); // code with the debug hooks, for this realm's CodeBlock alone
    for (let i = 0; i < 12; i++)
        fullGC(); // the other realm's CodeBlock goes
    values.push(plain.next(5).value, plain.next(7).value, plain.next(9).value);
    values.push(debugged.next(2).value, debugged.next(3).value);

    const expected = "p010 d010 p515 p1217 p2119 d212 d513";
    if (values.join(" ") !== expected)
        throw new Error(values.join(" ") + ", expected " + expected);
}, 0);
