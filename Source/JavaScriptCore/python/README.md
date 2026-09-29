# Python in JavaScriptCore

A second front end. Python source is scanned, parsed and compiled to JavaScriptCore bytecode, and runs in the same interpreter and
compilers, on the same heap, with the same values as JavaScript. There is no CPython here, and no boundary: a Python list *is* a
JavaScript array, a Python function *is* a JavaScript function, and a Python exception *is* an `Error`.

The language is Python 3.14. CPython is the specification: `Grammar/python.gram`, `Parser/Python.asdl`, `Python/symtable.c`,
`Python/codegen.c`, `Objects/*.c`, and its test suite. Where a function here follows one of CPython's, the comment above it names it.

## The pipeline

| | | checked against CPython by |
|---|---|---|
| `PythonLexer` | source to tokens. It alone reads the source | |
| `PythonParser` | tokens to `PythonAST`, by recursive descent | `$vm.pythonAST`: every file of `Lib`, and 150,000 damaged programs |
| `PythonSymbolTable` | what each name refers to | `$vm.pythonSymbolTable`: the same files, and 60,000 meddled programs |
| `PythonCodeGenerator` | the tree to bytecode, through `BytecodeGenerator` | running programs |
| the rest | the object model and the built-in types | running programs, and the audits |

The tree and the symbol table are CPython's own, node for node and flag for flag. The `ast` and `symtable` modules have to give
them out (see *Syntax trees*), and it means each stage can be compared with CPython's by itself.

## Decisions

### Which language a piece of source is in

`SourceProviderSourceType::Python`, beside `Program`, `Module` and `WebAssembly`. Everything that has code can reach its provider, and
`JSModuleLoader::makeModule` switches on it as it does for the others.

`Python::makeSource()` makes the provider. From bytes it does what PEP 263 says: the byte order mark, the coding line, the codec, and
newlines. That is a function of the engine and not something a provider does when it is made, since a codec can be written in Python
and can raise.

### All Python code is function code

A module's body, a class's body, a function, a lambda, a generator expression, and what is evaluated when it is asked for are each an
`UnlinkedFunctionExecutable`, with a `Python::FunctionInfo` that says which. So there is one place where the languages part:
`generateUnlinkedFunctionCodeBlock`.

Functions are compiled when first called, as JavaScript's are, from their range of the source. What an inner function needs to know
of the blocks around it is worked out when those are compiled, and kept in its `FunctionInfo`. **The whole file is checked when it is
loaded**, since Python reports a syntax error anywhere in a file before running any of it. What is wrong travels in a `ParserError`,
as for JavaScript, which has room for what Python has to say besides.

### What is evaluated when it is asked for

An annotation, the bound or the default of a type parameter, and what a `type` statement makes an alias of are not evaluated where they are
written but when they are asked for, each by a function that is made for it (PEP 649, 695 and 696). And `def f[T]`, `class C[T]` and `type A[T]`
have their type parameters for the variables of a function that is made for it and called at once.

- **These have no source of their own.** They are kinds of code like any other (`CodeKind::Annotations`, `TypeParameters`, `Evaluator`) and are
  compiled when first called like any other, from the source of what they belong to: `FunctionInfo::owner` says what kind of thing that is.
- **In a class they see its names without being part of its body.** The class keeps its namespace for them in a variable, `__classdict__`. Once there
  is a class that is the class's `__dict__`, so that what they find is what the class has by then.
- **Which annotations of a class or a module count depends on which statements were come to.** What has the annotations and what evaluates
  them are compiled at different times, so both work out which is which by going through the body in the same way.
- Where the names of a class can be seen so, a comprehension is a function that is called at once, and elsewhere it is part of the code that it is in.
- Under `from __future__ import annotations` an annotation is the expression written out again, as `_PyAST_ExprAsUnicode()` writes it:
  `PythonUnparse.cpp`.
- `list[int]`, `int | str`, type variables and aliases are CPython's `Objects/genericaliasobject.c`, `unionobject.c` and `typevarobject.c`. Like those they
  leave a good deal to the `typing` module, which is Python and is imported when it is first wanted: making a generic class asks it for
  `Generic[T]`. `_typing` is here, since it is only the way to these.

### `BytecodeGenerator` is shared

It has a constructor for a `Python::ScopeNode`, which is a `ScopeNode` whose `emitBytecode` walks a Python tree. Registers, labels,
constants, calls, `try` and `finally`, scopes and generators are `BytecodeGenerator`'s, unchanged.

### Values

| Python | is |
|---|---|
| `None` | `undefined`. `null` is `None` too: `x is None` asks whether it is either |
| `True`, `False` | `true`, `false` |
| `int` | an int32; past 2**31, a BigInt |
| `float` | a double, or a whole float: one with the value of an int32, marked as a float (see `TaggedArithmetic.h`) |
| `str` | a `JSString` |
| `list` | a `JSArray` |
| function | a `JSFunction` |
| generator, coroutine, asynchronous generator | a `JSGenerator` |
| exception | an `ErrorInstance` |
| `bytearray` | a `Uint8Array`, any that is not a `bytes`. So what JavaScript makes is one, a `Buffer` included |
| `bytes` | a `Uint8Array` whose prototype is the class `bytes`, beyond which is `Uint8Array.prototype`. Nothing in Python changes what is in it |
| `tuple`, `dict`, `set`, `complex`, `range`, `slice`, `memoryview`... | cells of their own |
| an instance of a class, a module | a `PyInstance`: an object with inline properties, like a `JSFinalObject` |
| an instance of a class derived from `list`, `bytes`, an exception | the same cell as the base's, with a `ClassInfo` derived from its |
| a class | a `PyType` |
| unbound, deleted | the empty value, as for JavaScript's `let` before it is initialized |

### Being told of what is run

`sys.monitoring` (PEP 669), and `sys.settrace()` and `sys.setprofile()`, which are made out of it as in CPython's `legacy_tracing.c`. It is all in `PythonMonitoring.cpp`.

**There is one version of the code, which has in it a place for each thing that can be told of.** Each does nothing but look at one word of the VM's, as `op_check_traps` does.

| | |
|---|---|
| `op_py_enter` | what a piece of code begins with, and a generator each time it is resumed: `PY_START`, `PY_RESUME`, `PY_THROW` |
| `op_py_line` | the beginning of a line: `LINE`. Before a jump back, `JUMP` too |
| `op_py_call` | before a call: `CALL` |
| `op_py_branch` | before a jump that depends on something, which it is given: `BRANCH_LEFT`, `BRANCH_RIGHT` |
| `op_py_jump` | before a jump forward that depends on nothing: `JUMP` |
| `op_py_leave`, `op_py_ret` | before a yield, and the return: `PY_YIELD`, `PY_RETURN` |
| the unwinder | `RAISE`, `RERAISE`, `EXCEPTION_HANDLED`, `PY_UNWIND` |
| `op_py_iter_next`, `yield from` | which are C++ already: `STOP_ITERATION`, and the branch of a `for` |

- **Why not two versions, one with the places in it, as the debugger has?** They would have two sets of offsets, where `co_lines()`, `co_branches()` and what is told have to agree. And a frame
  that is running has to be told of at once: `pdb.set_trace()` sets `f_trace` on the frames that it was called from, and expects to hear of the next line of each.
- **How deep.** `op_py_enter` counts how deep in Python's calls the thread is (`VM::m_pythonDepth`), and `op_py_ret`, `op_py_leave` and the unwinder count down. Beyond
  `VM::m_pythonLimitUnlessWatched` it takes the slow path, which raises `RecursionError`. That word is 0 while anything is to be told, so the one comparison finds that too.
- **A line is told of if the frame was last on some other**, as in CPython. So there is an `op_py_line` wherever the line changes as the code is written out, and wherever it can be jumped to. The frame
  object has the line that it was last on.
- **`C_RETURN` and `C_RAISE` have no place of their own.** The frame remembers the call that it was told to be making, and its end is told of at the next thing that is told of the frame, nothing being told of
  it in between. If that is the unwinder, it came out of the call if the frame is at the call.
- **What is told by the unwinder is called with the exception set aside**, as the debugger's hooks are, and from the frame that it is told of. What it raises is what is being thrown from there on.
- `INSTRUCTION` is told at each of these places. What the engine runs is its own business.
- How many times an exception is caught and sent on by what nobody wrote is not what it is in CPython, which wraps every generator in a handler, for one.

What it costs when nothing is being told, for each time round: nothing to 2% for a loop or a branch, 1ns of 10 for a call in Baseline and 2.6ns of 23 in the interpreter, and for a step of a generator nothing
in Baseline and 9ns of 51 in the interpreter. The DFG and the FTL can have a watchpoint.

### Going on from another line

`frame.f_lineno = n`, in what is told of a line, is how a debugger has a program go on from somewhere else: `PythonFrameJump.cpp`.

- **It is `op_py_line` that goes somewhere else.** Its slow path says what to run next. The interpreter goes on from there, and compiled code jumps to where `JITCodeMap` has it. Nothing is added to what is run when
  nothing is being told.
- **Where it goes to is just after an `op_py_line`.** A compiler that keeps values in registers from one instruction to the next puts them all back before an instruction that it knows nothing of, which this is. So just
  after one, nothing is expected to be in any register, and just before one, whatever is in them is written over the frame.
- **So a statement begins with its `op_py_line`.** What comes after that depends on nothing but the variables and what is told of below. Which line a statement is on is that of whatever in it is done first, which is
  not known when it is begun, so the instruction is emitted first and where it is from is said when that is come to (`beginLineOfStatement()`).
- **Whether it can be done is decided as CPython decides it**, by what would be on its stack: it can leave a loop, a `with` or an `except`, and go into none, but it can go from one to another of the same kind. Here what
  would be on the stack is in registers, of which each loop has its own. The compiler writes down what there is in each part of the code (`CodeDetails::JumpBlock`), each `op_py_line` says which part it is in, and
  what is needed is copied from the registers of where it is to those of where it is going.
