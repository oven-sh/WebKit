// The executable a function's code belongs to (the end of codeBlockFor's chain): the same for
// functions that share code, whichever tier either is in when asked. Two whole codeBlockFor strings
// are not, even back to back: a call finishes the compilations that are in flight after it has read
// the function's code block, and so can describe the block that it has just replaced.
// (A-F: Windows prints pointers with capital hex digits.)
export const executableOf = f => /->(0x[0-9a-fA-F]+),/.exec($vm.codeBlockFor(f))[1];
export const sameCode = (f, g) => executableOf(f) === executableOf(g);
