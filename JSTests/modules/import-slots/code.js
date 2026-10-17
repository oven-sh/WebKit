// The executable a function's code belongs to (the end of codeBlockFor's chain): the same for
// functions that share code, whichever tier either is in when asked.
export const executableOf = f => /->(0x[0-9a-f]+),/.exec($vm.codeBlockFor(f))[1];
export const sameCode = (f, g) => executableOf(f) === executableOf(g);