- **CPython has a copy of what comes after `finally` for each way of coming to it**, and can go from one copy into another. Here there is one copy and a register that says how it was come to, which is set.
- **Nothing is done until it is known that all of it can be.** What is refused, and what is not done because the warning that goes with it is an error, leaves the frame as it was.
- **A generator that is resumed is given back only what could be made use of from where it left off.** So the globals and the builtins are found again, which costs a generator nothing until this is done to it. As for
  the rest, whatever the engine's `BytecodeLivenessAnalysis` finds is made use of from where it is going has to be made use of from where it is as well, or have just been set. Otherwise it is refused.
- What is never come to is not there to go to. What comes after a `return`, a `raise`, a `break` or a `continue`, or after what always ends in one, has no lines and no constants, as in CPython.

`programs/jumping-to-another-line.py` tries each two lines of 43 pieces of code, which is 3,400 jumps, and looks at what is said, what is run afterwards and what is told of. What is otherwise on purpose is below.

### Code objects

A code object is a `FunctionExecutable`: a piece of source, and what has to be known to compile it that the source does not say (`FunctionInfo`). Instructions are made from
that when they are wanted, and can be thrown away and made again.

- **`co_code` is what there is to run, which is that: the source, and what is known about it.** It is not instructions. Two pieces of code are equal if they have the same, are called
  the same and begin on the same line, as in CPython, where a file that they say they are from does not come into it either. `code(...)` makes a code object out of it again, so what takes a function to pieces to
  send it somewhere can put it together there.
- **What a program says is known about some source is believed as far as the source bears it out.** `generateFunctionCodeBlock()` looks at what it parsed before it takes it for a
  `def`, and so on for whatever it would otherwise read on trust. All of it is compiled at once, so that what is wrong with it is a `ValueError` from `code()`.
  `JSTests/python/programs/code-constructor-malformed.py` gives it 38,000 that are not quite right.
- `replace()` can change what a code object says of itself: `co_name`, `co_qualname`, `co_filename`, `co_firstlineno`, and `CO_ITERABLE_COROUTINE` in `co_flags`, which is what
  `types.coroutine()` sets. It makes another executable for the same source. All else follows from `co_code` and can be given only as what it is.
- `co_lines()` and `co_positions()` are read out of the engine's `ExpressionInfo`, which has where in the source each instruction is from. Every statement says where it is, `pass` is a `nop`,
  and a constant that has a line to itself says so.
- `co_consts` is put together as CPython puts it together: what is written out in the source and the code of what is defined in it, as they are first come to, then what was worked out from that, and then what
  nothing loads is taken out again, but for the first. It has none of the ints from 0 to 255, which CPython has an instruction for. What is not there is what CPython's own instructions need and these do not: the
  names of the keywords of a call, what `import` is given, the name of a class in its body, how long a sequence has to be in a `match`.
- What is never come to, `if 0:` and `while 0:` and what `__debug__` rules out, is compiled, since that is how it is found what is wrong with it, and jumped over. It has no lines and no constants.

### Constants

**What is made of nothing but constants is worked out when the code is compiled**, as by CPython's `Python/flowgraph.c`, and what that will not do for being too large is not done here either: `PythonConstantFolding.cpp`.
A tuple of constants is a constant. So is a slice, and the defaults of a function. `x in [1, 2]` looks in a tuple and `x in {1, 2}` in a frozenset, `for x in [1, 2, 3]` goes through a tuple, and a list or a set of three
or more constants is a copy of one. CPython works things out by doing them, with the objects. Code here is compiled for no realm in particular, so there are no objects yet, and it is done with what is written down
of them, and only where that is sure to come to what running it would: ints of up to 127 bits, floats, by the very functions that are used when it is run, `+` and `-` of complex numbers, and `str`, `bytes` and
`tuple`. Nothing is made of the rest, nor of what would raise, and it is left to be run, which comes to the same. `programs/constants-worked-out-beforehand.py` tries 147,000 both ways.

**A constant that is an object is made once.** A number or a string is a constant of the engine's. A tuple, a frozenset, a complex, a slice or `Ellipsis` is an object of a realm's, and code that has not been linked is
any realm's. So among the constants of such code is a `PyCodeConstant`, which says which of `co_consts` it stands for, and `CodeBlock` puts the object in its place when the code is linked, which is what it does with
the array for a `JSTemplateObjectDescriptor`. The code object has the objects, so they outlast the instructions, and what the code loads is what `co_consts` has. Those that are alike are one object, however deep in
others they are.

**A list of numbers and strings that is written out is what it is in JavaScript**, `op_new_array_buffer`: an array that has what is in it in common with every other that the same code makes, until one of them is
written to. So it takes as long to make one of thirty as one of three. Nothing in `python/` reaches into an array but by way of `JSArray`, which sees to the copying, as it has to for the arrays that JavaScript writes
out and hands to Python.

**But not what has a `bytes` in it.** JavaScript can write to a `bytes`, and can give its buffer away, so if `b"abc"` were one object it could be made to be something else the next time. There is another each time.
`interop/constants-and-javascript.mjs` tries to change each kind.

### A function that is made from a code object

A variable that an inner function uses is a variable of a `JSLexicalEnvironment`, and a cell is a view of one: the environment, and where in it. That is what
`f.__closure__` is made of. `cell()` makes one that is a variable of nothing, and holds what is in it.

`function(code, globals, closure=cells)` is given cells that can be anyone's, and an environment cannot have a variable that is another's. So the function gets **another
executable for the same source** (`cloneExecutable()`), of which it is known that its free variables are given as cells (`FunctionInfo::variablesGivenAsCells`). Its
environment has the cells themselves for variables, and what is compiled looks into them, as `LOAD_DEREF` does. What is defined inside it finds the same names as cells too. An
ordinary closure pays nothing for this. `f.__closure__` gives back the very cells.

It is another executable even when there is no closure. What has been compiled has in it how far out each variable is, on the understanding that every function made
from the executable is in environments of the same shape.

`f.__code__ = code` does the same, with the cells that the function has already, and then `JSFunction::replaceExecutable()`. A call that has been linked goes by which function is
called and jumps to what it was compiled to, so all that were linked to the old code are unlinked. The DFG makes no `DirectCall` to a function of Python's, that being
linked to an executable for good.

### Where the bytes are is not kept

What is in a `bytearray` moves when it is resized, and JavaScript can give an `ArrayBuffer` away. Either can be done by anything that runs a program's code, and looking at
an argument does: `__index__()`, `__buffer__()`, going through an iterable. So a `std::span` of what is in something is good only until the next such thing, and is not kept.
`Buffer`, in `PythonBytes.h`, is what is kept: it holds the object and finds out where its bytes are each time it is asked. It is what stands between CPython's
`PyObject_GetBuffer()` and `PyBuffer_Release()`, and calls the `__buffer__()` and `__release_buffer__()` of a class that has them. What is written in C++ looks at all its
arguments first, and only then at how much there is of what it is a method of. A slice is resolved in two steps for the same reason, as in CPython:
`PySlice::unpack()`, which can run anything, and `PySlice::adjust()`, which is told the length as it is afterwards. The end of `programs/buffer-protocol.py` has every
method shrink what it is working on from within an argument.

A `bytearray` is a `Uint8Array` like any other, and until something asks for its `ArrayBuffer` it owns the memory that it is in, and Python changes its length where it is: `JSArrayBufferView::reallocateOwnedStorage()`
and what is beside it. What is taken from the front of a large one, `del a[:n]`, which is how what has been dealt with is taken out of what has come in, is not made up for by moving the rest. It begins further on, as
in CPython, and how far on is its `byteOffset`, which is what that would be if it had an `ArrayBuffer`, and is if it is given one. What was given up is given back when it comes to more than what is left.

The optimizing compilers put the length of a typed array, and where its elements are, into the code that they make, if it is an array that the code always uses. They go by nothing being able to change those until
the array has an `ArrayBuffer`, which it is given when the code is put to use, and after which Python may not change its length. But the code is made on another thread, while Python runs. There is no telling a
`bytearray` from any other `Uint8Array`, so not doing it for those is not to be had. So the VM counts how many times memory that a view owns has been changed where it is, `ownedTypedArrayStorageVersion()`, and code
that was made with anything of the kind in it is thrown away if the count has moved since the array was looked at. Only Python moves it. `interop/resized-while-being-compiled.mjs` goes wrong three times in four
without that.

### A class is the prototype of its instances

`instance.[[Prototype]]` is the class, so `type(x)` is a load from `x`'s structure. `class.[[Prototype]]` is what the instances of its first base have for a
prototype: that base, or if JavaScript made it, its `prototype`. Beyond a built-in class that JavaScript has too is JavaScript's: `Array.prototype` beyond `list`,
`TypeError.prototype` beyond `TypeError`.

Python does not walk that chain. A class has its method resolution order, a tuple, and `PyType::lookup` goes through it. Nor does
JavaScript get past the first class in it: see "What JavaScript sees".

A class knows what is derived from it, weakly. So what it remembers about itself, and what compiled code has been told about it, is
forgotten when it or a class that it is derived from changes, and not when any class does.

### Making a class

`newType()` is `type_new_impl()` of `Objects/typeobject.c` and what that calls, in the same order, which is the order of the class's
`__dict__`. `__slots__` are checked, mangled and sorted. The class whose instances are the first to have a `__dict__`, or to be weakly
referred to, has the `__dict__` or `__weakref__` descriptor.

Nothing here is laid out as in CPython. But CPython goes by how instances are laid out to tell which base a class is laid out after,
whether two classes can both be derived from, whether instances can be given `__slots__`, and whether an instance of one class can be
made an instance of another. So a class has `__basicsize__`, `__itemsize__`, `__dictoffset__`, `__weakrefoffset__` and `__flags__` as CPython would
have them, taken from CPython for a built-in class and worked out as CPython does for the rest, and the same questions are answered
from those in the same way. What is in a slot is a property under a private name that goes by where CPython would have the slot.

### Names

| | |
|---|---|
| local | a register. Empty when unbound, and `check_tdz` raises `UnboundLocalError` |
| cell, free | a variable of a `JSLexicalEnvironment`, found by `resolve_scope` as JavaScript's are |
| global | a property of the module, by `py_load_global` and a direct `put_by_id` |
| in a class body, or under `exec` | by name in a mapping, then global |

**A module is an instance of the class `module`, and its properties are the global variables of the code in it.** So `globals()`,
`vars(module)` and `module.__dict__` are one dict, JavaScript sees an object with properties, and a class can be derived from `module`.

