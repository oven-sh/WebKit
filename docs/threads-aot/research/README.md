# Survey of the two repositories

Nine reports that the design rests on. They are **not normative**: they record how things were at two commits, with `path:line` for each statement, so that a reader can check it and so that the survey can be repeated when the compiler has moved.

| Report | Subject |
| --- | --- |
| [R1](R1-object-model.md) | Objects with a layout, Extensions, extra properties, closures, environments, classes: layout, and each mutation write by write |
| [R2](R2-ir-proofs-effects.md) | The IR: forms, schedules, proofs, effects, each pass that moves or removes memory accesses, polls, what B3 is told, how to add things |
| [R3](R3-value-flow.md) | The whole-program flow analysis, its results, the table of built-ins, validation, how sharing results could be added |
| [R4](R4-runtime-state.md) | The `Instance`, pinned registers, property caches, method table, watchpoints, modules, lazily made things, the image |
| [R5](R5-heap-gc.md) | The collector and allocation; upstream's `Collector` for several heaps |
| [R6](R6-calls-frames-async.md) | Calling convention, borders, exceptions, faults, stacks, coroutines, promises, jobs, entering the VM |
| [R7](R7-ordinary-objects-interpreter.md) | Arrays, ordinary objects, Structures, Map and Set, strings, typed arrays, the interpreter, RegExp, the engine's unlocked caches |
| [R8](R8-bun-half.md) | Bun's half of the compiler, Workers, the table's sources, Bun's runtime per VM, flavours and trim, the development commands |
| [R9](R9-prior-threads-design.md) | Digest of the earlier threads work for the engine with a JIT (oven-sh/WebKit#249): API, memory model, hard problems and their cost, tests |

Commits: engine `5c2923e566cb` (oven-sh/WebKit#757), Bun `8bb8d41bffe` (oven-sh/bun#45631), earlier threads work: branch `sosuke/threads` of oven-sh/WebKit.

Nothing was built or run for the survey: every statement is from reading sources. Each report ends with "Not found / not sure".

Placeholders in paths: `<engine>/` is the root of oven-sh/WebKit at the first commit, `<bun>/` the root of oven-sh/bun at the second, `<threads-branch>/` the root of the earlier branch, `<bun-threads-branch>/` its companion branch of Bun.
