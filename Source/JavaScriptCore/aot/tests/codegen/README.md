# Tests of lowering: IR in, B3 or Air out

A test that runs `lower-to-b3` or `lower-to-air` hands `testaot` a program as IR text and checks the code the back end generates for one function.
Nothing runs, nothing is timed and no hardware counter is read, so every machine can check it. `../README.md` has the full grammar of the directives, patterns and commands.

## What the two commands print

- `lower-to-b3 f1` prints the B3 procedure of `f1` as the code generator built it. `lower-to-b3 optimized f1` prints it after the last B3 phase, and
  `after=<step>` after one step of `b3/B3Generate.cpp`. B3 is above instruction selection, so one set of check lines covers x86-64 and ARM64.
- `lower-to-air f1` prints the Air after register allocation, about one line per machine instruction. It differs by target, so it needs a `REQUIRES`.
- `loops` prints only the innermost loops. `lower-to-air loops` starts each with `instructions: <n>, patchpoints: <n>`; `lower-to-air size` counts a whole function.

## Writing a test

1. Write the smallest program that has the operation in a function of its own. Call it from the top level with values the compiler cannot fold.
2. Get the IR from the front end instead of writing it by hand, so the test covers what real programs send.
   ```
   bun build --compile --aot --aot-out=main.bin --outfile=main main.ts
   testaot print main.bin > main.ir
   ```
   Keep the `module`, `entry`, `typesite`, `layout`, `variable` and `function` records of your module, which come first. A stand-in replaces the library.
3. Put the source on top as comments, then the header. A count needs one run per target: `RUN(X64)` with `REQUIRES(X64): x86_64 unsanitized-code`, and the same for `ARM64` with `arm64`.
   ```
   # FORM: AsRead
   # OPTIONS: heat=EverythingIsHot disable=pass:InlineCalls
   # RUN: lower-to-b3 optimized f1
   # REQUIRES: unsanitized-code
   ```
4. `testaot run main.ir --show-output` prints the output of each `RUN`. Copy the hot block into check lines between `-LABEL: name="sum" role=Source` and
   `-LABEL: end of function`. Bind numbers (`b@[[x]]`, `#[[body]]`), end a pattern before a register name, and write `Reads:ObjectFields{{*}}` for a numbered heap.
   `slots/field-reads-in-a-row.ir` and `typed-array-loops/float64-array-sum.ir` are good models.
5. Watch it fail: `testaot run main.ir --option=disable=pass:ResolveAccesses`. `testaot list options` lists every `pass:`, `flow:`, `rule:` and `b3:` switch.

## Common mistakes

- The caller decides what is proven. `new Point(input, 2)` makes `y` a 32-bit slot and puts a test in front of `x`. `new Point(+input, +input)` makes two doubles.
- Another pass removes the operation: the call is inlined, or an object literal is passed as its fields. Turn that pass off in `OPTIONS`.
- A body that is a single `Oops` was proven unreachable. With the stand-in library, a built-in written in JavaScript (`Array.from`) returns `undefined`. Include
  its functions and add `# LIBRARY: own`. A built-in that only Bun's table has (`Buffer`) does not parse here: put the test in Bun's `test/aot/engine-tests/codegen`.
- Pin a count only for code without calls. The moves around a call follow the calling convention, which differs on Windows: `REQUIRES: not-windows`.