Code finds its globals and its builtins in two variables, `.globals` and `.builtins`, of the outermost environment of its scope chain. Which
builtins is settled when it is given its globals, from their `__builtins__`, as in CPython.

`py_load_global dst, globals, builtins, name` is what `globals` has, or failing that `builtins`, or `NameError`. It remembers, in its metadata,
the structure that the one had, that of the other if that is where it was, and the offset. The interpreter and the baseline JIT check
those and load. So shadowing a builtin, deleting what shadowed it, and changing the builtins are all seen at once by code that is
already hot.

### Calls

`f(a, b)` is `call`, with `undefined` for `this`. Python's parameters are JavaScript's, in order, so either language calls the
other's functions with no adapter.

- **A method call does not make a bound method.** `py_load_method` gives the function and `self`, or the callable and nothing, and
  there is a `call` for each case on the same argument registers. In the second case `this` is what the attribute was got from.
- **Keywords, `*args` and `**kwargs` are bound by the caller**, in `callKeywords` and `callSpread`. They call the function with
  every parameter filled in, `*args` already a tuple and `**kwargs` already a dict, and a marker for `this` that says so. So a
  function has two ways in and both are ordinary control flow: it starts with "if `this` is the marker, skip binding".
- Otherwise a function binds what it was given itself, in bytecode: checks the count, fills in defaults, makes the `*args` tuple.
- A function written in C++ gets keywords as extra arguments, and their names for `this`.
- Given keywords, a JavaScript function gets them as an object, as its last argument.

**`f(*values)` can be given any number of them.** What is given to a call goes on the stack, of which there is only so much, and a call is not to fail for that, nor to leave what it calls nothing to run in, nor to
go less deep when it calls itself. Most of what can be called does not need them there, and `callWithKeywords()`, which is where every such call comes to, does not put them there:

- What a function of Python's is given beyond what it has names for is on its way to a tuple, or to an exception. It goes straight there, by the way in that keywords use. So what goes on the stack is never more than
  the function has parameters.
- A class, an instance that has `__call__()`, and a method of a function of Python's only pass on what they are given. They are called from C++ with the arguments where they are: `PyType::call()`,
  `callInstance()`, `PyBoundMethod::call()`.
- A function written in C++ looks for them on the stack, and is done with them when it returns. So they go there if they take no more of the stack than they leave, which is asked of what there is at the time. If
  they would take more, they are put in a `JSCellButterfly`, like the names of the keywords, and that is the one argument, with the names for `this` even if there are none. `NativeArguments` is what looks at
  the arguments and knows of both. It costs a call without keywords nothing, since it is only when `this` is names that there is anything to ask.
- A function of JavaScript's has them on the stack, because that is what its `arguments` are. If there is no room it is a `RangeError`, as when JavaScript gives it too many, which is a `RecursionError` to Python.

`programs/a-great-many-arguments.py` gives up to 1,200,000 to everything that can be called, and 3,000 at each of 900 calls deep. `interop/a-great-many-arguments.mjs` gives them from each language to the other.

### What is written in C++ has a signature

In CPython what is built in has a signature, `__text_signature__`, and the code that takes its arguments apart is generated from it. Here it
has the same signature, as data, and **its arguments are checked against that before it is called** (`checkArguments`). So a
`PyNativeFunction` can rely on its first argument being an instance of the class that it is a method of, and on having the arguments that
are required. It gets any of them by its position however it was given (`args.at(i)`). It does not count them. What is said when they are
wrong is said in one place, in CPython's words. That every one has a signature is asserted when it is made.

The signatures, the docstrings, what kind of thing each attribute of a built-in class is, and how each built-in class is laid out are CPython's
own. `lib/dump-builtin-descriptions.py`, run by CPython, writes them to `lib/builtin-descriptions.json`, which is put into a header when this is
built. A module that is written in C++ is added to the list in that script. What CPython does not have gives its signature where it is
defined, and so does what has one in CPython that a program cannot see. It can begin with what the function is called when its arguments are
wrong, where that is neither its name nor its class's: `typevar(name, *constraints, ...)`.

A built-in class of a module other than `builtins` is named as in CPython, `types.GenericAlias`. That is what is said wherever something is said about
it or its instances, and its `__name__` and `__module__` are the two parts.

### Opcodes

Python gets its own where its semantics are its own *and* it matters how fast they are: arithmetic, comparison, truth, attributes,
subscripts, iteration, unpacking, globals, returning. One opcode serves a family, with the operator as an operand, which every tier but
the interpreter reads at compile time. Everything else is an existing opcode, or a call to a function of the runtime. Those are the
properties of one object, which is a link time constant.

`py_ret` and `py_load_global` are done in the interpreter and the baseline JIT. The rest call C++. Python code is not yet compiled by the DFG
or FTL.

### Exceptions, tracebacks and frames

`raise` is `throw`, and `try` is JavaScriptCore's. `except` compares classes.

Raising again is throwing the same thing again: the `JSC::Exception` that was caught, and not what is in it. The unwinder adds to the
traceback as it goes, and can tell the one from the other.

A frame object is a `PyFrame`. There is at most one for each time that a piece of code is run, and none until it is asked for. While the
frame is on the stack it reads and writes the registers. What leaves a frame says so (`py_ret`, and the unwinder), and the variables are
copied then. So a local variable is copied when it is loaded for later use: anything that is called may change it.

**A frame that is one too deep does not begin.** When `op_py_enter` finds that, the frame is put back where it was before there was anything to be seen of it, at its first instruction.
So it is in no traceback, nothing is told of it, and nothing in it catches the `RecursionError`, which matters for a generator that is resumed inside a `try`. What went wrong is the call.

**What is printed when an exception gets away** (`PythonExceptionDisplay.cpp`) is `TracebackException` of CPython's `Lib/traceback.py`, function for function, which is what CPython prints one with:
what part of a line to point at, and with what; lines that are the same over and over; what led to it, without going round for ever; notes; groups; and the name that may have been meant.

- Where that has the `ast` module parse a piece of a line, to find the operator or the brackets, this has the parser.
- It counts in code points, as Python does, and in columns of a terminal where it lines things up.
- It goes by where each instruction says that it is from, which is what `co_positions()` gives. That has one for each *two bytes* of code, because what wants to know where an instruction is from
  takes what comes at half its offset: `traceback.py` does with `tb_lasti`, and so does `dis`.
- When the standard library is there, `sys.excepthook` can go to its `traceback` module as CPython's does, and will find what it needs.

**`BaseExceptionGroup`, and what `except*` is compiled into calls of** (`PythonExceptionGroups.cpp`), are `Objects/exceptions.c` and `_PyEval_ExceptionGroupMatch()`, function for function.
`ExceptionGroup` has two bases, and is made when a realm is as a class statement would make it, as in CPython.

### Throwing into a generator

A generator of Python's is a `JSGenerator`, and `yield from` and `await` are a loop in bytecode round `yieldFromStep()`, which sends what the generator is sent to what it is waiting on. `throw()` is not done by that
loop. It is `_gen_throw()` of CPython's `Objects/genobject.c`, and is in `resumeGenerator()`, which everything that throws into a generator comes to:

- **What `throw()` was given goes on as it was given**, all three of them, to whatever is waited on at the far end, and only that says whether it is something that can be thrown. On the way it is in a
  `JSCellButterfly`, which is nothing that a program can get hold of. It is handed from one C++ function to the next, and is never itself thrown, so what is tracing or monitoring does not see it.
- **A generator that is waiting is not woken for it.** If what it is waiting on yields, that is what it yields, from where it is. It goes on only if that has come to an end, and then it is *sent* what came of it, whether
  that returned or raised, which `yieldFromStep()` takes for what it is. So what is watching sees the frames that CPython goes into and no others.
- **If it cannot be thrown, the generator is left as it was.** If making the exception goes wrong, what went wrong is what is thrown, as `PyErr_NormalizeException()` has it.
- What JavaScript throws in, with the `throw()` that a generator of JavaScript's has, wakes the generator, and the loop catches it and passes it on.

`programs/throwing-into-what-is-waited-on.py` has it with nothing watching, traced and monitored.

### What is said to be wrong with source

The parser is written by hand and goes down through the grammar, as the one for JavaScript does. CPython's is generated from `Grammar/python.gram`, and **what it says is wrong with source, and where, is
whatever comes of how that grammar is gone through**. So the parser here goes about it in the same way (`Parser::parseModule()` is `_PyPegen_run_parser()`).

- **The source is gone through twice, if it will not parse.** The first time nothing is looked for but whether it parses. The second time, what is in the grammar only to be recognized as a mistake (its
  `invalid_` rules) is tried as well, each where the grammar has it among the alternatives. Whichever is come to first is what is said. If none is, all that there is to say is `invalid syntax`, at the last
  token that was looked at *the first time*. Source that parses is gone through once, and none of this costs it anything.
- **A few things are said the first time:** a token that the grammar insists on (`else`, `try` and `finally` are followed by a colon), and what is found wrong in making something of what has been parsed, such as
  `b'' ''`. And, by an accident of how CPython's parser is generated, what is wrong with the pairs of a dict: it leaves out, the first time, whatever alternative *begins* with an `invalid_` rule, and that one
  begins with the brace. That is why `{1: 2, 3 4}` wants a colon and `{3 4}` a comma.
- **As much as will parse is what is parsed.** `a +` is `a`, with a `+` after it that whoever asked for an expression can make nothing of. It comes to the same for whether source parses, and decides which rule
  is the one to find fault.
- **What comes of a rule is kept, by the token that it began at,** for the rules that CPython does that for. That keeps the second time from taking for ever, and it can be seen. There is a part of the grammar in
  which mistakes are not looked for (`expression_without_invalid`), what is parsed there is kept like the rest, and so what is wrong with it is never found: `f(4, x for x in y)` is told that a generator
  expression must be parenthesized, and `"s" + f(4, x for x in y)` only that it is invalid syntax.
- **The scanner scans the whole source first, and CPython's is asked for one token at a time.** What is said depends on how far it has got: for where something is said to end, for whether the line that
  goes with it has its end, and for which comes first of what the scanner and the parser have found wrong. So the parser keeps the last token that it has looked at (`m_furthest`), and tokens that take up no room keep
  where the tokenizer would have been.
- **After whatever can be called, subscripted or asked for an attribute, a generator expression is looked for,** since `f(x for x in y)` is `f` and one of those. What is looked for when there is none may begin with
  any bracket. That is how what is wrong inside the braces of `a {b c}` comes to be found.

`compile()` for `'eval'` and `'single'` begins elsewhere in the grammar, and only for `'exec'` is a last line that nothing ends taken to be ended. For `'single'` the end of the source is taken for the end of a line
instead, which is why `if x: y` will not compile that way without one. `PyCF_DONT_IMPLY_DEDENT` and `PyCF_ALLOW_INCOMPLETE_INPUT` are for a prompt, which has to tell what is wrong from what is not yet all there.

`JSTests/python/audits/syntax-errors.py` measures all this: some fourteen thousand pieces of source that will not compile, out of CPython's tests and made by changing one token of the programs here. All that is
said of each is compared: the class, `msg`, `lineno`, `offset`, `end_lineno`, `end_offset` and `text`.

**Where more than one thing is wrong, what is said is what is come to first.** CPython generates the code of a function when it comes to the `def`, in the midst of what the function is in. Here a function is
compiled by itself, after what it is in. So what generates code keeps how many functions it had come to when it found something wrong, and those are compiled before that is believed
(`generateFunctionCodeBlock()`). What evaluates the annotations of a function comes before the function, and of a module after everything else in it (`isGeneratedLast()`).
`JSTests/python/programs/which-error-is-found-first.py`.

### Syntax trees

The module `_ast` is the syntax tree as objects, which `ast.py` is made of. `compile(source, ..., PyCF_ONLY_AST)` gives the tree, and `compile(tree, ...)` takes one, which a program may have made or changed:
that is how `pytest` rewrites `assert`.

- **`PythonASDL.h` is CPython's `Parser/Python.asdl` as a table:** the 126 classes, their fields, and what each field is. The classes are made by going through it. Of `Python/Python-ast.c`, which is generated
  from the same, all that is written out here is what is written by hand there: the class `AST`.
- **What the parser makes is not quite the tree that a program sees.** It has one kind of node for `def` and `async def`, and so on. `PythonASTWalker.h` goes through the one as if it were the other, and
  `PythonASTBuilder.h` makes the one out of the other. Neither knows what it is going to or from: what is derived from the walker makes objects, or JSON for the tests, or text, and the builder is given
  something to ask, which asks objects or reads text.
- **What a program has made is looked over before anything is made of it** (`PythonASTValidator.cpp`, which is `Python/ast.c`). What generates code takes it that a tree is such as the parser makes. Where
  that is more than `ast.c` sees to, it says what CPython says on finding out, and where CPython does not find out and falls over (`None` among the handlers of a `try`, or the names of a `global`) it is a
  `ValueError`.
- **Code has to have source, and the source of what is compiled from a tree is the tree, written out** (`PythonSyntaxTreeSource.h`). Code is made from its source when it is wanted, can be thrown away and made
  again, and gives its source as `co_code`, which is what is kept when `pytest` writes what it has rewritten to a `.pyc`. So the tree is written as text, that is the text of a `SourceProvider` of a kind of its
  own (`SourceProviderSourceType::PythonSyntaxTree`), it is read back where the text of a program would be parsed, and a function in it is a part of that text as a function in a program is. All else is as it
  is for a program. What reads it takes nothing on trust, since a program can say that anything is `co_code`.
- **Where an instruction is from is a part of the source, and here that is where the node is written.** Each node is written with where it says that it is first, so that it can be got from where the node
  begins. The provider answers the engine's own question, what line and column an offset is at, with that (`lineColumnInTextForOffset()`, as the provider of the builtins does), so a stack trace in JavaScript
  is right too.
- **A traceback quotes the file that the code says it is from, in the place that the tree says,** whether or not that is what the tree was made from. There is nothing else to quote.

`JSTests/python/audits/syntax-trees.py` takes the trees of some pieces of source, gives each field of each node each of a hundred values that mostly do not belong there, compiles what comes of it, and runs
that: 1,200,000 in all. It is the same as in CPython for every one, but for what is left out because CPython falls over. It found more wrong with what has nothing to do with trees than with them, which is in
`programs/what-auditing-syntax-trees-found.py`. And every one of `programs/` is run by way of its tree as well as from its source.

With `PyCF_TYPE_COMMENTS`, `# type: int` is a token where the grammar has a place for it and a mistake anywhere else, and `# type: ignore` is kept for the module to list. The mode `'func_type'` is for what such
a comment says of a function, `(int, str) -> bool`. `PyCF_OPTIMIZED_AST` shows the little that CPython does to a tree before it generates code (`PythonASTOptimizer.cpp`, which is `Python/ast_preprocess.c`). What
generates code here does not go by that: it works the same things out for itself.

### Warnings

`_warnings` (`PythonWarnings.cpp`) is `Python/_warnings.c`, function for function: it decides whether a warning is shown, raised or passed over. If `warnings.py` has been imported it has the filters, and shows what is
to be shown. What is written in C++ warns with `warn()` and `warnExplicit()`, which are `PyErr_WarnEx()` and `PyErr_WarnExplicitObject()`, and give false if the program has asked for such warnings to be errors.

**What is warned of when source is compiled is looked for by itself** (`PythonSyntaxWarnings.cpp`). CPython comes on most of it as it generates code. Here the code of a function is not generated until it is called, which
would be the wrong time, and could be more than once. So the whole of what was parsed is gone through when it is compiled, in the order that `Python/codegen.c` comes to things in, and in CPython's three stages: what the
lexer finds, then `return`, `break` and `continue` in a `finally`, then the rest.

- CPython generates the code of a `finally` again for each `return`, `break` and `continue` that goes by way of it, and warns of what is in it each time. Here it is warned of once.
- Where CPython says that a bad escape is goes by where the string *ends*, if it is an f-string, and leaves out what comes before the quotes. Here it is where it is.

### What a class has by a special name need not be a function

`__hash__ = classmethod(...)` is allowed, and so is anything that has a `__get__()`. So what is found in a class by a special name is never simply called with the instance to begin with. It goes through
`callSpecial()`, or `lookupSpecial()` and `callMethod()`, which are `lookup_maybe_method()` of CPython's `Objects/typeobject.c`: a function is given the instance, without a bound method being made, and anything
else is asked by `__get__()` what it is for the instance. `__new__` is found as `type.__new__` would find it, with no instance.

What such a method returns is not to be taken for what it should be either. `stringIn()` says whether something is a `str` or an instance of a class derived from `str`, and gives the string that is in it, and
`asString()` is only for what `isString()` has been asked of: an instance of a class derived from `str` is another kind of cell. And where `str()`, `repr()`, `format()` and `bytes()` give what the method
returned, they give that very object, of whatever class it is: `strObject()` and `reprObject()`.

### Characters and code units

A `str` is a JavaScript string, and stays one whichever language has it. That is made of code units of 16 bits, and Python counts in characters, of which one past U+FFFF is two code units, a surrogate pair. None
of that is to show in Python, and `len(s)` and `s[i]` are to take no longer for a long string than for a short one. `PythonCharacters.h` sees to both.

- **Most strings have no pairs**, and then a character is a code unit. That is so of any string of 8 bit characters. Of the rest it is found by going through the string once, and since a string does not change it
  is kept from then on, in a bit of `StringImpl`'s flags, beside the one that says how wide the characters are. A part of such a string is another, so `createSubstringSharingImpl()` hands it on. JavaScript pays
  nothing for it: the bit was spare.
- **For a string that has pairs**, which character each of them is is written down, in order: `SurrogatePairs`. The code unit that a character begins at is its index and one more for each pair before it, and how
  many those are is found by halving. There is as much of it as there are pairs, so a page of text with one emoji in it costs four bytes, and there is nothing to choose in how it is laid out. A string has
  nowhere to keep it, so it is kept for the strings lately asked about, in a cache of the VM's that is emptied when the collector has run, as what `String.prototype.split()` came to is. Whoever is using one holds
  on to it, since the collector can run in the middle.
- **`Characters`** is what the rest of the code uses: `count()`, `codeUnitOf(index)` and `characterAt(offset)`, of one string.
- **Order is by character.** The halves of a pair are numbered below U+E000 and a pair stands for what is above U+FFFF, so `compareStrings()` finds the code unit that two strings first differ in, goes back one if
  that is in the middle of a pair, and compares the characters there.
- **Half a pair by itself is a character**, as it is in Python, where `surrogateescape` makes them. It is not part of a pair that has the same for one of its halves: `'\ud83d' in '\U0001f600'` is false. So what is
  looked for with `findCharacters()` and the rest is not found in the middle of a pair. That takes looking into only if it begins with a second half or ends with a first half, which is next to never.
  And `strip()` goes by a character at a time, or it would take half of one emoji for half of another.

### Adding to a string

`s += x` in a loop is how a great deal of Python puts a string together, and it looks at what it has so far as it goes: `s[-1]`, `s.endswith(...)`, `len(s)`. CPython adds to the string where it is, if nothing else
has hold of it, so that takes as long as there is to add. In JavaScriptCore `s + x` is a rope, which is made in no time, and looking at it makes one string of it, which copies all of it. So doing both in turn took
the square of the length, in JavaScript as well.

Now a rope that is mostly its first string, if that is a long one, is made into a string in a buffer that it shares with that first string: `ExtensibleStringImpl`, in WTF, and `JSRopeString::tryResolveRopeIntoExtensibleBuffer()`. The
buffer has room to spare. Each string is an ordinary `StringImpl` that is the first so many characters of it, and does not change. If the first string ends where what has been used of the buffer ends, then what is
added is written after it, where no string that there is can see it, and the result is a longer piece of the same buffer. If something has been written there already, because the same string was added to twice, it
is copied as it always was.

- It is for a rope whose first string is at least half of it. If more is added than was there, copying what was there does not change how the time goes up.
- It is for a first string that is longer than a page. Sharing a buffer takes one more allocation than not sharing one, which is more than it costs to copy a short string, and adding to a short string once is among
  the commonest things that JavaScript does. Done for every rope, that took 23ns in place of 14ns with 10 characters, and made no difference that could be measured with 10,000. What is left by leaving short strings
  out is little: putting together a string of that length by copying it each time comes to a millisecond or so, once. As it is, a rope with a short first string is put to one comparison, which cannot be measured.
- The first time, the buffer has no room to spare, so adding to a string once costs no memory. It is when what comes of that is added to in its turn that the buffer is made twice as large.
- A string says that it is such a piece in the low bit of the pointer that it has to the buffer, so it takes none of `StringImpl`'s flags.

What Python has found out about the surrogate pairs of the first string is a start on the whole, whether or not they share a buffer, so it is handed on whenever a rope of 16 bit characters is made into a string:
`handOnWhatIsKnownOfSurrogatePairs()`. Otherwise each string on the way would be gone through from the beginning, which is the square of the length again. There is something to hand on only if Python has asked
about the first string, so JavaScript by itself does no more than find what the first string is.

- That there are no pairs is handed on by looking through only what was added.
- Where the pairs are is one `SurrogatePairs` between all the strings that begin alike, which is only ever added to. A string may end between the halves of what is a pair to a longer one, so each counts those that
  are wholly in it. And two strings that begin alike may go on differently, so the cache keeps, for each, how far it is known to be the same as what has been written down. The second of the two to be looked through
  finds that it has been added to from the other, and takes a copy of what they have in common.

### What there is no room for

Nearly everything that is put together here has in it something that is as long as a program makes it, and a program of one line can make that as long as a string can be: `getattr(1, 'a' * (2 ** 31 - 5))` has
only to say what it did not find. WTF's `makeString()`, `StringBuilder` and `Vector` bring the process down when it comes to too much, which is right for what the engine decides the size of, and not for this.
JavaScriptCore has `tryMakeString()`, and builders that remember that they overflowed, for where a program decides, and throws an out of memory error. So it is here, and it is a `MemoryError`:

- **Text** is put together with `concatenate()` and `TextBuilder`, of `PythonText.h`, and never with `makeString()` or `StringBuilder`. They give a null string if there was no room, and a null string that goes into
  either makes what comes out null, so it does not matter how far down it went wrong. So a string that is left out is `emptyString()`, and not `String()`.
- **What is done with a null string** is one of three things. As what an exception was to say, it makes the exception a `MemoryError`, which `createException()` and `raise()` see to, so nothing need be done
  where it is raised. As what was to be a `str`, it goes through `strOrMemoryError()`. And returned as text, from what says that it raised by returning null, it goes through `textOrMemoryError()`.
- **Bytes** are put together in a `ByteVector`, which holds as many as a typed array does and remembers likewise. `newBytes()` and `newByteArray()` ask it. It will not give up what is in it once it has overflowed.
- **Values** are gathered in a `MarkedArgumentBuffer`, which takes no more when there is no room and says nothing unless it is asked. `newList()` and `PyTuple::createFromArguments()` ask it.
- **How long something will be** is worked out by dividing what there is room for, and not by multiplying, which comes round to a small number: `[0, 1, 2, 3] * 2 ** 62` was once an empty list.
- **What may have no end**, as an iterator may not, is not gone on with once there is no room for what it gives.

### `+` and `*`, of numbers and of sequences

`binaryOperation()` is in two parts, as `PyNumber_Add()` and `PyNumber_Multiply()` are. **First as numbers**: `__add__` and `__radd__`, and so on. **Then as sequences.** What `str`, `list`, `tuple`, `bytes` and `bytearray` have
for `+` and `*` is not for the first part: in CPython it is `sq_concat` and `sq_repeat`, and `__add__` and `__mul__` are only how a program comes by those. So what a sequence is added to or multiplied by is
asked first, `"ab" * x` takes whatever has `__index__`, and `[1].__add__(2)` raises where `(1).__add__([])` gives `NotImplemented`.

- A class that a program derives from `list` or `bytearray` has their `+=` in the first part too. That is what comes of how CPython fills in what such a class can do, and it decides what is run.
- With `*=`, what is on the right is not come to if what is on the left can do anything that a sequence can, though it cannot do this.

### `__dict__` is the object

The attributes of an instance and the globals of a module are properties, which is what lets them be cached inline. `obj.__dict__`,
`vars()` and `globals()` give a real `dict` that is *backed by* the object: its items with string keys are those properties. Neither
can be out of date, since there is only the one copy. A plain dict that is given to `exec()` for its globals becomes backed by a
bare object, so compiled code always finds its globals the same way.

- **An attribute is a property that is enumerable.** What is not enumerable is JavaScript's business, and Python does not see it: the
  `name` and `length` of a function, the `stack` of an `Error`.
- What CPython keeps in a field of a C struct is a property under a private name, which neither language can name.
- A name that JavaScript would take for an index, as `"0"`, cannot be that of a property. It is kept where a key that is not a string is: in the
  dict's own table.
- Two objects can have one `__dict__`. The second finds its attributes in the first.

How many attributes there are is `len(o.__dict__)`, and it is asked at each step of going through the dict, to say so if it changes. What Python counts is what `Object.keys()` would list: what is enumerable and is
not keyed by a symbol, which leaves out what the engine keeps in the object for itself. An object with many properties has a table of its own that is changed where it is, so the count cannot be kept by
`Structure`. `PropertyTable` keeps it, `enumerableStringKeyCount()`, and it changes where a property is added, removed or has its attributes changed, none of which is where an inline cache goes. It fits in what
was padding.

### Modules, and what is up to the host

Importing is divided as CPython divides it. What the statement means, `__import__()`, `sys.modules` and the tables of what is built in and what is frozen are `Python/import.c`, which is `PythonImport.cpp`, and
`_imp`, which is `PythonImpModule.cpp`. Finding a module and loading it is `importlib`, which is written in Python, and is CPython's own: `lib/importlib/_bootstrap.py` and `_bootstrap_external.py` are its files as
they are, and are in the engine as they are in CPython's binary. So finders, loaders, specs, path hooks, namespace packages and the locks are not like CPython's. They are CPython's. Its frames are on the stack and are taken
out of tracebacks when CPython takes them out: `removeImportlibFrames()`.

The statement calls whatever `__import__` is in the builtins of the frame, with the globals and the locals of the frame, unless it is the one that there was to begin with, which there is no need to call.
`import a.b.c as d` imports `a` and gets `b` from it and `c` from that, as `from` would.

A frozen module is source here, and not code that has been compiled and marshalled, since there is nothing yet that compiles ahead of time. What `_imp.find_frozen()` gives for the data is made when it is asked for.

The name of the file that code is from goes with the source, and all the code that is compiled from one source has the one name. `_imp._fix_co_filename()` gives the source another name, which is what it does in CPython
to the code and all the code in it. It does it as well to code that `code.replace()` made from that, which in CPython has a name of its own.

An ES module can import a Python file: it is a synthetic module record, made when it is needed as for CommonJS, that exports each global by name and the module as the default. The file is imported by the name that
`import` would find it by, if it is somewhere on `sys.path`, and otherwise as `importlib.util.spec_from_file_location()` would have it.

What is up to the host is asked of it as JavaScript asks it, through `GlobalObjectMethodTable::configurePython`, which fills in a `Configuration`, as whoever embeds CPython fills in a `PyConfig`: `sys.argv`, `sys.path` and
the like, and what there is to import besides what the engine has, `builtinModules` and `frozenModules`, which are `PyImport_AppendInittab()` and `PyImport_FrozenModules`. `posix` is one: it is what a program reaches the
system with, so whether there is such a module is the host's to say. It is written here all the same (`createPosixModule()`, which the host lists or does not), since otherwise each host would write it again.

What is done to an open file is asked of the host too, if it wants to be asked: `Configuration::files`, a `FileOperations`, which is `open`, `close`, `read`, `write` and the few others that `io.FileIO` is made of.
`FileIO` and the functions of `posix` that do the same go through it. That is so that what Python writes to the standard output can go the way that what JavaScript writes there goes, and the two come out in the
order in which they were written. Left alone, it is the system calls.

### Where the library is written

What CPython writes in C is written here in C++, and what CPython writes in Python is that Python, as it is. A program cannot see into what is written in C: all that there is to match is what it does. What is
written in Python it can see all the way into. It can replace a function in it, derive from a class in it and override any method, read what begins with an underscore, find its frames on the stack and its
lines in a traceback, and ask for its source. Programs do. So a module is written in the language that CPython wrote it in, function for function, and each file here that is a port says of what.

A class that is built in and belongs to a module is made when the module is first imported: `createBuiltinType()`. What the module has to keep for the realm, those classes among it, is a struct in `PyRealm`,
as CPython keeps a module's state: `ThreadModuleState`.

An instance of such a class has what in CPython is a C struct. Here it is a struct as well, derived from `NativeState`, and one kind of cell holds any of them: `PyStateObject`. So a class that is ported does not
take a cell type, a subspace and a destructor of its own, only what it has and what of that the collector is to be told of. Where CPython leaves making the instance to `object.__new__()` and its `tp_alloc`, so that
one can be had that `__init__()` has never been called on, the class says how one is made: `PyType::setAllocator()`.

Besides that there is what the front end itself is easier said in Python for: `lib/_framelocals.py`, which is frozen, is imported when a realm is made, and is in no traceback. It is a class that CPython writes in C, and
is to be written in C++.

### What is put off

Some things come up when nothing can be run, and are to be run as soon as something can: the callback of a weak reference, which comes up in the middle of a collection, and `KeyboardInterrupt`. CPython has its
"eval breaker" for that, a word that running code looks at now and then. Here the word is the one that it looks at anyway, `VM::m_pythonLimitUnlessWatched`, which is 0 while there is something to see to, so
that being able to be asked costs code that is running nothing. `op_py_enter` and `op_py_line` then go the slow way, which is `doPendingWork()`.

### Weak references

`weakref.ref` is a cell that has what it refers to and does not tell the collector, as `JSWeakObjectRef` is, and it lets go of it by the same means: when the collector has found what there is to keep, each
reference that it is keeping looks whether what it refers to is among that (`reconcileWeakReferencesAtGCEnd()`). If it is not and there is a callback, the reference goes on a list that begins in the `VM` and goes
from one to the next, which does keep them, and there is something to see to.

An object knows the references to it, so that `ref(x) is ref(x)`, and does not keep them. It has a `PyWeakReferenceList`, in a property that no program can name, in either language. That has the first of
them and each has the next, as in CPython, and the collector is told of none of it. What the collector is not keeping is dropped from the list at the same time as above, before anything is swept. So there is
no table of what refers to what, and nothing that a reference costs an object that has none.

There can be one to whatever CPython allows one to, and to an object of JavaScript's. An array is a `list` and an `Error` is an exception, and there cannot be one to those.

### Locks

`_thread.lock` is a cell with a word in it, and to wait for it is to wait for the word as `Atomics.wait()` does, by the same means (`WaiterListManager`). So the engine knows of a thread that is waiting, as it
does of one that JavaScript has made wait, and can wake it to be terminated. To take one that is free, or let go of one that nothing is waiting for, is to write the word.

There is one thread, and `_thread` is as it is in CPython for a program that has not started another. Starting one fails as it does there when the system will not have it.

### `marshal`

What is written is what CPython writes, byte for byte, and each can read what the other has written, but for two things. What may be come upon again is marked, from version 3 on, and to CPython that is what has
more than one reference to it. There is no telling that here, so everything is marked that could be, which is read the same. And code is written in the same form, with this engine's `co_code` in it: see
*Code objects*.

### Codecs

`PythonCodecsUTF.cpp` and `PythonCodecsBytewise.cpp` are the codecs of `Objects/unicodeobject.c`, `PythonCodecRegistry.cpp` is `Python/codecs.c`, and `PythonCodecsModule.cpp` is `_codecs`. The rest of what CPython has, `codecs.py` and
`encodings/`, is written in Python over those.

What a codec does about what it cannot encode or decode is to call a function, which is looked up by name and can be one that a program has registered. It is told by way of an exception, and there is one for
the whole of a call to the codec, which is told each time where the trouble is now: `DecodeErrors` and `EncodeErrors`. A decoder's exception has a copy of what is being decoded, and the handler can put something
else there, which is what is decoded from then on. So a decoder does not keep where the bytes are across a call to a handler, and asks the exception afterwards.

An encoder counts in characters, since that is what a handler is told and what it answers in: `CodePoints`, which makes nothing unless there are surrogate pairs in the string.

Until `encodings` is imported when a realm is made, the names that it knows and the codecs that it has in Python over `_codecs` are known to `PythonCodecs.cpp`, in a part of it that says so and is to go.

The names of characters, for `\N{...}` and `namereplace`, are ICU's, which has not the names of control characters nor abbreviations (`\N{LF}`), and is of whatever version of Unicode the system has. They are to be
`unicodedata`'s own.

### `io`

`_io` is `Modules/_io/` of CPython, file for file: `PythonIOBase.cpp`, `PythonFileIO.cpp`, `PythonBytesIO.cpp`, `PythonBufferedIO.cpp`, `PythonTextIO.cpp`, `PythonStringIO.cpp`. What matters in the port of the buffered
classes is not only what a program gets from them but what they ask of the raw stream underneath, since that can be a class of a program's: which method, with how many bytes, and in what order. So that is what
`programs/buffered-io.py` compares, over streams that take note of it.

A buffered stream reads into its own buffer and writes from it, and what the raw stream is given for that is a `memoryview`, made for the one call and released after it, so one that is kept sees nothing later.

`StringIO` counts in characters, so what is in it is a character to each 32 bits, as in CPython, but while it has only ever been added to at the end, when it is a string that is being built.

`TextIOWrapper` counts in characters as well: how many to read, how long a line may be, and how many of what has been decoded have been given out, which is part of what `tell()` returns. What it returns is the
same number as in CPython, since a program can keep one and give it to another. `tell()` guesses how far into the bytes it has got by how many bytes there were to a character in the last piece that was read,
which after `seek()` is a guess about something else and can come to more bytes than there are. CPython looks past the end of them then. Here it is as many as there are.

What `io.open_code()` does is the host's to say, if it wants to: `Configuration::openCode`.

### `posix`

`Modules/posixmodule.c`, in parts: `PythonPosixShared.cpp` has what turns arguments into names of files, descriptors and ids, and what raises; `Paths`, `Descriptors`, `Directories`, `Identity` and `Processes` have the
functions; `Constants` has the constants, and the names that `sysconf()` and the like go by. It has everything that CPython's has on macOS but `fork()`, `forkpty()` and `register_at_fork()`: see *Where it differs*.
What CPython has only on Linux is not written, and what is written for Linux has not been compiled.

### One rounding or two

`a * b + c` is one instruction on some processors, and it rounds once where a multiplication and then an addition round twice. JavaScriptCore is built with `-ffp-contract=off`, so that it is never used unasked, as
JavaScript's arithmetic requires. What CPython is built with does use it, for what is written in one expression, so `_Py_c_quot()` and `st_mtime` come out one way on ARM64 and another on x86-64, in the last digit.
A program can see that, and one that keeps a time to compare it with later does. So where CPython has such an expression it is written `multiplyAdd()` here, which is the one instruction where CPython's would be.

## The two languages

**A value that crosses from one language to the other stays what it is.** There are no wrappers, so there is nothing to keep track of and
nothing for the collector to be told. What has to be decided is only what each language *sees* of what is the other's.

### What JavaScript sees

**What is Python's is an exotic object, whose `[[Get]]`, `[[Set]]`, `[[Delete]]` and `[[HasProperty]]` are `getattr()`, `setattr()`, `delattr()` and
`hasattr()`.** In Python, what an attribute is is settled when it is got: a function of the class becomes a method bound to the instance, a
property is computed, `__getattr__` is asked. So `obj.method` is a bound method, `obj.prop = 1` calls the setter, and a frozen dataclass is
frozen. `[[GetOwnProperty]]` and `[[OwnPropertyKeys]]` are ordinary: `Object.keys(obj)` is what is in its `__dict__`.

- The class of the receiver answers, once, with what `getattr()` gives. The classes beyond it have nothing to add.
- An attribute cannot be made an accessor, read-only, hidden or permanent, and the object cannot be frozen, since there is nowhere for Python
  to keep that.
- `x instanceof C` is `isinstance(x, C)`. `C.prototype` is `C`, without being an attribute. `new C()` is `C()`.
- `typeof C` is `"function"`, and `C.call()` and `C.bind()` are there, but `C instanceof Function` is false. That goes by what a thing is derived from, and a class is what its instances are derived from. If
  `Function.prototype` were beyond it, every instance would be an `instanceof Function` too, which is what is asked to find out whether a thing can be called.
- **A loop is the language's that it is written in.** `for (x of generator)` closes a generator of Python's that it leaves early, as it does one of JavaScript's. `for x in generator:` closes neither, so that to leave a
  loop and go on with `next()` afterwards does the same with both.
- What a class defines is not enumerable, as in JavaScript.
- Names that JavaScript expects and Python has no use for are provided: `toString`, `Symbol.iterator`, `next`, `toJSON`, `length` or `size`, `constructor`, and those of the sections below.

**Reading an attribute that the class has nothing to say about is cached inline as for any object.** A class has a watchpoint set,
`instanceAccessIsAsFound()`, which is named in the `PropertySlot`. It stops holding when the class, or one that it is derived from, is given a data
descriptor or a `__getattribute__`, `__setattr__` or `__delattr__` after it was made.

Every kind of cell that can have attributes has these methods (`PYTHON_DECLARE_EXOTIC_METHODS`).

**A function is a `JSFunction` and has none of them, and needs none.** It inherits from the class `function`, and by way of that from `Function.prototype`
(`JSFunction::selectStructureForNewFuncExp()`). A class that is come to as a prototype answers for whatever came to it, for `[[Get]]` and for `[[Set]]` alike, so
`f.__doc__`, `f.__defaults__ = ...` and `f.call()` are all found. Only `[[Delete]]` looks no further than the object, and `JSFunction::deleteProperty()` sends on what the function has
no property for. What is written in C++, and a bound method, inherit from their classes in the same way. What a function is called is its `name`, which `__name__` gets and
sets. It costs JavaScript a test of a bit when a closure is made: nothing in the interpreter, the DFG or the FTL, and 0.2ns of 11 in Baseline.

### What Python sees

An object of JavaScript's is an instance of its class: `type(js.Map.new()) is js.Map`. All are derived from `js.Object`, and a function that is no class is
an instance of `js.Function`. Its attributes are its properties, as JavaScript finds them. `obj[key]`, `len()`, `in`, iteration and `isinstance()` do what they
would in JavaScript. `import js` is the global object.

- `obj.f(x)` passes `obj` as `this`.
- **A function that is got from what an object inherits from is bound to the object**, as one got from a class is in Python. One that the
  object has of its own is as it is, as one in an instance's `__dict__` is. So `js.Math.floor` and `js.Array` are themselves.
- **Calling a class makes an instance**, Python having no `new`. That is asked only where there was nothing left to do but throw
  (`callConstructorWithoutNew`), so no call that works pays for it. What can be both called and constructed with is called, and `.new()` constructs.
- JavaScript's methods are not attributes of a `list` or a `str`. `hasattr(x, "keys")` is how Python tells a mapping.
- **A `list` is any `Array`, and JavaScript can make some that Python cannot.** A hole is `None`. An element that is not simply there to be read or written
  is read and written as JavaScript would in strict code: a getter is run, and an array that is frozen raises `TypeError` and stays as it was
  (`listGet`, `listSet`). The strings that a tag function is given are such an array.
- What a `t"..."` has, `strings` and `values`, is what a tag function is given, so `tag(t.strings, ...t.values)` needs nothing from either side. And a
  function of Python's can be a tag.

### A class is a class, whichever language made it

**A class of either language can be derived from one of the other's, to any depth and in any order, and neither is made into anything else.**
`class C extends B` is what it always was, a constructor and a prototype, when `B` is Python's. `class B(A)` is a class of Python's when `A` is JavaScript's.

To JavaScript a class of Python's is a class: it can be called and constructed with, `C.prototype` is `C`, and it has a `name` and what functions have.
Its `[[Construct]]` goes by `new.target`.

To Python a constructor of JavaScript's is a class as it is. `type(new C) is C`, `C.__mro__` is `(C, B, A, Object, object)`, and `isinstance()`, `issubclass()`,
`super()`, `except` and `match` take it. What has `[[Construct]]` is a class, which is JavaScript's own test. So an arrow function is not.

There is as much to know about such a class as about any: its order of resolution, how its instances are laid out, what is derived from it. That is
kept in a `PyType` that the constructor has under a private name, as a function has its `FunctionRareData`. **No program is given it.** `asType()` finds it from
the constructor, `PyType::object()` is the way back, `isClass()` asks whether a value is a class of either kind, and `isType()` whether a cell is a `PyType`.
`JSTests/python/interop/class-identity.py` goes through every way of coming by a class.

| a class of JavaScript's | is to Python |
|---|---|
| a property of the prototype | an attribute of the class: both are there for the instances |
| what is `static` | the class's and not its instances', as what a metaclass defines is |
| `get x() {}`, `set x(v) {}` | a descriptor with `__set__` |
| `[[Construct]]` | `__new__`: `C.__new__(cls, ...)` is `Reflect.construct(C, [...], cls)`. `__init__` does nothing |

- **Where Python would pass `self` to a function found in a class, one of JavaScript's gets `this`.** So a method of a class of Python's that calls
  `self.sound()` gets what a class of JavaScript's overrides it with.
- **What comes to a class of Python's looking for a property looks from that class on in the order of resolution.** That is right for an ordinary lookup,
  which has been through what comes before, and for `super.name`, which does not want it.
- **Making an instance.** For js `A`, py `B(A)`, js `C extends B`: `new C(x)` comes to `super(x)`, which is `[[Construct]]` of `B` with `C` for `new.target`. `B` looks
  for `__new__` from its own place in the order and finds `A`'s, which constructs with `A`, still for `C`. Then `B.__init__`, then the rest of `C`'s constructor.
  Each is run once, whichever language asked for the instance, and whatever is derived from `C`.
- **An instance is the kind of cell that the class at the root makes.** `class Counter(js.Map)` makes a `Map`, and `class D extends dict` a `PyDict`. A constructor
  that is not written in JavaScript makes a kind of its own, so a class cannot be derived from `Map` and `Set`, or from `Map` and `dict`.
- **A class of Python's is told when one of JavaScript's is derived from it**: `__set_name__` and `__init_subclass__` are called when the class has been
  defined. That takes two checks at the end of a class definition that has `extends` (`emitTellPythonOfDerivedClass`), which cost less than could be
  measured.
- The metaclass's `__new__` is not run for a class of JavaScript's, there being a class already.

### Operators

**What an operator of JavaScript's does with something of Python's is what the same operator does in Python.** `a + b` is `a.__add__(b)` or `b.__radd__(a)`, and so
for `- * / % ** << >> & | ^`, unary `-` and `~`, and `== != < <= > >=`. `a += b` tries `a.__iadd__(b)` first. What has no meaning raises `TypeError`.

`===` is identity, `== null` asks whether there is anything there, and `x++` is `x = x + 1`. A list is an `Array` and a `bytes` a
`Uint8Array`, so for those the operators are JavaScript's, unless they are of a class derived from those that has its own.

**With a string of JavaScript's it is the object that is asked, and the string is not**: `x + "s"` is `x.__add__("s")` and `"s" + x` is `x.__radd__("s")`. If it has nothing to say
they are concatenated, as any object and a string are, so that `"value: " + x` says what `x` is. In Python that would be a `TypeError`, a `str` being added to nothing but a `str`.
What the object raises is thrown.

Every cell of Python's has `OverloadsOperators` in its `TypeInfo`, and `MethodTable::operate`. **There is no switch.** What the compilers may assume of an operator
depends on the types of its operands and on nothing else. Two objects are compared by their addresses, having been checked for the flag, unless that
check has failed there before: 0.03 to 0.06ns for each `==` of two objects, and nothing that could be measured for anything else. That an operator is
that of a compound assignment is said in the instruction, and read only when an object that overloads it has turned up.

JavaScript compiles few additions of strings to additions. A string plus anything being a string, `"a" + x + "b"` is a conversion of `x` and one concatenation, and `x + ""` is two
conversions. That still holds, and what an object says is still heeded, when it would have been and in the same order: "String concatenation and overloaded operators" in
`runtime/Operations.h` says how. The conversion already left the interpreter for any object, and that is where it is found out. It costs a load and a compare in the interpreter
and in Baseline for each operand that is converted, which could not be measured (52.6ns and 30.3ns for a concatenation, with and without), and nothing in the DFG and FTL, which
exit the first time and are compiled again to do what the interpreter does. `JSTests/stress/overloaded-operators-strings.js` writes each expression twice, as one would and as
additions of two operands, and has both do the same things in the same order.

### Waiting

**There is one event loop, and each language waits for what is the other's.** A coroutine stays a coroutine and a promise a promise.

- **To JavaScript, what can be awaited is a thenable.** It has `then`, which is what `await`, `Promise.all()` and the rest go by, and `catch` and `finally`. `then` gives it a
  promise and begins on it at once (`toPromise`). A coroutine can be awaited once, so it has the one promise however often it is asked for.
- **To Python, a promise can be awaited, and so can anything that JavaScript would wait for.** `await promise` yields the promise, once, to whatever is running the
  coroutine, as a `Future` of asyncio's yields itself. What is sent back is what it came to. A rejection is thrown in.
- **What runs a coroutine for JavaScript is what runs an async function of JavaScript's**: it is resumed until it yields, what it yielded is awaited by
  `JSPromise::resolveWithInternalMicrotaskForAsyncAwait`, and a microtask of a kind of its own, `InternalMicrotask::PythonAwaitResume`, goes on with it. There are no closures, and
  nothing besides the coroutine: it has its promise under a private name. So a coroutine takes as many turns of the loop as an async function that does
  the same. One coroutine awaiting another takes none, as in CPython.
- A bare `yield` in an `__await__` gives everything else a turn. Anything else that is yielded and cannot be waited for is `RuntimeError: Task got bad yield`.
- An asynchronous generator, or anything with `__aiter__`, has `[Symbol.asyncIterator]`, and what has `__anext__` has `next()`, `return()` and `throw()` that give promises. So `for await`
  goes through it, and closes it on the way out. `async for` goes through what `for await` would: what has `[Symbol.asyncIterator]`, or failing that `[Symbol.iterator]`.
- A program in Python sets a coroutine going with `js.Promise.resolve(main())`. `asyncio` is the library's business.

### Context variables

**What a `contextvars.ContextVar` has is kept where JavaScript keeps the like**: `JSGlobalObject::m_asyncContextData`, which whatever puts something off takes note of, and puts back for as long as that
runs (`AsyncContextSwapScope`). It is what an embedder's `AsyncLocalStorage` is made out of. So what a variable has follows what is being done from one language to the other and across `await`, by means
that were there already, and it costs JavaScript nothing that it was not paying. `PythonContextVars.cpp`.

- What is kept there is a chain of frames, `{ storage, value, prev, masked }`, each of which binds something to a value. `AsyncLocalStorage` makes them, and so does whatever else has something to keep there,
  with no prototype. What goes through them looks for its own `storage`, passes over the rest, and copies what it has to as it is.
- **Python has one frame, whose value is a map from variables to what they have.** The map is never changed, and setting a variable makes another: a hash array mapped trie, as in CPython. So to take note
  of how things are is to take note of one pointer, and JavaScript has one frame more to pass over however many variables there are.
- Not a frame for each variable. `AsyncLocalStorage.getStore()` would have them all to pass over, and `Context()`, which has nothing in it, would have nothing of the embedder's in it either.
- **Entering a `Context` and leaving it changes Python's frame and no other**, as `AsyncLocalStorage.run()` changes only its own. `AsyncLocalStorage.snapshot()` has Python's variables in it, being a note of the
  whole chain. `copy_context()` has Python's alone.
- A coroutine that JavaScript waits for runs in a context of its own, which began as a copy of that of what first waited for it, as a `Task` of asyncio's does.
- A generator runs in the context of what resumes it, in either language, and so does an asynchronous one. One of Python's that JavaScript goes through with `for await` does with a variable just what one
  of JavaScript's does with `enterWith()`.
- What is set where no `Context` has been entered is set for the rest of what is being done, and for what that puts off: `enterWith()`. What was put off before does not see it, as what
  asyncio's `call_soon()` was given does not.

### `using` and `with`

**`using x = manager` is `with manager as x`.** For a context manager of Python's, `using` calls `__enter__`, `x` is what that gives, and `__exit__` is told what the block threw, if it
threw, and may deal with it. All three matter. What `contextlib.contextmanager` makes does nothing until it is entered. What it is entered for is often not the
manager. And a transaction that is not told that something was thrown commits.

JavaScript has no step for entering, so this is in the engine: `emitPrepareDisposable` and `emitUsingBodyScope` know the function that is the `[Symbol.dispose]` of every context
manager, by comparing with it. It costs `using` with something of JavaScript's nothing that could be measured. `await using` is `async with`. `DisposableStack.use()` enters too, and
gives what entering gave. A stack that `using` disposes of is told what was thrown, in two fields of its own, so that what is in it can be.

**`with obj` is `using`**, for what has `[Symbol.dispose]`: there is nothing to entering it, and leaving it disposes of it. `async with` goes by `[Symbol.asyncDispose]`, or failing that
`[Symbol.dispose]`, as `await using` does. Such an object does not pretend to have `__enter__` and `__exit__`.

### Where JavaScript wants a number or a string

`obj[Symbol.toPrimitive]` is there for anything of Python's but an exception. `Number(obj)`, `+obj` and `Math.floor(obj)` are `float(obj)`, or failing that what it is as an index. `String(obj)` and
`` `${obj}` `` are `str(obj)`. Whether something is true is not asked of it: to JavaScript every object is.

### Errors

An exception is an `ErrorInstance`, in the way that is provided for errors that have their message by other means. So `Error.isError()` is true
of it, and it has a `stack` with the frames of both languages in it in order.

| Python | JavaScript |
|---|---|
| `BaseException` | `Error` |
| `TypeError` | `TypeError` |
| `SyntaxError` | `SyntaxError` |
| `NameError` | `ReferenceError` |
| `ValueError`; `RecursionError` and `MemoryError` for those two | `RangeError` |

Each is an instance of the other's class, whichever language made it. `name`, `message`, `cause` and `stack` are to JavaScript what they are for any
`Error`, being not enumerable, and `AttributeError.name` is Python's. What JavaScript throws that is no `Error` passes through `except`, though not
through a bare one or `finally`.

### JavaScript cannot make one kind of cell pass for another

What is written in C++ goes by the class of what it is given for what kind of cell that is. But JavaScript can give anything any prototype.

- What has a class for its prototype, or the prototype of a class of JavaScript's that is derived from one, is an instance of it only if it is the kind
  of cell that its instances are (`typeOf`). Otherwise it is an object of JavaScript's that inherits from a class, and what it inherits is what the class
  itself has.
- Every `Structure` for a cell of Python's has `IsImmutablePrototypeExoticObject`. Assigning to `__class__` sets the prototype, having checked.
- One class of Python's, one class of cell.

## Where it follows the engine

Before something is invented, how the engine already models the nearest thing is looked for, and that is extended, in whatever directory it
is. So far: another language is a `SourceProviderSourceType`, as WebAssembly is. What the host provides is in `GlobalObjectMethodTable`. What is not to
be in a stack trace has `ImplementationVisibility::Private`. What a cached property depends on is a watchpoint set in the `PropertySlot`. An error
with its own idea of a message uses `finishCreationForEmbedderError`. A class of cell that is all of a size has an `IsoSubspace`. An object that is
some values and nothing else is a `JSInternalFieldObjectImpl`. What is known about a constructor hangs off the constructor. What speculation turned out
wrong somewhere is an exit site. What goes on after an `await` is an `InternalMicrotask`. A cell has no vtable, so an `Array` that behaves a little differently is an `Array`
whose `Structure` has another `ClassInfo`.

There is nothing that is per process, nothing that is set after something is made, and nothing that only the shell can do.

## Where it differs from CPython on purpose

- **A `bytearray` can be resized while there is a `memoryview` of it.** CPython raises `BufferError`, and can because the view is released the
  moment the last reference to it goes. Here it would stay locked until the next collection, and programs that are right would fail. A view
  holds no pointer, only where it is looking, and checks each time. For the same reason the `__release_buffer__()` of a class that has one is called when
  the last `memoryview` of it is released, by `release()` or by `with`, and not when it is merely let go of.
- **What line a frame that is over says that it is on**, if it was left by way of what nobody wrote: the end of a `finally`, or what is done at the end of `except E as e`. In CPython it is the last line that was
  written by somebody, which it has because it writes such things out again wherever they are come to. Here there is one of each, and it is said to be where the `try` is. Knowing better would take writing down
  each line as it is come to. `tb_lineno` is right, which is what a traceback is printed from.
- **Where `frame.f_lineno = n` will not go** (`interop/jumping-where-cpython-does-otherwise.py`):
  - It is only for the frame that is being told of, and only when it is a line that is told. CPython takes it for a frame further out, which is in the middle of a call, and does not survive. It also takes it as a
    generator yields or is resumed.
  - What is of one kind is not taken for another. CPython tells apart iterators, what was being handled, and everything else, so it will go from inside two loops to inside a `with`, and call an iterator for `__exit__`.
  - It does not go into the middle of something that is written over several lines, but from that very place. CPython does if there is as much on the stack, whatever that is: `x = [len(o), 7]` can come to
    `[<method 'append' of 'list' objects>, 7]`.
  - Which of its copies of a `finally` CPython goes to depends on what is next to what in its code. A place counts as the beginning of a line if what is before it, as the code is laid out, is on some other line
    (`marklines()`), so the copy for an exception is left out if the `finally` is one line long and the copy before it happens to be the other one. Here it is as if they all counted.
- **What is said of the wrong arguments** by `super.__init__()`, which in CPython is worded one way if it is called and another if `super` is, and by a method written in C++ that is called by way of a `super` that was
  kept first, which here goes by the class that the method is in.
- **A set is in the order in which it was added to**, and not in the order of a hash table's slots. What sets do with one another is CPython's `Objects/setobject.c`, function for function: how long each takes, which of two keys that are equal
  is kept, and what is asked of the keys and how often (`programs/sets-with-one-another.py`). The one thing that is otherwise is for the sake of the order: `a ^ b` has what is only in `a` first.
- **One NaN is another.** A float is a value and not an object, so `x is y` is true of two NaNs, and a set has room for one.
- **In a `__dict__`, keys that are strings come before those that are not**, and before those that JavaScript would take for an index.
- **There is less room.** An int can be no larger than a `BigInt`, which is 2\*\*30 bits, a `str` no longer than a JavaScript string, which is 2\*\*31 - 1 code units, a `bytes` no longer than a typed array,
  which is 2\*\*32, and a list no longer than what a JavaScript array keeps side by side, which is 2\*\*28 elements. Past that it is a `MemoryError`, where CPython makes it if there is room. And what says that it
  will come to more, as `range(2 ** 40)` does when `list()` asks it, is taken at its word, where CPython goes by whether it can have that much memory set aside, which depends on the machine. What has to be gone
  back and forth in on the way to being made, as a `str` that is not ASCII is when it is encoded, can be refused sooner than that.
- **The first half of a surrogate pair followed by the second half is the pair.** `'\ud83d' + '\ude00'` is one character here and two in CPython. In code units of 16 bits there is no telling them apart, and a `str`
  is to be the same string to both languages. Nothing but a program that puts halves together by hand can tell.
- **In a syntax tree, what an `Interpolation` says its source is has to be what a constant can be.** CPython takes anything, and finds out when it comes to keep it, or never. And a node that says it is on a
  line before the first, or at a column before the first, is on line 0 or at column 0, where to CPython it is nowhere.
- **A `BytesIO` can be written to while there is a view of it** from `getbuffer()`, where CPython raises `BufferError`. It is as it is for a `bytearray`, and for the same reason.
- **There is no `os.fork()`.** What it would leave in the new process is the one thread, and the collector and the compilers have threads of their own, which would be waited for and never answer. `os` has none on the
  systems where it cannot be had, so a program that can do without looks first, as the library does. `posix_spawn()`, `exec*()` and `system()` are there.
- **`os.closerange()` closes what is open**, which it finds out, where CPython on macOS tries every number in the range.

## Tests

| | |
|---|---|
| `JSTests/python/run-programs.sh <jsc>` | programs whose output is CPython's, byte for byte, each in five configurations of the engine, and once by way of its syntax tree. What is expected was taken on macOS on ARM64, and a few of them show it |
| `JSTests/python/run-interop.sh <jsc>` | the two languages together. There is nothing to compare these with: what is expected was read and found right |
| `JSTests/python/audits/run-audits.sh <jsc> [n]` | not tests but measures of how far there is to go, over everything that is built in |
| `JSTests/python/parser.js`, `symbol-table.js` | the first stages by themselves |

Two of the audits are of what the built-in classes do, and not of what they have. `operations.py` tries every operator, in place and not, between every two of some hundred values, and the built-in functions
of one and of two of them. `methods.py` calls every method of several instances of each class with no argument, with each of some ninety, with each two of some forty, with each three of a few, and by keyword, and the methods that the operators are made of as well.
That is ten million things tried. What is compared with CPython is what comes back and its class, or the exception and what it says, and what has become of what it was done to. Among the values are instances
of classes derived from `int`, `str`, `float`, `list` and `tuple`, and it is those that found the most. Each prints a line for each operator or method of each value, with a number that stands for all that came
of it, so that what is kept to compare with is small, and given `everything` prints it all, which is how to find what is behind a line that differs.

`special-methods.py` is the other way about: it is of what the language does with what a program's classes do. Each special method is given each of some eighty things to do, which are to return something, mostly
not what is wanted of it, to raise something, to take the wrong number of arguments, and not to be a function at all. Then everything that would call it is tried: the operators, the statements, the built-in
functions, and the methods of the built-in classes that take such a thing. That is ninety thousand things tried.

`other-objects.py` is `methods.py` for everything else that is built in: what goes through things, views, generators and coroutines and what is made to wait for them, functions and methods, descriptors, code, frames,
tracebacks, cells, modules, classes, `super`, exceptions. Most of these have a state, so each is tried as it is when it is new, part of the way through, at its end, and with what it goes through changed under it, and
some as they are only while something is going on: a generator to itself while it runs, a frame that is running, a traceback that is being handled. So what is tried is not an object but a function that makes one and
does something with it there. Every attribute is got, is set to each of some twenty things, and is deleted, and what can be called is called. Nothing is used twice, neither what is tried nor what it is given, since
what one engine gets wrong would otherwise show in what comes after. A call is made both by way of `getattr()` and as it would be written, `x.method(a, k=v)`, which CPython does not go about in the same way, so that
what it says of the wrong arguments can be one thing or the other. That is 1,800,000 things tried, of which four kinds are more than CPython 3.14.7 survives, and are left out of what is compared.

`complexity.py` is of how long things take as what they are done to gets longer. Each of four hundred things is done about n times to something of about n elements, and timed at n and at four times n, so what
it prints is the power of n that the time goes up by: 1 if each takes as long however many there are, and 2 if each takes as long as there are elements. `compare-complexity.py` puts what CPython printed beside
what this engine printed, and picks out what goes up faster here and what takes many times as long. It goes by the clock, so it is something to run and read, and not a test. It was written after `len(s)` was
found, by accident, to take as long as the string is, and the first time that it was run it found a dozen more of the kind.

`run-large.sh` is apart from the rest. It makes things that there is no room for, or barely room for, each in a process of its own, and what is looked for is that the engine is still there afterwards. It takes
gigabytes of memory and a quarter of an hour, so it is run when what it is about has been changed. `programs/no-room.py` and `interop/how-much-room-there-is.py` are of what is refused before anything is made,
which takes no time.

## What is not decided

Threads, when objects are finalized (`__del__`, and `with`-less file handles), and extension modules written for CPython's C API.

What follows from the second:

- **A file that is let go of without being closed is not closed, and what has been written to it and not yet sent on is lost.** `open(p, "w").write(s)` is written a great deal, and in CPython it works, because the
  file is finalized as the statement ends. `__del__()` is there to be called, and does what it does in CPython. Nothing calls it.
- What a weak reference refers to is gone when the collector finds that it is, and not when the last reference to it goes, and the callback is called some time after that.
- What is warned of when something is let go of while it is open (`ResourceWarning`) is not.
