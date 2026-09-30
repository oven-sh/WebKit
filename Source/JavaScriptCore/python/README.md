# Python in JavaScriptCore

A second front end. Python source is scanned, parsed and compiled to JavaScriptCore bytecode, and runs in the same interpreter and
compilers, on the same heap, with the same values as JavaScript. There is no CPython here, and no boundary: a Python list *is* a
JavaScript array, a Python function *is* a JavaScript function, and a Python exception *is* an `Error`.

The language is Python 3.14. CPython is the specification: `Grammar/python.gram`, `Parser/Python.asdl`, `Python/symtable.c`,
`Python/codegen.c`, `Objects/*.c`, and its test suite. Where a function here follows one of CPython's, the comment above it names it.

## The pipeline

| | | checked against CPython by |
|---|---|---|
| `PythonLexer` | source to tokens. It alone reads the source | `tokenize`: every file of `Lib`, and 100,000 damaged programs |
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

**`jsNumber()` will not take anything wider than an int32 here.** To JavaScript a number is a number, so `JSC::jsNumber()` makes a double of an integer that does not fit, which to Python is a `float`, and an int32 of a double that has
nothing after the point, which is an `int`. `socket.ntohl(0xFFFF)` was `4294901760.0`, and the hash of a function of JavaScript's was a `float` whenever the function was far enough up in memory, so that it could not be a key.
Inside `namespace Python` the name is a template that takes only what is sure to fit, and hides the other, so anything else does not compile: it is for `intFromInt64()`, `intFromUInt64()` or `floatFromDouble()`. What really is
wanted as a number of JavaScript's, as an operand that the code generator gives or something kept in an internal field, is asked for as `JSC::jsNumber()`.

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

### How long a list is is not kept either

Anything that compares the elements of a list, or makes a number of an index, runs a program's code, which can make the list shorter. So how long it is is asked for again after each such thing, as CPython asks for `Py_SIZE()`
in the condition of the loop. To read past the end of a list is safe, since `listGet()` looks first, but what it finds there is `undefined`, which is `None`: `l.index(None)` found a `None` in a list that had just been emptied, and
`l[i] = x` wrote past the end of one. `list-changed-meanwhile.py` does everything to a list that runs a program's code, with elements that change the list in eight ways.

### `list.sort()`

It is CPython's, in two parts. `PythonListSortKernel.h` is the part of `Objects/listobject.c` that has to do with the order of things and not with what they are: finding runs, binary insertion, galloping, merging, and which runs to
merge when. It is made by `lib/convert-list-sort.py` and is not to be changed by hand. Which things are compared with which, in what order and how many times, is something that a program can see, in a `__lt__()` that counts or
prints or raises, and it follows from every line of that. `PythonListSort.cpp` is the rest, written by hand: how two things are compared, the look at all the keys beforehand that decides that, and what is done with the list.

- **There is nothing in the list meanwhile**, and it says so: `len()` of it is 0. What was in it goes back afterwards whatever has happened, in place of whatever has been put there.
- **But not of an array that JavaScript has done something to**: sealed it, so that it cannot be emptied; kept it from being added to, so that it could be emptied and not filled again, and all that was in it would be lost; or
  made an element of it read-only, which taking it out and putting another in would get round. What is in one of those stays there meanwhile, and is written over afterwards if JavaScript lets it be.
- **`ValueError: list modified during sort`.** CPython tells by setting `allocated` to -1, which anything that makes room in the list sets to something else. Here the list is left with no room at all,
  `JSArray::releaseVector()`, and has been changed if it has any afterwards. So `l.append(1); l.pop()` is noticed, and `l.clear()` and `l.extend([])` are not, in both.
- **What is being sorted is in memory that the collector does not look at**, so that the kernel can move it about as C does. All of it is in a `MarkedArgumentBuffer` as well for as long as that goes on, and nothing is moved
  by the collector.
- A `__lt__()` that gives `NotImplemented` is called twice when everything is of one class, as in CPython: once outright, and again in the ordinary way.

### Where the bytes are is not kept

What is in a `bytearray` moves when it is resized, and JavaScript can give an `ArrayBuffer` away. Either can be done by anything that runs a program's code, and looking at
an argument does: `__index__()`, `__buffer__()`, going through an iterable. So a `std::span` of what is in something is good only until the next such thing, and is not kept.
`Buffer`, in `PythonBytes.h`, is what is kept: it holds the object and finds out where its bytes are each time it is asked. It is what stands between CPython's
`PyObject_GetBuffer()` and `PyBuffer_Release()`, and calls the `__buffer__()` and `__release_buffer__()` of a class that has them. What is written in C++ looks at all its
arguments first, and only then at how much there is of what it is a method of. A slice is resolved in two steps for the same reason, as in CPython:
`PySlice::unpack()`, which can run anything, and `PySlice::adjust()`, which is told the length as it is afterwards. The end of `programs/buffer-protocol.py` has every
method shrink what it is working on from within an argument.

A class that is written in C++ and has bytes of its own to show, as `array.array` has, keeps them in a `Uint8Array` that no program ever has hold of, and says so: `NativeState::exportedBytes()`. So they are the collector's to
account for and to free, `bytearray`'s way of being resized is theirs as well, and `builtinBufferOf()`, and so `Buffer` and `memoryview`, find them as they find anything else's, each time that they are asked.

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
One thing that CPython asks cannot be seen from Python, which is whether a class is done away with as its base is (`tp_dealloc`). Of the exceptions that are built in, each that has something more to keep has its own way and is the larger for it, so one that is no larger
than its base has its base's. So a class derived from `Exception` can be given `KeyError` for a base, and not `OSError`.

**Whether a class can be added to** is one of those flags too, `Py_TPFLAGS_IMMUTABLETYPE`, and not whether it is written in C++ (`PyType::isImmutable()`). Most of what CPython writes in C cannot be. `ast.AST`, `typing.Generic`,
`time.struct_time`, `os.stat_result` and a few more can, and programs do. Those of them that are derived from `tuple` are `IsDerivedFromBuiltin`, so that nothing is done to one as to a tuple without looking at what its
class now has. Giving such a class a `__name__` is giving it all that it is called, as in CPython: what module it is in, which had been the first part of that, is `__module__` from then on.

**How the methods come to know which class they are in.** `__class__` is a variable of the body of the class, which the methods are inside. As in CPython, the body leaves the cell that it is in the namespace, as `__classcell__`, and
returns it. `type()` takes it out of the namespace and puts the class in it, before it calls anything of the program's that might use `super()`: `mro()`, `__set_name__()`, `__init_subclass__()`. `enum` makes the members of a class from
`__set_name__()`, with a `__new__()` that may well call `super().__new__()`. `__build_class__()` looks afterwards whether the class that it got is what is in the cell, since a metaclass may have kept the cell back. `__classdictcell__` is the
same for `__classdict__`, which is where annotations look for names.

**`super()` with no arguments** is a call of whatever goes by the name of `super` at the time. In CPython it is `super` that finds the class and the first argument, in the frame of what called it. Here the compiler hands them to
`implicitSuper()`, along with what is being called. If that is not `super` it is called as it was written, with nothing. If it is, this is `super_init_without_args()`, which has something to say about each thing that may be missing.
`programs/the-cell-of-a-class.py`.

### Calling a class

To JavaScript a class is a function that is written in C++, as `Map` and `Array` are, and `PyType` is an `InternalFunction` as their constructors are. So what calls one remembers it, and goes straight to `callType()`, in every tier.

`C(...)` is `type(C).__call__(C, ...)`, which for nearly every class is `type.__call__()`: `__new__()`, and then `__init__()` (`instantiate()`). Both are looked for, and each is called from C++, which is a good deal to do for what a program does all the
time. CPython has two ways round it, and so has this.

- **`tp_new` and `tp_init`** are kept as they should be while a class and its bases are changed. `PyType::Construction` is what calling the class comes to where `tp_new` would be `object_new()`: an instance is made, with such a structure, and given to such a
  function. It is worked out when it is first wanted, and forgotten when the class, or anything that it is derived from, is given a `__new__` or an `__init__` or has one taken away, has other bases, or has something left in it for others to do
  (`constructionMayHaveChanged()`). A class of some other class than `type` has none. It is the same for a class of exceptions whose `tp_new` would be `BaseException_new()`, which is most of those that are built in and nearly all that a program
  derives from them: an exception is made, and keeps what it was given as `args`.
- **`tp_vectorcall`**: `int`, `str`, `float`, `bool`, `list`, `tuple`, `dict`, `set`, `frozenset`, `range`, `enumerate`, `map` and `filter` are called without either being looked for (`PyType::setVectorcall()`). A class that is derived from one is not. What is
  here does what is ordinary, and leaves the rest to `__new__()` and `__init__()`, which is where it is said what is wrong with the arguments.
- `object.__init__()` does nothing if the class has a `__new__()` of its own, whatever it is given, so it is not called.

**The DFG goes by the first of those** (`handleConstantFunction()`). `P(x, y)` is a `NewObject`, a `Call` of `P.__init__` as one function calls another, and `PyCheckInitializerResult`, for as long as `Construction::isAsFound` holds. A `PyInstance` is a `JSFinalObject` in all
but name, so what makes the one in line makes the other, and what puts one back together that was never made, because nothing was seen to need it, does too (`PyInstance::createWithButterfly()`). If `__init__()` returns something, that cannot be left to the baseline JIT to say, which would call it again, so it is said there. It is so whichever language does the calling, so the FTL has all three.

**`tp_new` stays `slot_tp_new()` once it has been that.** A class that is given a `__new__` and has it taken away again finds that of `object` when it looks. But `update_one_slot()` leaves `tp_new` as it is when what it finds is what a built-in class has, and
`object.__new__()` goes by `tp_new` to tell whose the arguments are. So `C(5)` is then a `TypeError`, for that class and for whatever is derived from it, then or later: `PyType::NewIsLookedFor`.

`programs/making-a-great-many.py` makes instances until what does so has been compiled, changes something that it depends on, and makes some more. `interop/classes-that-javascript-calls-often.py` is the same from JavaScript, often enough for the FTL.

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

**The globals can be a dict of a class that a program has derived**, given to `function()`, `exec()` or `eval()`, and so can the builtins. CPython then asks it as it would ask any mapping, so that its `__getitem__()` and
`__missing__()` are heard. `annotationlib` depends on that: to say what an annotation is that names something not yet defined, it runs what works the annotations out again, with globals that make something up for whatever is
not there. `dataclasses` does that to every class it is given. The object that such a dict keeps its items in has a property that says so, `loadGlobal()` looks for it, and nothing is remembered of what comes of asking. It being a
property, no such object has the structure of one that is not, so what has been remembered about other globals is not taken to be about these. Which of CPython's instructions a name is loaded by matters here:
`LOAD_GLOBAL` and `LOAD_FROM_DICT_OR_GLOBALS` ask, and `LOAD_NAME`, having asked the locals, looks in the globals for itself. Storing and deleting never ask.

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

**An instance can be called if its class has `__call__()`, whatever kind of cell it is.** To the engine that is `getCallData()`, which is asked only of a cell whose `Structure` says that it has something to say. A class that a
program derives from `tuple`, `dict`, `int`, an exception or anything else can be given `__call__()` at any time, so the `Structure` of its instances says so: `PyType::createInstanceStructure()`. What is built in never can, so a
tuple that is only a tuple is not asked, and costs JavaScript's `typeof` nothing. They all answer with the one function, `callInstance`, and that is how one is known where it matters (`isCallOfInstance()`), and not by a list of
kinds. To JavaScript such an instance is a function, though not one that is derived from `Function`. `programs/instances-that-are-called.py` derives from every class that can be derived from.

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

A `__new__` can rely on more: that the class it is given is made as its own is. `int.__new__(bool)` is refused, as `tp_new_wrapper()` refuses it in CPython, and for the reason that it does: what came of it would be laid out as one
thing and taken for another. The nearest class to the one given whose `__new__` is not a program's has to have the same function for it. The constructor of a class of JavaScript's counts as a program's.
`programs/new-of-another-class.py` tries every class that is written in C++ with every class that is derived from it.

The signatures, the docstrings, what kind of thing each attribute of a built-in class is, and how each built-in class is laid out are CPython's
own. `lib/dump-builtin-descriptions.py`, run by CPython, writes them to `lib/builtin-descriptions.json`, which is put into a header when this is
built. That is on macOS. Run by CPython on Linux, and given that file, it writes only how Linux differs, which is `lib/builtin-descriptions-linux.json`:
what there is only there, what is otherwise there, and what is not there at all. A module that is written in C++ is added to the list in that script. What CPython does not have gives its signature where it is
defined, and so does what has one in CPython that a program cannot see. It can begin with what the function is called when its arguments are
wrong, where that is neither its name nor its class's: `typevar(name, *constraints, ...)`.

A built-in class of a module other than `builtins` is named as in CPython, `types.GenericAlias`. That is what is said wherever something is said about
it or its instances, and its `__name__` and `__module__` are the two parts.

### Opcodes

Python gets its own where its semantics are its own *and* it matters how fast they are: arithmetic, comparison, truth, attributes,
subscripts, iteration, unpacking, globals, returning. One opcode serves a family, with the operator as an operand, which every tier but
the interpreter reads at compile time. Everything else is an existing opcode, or a call to a function of the runtime. Those are the
properties of one object, which is a link time constant.

In the interpreter `py_ret`, `py_load_global` and what looks at the one word (see "Being told of what is run") are done in place, and the rest call C++.

### The baseline JIT

`jit/JITPython.cpp`. It does for itself what is most often wanted, and leaves the rest to the same C++ that the interpreter calls, which with `TaggedArithmetic.h` is the definition of what is to come of it.

- **Numbers.** What is done to two ints that are not BigInts is done in line: `+ - * // % & | ^ << >>`, the comparisons, `-x`, `~x`. Whatever does not fit is for C++, which makes a BigInt. What is done to floats is compiled too, out of line, where a
  slow case goes: `+ - *`, the comparisons. `/` gives a float whatever it is given, so there is one way to do it.
  - An int32 is boxed by putting the tag over it, so there is to be nothing above its 32 bits. One that has had its sign extended comes out with `WholeFloatMark`, and is a float.
  - A double that JavaScript made can have the value of an int32, and then it is an int. So what adds floats looks (`branchIfDoubleIsNotTaggedInteger()`). Which of two numbers is the greater does not depend on it, nor does what one divided by the other is.
- **`x is None`** is a comparison with two bit patterns, and `x is y` of two cells with one. `if x:` of `True`, `False`, an int or `None` is done in place.
- **What is in a list or a tuple, by a number**, from either end, and setting what is in a list. Only what is simply there: see `loadPythonListItem()`. A list that shares what is in it with the literal that it came from is not written to here.
- **Going through** a `range`, a list or a tuple. **Taking apart** a tuple of the right length.
- **Whether it is a tuple, or of a class derived from tuple that may have a `__getitem__()` of its own, is said by the type of the cell**: `PyTupleType` and `PyDerivedTupleType`, and so for `dict` and `set`, as JavaScriptCore has `ArrayType` and
  `DerivedArrayType`. Which it is goes by `PyType::instancesAreWhatLiteralsMake()`, which is what the operators in C++ go by.

**Attributes are remembered as properties are**, by the same inline caches. `AccessType::PyGetAttr`, `PyLoadMethod` and `PySetAttr` are used as `GetById` and `PutById` are: the same registers, and the same handlers, since what they have to do is the same, which is to
check a structure and load or store at an offset, in the object or in some other. What is Python's own is what is called when nothing is remembered (`operationPyGetAttrOptimize()` and its like), and what may be remembered (`tryCachePyGetAttr()`,
`tryCachePySetAttr()` in `Repatch.cpp`). Those ask `locateAttribute()` and `classIfAttributeIsSetAsProperty()`, which look at how things stand once it has been done, and say nothing of what cannot be relied on.

| | is remembered as | for as long as |
|---|---|---|
| what the object itself has | `Load`, `Replace`, `Transition` | the class has nothing to say about it |
| what it has from a class, that is to an instance what it is | `Load`, from the class | the same, and the class has it there, and those before it have nothing by the name |
| a method that is to be called | the same | the same |

- **That the class has nothing to say** is `PyType::instanceAccessIsAsFound()`. It stops holding when the class or a base of it is given a descriptor with `__set__`, or a `__getattribute__` or the like, or other bases. There is another in its place from then on, so a class that
  is made in stages, as some libraries make them, is none the worse for it afterwards.
- **What a class has can be changed for another of the same kind and nothing is forgotten**, since what is remembered is where it is and not what: `C.count += 1`, or one function for another. If it is changed for what comes to an instance in another way, a number for a
  function, that fires the same thing (`isGotFromInstanceInTheSameWay()`).
- **Where a class has it, and that those before it have not**, are conditions on the classes, as for a property that is found in a prototype. But they are those of the order of resolution, which is not what the prototype of each is:
  `generateConditionsForPythonClassAttribute()`. A class that is built in cannot be given anything, so nothing is asked of it. `"abc".upper()` checks a structure and nothing else holds it up.
- **`py_load_method` has two things come of it.** What the function is to be given first is in a register that nothing of `GetById`'s touches, and is the object to begin with. So what loads from some other object, as it is, is what loads a method. What loads what the
  object itself has clears it (`pyLoadMethodLoadOwnPropertyHandler()`), as `module.function()` needs.
- Not remembered: what has a `__get__()` of its own, a property, one of `__slots__`, a method that is got and not called, which has to be made, an attribute of a class, and anything of an object of JavaScript's.

**What that took of JavaScriptCore is there for JavaScript too.** An `AccessCase` of any kind can now have a `WatchpointSet` besides its conditions, where only those that load could, and a `PutPropertySlot` can name one as a `PropertySlot` can. So `object.x = 1` in JavaScript, of
something of Python's, is remembered like any other. What has no way to be told that it has fired does not remember, which is the interpreter's own caches. What the DFG works out from a `GetByStatus` or a `PutByStatus` has: see below.

`programs/code-that-is-run-often.py` has every operator on every pair of some forty values that are near where one way of doing it gives way to another. `programs/attributes-that-are-remembered.py` does each thing many times over, changes something, and does it again.
`interop/attributes-that-javascript-remembers.py` is the same for JavaScript, often enough for the FTL.

### The DFG

Python's opcodes have nodes of their own (`Py...` in `DFGNodeType.h`, compiled in `DFGSpeculativeJITPython.cpp`), and everything else in the code is JavaScript's and is compiled as JavaScript's is. What is hard about it is not speed. It is that a good deal can
look at code in Python from outside while it runs, and the DFG is at liberty to keep things where it likes.

**What can be seen from outside is where it can be found.** `CodeBlock::registersSeenFromOutside()` are the registers that a frame object reads and writes: the variables, and the few that say what the frame is in.

- They are made use of by every instruction from `py_enter` on (`registersSeenFromOutsideAt()`, in `BytecodeUseDef.h`), so they are never taken to be finished with. Before that the code is putting what it was called with where it belongs, and to Python there is no
  frame yet.
- They are kept as values like any other, never as a bare number, are stored whenever they are given something, and are taken to be read by whatever reads the world.
- Where the DFG has put each of them is written down (`CommonData::m_machineRegistersSeenFromOutside`), and `PyFrame` goes by that (`registerOf()`).
- **They are variables to JavaScriptCore as well**, which is to say that they are the first so many of the registers, and are given something when the code is entered. `allocateVariables()` sees to it before anything else has a register. What comes after
  them has in it whatever was left there, until it is given something.
- **What is written from outside** (`frame.f_locals["x"] = 1`) is written where it would be read from, and what has been compiled is thrown away. It finds that out when it is come back to, and goes on in the baseline JIT with what it finds in the frame.

**An exception is thrown from the baseline JIT's frame.** It remembers every frame that it comes to, with its variables, whether or not anything there catches it. So to `Graph::willCatchExceptionInMachineFrame()` every frame of Python's catches everything. What
it goes to is an exit like any other, to the instruction that threw and not to a handler, and at the end of it the exception is thrown again as if the baseline JIT had been running all along (`adjustAndJumpToTarget()`). What catches it is looked for then. The
unwinder does nothing of Python's for a frame that is still the DFG's.

**Returning** looks whether there is a frame object, which outlives the frame and is told to take what it needs. If there is, that is for the baseline JIT (`PyCheckNoFrameObject`). It is no reason to think the worse of what was compiled, so it is not counted
against it (`ExitKind::PythonFrameObjectExists`).

**Nothing is being told of what is run, or this would not be what is running.** `py_line`, `py_call`, `py_branch` and `py_jump` come to nothing. That holds for as long as `VM::pythonIsNotWatched()` does, which fires when the first thing asks to be told, and
there is another in its place when the last has done. While anything is being told, nothing of Python's is compiled by the DFG. What has been put off (a signal) is seen to on the way into a function and on the way round a loop.

**A loop that calls nothing is taken to call nothing.** Every trip round a loop passes two things that may run code of a program's: going on with what is gone through (`PyIterNext`), and looking whether anything has been put off (`PyCheckPendingWork`).
Neither does as a rule. If they are taken to, then each time round every variable that has changed is stored for whoever may look, what had been found out about the heap is forgotten, and nothing that is made in the loop can be done without. So
each is compiled at first so as to run nothing, and to leave for the baseline JIT if it would have to. If that has been seen to happen there, it is compiled the other way, which is JavaScriptCore's usual arrangement.

- `PyIterNext` runs nothing if what it goes on with is what goes through a range, a list or a tuple (`PyIterator::runsNothing()`). Whether it has been given anything else is kept by the interpreter and the baseline JIT beside the instruction, and
  by the exit if it was given it after it was compiled (`ExoticObjectMode`).
- `PyCheckPendingWork` leaves with `PythonHasSomethingToSeeTo`. One signal costs nothing that matters. A program that is sent one a hundred times a second would spend its time getting back to what had been compiled, some 5 ms each time, so those exits
  are counted like any others, and after five in a loop it is compiled again so as to call the handler from where it is.
- **What says that something has been put off is written by a signal handler, at any time**, and B3 takes what nothing that it can see writes to be what it was when it was last read. With a call in the loop it could see something. Without, `while True:
  pass` went by what was read on the way into the function, and there was no stopping it. So in the FTL it is read by what is said to write it too (`pythonLimitUnlessWatched()`). The DFG's own code motion is kept off by `InternalState`, as for `CheckTraps`.
- The call that goes on with a generator is in the same loop as the `py_iter_next` that does for everything else, and has what is seen to come of it in common with it. If it has never been made it is left out (`ForceOSRExit`), or it would be the one
  thing in the loop that can change anything.

`programs/loops-that-call-nothing.py`: what is gone through changes on the way; something else is given to go through after nothing but ranges, which looks at the variables of what is going through it, or changes them; a signal comes from outside, or from a
timer while nothing at all is being called, in a loop that there is a way out of and in one that there is not. It is 1.1 to 1.8 times as quick where something in the loop can now be kept, and no different where there was nothing to keep.

**Where it can be told what the operands are, an operator is what JavaScript's own would be** (`fixupPyBinaryOp()` and its like, in `DFGFixupPhase.cpp`), and everything that the DFG knows how to do with those is done.

| | on | becomes |
|---|---|---|
| `+ - *`, `-x` | ints, floats, or one of each | `TaggedAdd` and its like, and so `ArithAdd` on Int32s that looks for what does not fit, or on doubles with `BoxTaggedFloat` |
| `/` | numbers | the same, after `PyCheckDivisor` |
| `// %` | ints | `PyFloorDiv`, `PyMod`, which round down. By a power of two, a shift and a mask, which do as well |
| `& \| ^ ~`, `<< >>` by a constant | ints | `ArithBitAnd` and its like. `<<` is a multiplication that may not fit |
| `< <= > >= == !=` | ints, or numbers | `CompareLess` and its like |
| `== !=` | strings | `CompareStrictEq`. Which of two strings comes first is another matter, since JavaScript goes by code units |
| `if x:`, `not x` | a bool, an int, a string, `None` | `ToBoolean`, `LogicalNot`. A float is compared with 0, since NaN is true |
| `x[i]`, `x[i] = y` | a list and an int | `GetByVal`, `PutByVal` |
| a global | | `CheckStructure` and `GetByOffset`, by what the interpreter remembers |

- What does not fit, dividing by 0, a place that is not in the list or is counted from the end of it: all are left to the baseline JIT, and where that has happened the operator is left as it is the next time.
- **That a variable may have nothing in it is not held against it.** Every variable begins that way, and what goes through something has nothing left in the end, so it is among what is expected of most things. It is left out of account in deciding
  (`predictionIfBound()`): what has nothing in it is none of the kinds that are looked for.
- **And whatever decides again has to leave it out of account too.** What is made a node of JavaScript's is then seen to as JavaScript's are, and that went by what the operands might be with nothing left out. So having been
  found to be a list and an int, they were found not to be, and what JavaScript does when it cannot tell is right for JavaScript. `GetByVal` and `PutByVal` did what is done with any object and any key, to which a place before the
  start of an array or past the end of it is a property like another: `items[-1]` was `None`, `items[-1] = x` did nothing that could be seen, `items[len(items)]` was `None` and not `IndexError`, and `items[len(items)] = x` made the list longer.
  And what was made a double of was made one as JavaScript makes one of anything, so `None / 2` was `nan`. All in what had been run a few thousand times, with the variable of a loop, and in nothing else. Now how the array is got at is
  settled from the same as it was decided from (`predictionForArrayMode()`), and it is asserted that it comes to the same. `programs/places-in-a-list-that-are-not-there.py`.
- `programs/used-to-one-thing-and-given-another.py` does each of some two hundred things often enough with the usual operands to be compiled for them, and then with every other sort: fifty thousand in all, and all as in CPython.
  It does each to the parameters of a function, to variables, and to the variables of loops, and it was only the last that found any of this.
- **A list is not to become one that keeps floats as they are**, which is what JavaScript would make of a list of ints that is given a float, since it could not then be told which of them are whole. So that is not made a `PutByVal`.
- `py_get_item` and `py_set_item` remember what kind of array they have been given, as `get_by_val` and `put_by_val` do, and their nodes have the same things in the same places, so that the one is made the other by saying so.
- `a, b = b, a` makes no tuple.
- What is left finds out what it has been given when it runs, in line, as the baseline JIT does.

**What an inline cache remembers of an attribute is done without asking it.** It is remembered as a property is, so `py_get_attr` and `py_set_attr` are given to what `get_by_id` and `put_by_id` are given to (`handleGetById()`, `handlePutById()`), and
`py_load_method` to the part of that which loads. What comes of it is a check of the structure, and a load or a store at an offset. What is in a class is a constant for as long as nothing else is put there, so `p.norm()` is a check and a call of a function that is known.
That the class has nothing to say about it is something that an `AccessCase` could say and a `GetByVariant` or `PutByVariant` could not. Now they can (`additionalSet()`), whoever goes by one watches it (`watchAdditionalSets()`), and two that hold for as long as
different things do are not made one. So what JavaScript reads and writes of an object of Python's is done so as well.

**The globals of a module are known when its functions are compiled.** The environment that has them is made by whoever runs the code, and its `SymbolTable` belongs to what is run (`FunctionExecutable::pythonGlobalsSymbolTable()`), as that of a scope that code
makes for itself does. It is by the table that JavaScriptCore knows whether there has been more than one. There is one for a module, so `.globals` is a constant, and what a global function is may be too. Code that `exec()` is given twice has two, and then it is not.

**What that found.**

- The operations that make a function when there is no room to do it in line are one for each structure that a function of JavaScript's can have, and were chosen without asking whose it is (`selectNewFunctionOperation()`). And the DFG made every generator one of
  JavaScript's (`JSGenerator::selectStructureForNewGenerator()`). `interop/what-is-made-by-code-that-is-run-often.py`
- Until an `ArrayBuffer` has been detached, the DFG takes it that a view is as long as it ever was. A `bytearray` that is made longer or shorter where it is says that one has been (`JSArrayBufferView::didChangeOwnedStorage()`).
- A `WatchpointSet` that a plan means to watch was kept only by whatever the plan kept, and a status does not keep what its variants have: it lets go of those that turn out not to matter, and the collector has it let go of those with something dead in
  them. A class has another set in place of one that has fired, so the plan could be left asking something that was no longer there whether it still held, and then adding to it. `DesiredWatchpoints::addLazily(Ref<WatchpointSet>&&)`
- What has been compiled and not yet installed kept from the collector whatever the function had in its variables when compiling was begun, for as long as it waited, which could be for good. It lets go of them when compiling is over, and refers weakly to what it
  will refer to weakly once it is installed: if any of that goes, so does it (`Plan::isKnownToBeLiveAfterGC()`).

**What a variable had is kept until it has something else**, though nothing in the code looks at it again, since whatever the code calls in the meantime may. To the bytecode every instruction uses the registers that a frame object sees. To the graph that is
a `Flush` of what the register had before each `SetLocal`, which is what JavaScriptCore does with the register that a debugger looks for the scope in. Without it the store went, and in SSA there was no `Phi` for what the bytecode said was live.

**When it is compiled.** JavaScriptCore puts off compiling code again until three quarters of its `ValueProfile`s have seen something, up to five times, taking that for a sign of how much of the code has been run. It leaves out those of the arguments. Code in Python
begins by giving its parameters what it was called with, and most of that is for what is out of the ordinary: too few, too many, some left to their defaults. In a small function that was enough for it to be put off all five times. What is found out
there says what the parameters are, as the profiles of the arguments do, so it is left out with them: `CodeBlock::numberOfValueProfilesOfArgumentBinding()`.

**The graph is looked over after every phase** (`--validateGraphAtEachPhase`) in the configuration in which the DFG compiles nearly everything. What that found: the register that the code of a module has its namespace in, if the module is a coroutine, was no variable, and
so had nothing in it on the way back in; what is taken out of a tuple of results did not say that anything came of it until later; and `Return` and `PyGetTupleItem` were taken to be able to leave for the baseline JIT where nothing may.

`programs/what-was-expected-and-then-was-not.py` runs each operator with one kind of thing until it has been compiled for that, and then gives it every kind. `programs/looking-at-what-is-run-often.py` looks at, and changes, what has been compiled: its variables,
what an exception remembers of it, a frame that is kept, being told of what is run beginning in the middle of it. The runners have a configuration in which the DFG compiles nearly everything, with next to nothing known of it.

Not yet: code in Python is not inlined. An attribute that has been found in more than one way is asked of the inline cache.

### The FTL

It has the graph that the DFG has, so what there is to it is what each of Python's nodes comes to in B3 (`compilePy...()` in `ftl/FTLLowerDFGToB3.cpp`), and what follows from the graph being in SSA.

- **What the DFG does in line, it does in line**, test for test: going through a range, a list or a tuple, what is at a place in a list or a tuple, counting the frame. The fields have heaps of their own (`PyIterator_index`, `PyTuple_values` and so on in
  `b3/B3AbstractHeapRepository.h`), so that B3 knows what a store to one can have changed.
- **Attributes are asked of the inline caches that it has for JavaScript's properties**: `getById()` and `cachedPutById()`, with `AccessType::PyGetAttr` and `PySetAttr`. `PyLoadMethod` calls C++, since two things come of it and only its own handler knows that. By then it is only
  what has been found in more than one way.
- **Where a frame object looks.** `PutStackSinkingPhase` puts off storing a variable until something reads the stack, and whatever reads the world reads the registers that a frame object sees (`PreciseLocalClobberize`), so they are stored before each call and not otherwise.
  Where they are is `localsOffset` further on than the graph says (`FTLCompile.cpp`).
- **`%` and `//` put right which way they round with branches.** With `select` what comes next waits to be told which it was: 4.7ns and not 3.6 each time round a loop that goes by what is left over.
- **A call that is part of what an instruction does is not the call that the instruction makes.** The FTL asks what the DFG's code found out about each call, by where in the bytecode it is. Where a class is called and the DFG has made the instance in line, the call that is there is to
  `__init__`. Taken for what is known of the call to the class, that made two of what there was one of, and nothing was made in line any more. `Node::isCallOnBehalfOfInstruction()`, `CallLinkInfo::isOnBehalfOfInstruction()`.
- `CheckTierUpAtReturn` comes after `PyLeave`, where nothing may leave for the baseline JIT. It never does.

- **What is made and goes nowhere is not made**, an instance of a class of Python's like any object of JavaScript's. A variable is somewhere: it is where a frame object looks, so what it has is made before anything is called. It is only if nothing is called that it is not, and
  then it is put together if the code is left for the baseline JIT. `CombinedLiveness` took what is live where a block ends in `Return` for what is live afterwards, which for JavaScript is nothing. `py_ret` looks at every variable, so nothing was ever left out.

The runners have a configuration in which the FTL compiles nearly everything with next to nothing known of it, and one in which it compiles what has been run some tens of times, which is when it leaves out the most. `programs/what-need-not-be-made.py`

### Exceptions, tracebacks and frames

`raise` is `throw`, and `try` is JavaScriptCore's. `except` compares classes.

Raising again is throwing the same thing again: the `JSC::Exception` that was caught, and not what is in it. The unwinder adds to the
traceback as it goes, and can tell the one from the other.

A frame object is a `PyFrame`. There is at most one for each time that a piece of code is run, and none until it is asked for. While the
frame is on the stack it reads and writes the registers. What leaves a frame says so (`py_ret`, and the unwinder), and the variables are
copied then. So a local variable is copied when it is loaded for later use: anything that is called may change it.

**A frame that is one too deep does not begin.** When `op_py_enter` finds that, the frame is put back where it was before there was anything to be seen of it, at its first instruction.
So it is in no traceback, nothing is told of it, and nothing in it catches the `RecursionError`, which matters for a generator that is resumed inside a `try`. What went wrong is the call.

**What is written in C++ can put itself in a traceback**, as `pyexpat` does what calls a handler, so that what a handler raises is seen to have come by way of `StartElement` in `pyexpat.c`: `addTracebackEntry()` with a name, a file and a
line, which is `_PyTraceback_Add()`. There it is a frame made for a code object that does nothing, `PyFrame_New()` of `PyCode_NewEmpty()`. Here it is too: `PyFrame::forWhatIsNotRun()`, which is over from the first, of code that is compiled and
never run, so nothing that is told of what is run is told of it, and it is on no stack while the handler runs. The line follows from the code, since `traceback.py` goes by `co_positions()` and not by `tb_lineno`.

**What prints an exception that gets away is the library's `traceback`**, as in CPython: `PyErr_Display()` imports it and calls `_print_exception_bltin()`. So what a program has done to `traceback` or to `linecache` shows,
and so does the source of what was never in a file, which `linecache` is told of: what follows `python -c`, and what is typed.

**If that cannot be had, or raises, there is `PythonExceptionDisplay.cpp`.** It is for when there is no library, or Python has not got as far as being able to import anything. CPython has something in C for that too, which
says less. This is `TracebackException` of `Lib/traceback.py`, function for function: what part of a line to point at, and with what; lines that are the same over and over; what led to it, without going round for ever;
notes; groups; and the name that may have been meant.

- Where that has the `ast` module parse a piece of a line, to find the operator or the brackets, this has the parser.
- It counts in code points, as Python does, and in columns of a terminal where it lines things up.
- It goes by where each instruction says that it is from, which is what `co_positions()` gives. That has one for each *two bytes* of code, because what wants to know where an instruction is from
  takes what comes at half its offset: `traceback.py` does with `tb_lasti`, and so does `dis`.

**What is printed of an exception that has nowhere to go**, by what `sys.unraisablehook` is at first, is less, because in CPython that is written in C: `formatTraceback()` is `_PyTraceBack_Print()` of `Python/traceback.c`. There is
one line of source for each frame, read from the file, with nothing under it. `sys.tracebacklimit` says how many frames, and it is the innermost that are kept. What led to the exception, and its notes, are not shown.

**`BaseExceptionGroup`, and what `except*` is compiled into calls of** (`PythonExceptionGroups.cpp`), are `Objects/exceptions.c` and `_PyEval_ExceptionGroupMatch()`, function for function.
`ExceptionGroup` has two bases, and is made when a realm is as a class statement would make it, as in CPython.

### Going through a generator

`for x in generator` goes on with the generator each time round. From C++ that is `resumeGenerator()`, which enters the VM to do it. So once a loop has been compiled it is done as JavaScript does it for its own, by a function that is written in JavaScript
and compiled like anything else, which the DFG inlines: `pythonGeneratorNext()` in `builtins/GeneratorPrototype.js`. One piece of compiled code calls another. A generator of JavaScript's is gone on with in the same way, so it does for both.

- **`py_is_resumed_by_call` says which way it is to be.** Both ways come to the same thing, so it is a matter of which is better. The interpreter says not: it would be interpreting all that. The baseline JIT says so of a generator unless something is being told of
  what is run, since what is told of a loop is told by `py_iter_next`. To the DFG it is whether it is a generator.
- **What is being handled is where that function can get at it**: `PyRealm::handledExceptions()`, an `InternalFieldTuple`, and what a generator was handling when it yielded is a property under a name that it can name, `@pythonHandled`.
- **It comes back with nothing at all when there is no more**, as `py_iter_next` does (`@emptyValue()`). Anything else would be taken for one of the things that the generator yields, and a loop through ints would no longer be known to be one. What a call
  comes back with is taken by the DFG to be something, so if it has not inlined this one it is told otherwise: `PyValueOrNothing`, which goes by `PythonGeneratorNextIntrinsic`. That is what is put in the register, so it is what the register is said
  to have as well (`MovHint`). When it was said to have what came of the call, nothing kept that past the end of the block, and the block ends there. So if what had been compiled was thrown away while the generator ran, which a generator can see to by
  changing a variable of the loop's function and the collector by letting go of something, the loop went on with `None` and not with what had been yielded.
- **What has been found to come of it is remembered in one place**, whichever way it is come by. The two instructions have the one `ValueProfile`, so there is not one in every loop that never sees anything.
- What is out of the ordinary is for C++: a generator that is running or has come to an end (`pythonGeneratorNextSlow()`), and what is to come of an exception that gets out (`pythonGeneratorRaised()`).
- `next()`, `send()`, `yield from` and `await` are from C++ still.

`programs/going-through-generators-often.py`, `interop/generators-of-either-language-often.py`

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
- **What has to go back and forth by so many characters at a time**, as what matches a regular expression does, has such a string with each character in a place of its own: `ExpandedString`. That is how CPython keeps
  one all the time. It is made once and kept in the same cache, so that `pattern.match(text, pos)` in a loop does not go through the whole of `text` each time round, and the collector is told how much there is of it.
- **Order is by character.** The halves of a pair are numbered below U+E000 and a pair stands for what is above U+FFFF, so `compareStrings()` finds the code unit that two strings first differ in, goes back one if
  that is in the middle of a pair, and compares the characters there.
- **Half a pair by itself is a character**, as it is in Python, where `surrogateescape` makes them. It is not part of a pair that has the same for one of its halves: `'\ud83d' in '\U0001f600'` is false. So what is
  looked for with `findCharacters()` and the rest is not found in the middle of a pair. That takes looking into only if it begins with a second half or ends with a first half, which is next to never.
  And `strip()` goes by a character at a time, or it would take half of one emoji for half of another.

### What kind of thing a character is

Whether a character is a letter, a digit or a space, whether `repr()` shows it, whether a name can have it in it, and what it is in another case, are asked of `PythonUnicodeType.h`, which is `Objects/unicodectype.c`
over the same table, `PythonUnicodeTypeDatabase.h`. `str`'s methods go by it, and the lexer, `int()` and `float()`, `repr()`, format specifications and regular expressions. ICU is not asked. It knows whatever version of
Unicode came with the system, so a program would do one thing on one machine and another on the next, and it does not always mean the same by the question: one character for one, the sharp s in upper case is itself to
ICU and `S` to CPython, which regular expressions that ignore case depend on.

The table is not written by hand. `lib/convert-unicode-type-database.py` makes it from CPython's, and is to be run again when the version of CPython changes. `programs/every-character.py` asks everything of every character.

The rest of what there is to know about a character is `unicodedata`'s: `PythonUnicodeData.cpp`, which is `Modules/unicodedata.c`, over `PythonUnicodeDatabase.h` and `PythonUnicodeNames.h`, which
`lib/convert-unicode-database.py` makes from CPython's tables in the same way. It is in the engine and not left to the host because the engine wants it for itself: the lexer for the NFKC form of a name and for `\N{...}`, the codecs
for `\N{...}` and `namereplace`, and what shows an exception for how wide a character is where a caret is put under it. So ICU is asked nothing at all, and what a name in source is taken for does not depend on which system it is
compiled on. `programs/unicodedata-module.py` asks everything of every character, as things are now and as they were in 3.2.0, which is what the IDNA encoding goes by.

`unicodedata._ucnhash_CAPI` is a `PyCapsule`, which is how one module written in C hands another a pointer. Nothing here is handed one that way. A program can see that it is there and what it is called, so it is: `PythonCapsule.cpp`.

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
- The first time, the buffer has no room to spare, so adding to a string once costs no memory. It is when what comes of that is added to in its turn that the buffer is made twice as large as the string.
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

### A method is not all that CPython goes by

A class that CPython writes in C fills in slots, and `__getitem__` and `__add__` are made from those for a program to call. There are two slots for each, one for a sequence and one for a mapping or a number, and which of
them a class has filled in decides things that the method alone does not. Here there are only the methods, so a class that is written in C++ says the rest in its flags, and what an operator does goes by the class that the
method it found belongs to. So a class that a program derives is the same as its base until it has a method of its own, as with slots.

- **`IsSubscriptedAsSequence`**: `sq_item` and no `mp_subscript`, as `deque` has. `d[key]` takes a number and nothing else, and says so in other words than `d.__getitem__(key)` does.
- **`AddsAsSequence`**: `sq_concat` and `sq_repeat`. See above, and `isSequenceSlot()`.
- **`isSequence()`** is `PySequence_Check()`: whether there is `sq_item`. `iter()`, `reversed()` and `in` go through what has `__getitem__` and no `__iter__` only if so. A `re.Match` has `mp_subscript` alone, and cannot be gone through.
- **`IsDerivedFromBuiltin`**: what the operators do with a `dict` or a `list` without asking, they do only if that is exactly what it is. A class that a program derives is known by being a heap type. One that is written in
  C++, as `defaultdict` is, says so.

### `__dict__` is the object

The attributes of an instance and the globals of a module are properties, which is what lets them be cached inline. `obj.__dict__`,
`vars()` and `globals()` give a real `dict` that is *backed by* the object: its items with string keys are those properties. Neither
can be out of date, since there is only the one copy. A plain dict that is given to `exec()` for its globals becomes backed by a
bare object, so compiled code always finds its globals the same way.

- **An attribute is a property that is enumerable, and is a value.** What is not enumerable is JavaScript's business, and Python does not see it: the
  `name` and `length` of a function, the `stack` of an `Error`. So is an accessor.
- **A function is the one object of Python's that JavaScript can do anything to**, being a `JSFunction`. It can be frozen, and a property of it can be made an accessor, or read-only. So
  before an attribute of one is set or deleted, it is seen whether JavaScript has left it so that it can be (`tryPutStoredAttribute()`), and if not that is `AttributeError`. Every other
  cell of Python's refuses to have such a thing done to it, and is not asked.
- What CPython keeps in a field of a C struct is a property under a private name, which neither language can name.
- A name that JavaScript would take for an index, as `"0"`, cannot be that of a property. It is kept where a key that is not a string is: in the
  dict's own table.
- Two objects can have one `__dict__`. The second finds its attributes in the first.

How many attributes there are is `len(o.__dict__)`, and it is asked at each step of going through the dict, to say so if it changes. What Python counts is what `Object.keys()` would list, less the accessors: what is enumerable, is
not keyed by a symbol, which leaves out what the engine keeps in the object for itself, and is a value. An object with many properties has a table of its own that is changed where it is, so the count cannot be kept by
`Structure`. `PropertyTable` keeps it, `enumerableStringKeyedValueCount()`, and it changes where a property is added, removed or has its attributes changed, none of which is where an inline cache goes. It fits in what
was padding.

### Modules, and what is up to the host

Importing is divided as CPython divides it. What the statement means, `__import__()`, `sys.modules` and the tables of what is built in and what is frozen are `Python/import.c`, which is `PythonImport.cpp`, and
`_imp`, which is `PythonImpModule.cpp`. Finding a module and loading it is `importlib`, which is written in Python, and is CPython's own: `lib/importlib/_bootstrap.py` and `_bootstrap_external.py` are its files as
they are, and are in the engine as they are in CPython's binary. So finders, loaders, specs, path hooks, namespace packages and the locks are not like CPython's. They are CPython's. Its frames are on the stack and are taken
out of tracebacks when CPython takes them out: `removeImportlibFrames()`.

The statement calls whatever `__import__` is in the builtins of the frame, with the globals and the locals of the frame, unless it is the one that there was to begin with, which there is no need to call.
`import a.b.c as d` imports `a` and gets `b` from it and `c` from that, as `from` would.

**`import` finds what is JavaScript's too.** `JavaScriptImporter` is the last on `sys.meta_path`, so what is Python's by a name is what the name means, and a program that has no JavaScript in it is asked nothing new
until an import has failed. It is `lib/_javascript_importer.py`, twenty lines in the shape of `BuiltinImporter`, over `_javascript.find_module()` and `load_module()`.

- **Where to look is for the host to say**, `Configuration::findJavaScriptModule`, since what a name means is a matter of `node_modules` and the like, which the engine knows nothing of. It is given the name and where Python
  looked, and gives back what the module loader knows the module by, the file that it is in, and what its `__path__` is to have in it if there may be modules below it. If it is null there is no such importer.
- **A name that is the standard library's is never JavaScript's**, whether or not there is such a module here. Much of the library tries `import readline` or `import zlib` to see whether it can, and is not to be given something else.
- **It is loaded and run before `import` returns**, by `JSModuleLoader::loadModuleSync()`, which is what `require()` of an ES module is done with. Python's `import` does not wait, so a module that awaits something as
  it is run, or imports one that does, is `ImportError`. Once JavaScript has waited for it, with `await import()`, it can be imported like any other.
- **JavaScript remembers what it has loaded.** Taken out of `sys.modules` and imported again, it is the same object and is not run again. One that threw throws the same again.
- **In a cycle it is the module of Python's that is seen half done.** What JavaScript imports that is Python's is run once all of the graph has been loaded and before any of it is linked, since what it exports is not known
  until then. So if it imports what imported it, that is the first that has been asked of that module, and it is run there and then, as it would be by `require()`.

`interop/importing-javascript.py` has each of these.

A frozen module is source here, and not code that has been compiled and marshalled, since there is nothing yet that compiles ahead of time. What `_imp.find_frozen()` gives for the data is made when it is asked for.

The name of the file that code is from goes with the source, and all the code that is compiled from one source has the one name. `_imp._fix_co_filename()` gives the source another name, which is what it does in CPython
to the code and all the code in it. It does it as well to code that `code.replace()` made from that, which in CPython has a name of its own.

An ES module can import a Python file: it is a synthetic module record, made when it is needed as for CommonJS, that exports each global by name and the module as the default. The file is imported by the name that
`import` would find it by, if it is somewhere on `sys.path`, and otherwise as `importlib.util.spec_from_file_location()` would have it.

What is up to the host is asked of it as JavaScript asks it, through `GlobalObjectMethodTable::configurePython`, which fills in a `Configuration`, as whoever embeds CPython fills in a `PyConfig`: `sys.argv`, `sys.path` and
the like, and what there is to import besides what the engine has, `builtinModules` and `frozenModules`, which are `PyImport_AppendInittab()` and `PyImport_FrozenModules`. `posix` is one: it is what a program reaches the
system with, so whether there is such a module is the host's to say. It is written here all the same (`createPosixModule()`, which the host lists or does not), since otherwise each host would write it again.

**A module that CPython writes over a library is the host's to write**, if it has the library: `_hashlib` over OpenSSL, `zlib`, `pyexpat`, `_ssl`, `_sqlite3`. The engine has none of those libraries and is not to want them. Such a module is written as the
ones here are, with what they are written with, all of which is in headers that a host can include: `PythonBuiltins.h`, `PythonOperations.h`, `PyStateObject.h`, `PythonBytes.h`. What its functions take and what they are for come from the same table as
everything else's, since that is about CPython and not about who wrote what: it is added to `MODULES` in `lib/dump-builtin-descriptions.py`.

What is done to an open file is asked of the host too, if it wants to be asked: `Configuration::files`, a `FileOperations`, which is `open`, `close`, `read`, `write` and the few others that `io.FileIO` is made of.
`FileIO` and the functions of `posix` that do the same go through it. That is so that what Python writes to the standard output can go the way that what JavaScript writes there goes, and the two come out in the
order in which they were written. Left alone, it is the system calls.

### Starting, running and stopping

There are two stages to starting, as there are in CPython. The first is when a realm is made, which is when Python is first used in a global object: the classes, `builtins`, `sys`, and `importlib` as far as what is built
in and what is frozen. It reads no files and cannot fail. The second is `startPython()`, which is `init_interp_main()`: importing from files, `encodings`, the standard streams, `builtins.open`, `__main__` and `site`. It
runs a good deal of the library and can fail, most simply because there is no library, and then it throws. There are two ways into Python from outside, `runMain()` and an ES module importing a Python file, and each calls
it first (`PythonLifecycle.cpp`).

The part of the library that is written in Python does not come with the engine. Where it is is for the host to say, in `Configuration::moduleSearchPaths` or `frozenModules`. The shell goes by `PYTHONPATH` and
`PYTHONHOME`, as CPython does.

### The command line

**`readCommandLine()` is what `python` does with what it is started with** (`PythonCommandLine.cpp`): `Python/getopt.c`, and what has to do with the command line or the environment in `Python/preconfig.c` and
`Python/initconfig.c`. It fills in a `Configuration`, which has grown to hold the rest of `PyConfig`. It needs nothing of the engine's and is done before there is an engine, so that `python -h` and `python -Z` make none.
If there is nothing to run after all it gives the status to end with, having said what there is to say: what was asked for, or what is wrong, in CPython's words, down to `Fatal Python error: config_init_hash_seed: ...`
and how far Python is said to have got.

**What it says of itself is CPython's text.** `lib/convert-usage-text.py` takes it out of `initconfig.c` as it is (`PythonUsageText.h`).

**`runMain()` with no file is `pymain_run_python()`** (`PythonLifecycle.cpp`): whichever of a command, a module, a file, a directory or an archive with a `__main__.py` in it, or what comes in on the standard input the
`Configuration` says to run, and what `sys.path` begins with for each. What follows `-c` has taken from it whatever blanks all of its lines begin with, and is given to `linecache`.

**Whose command line it is is the host's to say.** The shell takes it for Python's if it was started by a name that begins with `python`. That is how one program that is Python's starts another: it runs `sys.executable`
with what `python` takes, which `subprocess`, `multiprocessing`, `venv`, `ensurepip` and most of CPython's own tests rely on. What is to be said to the engine can then be said in the environment (`JSC_useJIT=0`).

**Where things are is worked out by CPython's own `Modules/getpath.py`**, which is in `lib` as it is there, and is part of the program as it is part of `python`. `PythonGetPath.cpp` is `Modules/getpath.c`: it gives that
what it goes by and a dozen functions to look at files with, runs it as Python starts, and takes what it comes to: `sys.executable`, `sys.prefix` and its like, `sys._stdlib_dir` and `sys.path`. So `PYTHONHOME`, `pyvenv.cfg`,
`python3.14._pth` and a build directory all mean what they mean to CPython. It is asked for with `Configuration::computesPaths`. A host that knows where things are says so instead.

- It goes by where the program *really* is. A link named `python3` finds the library beside what it is a link to, and not beside itself.
- CPython has built into it where `make install` was told to put it, and falls back on that if it cannot make out where it is from the name that it was started by. That is `<prefix>/bin/python3`, so here it is two
  directories up from where the system says that the program is.
- `sys` is made before this is known. `updateSysFromConfiguration()`, which is `_PySys_UpdateConfig()`, sets all of it that follows from the `Configuration`, when `sys` is made and again after this.

**What an option is looked at by:**

| | |
|---|---|
| `-O`, `-OO` | `compileSource()`, if it is not told otherwise, and `compile(optimize=-1)`. Not what is frozen, which CPython compiled when it was built. |
| `-b`, `-bb` | `bytes.__str__()`, and `__eq__()` and `__ne__()` of `bytes` and `bytearray`. That the second is an error is a filter in `sys.warnoptions`. |
| `-X warn_default_encoding` | `io.text_encoding()` and `TextIOWrapper()` |
| `-X importtime` | `findAndLoad()`, and for `=2` `ensureIsInitialized()`, in `PythonImport.cpp` |
| `-X no_debug_ranges` | `co_positions()`, which then has no columns, and that is all that `traceback` goes by |
| `-X int_max_str_digits` | `PyRealm::maximumDigitsOfIntAsString` |
| `-X cpu_count` | `os.cpu_count()` |
| `-X frozen_modules=off` | Nothing yet. It is for having what is in a file instead, and there is no file that what is frozen here is in. |
| `-u`, `PYTHONIOENCODING` | `initializeStandardStreams()` |
| `-W`, `-X dev`, `-E`, `-I`, `-P`, `-s`, `-S`, `-q`, `-v` | The library, by way of `sys.flags`, `sys.warnoptions` and `sys._xoptions`, and `getpath.py` |

**What is taken and does nothing:** `-B` and `PYTHONDONTWRITEBYTECODE`, since nothing is written unless the host asks; `-R` and `PYTHONHASHSEED`, since what a `str` hashes to is the engine's and is the same from one run to
the next; `-d`; `--check-hash-based-pycs`; `-X tracemalloc`, `-X faulthandler`, `-X perf` and `-X showrefcount`. Each is still found fault with as CPython finds fault with it.

**What is wrong when Python cannot start** is said as `fatal_error()` says it. `startPython()` keeps what it is about, in the words of the `PyStatus` that CPython would end with: `Failed to import encodings module`.

### What is typed

**When a statement that is typed is over is settled by the parser wanting no more of it**, as in CPython, and not by trying what has been typed so far to see whether it will compile, which is what `codeop` has to do. The
scanner reads a line when it needs one to make the token that the parser has asked for (`TypedTokens`, over a `LineSource`, which is what CPython's tokenizer has as `underflow`), and that is when the prompt is shown. So a
prompt is shown, and something is said to be wrong, at just the point that CPython does either.

- **Tokens stay where they are put** (`TokenBuffer`, a `SegmentedVector`). The parser holds on to tokens while it looks at others, and now more can be made meanwhile. CPython's parser has an array of pointers for the same
  reason. All parsing uses it. It is a little quicker than the `Vector` that it took the place of, which copied them all whenever it grew: 19.5 ms against 20.6 for ten of the largest files in the library.
- **Reading a line at a time and giving tokens as they are written are two things.** `tokenize` wants both. The parser wants the first and not the second: it wants what a token comes to.
- **What is different about a prompt is asked of the scanner by itself** (`Arena::isTypedAtPrompt`, which is `tok->prompt != NULL`): a line with nothing at all on it ends whatever has been begun, a first line with nothing on it to
  speak of is a statement that does nothing, and nothing is added to a last line that nothing ends. It has to be by itself because code is generated from source, so what was typed is parsed again when it is all there.
  Code keeps that it was typed, in `FunctionInfo::futureFeatures` (`IsTypedAtPrompt`), and so does what `marshal` writes.
- **Going over what is wrong, to see what to say of it, is no reason to ask for more** (`stopReading()`, which is `IUNDERFLOW_STOP`). Nor is what comes after looked at, to see whether the reason is there: there is no after.
- **What there is to warn of in a line is warned of before the next is asked for.**
- **Each statement has a name of its own**, `<stdin-3>`, which `linecache` is given the source by. It is shown as `<stdin>`. What the parser finds wrong is said of `<stdin>`, and what is found wrong afterwards of `<stdin-3>`.

**The rest is `PythonInteractive.cpp`:** `PyOS_StdioReadline()`, `tok_underflow_interactive()`, `_PyRun_InteractiveLoop()` and `sys._baserepl()`, and `input()`, which reads a line the same way if it is typed at a terminal. What is
around them in `Modules/main.c` is in `PythonLifecycle.cpp`: `-i` and `PYTHONINSPECT`, `PYTHONSTARTUP`, `sys.__interactivehook__`, and `_pyrepl` if it is a terminal and that can be had.

- **A line is read by way of stdio**, as in CPython, and not by way of `sys.stdin`, which holds on to what it has read ahead. So which of the two gets what, of input that is not typed, is the same here as there.
- **What `from __future__ import` asks for goes on being so** for what is typed afterwards.
- **`SystemExit` is the end once statements are being typed.** Before that, with `-i`, it is shown like anything else: `Configuration::inspect` is what is looked at, and it is cleared on the way in, which shows in `sys.flags`.

### When it is over

When a program is over is for the host to say as well, since one that is in two languages is not over when the Python that began it has been run to its end: `finalizePython()`. It does what `Py_FinalizeEx()` does while
there is still an interpreter to do it in, in the same order: `threading._shutdown()`, what has been registered with `atexit`, and flushing the standard streams. The two that Python started with are flushed as well as the
two that are there now. In CPython those are flushed when they are let go of, with everything else, and nothing is let go of here: without it `print('x'); sys.stdout = None` prints nothing.

### Where the library is written

What CPython writes in C is written here in C++, and what CPython writes in Python is that Python, as it is. A program cannot see into what is written in C: all that there is to match is what it does. What is
written in Python it can see all the way into. It can replace a function in it, derive from a class in it and override any method, read what begins with an underscore, find its frames on the stack and its
lines in a traceback, and ask for its source. Programs do. So a module is written in the language that CPython wrote it in, function for function, and each file here that is a port says of what.

A class that is built in and belongs to a module is made when the module is first imported: `createBuiltinType()`. What the module has to keep for the realm, those classes among it, is a struct that the realm keeps for
it, as CPython keeps a module's state: `PyRealm::moduleState<T>()`, which makes one the first time that it is asked and tells the collector of what is in it. So a module is added without `PyRealm.h` hearing of it.

Where a class of CPython's has `PyObject_GenericGetAttr` written into its own `tp_getattro`, which does nothing that it would not have inherited, it has a `__getattribute__` of its own for a program to find:
`addGenericGetAttribute()`. It costs nothing, since `PyType::hooks()` knows it for `object`'s.

One file of the library CPython writes when it is built: what `sysconfig` is told about the build, `_sysconfigdata__darwin_darwin` or the like. It is Python there, so it is Python here (`lib/_sysconfigdata.py`), and comes with the
engine since it is about the engine. It says what is true of this implementation, which is little: nearly all of CPython's is about the C compiler. A host that has more to say lists one of its own, which is found first.

What is ported from CPython is CPython's authors' work made over, and is under CPython's licence, which is in `lib/importlib/LICENSE`. What matches regular expressions has a notice of its own, which is at the top of it, and so has the Mersenne Twister.

An instance of such a class has what in CPython is a C struct. Here it is a struct as well, derived from `NativeState`, and one kind of cell holds any of them: `PyStateObject`. So a class that is ported does not
take a cell type, a subspace and a destructor of its own, only what it has and what of that the collector is to be told of. Where CPython leaves making the instance to `object.__new__()` and its `tp_alloc`, so that
one can be had that `__init__()` has never been called on, the class says how one is made: `PyType::setAllocator()`.

What such a struct keeps that the collector did not allocate, it says how much of: `NativeState::memoryOutsideTheHeap()`. A `zlib.compressobj()` is a few words to the collector and a quarter of a megabyte to the process. In CPython it is freed when the
last reference to it goes. Here it is freed when the collector next runs, and how soon that is goes by how much the collector takes there to be, as it does for an `ArrayBuffer`.

**A module that a host writes may be over one that is here**, as `_ssl` is over `_socket`: it is given a socket, and reads and writes what the socket has open, waiting for as long as the socket says. In CPython `_ssl.c` includes `socketmodule.h`. So
`PythonSocket.h` is among the headers that a host can include, with what it includes in its turn, and so are `PyWeakReference.h`, `PythonIO.h` and `PythonTime.h`.

Besides that there is what the front end itself is easier said in Python for: `lib/_framelocals.py`, which is frozen, is imported when a realm is made, and is in no traceback. It is a class that CPython writes in C, and
is to be written in C++.

### What is put off

Some things come up when nothing can be run, and are to be run as soon as something can: the callback of a weak reference, which comes up in the middle of a collection, and what a program has for a signal. CPython has its
"eval breaker" for that, a word that running code looks at now and then. Here the word is the one that it looks at anyway, `VM::m_pythonLimitUnlessWatched`, which is 0 while there is something to see to, so
that being able to be asked costs code that is running nothing. `op_py_enter` and `op_py_line` then go the slow way, which is `doPendingWork()`.

**Signals are seen to first**, as by `_Py_HandlePending()`. What a handler raises is raised where the program is. If callbacks came first, the first function that one of them entered would find the signal, and
what the handler raised would be shown and forgotten with the callback half run: a wait that the signal was to end would go on. `interop/a-signal-and-a-callback-at-once.py` has both come up together.

### Where an event loop of Python's waits

`asyncio` is CPython's, as it is, event loop and all. It waits in one place, for what it is watching or until it has something to do: `select.kqueue.control()`, or on Linux `select.epoll.poll()`. What it watches with is a descriptor that can itself be
watched. So a host that has an event loop of its own is given that to watch, `Configuration::waitForDescriptor`, and goes on with its own meanwhile: JavaScript's timers and promises carry on while `asyncio.run()` does, and
what they run is inside the loop, as any callback is. `control()` looks without waiting before and after, and sees to signals each time round. With no such host it waits in the system, as it did.
Both go about it in the same way, so it is written once: `waitForEvents()`, which is given what looks.

Nothing else that waits does this. What calls `time.sleep()` means nothing to happen meanwhile.

### `Future` and `Task`

`_asyncio` is `Modules/_asynciomodule.c`, function for function: `PythonAsyncioModule.cpp`. `programs/asyncio-module.py` puts every method, getter and function of it to a loop that is a list, so that nothing depends on when
anything happens, and CPython's own `test_asyncio` runs each of its tests of futures and tasks against these and against the ones that are written in Python. What fails there fails for both, and is to do with when things
are destroyed, or with threads.

- CPython has the tasks that are its own on a list that does not keep them alive. Here they are in a `WeakSet`, where it has those that are not its own.
- `asyncio` imports `_asyncio`, which imports `asyncio`. So a module that is built in can have something left to do once it is in `sys.modules`, `BuiltinModule::execute`, which is `Py_mod_exec`. Without it there were two of the module.
- `__del__()` is there to be called, and says what CPython's says. Nothing calls it yet.

**A task waits for a promise.** `await promise` yields the promise to whatever is running the coroutine, and where CPython's `Task` says `Task got bad yield`, this one waits for it (`waitForPromise()`).

- It waits for a `Future` that stands for the promise, as it would for any. So `task.cancel()`, `wait_for()` and `timeout()` work, though a promise cannot be cancelled: the future is, and what the promise comes to is then nobody's business.
- JavaScript settles a promise when it settles it, which is likely to be while the loop is waiting for something to do. So the loop is told with `call_soon_threadsafe()`, which wakes it.
- What the promise came to is sent to the coroutine, and what it was rejected with is thrown in.
- **A promise can be rejected with anything.** What is no exception is not caught by `except`, and comes all the way out of the coroutine. It is then what the task ended with, as it is: `result()` throws it, and it is thrown into whatever
  awaits the task. Left pending, the task would keep whatever was waiting for it waiting for ever.

### What JavaScript waits for, and asyncio

**JavaScript can wait for a coroutine that uses asyncio, with no `asyncio.run()` anywhere.** `await app.main()` in JavaScript, where `main()` sleeps, gathers, makes tasks and times out. `PythonAsyncio.h`.

- **What JavaScript is running is in a loop.** `get_running_loop()` and the like, asked from inside a coroutine that JavaScript is waiting for (`JavaScriptStep`, in `resumeAwaitable()`), give the loop that is running. If none is, they give
  one of asyncio's own, made as `asyncio.run()` would make it, that nothing in Python runs (`hostedLoop()`). At the top of a program nothing is being waited for, so there is no loop, and `asyncio.run()` is as it ever was.
- **And what is running in a loop is a task.** It is a `Task` from when it first asks which loop it is in, or first yields a future (`adopt()`). It has got as far as it has got, so there is no first step to arrange. From then on it is
  the task that goes on with it, in the context that it has been running in, and the task settles JavaScript's promise as soon as it is done. Until then it is run as it always was, and one that has nothing to do with asyncio always is.
- A future is asked after before a thenable is. To JavaScript whatever can be awaited is a thenable, a future among them, but what its `then` does is to await it, which is what is being done already.
- **JavaScript is given a promise for a `Future`** that is settled when the future is done, with nothing to run (`promiseOfFuture()`).
- **A task is of a coroutine**, and `repr()` of one insists on it. So what JavaScript waits for that is not a coroutine is awaited by one from the start, `lib/_javascript_awaiting.py`, which is what `ensure_future()` does with it.
- **The host turns the loop, once round at a time** (`turnEventLoop()`), from its own event loop and with nothing of Python's on the stack. A turn is `run_forever()`. Where the loop would wait, in `kqueue.control()`, it does not: it looks
  without waiting, notes how long it would have waited and how much it is watching for, and calls `stop()`, so that `run_forever()` returns when what is ready has been run. The host is then told what to watch for
  (`Configuration::watchEventLoop`): the descriptor, and that long. So all of asyncio is as it is, and nothing of it that is not public is used.
- How long it would have waited was worked out before it ran anything, and what it ran may have given it more to do. It ran something only if something was ready, when it would not have waited at all, or if something had
  happened to what it is watching. In either case it is turned again at once.
- The loop is given something to do from outside a turn in two ways. By what JavaScript is running, and it is turned when that step is over. Or with `call_soon_threadsafe()`, which makes the descriptor readable.
- **The process stays** for a time that the loop is waiting for, and for the descriptor if the loop is watching for more than its own being woken, which is how much it watched for when it was made. Not for a future that
  nothing will ever settle, as it does not for such a promise.
- `kqueue.close()` tells the host to leave off first. The next descriptor to be opened may well have the same number.
- If a loop that Python is running is waiting, and so the host is going on with its own, the hosted loop is turned from there as it might be from another thread.
- A program can get hold of the loop and run it for itself, with `run_until_complete()`. The host leaves it alone for as long as that lasts, and is asked to turn it again when `_set_running_loop(None)` says that it is over.
- With no `watchEventLoop` there is no such loop, and a future that is yielded with no loop running is `RuntimeError: no running event loop`.

### A module can await

**`await`, `async for` and `async with` can be written at the top of a module**, and so can a comprehension that awaits. In CPython that is a `SyntaxError` unless `compile()` is given `PyCF_ALLOW_TOP_LEVEL_AWAIT`. Here what is compiled
as a module, or as what is typed at a prompt, is always given it (`compileSource()`). An expression, which is what `eval()` compiles, still has to ask. A module that awaits nothing is compiled as it ever was.

**What awaits is a coroutine when it is called, and it is run to its end before whoever ran it goes on**: `runToItsEnd()`. That is the program, and it is `exec()`, which gives nothing back, so that in CPython nothing would ever come of it.
`importlib` runs a module with `exec()`, so `import` returns when the module is done, whoever imports it: a module that awaits, one that does not, a function, or JavaScript. `eval()` of such code gives the coroutine to be awaited, as in CPython.

- It is begun as it would be if JavaScript awaited it, `toPromise()`, so all of "What JavaScript waits for, and asyncio" is so of it: it is in a loop with no `asyncio.run()` anywhere, and it is a task from when it first asks.
- **The host goes on with its own event loop until the promise is settled** (`Configuration::waitForPromise`). With no host, what is waiting to be run is run and no more.
- If the host has nothing left to stay for, what is awaited will never come, and that is `RuntimeError`.
- **An event loop that is going round cannot wait for one.** It goes round once at a time, and cannot go round again until what it is running has come back. So a module that awaits cannot be imported by what a task of asyncio's
  is running, once it is the loop that runs it, nor from inside `asyncio.run()`. That is `RuntimeError` too, as `require()` of a module that awaits is an error in JavaScript. What is imported before the first thing is awaited is not in that case.
- **`KeyboardInterrupt` is as `asyncio.run()` has it.** The first cancels what is being run, if it is a task, so that it can tidy up, and it is waited for still. If it ends in `CancelledError` that is `KeyboardInterrupt`.
  What gets out of the loop that the host turns is as a rule the host's to report. While something is waiting it is for that: `WaitingForHost`.

What is given with `-c`, and what is typed at the prompt, is run in the same way: `runModuleBody()`.

`interop/a-module-can-await.py` has what needs no event loop. What does is the host's to test.

### Signals

`PythonSignals.cpp` is `Modules/signalmodule.c`. As in CPython, what the system calls when a signal comes does next to nothing, and what the program has for the signal is called later, by Python code, between one thing and
another.

**What the system calls** is given the number of the signal and nothing else, on whatever thread the system likes, in the middle of whatever that thread is doing. So what it is to find is the process's, which nothing else here
is, and all that it does to it is load and store: which signals have come, the descriptor of `set_wakeup_fd()`, and which `VM` is to be told. What the program has for each signal is the realm's, where the collector finds it.

**Telling the `VM`** is `VM::notePythonSignal()`: it says that a signal has come, and then stores 0 in the word above. The thread that runs Python stores in that word too, whenever something that it goes by changes, and neither
waits for the other. So whoever stores there for any other reason looks afterwards whether a signal has come, and if so stores 0. A signal that had not said so by then has yet to store, and stores last. The 0 can also
be stored after the signal has been seen to, if something was looking already, and then what goes the slow way finds nothing to do and puts the word right. `signals-are-not-lost.py` goes round for ever if any of that is wrong.

**Whose the signals are.** In CPython it is the main interpreter that can say what is done about a signal. Here it is the first realm that is started, in whichever `VM`. In any other, `signal.signal()` raises what it raises in
CPython outside the main thread. When that realm goes, what it had set is set back, and it waits for any thread that is still in the middle of being told.

**What is up to the host.** Like `posix`, `_signal` is there if the host lists it. `Configuration::installsSignalHandlers` is `PyConfig.install_signal_handlers`: whether `SIGINT` raises `KeyboardInterrupt` from the start. That is for a
program that is Python's. One that is JavaScript's, and imports something, would find that it could no longer be interrupted, since it may never run Python again. `finalizeMain()` is the end of `Py_RunMain()`: if what
ended the program was a `KeyboardInterrupt` that nothing caught, the process is ended by `SIGINT`, so that a shell that started it knows.

What a program has for a signal can be a function of either language, and what it throws goes to whoever is next out, in either.

**A signal that is sent to the process** is given by the system to any thread that is not keeping it back. CPython has the one thread, unless the program starts more. Here there are the collector's and the compilers', and
the host's, of which a program knows nothing. It matters when the program keeps a signal back, to take it later with `sigwait()`, `sigpending()` or `sigwaitinfo()`: some other thread would be given it, and what is usually done
about a signal is to end the process. `programs/signals-for-the-process.py` and `command-line/ended-by-a-signal.py` are about that.

- The threads that WTF starts keep back every signal but those that tell a thread of a fault of its own: `Thread::establishHandle()`. So the system keeps the signal for the thread that Python runs in, along with who sent it.
- A host may have threads that do not. So what the system calls sends the signal on to Python's thread if it finds itself in another. And while a signal is kept back that the program has said nothing about, or has said is
  to be ignored, `keepForPythonThread()` is there to be called for it, which does the same. What the program said is done first, so that the system throws away what it would have thrown away, and is put back before the
  signal is let through. On macOS one that is ignored, or that nothing is done about as a rule, is not kept though it is kept back, so nothing is done for those.
- What is sent on comes from this process, so `sigwaitinfo()` says so. Linux lets a thread say who a signal is from only if it is sending it to itself.
- `os.kill()` of the process itself sends to the thread that calls it. CPython relies on the signal having come by the time that `kill()` returns, which POSIX promises only if no other thread will have it.

### Weak references

`weakref.ref` is a cell that has what it refers to and does not tell the collector, as `JSWeakObjectRef` is, and it lets go of it by the same means: when the collector has found what there is to keep, each
reference that it is keeping looks whether what it refers to is among that (`reconcileWeakReferencesAtGCEnd()`). If it is not and there is a callback, the reference goes on a list that begins in the `VM` and goes
from one to the next, which does keep them, and there is something to see to.

An object knows the references to it, so that `ref(x) is ref(x)`, and does not keep them. It has a `PyWeakReferenceList`, in a property that no program can name, in either language. That has the first of
them and each has the next, as in CPython, and the collector is told of none of it. What the collector is not keeping is dropped from the list at the same time as above, before anything is swept. So there is
no table of what refers to what, and nothing that a reference costs an object that has none.

There can be one to whatever CPython allows one to, and to an object of JavaScript's. An array is a `list` and an `Error` is an exception, and there cannot be one to those.

### `gc`

CPython counts references, and has a collector besides for what refers to itself. `gc` is how a program talks to that one. Here there is the one collector, JavaScriptCore's, for everything of both languages, and it is not a program's
to tune. So `PythonGCModule.cpp` is `Modules/gcmodule.c` as far as that goes:

- **`collect()` collects**, all of it, or only what is new if it is asked for a generation that is not the last. What is to be told that something has gone has been told by the time that it comes back, as in CPython, and so have
  `gc.callbacks`, before and after. It comes back with 0, since how many things there were that nothing could reach is not known.
- **What a program sets it gets back, and nothing goes by it**: `enable()` and `disable()`, `set_threshold()`, `set_debug()`, `freeze()`. `timeit` and a good many programs turn the collector off for a while, and lose nothing by its going on.
- **There is nothing in any generation**: `get_count()` is all zeros, `garbage` is empty, and `get_stats()` counts only how many times `collect()` has been called.
- **`is_tracked()` is what CPython comes to**, once it has collected: a number, a string or `bytes` is not, and a tuple is if anything in it is, however far down.
- **`get_objects()`, `get_referrers()` and `get_referents()` find nothing.** They are audited, and say what CPython says of what they are wrongly given, and come back with an empty list.

`programs/the-gc-module.py`

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

The names of characters, for `\N{...}` and `namereplace`, are `unicodedata`'s.

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
functions; `Constants` has the constants, and the names that `sysconf()` and the like go by. `Linux` has what there is only on Linux. It has everything that CPython's has on either but `fork()`, `forkpty()` and `register_at_fork()`: see *Where it differs*. `programs/posix-on-linux.py` lists what is in it,
and every number, and tries what is only on Linux, of `signal` and `time` as well.

**`_posixsubprocess`** (`PythonPosixSubprocess.cpp`), which is what `subprocess` starts a program with, does fork, though there is no `os.fork()`. What is wrong with forking is what is run afterwards, in a process that has this
thread and none of the engine's others. Here that is `child_exec()`, which CPython wrote to be run in the middle of anything: it asks for no memory, locks nothing, and runs none of the engine, and then the process is another
program. All that it goes by is worked out beforehand. The one thing that it cannot be asked to do is call `preexec_fn`, which is Python: see *Where it differs*.

### `math` and `_random`

`Modules/mathmodule.c` is in two parts here. What takes numbers of C's and gives numbers of C's, and the tables that go with it, is `PythonMathKernels.h`, which `lib/convert-math-kernels.py` makes from CPython's source without
changing an expression: the Lanczos approximation for `gamma()`, the sums in two and three doubles that `hypot()` and `sumprod()` are worked out in, `remainder()`, and what `isqrt()`, `factorial()` and `comb()` look up. What has
to do with objects is `PythonMathModule.cpp`, by hand. See *One rounding or two* for why the first part is taken as it is. The rest is the C library's, which is the one that CPython would be calling.

`PythonRandomModule.cpp` is `Modules/_randommodule.c`, and has the notice of the authors of the Mersenne Twister at the top of it. From the same seed come the same numbers as in CPython, from every function in `random.py`. To seed
`random` with a `str` or `bytes` takes `hashlib`, which is the host's.

### `struct`

`PythonStructModule.cpp` is `Modules/_struct.c`. There is a table for each order that bytes can be in, as there, with what packs and what unpacks for each character of a format. CPython has a function for each size of int in each
table. They differ in how they come by the bytes and not in what the bytes are, or in what is said of a number that there is no room for, so here there is one, which goes by the size in the table. Where the least byte comes first,
CPython puts what it does natively into the table for `<` wherever the sizes are the same, and that shows: `<F` says nothing of a number that a `float` of C's has no room for, and `>F` raises. The tables here are as that leaves them.

Floats are packed by `packFloat2()` and the rest, in `PythonNumbers.cpp`, which are `PyFloat_Pack2()` and the rest of `Objects/floatobject.c`, and are `memoryview`'s as well.

Turning what is given into numbers can run anything, so `pack_into()` packs into bytes of its own and copies them afterwards, if there is still room where they were to go: see *Where the bytes are is not kept*.

### `_abc`

`PythonABCModule.cpp` is `Modules/_abc.c`, which `abc.ABCMeta` is written over. CPython has the same in Python, `_py_abc.py`, for where there is no `_abc`, but it is not the same to a program. It is `_abc` that makes a sequence pattern
match what is derived from `collections.abc.Sequence`, or registered with it, and likewise for a mapping. And `typing` lets a protocol be asked about only by `abc` and `functools`, going by the name of the module that is asking, which
with `_py_abc` is `_py_abc`: `isinstance()` of a protocol that has attributes that are not methods then raises `TypeError`.

### `_operator`

`PythonOperatorModule.cpp` is `Modules/_operator.c`. `operator.py` has all of it in Python as well, for where there is no `_operator`, but that is not the same to a program. A function that is written in Python is a method of what
it is an attribute of, and one that is built in is not. `glob.py` has `concat_path = operator.add` in a class, so with the one in Python `Path.glob("**/*")` raised `TypeError`. And `operator.index()` in Python is
`a.__index__()`, which is not what `PyNumber_Index()` does with an int, or with what does not give one.

### `_functools`

`PythonFunctoolsModule.cpp` is `Modules/_functoolsmodule.c`: `partial`, `Placeholder`, `reduce()`, `cmp_to_key()`, and what `lru_cache()` makes.

What is in a cache that has a limit is in a ring, from what was wanted longest ago to what was wanted last. In CPython the pointers of the ring do not count, and each thing in it is counted once besides for being there, apart from
being in the dict. Here to be in the ring is to be kept: the links are looked at by the collector like anything else. So one that has been taken out of the dict by what was called, and is still in the ring, is still there.
The dict is one that no program sees, and it is asked with a hash that is known already, so that `__hash__()` is called once for each call, as in CPython.

`partial(p, ...)` is of what `p` is of only if `p` has no `__dict__` yet, and asking for that makes one. Here that is whether the object has any properties.

### `_heapq` and `_bisect`

`PythonHeapqModule.cpp` is `Modules/_heapqmodule.c` and `PythonBisectModule.cpp` is `Modules/_bisectmodule.c`. Both are gone about just as they are there, since which things are compared with which, and in what order, is
something that a program can see: after each comparison the list is looked at again, and how long it is; a heap of more than 2500 is made in another order than a smaller one; and what is of the same class as what is looked for is
compared by asking the class outright, until it has nothing to say. The two kinds of heap are one template.

`_heapq.__about__` is four thousand characters of prose, and is not all ASCII. It comes from CPython with the signatures and the docstrings, in a table of its own: `findModuleText()`.

### `cmath`

`Modules/cmathmodule.c` is in two parts here, as `mathmodule.c` is and for the same reason. What takes complex numbers of C's and gives complex numbers of C's, with the tables of what is given for infinities and NaNs, is
`PythonCMathKernels.h`, which `lib/convert-cmath-kernels.py` makes. `PythonCMathModule.cpp` is what has to do with objects. It is compiled with `#pragma STDC FP_CONTRACT ON` too: without that, ten of the functions differ from
CPython's in the last bit somewhere among the numbers that `cmath-module.py` tries. It is not compiled together with anything else, since the kernels have names of one letter for things.

### `resource`

`PythonResourceModule.cpp` is `Modules/resource.c`. Like `posix`, it is there if the host lists it: `createResourceModule()`. `resource.struct_rusage` is the class that `os.wait3()` and `os.wait4()` give one of, as in CPython, where
they import this to get it.

### `_symtable`

`PythonSymtableModule.cpp` is `Modules/symtablemodule.c`, and the class of what it gives. What every name refers to is found by what finds it for the compiler, `PythonSymbolTable.cpp`, which is `Python/symtable.c` with the same
flags, so there is nothing to do but hand it over. Of the 1869 files of CPython's `Lib`, with 156,627 blocks in them, it gives what CPython gives for every one.

The names in a block are in the order in which they were first seen. After them come those that something inside the block uses and the block does not, and those are what is in a set, in whatever order that gives them. It is not
the same from one run of CPython to the next, nor here.

**What a `SyntaxError` is made of depends on what found it.** The parser and the code generator make one of what is wrong and where. `symtable.c` and `future.c` make one of what is wrong, and then tell it where by setting its
attributes, so its `args` is the message alone, and so is what `repr()` shows. `FoundIn::WhatNamesReferTo` in `PythonCompiler.cpp` is those two.

### `_csv`

`PythonCSVModule.cpp` is `Modules/_csv.c`, which `csv` is written over. There is nothing in Python to take its place. What reads goes from one state to another a character at a time, just as there. A character is what Python
takes for one, so what JavaScript has as two halves is one. CPython goes through each field twice to write it, once to find how much room it wants and whether it is to be quoted; here what was written is thrown away if it turns
out that it was to be.

`Dialect()` takes its arguments apart by something that does not say what it is called, so what is wrong with them is said of "function", or of "this function". A signature says so with `?` for the name.

### `_socket`

`Modules/socketmodule.c`, which `socket` is written over, in four files and a header. `PythonSocketAddresses.cpp` has what raises, what waits, and addresses both ways, and `PythonSocketLinux.cpp` the kinds of address that there are only on Linux. `PythonSocketObject.cpp` is the class. `PythonSocketModule.cpp`
is the functions and what is in the module. It has to do with the world outside, so like `posix` it is one that the host lists.

**Everything that can wait goes through `callSocket()`**, which is `sock_call_ex()`: it waits with `poll()` if there is a time to wait no longer than, calls what makes the system call, and goes round again if that was interrupted
or turned out to have nothing to do. The time is counted from when it began, and not from when it was last interrupted. What sees to a signal runs between one try and the next, and can do anything, so what makes the system
call asks where the bytes are each time.

**The numbers are CPython's own lines.** `lib/convert-socket-constants.py` takes them out of `socket_exec()` with the conditions they are on, into `PythonSocketConstants.h`, and checks that it understood every line and lost
none. Most of the conditions are whether the system's headers define the thing, so each system gets what it has. On macOS what RFC 3542 added for IPv6 is only there if `__APPLE_USE_RFC_3542` is defined before any header is
read, so that file is compiled by itself.

**A function that takes its arguments apart with `PyArg_ParseTuple()`** has its own way of saying that there are too many or too few, and goes by its bare name or by its class's as well depending on how it was called.
`Arguments::AreCheckedAsByParseTuple` is that. Nearly all of this module is such, and few of them say what they take in a way that anything but a person can read, so a signature is written for each here.

What CPython does to find out whether `SOCK_CLOEXEC` and `accept4()` work is for Linux before 2.6.28. Where they are defined they are used.

- **There is no Bluetooth.** Its names are left out of the module, as they are from a CPython that was built without its headers. Every other kind of address that CPython has on macOS or on Linux can be read and written.
- **What a kernel has of the kinds that are Linux's depends on how it was built, and on who asks.** So `programs/socket-on-linux.py` tries how an address is taken apart with a socket that only says that it is of the family, and
  what the kernel does with the real thing it looks at for itself, and is content if there is none. Where it was written there were `AF_NETLINK`, `AF_VSOCK`, `AF_TIPC`, `AF_CAN` and `AF_ALG`. `AF_PACKET` takes root, and was tried by hand.
- **`bind()`, `listen()`, `_accept()` and `connect()` have only been seen to fail.** Where this was written nothing is allowed to bind a name or a port, in CPython either. All that can be tried between the two ends of a
  `socketpair()` has been, and everything that is said of an address that will not do. `programs/socket-module.py` is that.
- Nothing is done in another thread, so looking up a name stops everything until it is done.

### `array`

`PythonArrayModule.cpp` is `Modules/arraymodule.c`. An array is one kind of cell whatever it is an array of, with a table of what is done for each kind of item, as in CPython. Its items are in a `Uint8Array` of its own: see
*Where the bytes are is not kept*. How many CPython would have room for is kept count of, by its own arithmetic, since `__sizeof__()` shows it.

An array of characters says that its items are `w`, which a `memoryview` knows nothing of. It can be looked through and cut up, and cast to bytes, but not looked into.

### `binascii`

`PythonBinasciiModule.cpp` is `Modules/binascii.c`. The tables for the two checksums are worked out from their polynomials when it is compiled. `b2a_qp()` goes through what it is given once, where CPython goes through it twice, the
first time to find how long the result will be.

### `_md5`, `_sha1`, `_sha2`, `_sha3` and `_blake2`

CPython has each hash twice over. `_hashlib` is over OpenSSL, and is what `hashlib` goes by if it is there. These five need nothing but themselves, and are what it goes by otherwise, and for what OpenSSL will not do: BLAKE2 with a key, a salt or
a place in a tree. It is the same here. A library like OpenSSL is something that a host may have and the engine has not, so `_hashlib` is up to the host, and these are the engine's. With them `hashlib`, `hmac`, `uuid.uuid3()` and
`uuid5()`, and `random.seed()` of a string, all of which are written in Python, work as they are.

- **`PythonHashAlgorithms.cpp` is the algorithms and knows nothing of Python or of JavaScriptCore**: each is written from what defines it (RFC 1321, FIPS 180-4, FIPS 202, RFC 7693), where CPython has HACL*, which is generated from a proof. So it can
  be compiled by itself and given a great deal more than a test has time for.
- **A step is written once and the compiler writes it out for each** (`unrolled<>()`), so that which word a step takes and how far it turns it are part of the instruction. MD5 is two and a half times as quick for it. SHA-2 keeps the last
  sixteen words of its schedule and no more, which there are registers for, and SHA-3 takes in a word at a time. Each is then between 0.9 and 1.6 times as quick as CPython's, on ARM64.
- **`PythonHashModules.cpp` is the five modules**, which are as alike as they look: one kind of object, that has any one of the algorithms.
- What is said of arguments that are wrong follows CPython's, down to all six classes of `_sha3` saying that they are `sha3_224()`.

`programs/the-hash-modules.py`, `programs/hashing-with-the-library.py`

### `select`

`PythonSelectModule.cpp` is `Modules/selectmodule.c`: `select()`, `poll`, `kevent` and `kqueue` where there are those, and `epoll` where there is that. Each of them begins again when
a signal has interrupted it and been seen to, for as long as is left. The fields of a `kevent` can be set, and are fields of a C struct, so what is stored is cut down to fit as C would, with a warning: `PythonStructMember.cpp` is
`PyMember_SetOne()`, for whatever else has such fields.

They stop the thread, as `time.sleep()` does, but for the two that `asyncio` waits in: see *Where an event loop of Python's waits*.

### `fcntl` and `termios`

`PythonFcntlModule.cpp` and `PythonTermiosModule.cpp` are `Modules/fcntlmodule.c` and `Modules/termios.c`, and are for the host to list, as `posix` is. `tty` and `pty` are written over them, and `getpass`, `mailbox` and `asyncio` want one or the other. The
constants of each are CPython's own list, each on the condition that CPython has it on: `lib/convert-fcntl-constants.py`, `lib/convert-termios-constants.py`. What `fcntl()` and `ioctl()` give the system to write in is a copy with something after it that
shows if the system has written more than it was given room for, as in CPython.

`programs/fcntl-module.py` and `termios-module.py` are what can be tried with no terminal. `a-terminal.py` has the two ends of a pseudo-terminal: what goes through it a line at a time and raw, what is rubbed out, what is shown as it is typed, how big it is. What is
expected of that was taken on Linux only.

### `pwd` and `grp`

`PythonUserDatabaseModules.cpp` is `Modules/pwdmodule.c` and `Modules/grpmodule.c`, which are as alike as what they are about, and are for the host to list, as `posix` is. `os.path.expanduser("~someone")`, `getpass.getuser()`, `Path.owner()` and
`shutil.chown()` by name come down to them. `programs/the-user-database.py` says nothing of anyone but root, since who there is differs from one machine to the next. For the same reason they are not among the modules of `audits/native-modules.py`,
whose reference has what everything returns: run by hand with only these two, none of its 3,328 lines differs.

### `syslog`

`PythonSyslogModule.cpp` is `Modules/syslogmodule.c`, and is for the host to list, as `posix` is. There is one log to a process, so what is kept of it is kept once, behind a lock. `openlog()` in C keeps hold of the name that it is given and makes no copy, so that
is kept for as long as it may be looked at. `LOG_MASK()` and `LOG_UPTO()` are macros that shift by whatever they are given, and C says nothing of shifting by less than nothing or by more than there is room for. What comes of it where CPython is compiled is
what comes of it here, but for `LOG_MASK(-1)`, which one compiler makes an error with no exception of.

What is sent to the log is not to be had back. `programs/the-syslog-module.py` goes by what the hooks of `sys.addaudithook()` are told, and by what `LOG_PERROR` has written to the standard error as well.

### The codecs for Chinese, Japanese and Korean

`Modules/cjkcodecs` of CPython is 23,000 lines. 17,000 are tables. 3,200 are the codecs, which are written with the macros of `cjkcodecs.h` and have to do with Python in two places: where a character is read and where one is written.
**`lib/convert-cjk-codecs.py` converts both**, as other scripts there do the tables of `unicodedata` and the engine of `re`. It changes what C allows and C++ does not, each thing so many times and no other, so that it is noticed if CPython comes to
be written otherwise. `PythonCJKCodecs.h` has the macros, and `PythonCJKCodecs*.cpp` and `PythonCJKMappings*.h` are what is made, which is not to be changed by hand. Each is compiled by itself, since each says with macros what it wants of that header.

A codec reads a `CodePoints` and writes to a `TextWriter`, as those of `PythonCodecs.h` do. `PythonMultibyteCodecModule.cpp` is `multibytecodec.c`, which runs them, and is written by hand.

One of these modules wants tables that another has, and comes by them as in CPython: it imports the module and takes what is there by the name, which is a capsule. So a capsule can have a pointer in it (`capsulePointer()`).

`programs/the-cjk-codecs.py` puts every character through every codec and every two bytes back, cuts what each makes of a sample everywhere, and has handlers of errors do what they should not.

### `mmap`

`PythonMmapModule.cpp` is `Modules/mmapmodule.c`, and is for the host to list, as `posix` is. What is mapped is an `ArrayBuffer`'s, which is how anything here has bytes to show: `NativeState::exportedBytes()`. So a `memoryview` of it is of the pages themselves, the
collector counts them as it does any `ArrayBuffer`'s, and they are unmapped when the `ArrayBuffer` is detached or freed. `mremap()` may put them somewhere else, and an `ArrayBuffer`'s are where they are, so `resize()` makes another.

Three things were wanted of the engine. What is shown may be not to be written to (`ExportedBytes::isReadOnly`): `ACCESS_READ` maps pages that cannot be. What has bytes to show may refuse when it is asked for them (`NativeState::willExportBytes()`), as `bf_getbuffer` can: one that is closed does.
And a class may have an `sq_item` besides its `mp_subscript` that does not come to the same (`PyType::HasSequenceItemOfItsOwn`, `NativeState::sequenceItem()`): `m[0]` is an int, and going through `m` gives a `bytes` of each.

**CPython counts who is looking, and will not `close()` or `resize()` while anybody is.** Nobody is counted here, of this or of a `bytearray`, since when a `memoryview` is done with is for the collector to find. To close it is to detach the `ArrayBuffer`, and then whatever was looking has
nothing to look at. Nothing keeps where the bytes are: see `Python::Buffer`.

### `_lsprof`, and so `cProfile`

`PythonLsprofModule.cpp` is `Modules/_lsprof.c`. It is told of calls and returns by `sys.monitoring`, which it asks as a program would, so there is nothing for it in the engine.

- **What an entry is found by** is where a code object is, or a `PyMethodDef`, which every `l.append` has in common. Here that is the `PyNativeFunction`, which is what a bound one is bound from.
- **What it keeps is kept in a list**, the code objects and what entries are found by, since the collector goes through a `NativeState` while the program is running and is to be told of nothing that is in what grows. What is counted is beside it, in C++.
- **`getstats()` gives them in the order in which they came.** CPython has them in a tree that goes by where things are in memory, and gives them in the order of the tree.
- It goes by `m_self` and not by `__self__`, and what a static method has there is its class: `{built-in method maketrans}`.

`programs/profiling-with-cprofile.py` gives it a clock that goes on by one each time that it is read. So how long everything is said to have taken is a matter of what the profiler was told of and in what order, and is compared to the last digit.

### `time`

`PythonTimeModule.cpp` is `Modules/timemodule.c`, and `PythonTime.cpp` what it wants of `Python/pytime.c`: a time is a number of nanoseconds, and how a float or an int is made one, rounded which way, and what is said if
there is no room for it, are all CPython's. The clocks are the ones that CPython reads on the same system, so `get_clock_info()` says the same.

`strftime()` goes by the locale of the realm and not that of the process, which is left as it is: see *Regular expressions*. `tzset()` is the C library's, and there the time zone is the process's. JavaScript's `Date` has its own
idea of the time zone, and is not told.

`sleep()` stops the thread, and so whatever else the thread would have been doing, in either language. That is what it means.

### Regular expressions

A regular expression is compiled by Python, in the package `re`, to a list of numbers. `_sre` is what goes by them.

- **`PythonSREEngine.h` does the matching.** It is `Modules/_sre/sre_lib.h`, which is a machine with a stack of its own for going back, all labels and `goto`. There is nothing of Python's objects in it. It is not written out again
  by hand, since what would go wrong in doing so is what no test is sure to find: `lib/convert-sre-engine.py` makes it from CPython's, changing what C++ requires and nothing else. CPython includes the file three times, once for
  each width of character, and here it is a template. It keeps CPython's names, so that the two can be laid side by side.
- **`PythonSRE.cpp` is the rest**, `Modules/_sre/sre.c`: `Pattern`, `Match`, the scanner, what a replacement is compiled to, and what looks over the numbers before they are trusted, since a program can call `_sre.compile()` itself.
- **A `str` is gone through as it is** if a character of it is a code unit, which is nearly always. Otherwise see `ExpandedString`, under *Characters and code units*.
- **Bytes are gone through where they are**, without a copy, and any of JavaScript's typed arrays and `ArrayBuffer`s will do. The engine keeps pointers, which nothing else here does: see *Where the bytes are is not kept*. They are good for as
  long as nothing of a program's is run. That is so within one run of the engine, but for what has been put off, so after that has been seen to it looks whether the bytes are where they were, and gives up if not. Between one run and
  the next, in `sub()` with a function and in `finditer()`, anything may have happened, so `State::findBytesAgain()` is called before each run, and what is cut out of the bytes is cut from what there is now.
  `interop/regular-expressions-over-bytes-that-change.mjs` empties, resizes and gives away what is being gone through.
- **`re.LOCALE`** asks the C library, of a locale that belongs to the realm: `characterLocale()`. The locale of the process is not Python's alone and is left as it is.

`programs/sre-at-random.py` makes regular expressions at random and tries them on strings made at random, of each width and of bytes. Both this and CPython are given the same numbers to go by, so what differs is the engine's doing.

### `_opcode`, `opcode` and `dis`

`co_code` is not made of CPython's instructions: see *Code objects*. So `dis.dis()` has nothing to take apart. But `opcode.py` and `dis.py` are imported by a good deal of the library that never comes to use them, `inspect` and
`traceback` among it, and `_opcode_metadata.py`, which is Python and is as it is, has the numbers of the instructions. `_opcode` says about those numbers what CPython says: `PythonOpcodeMetadata.h`, which
`lib/convert-opcode-metadata.py` makes from CPython's headers.

### `tokenize`

CPython has one tokenizer. Its parser asks it for tokens, and so does the module `_tokenize`, which `tokenize` is written in Python over. It is the same here: `TokenStream`, in `PythonLexer.h`, is the scanner that the compiler
has, asked for one token at a time by whoever has the source a line at a time. So what `tokenize` says of a program is what the compiler makes of it, and every file that is put through `tokenize` and compared with
CPython is a test of the compiler's scanner.

- **No more is read than it takes to make out the next token.** What has the source at a prompt, or from a generator, sees the tokens of a line before it is asked for the next. When the scanner comes to the end of what it has
  it asks for more: `Lexer::readMore()`, which is CPython's `tok_underflow_readline()`. For the compiler, which has all of the source, that is a test for null where it was already at the end.
- **A line is whatever was given for one**, however many newlines are in it, and lines are counted by how many times it has asked. Newlines are not made alike first, as they are for the compiler, so a carriage return by itself
  ends nothing.
- **What a token comes to is not worked out**: not the value of a number, nor of a string, nor which names are keywords. What is wrong with an escape is for the parser to say.
- **With `extra_tokens`** there are comments and the ends of lines that mean nothing, and less is found fault with, so that what is not yet a program can be coloured in as it is typed.
- **Each token comes with what the tokenizer knew on giving it**: `StreamedToken`. `PythonTokenizeModule.cpp` is `Python/Python-tokenize.c` over that, with what it does to keep from making the line again for each token, since
  which line a token is said to be on comes of it.
- **Where CPython's tokenizer raises nothing** and only says that it can go no further, the parser has one thing to say about it and `_tokenize` another: `SyntaxError::Stop`.

`programs/tokenize-module.py` has what is written by hand, in what order lines are asked for and tokens given, and a program that is spoiled at random. All of CPython's `Lib`, 1869 files of which some are wrong on purpose, comes out
the same both ways of asking and both ways of giving the lines. That is not among the tests, since it is to be compared with the CPython that the files came with.

### One rounding or two

`a * b + c` is one instruction on some processors, and it rounds once where a multiplication and then an addition round twice. JavaScriptCore is built with `-ffp-contract=off`, so that it is never used unasked, as
JavaScript's arithmetic requires. What CPython is built with does use it, for what is written in one expression, so `_Py_c_quot()` and `st_mtime` come out one way on ARM64 and another on x86-64, in the last digit.
A program can see that, and one that keeps a time to compare it with later does. So where CPython has such an expression it is written `multiplyAdd()` here, which is the one instruction where CPython's would be.

That will do for an expression here and there. `math` is made of them, and which of two products in a sum is taken with the addition is for the compiler to say. So `PythonMathModule.cpp` is compiled as CPython is, from a
`#pragma STDC FP_CONTRACT ON` at the top of it to an `OFF` at the end, and what is written in it is written as CPython writes it, expression for expression. It goes by how a sum is written and not by what the optimizer
makes of it, so the same source is rounded the same way. With the pragma taken out, `math-module.py` fails for `gamma()`, `lgamma()` and the logarithm of a large int.

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
would in JavaScript. `import js` is the global object, and a module is a module: see below.

- **Whether a property can be set or deleted is for JavaScript to say.** A setter is run, a `Proxy` is asked, and what is frozen stays as it was. What it will not do is `AttributeError`, which is
  what a dataclass that is frozen raises, and what `importlib` and the like expect of what will not take an attribute. Deleting what is not there is `AttributeError` too, though JavaScript
  is content to. `interop/setting-what-javascript-guards.mjs` does each to some forty kinds of object.
- **`obj.__dict__` is what `Object.entries()` would give, as it is now, and cannot be changed.** It is a `mappingproxy`, as the `__dict__` of a class is. A dict that kept up would have to run getters and
  traps from the middle of whatever a dict does.

- `obj.f(x)` passes `obj` as `this`.
- **A function that is got from what an object inherits from is bound to the object**, as one got from a class is in Python. One that the
  object has of its own is as it is, as one in an instance's `__dict__` is. So `js.Math.floor` and `js.Array` are themselves.
- **Calling a class makes an instance**, Python having no `new`. That is asked only where there was nothing left to do but throw
  (`callConstructorWithoutNew`), so no call that works pays for it. What can be both called and constructed with is called, and `.new()` constructs.
- **What a class of Python's says that is derived from one of JavaScript's, Python goes by and JavaScript does not.** An instance of `class Counter(js.Map)` is a `Map`, made by `Map`, and a `Map` is not asked what `+` does with it or whether
  it can be called. Python asks the class, so `counter + 1` and `counter()` are `__add__()` and `__call__()` there. Calling it is asked only where there was nothing left to do but raise: `callWhatOnlyPythonCalls()`.
- **Neither language is to leave to the other what the other leaves to it.** Such an instance inherits from classes of both. `js.Object` has `__str__()`, `__iter__()`, `__len__()` and the like, which ask JavaScript. And a class of Python's, being among
  what the instance inherits from, offers JavaScript `Symbol.toPrimitive`, `Symbol.iterator`, `length` and the like, which ask Python. With nothing more said, `print(counter)` went round for ever. So what Python offers JavaScript it
  offers only if it has something of its own to go by: if the first class to have the special method, in the order of resolution, is not one of JavaScript's (`has()`, in `getPropertyForJavaScript()`). Otherwise JavaScript looks
  further and finds what `Map`, `Array` or `Date` has. `Symbol.toPrimitive` stands for several methods at once, so it is there that it hands on what it has nothing for.
- **What such an instance has of its own is what it was given as an attribute, and then what JavaScript gave it**: how long an `Array` is, the message of an `Error`, what was defined on it with a getter. Those are found, set and deleted as
  JavaScript does with what is an object's *own*. What it inherits is for the classes, in Python's order, and JavaScript is not asked, since it would ask the class. Its `__dict__` is a dict that can be changed, as that of any instance of a class of
  Python's is. An `Error` is no exception to this, though one that JavaScript makes for a class that is built in, as `new TypeError()`, is an object of JavaScript's altogether.
- **What is set on a class of JavaScript's is a property of its prototype, and is as what is written in a class there is**: it does not show in `for (key in instance)`. `copy.copy()` sets `__slotnames__` on the class of whatever it is given.
  A function of Python's that is set on one is a method: got by way of an instance it takes the instance as its first argument.

`interop/javascript-classes.py` has all of that.

- JavaScript's methods are not attributes of a `list` or a `str`. `hasattr(x, "keys")` is how Python tells a mapping.
- **A `list` is any `Array`, and JavaScript can make some that Python cannot.** A hole is `None`. An element that is not simply there to be read or written
  is read and written as JavaScript would in strict code: a getter is run, and an array that is frozen raises `TypeError` and stays as it was
  (`listGet`, `listSet`). The strings that a tag function is given are such an array.
- What a `t"..."` has, `strings` and `values`, is what a tag function is given, so `tag(t.strings, ...t.values)` needs nothing from either side. And a
  function of Python's can be a tag.

### A module is a module, whichever language it is in

**A module of JavaScript's is, to Python, its namespace object, as it is.** That is an instance of `module`, as an `Array` is a `list`: `typeOf()` says so. It is the same object that `import * as ns` gives JavaScript,
so it is the same from either side, and nothing stands for it.

- **What it exports are its attributes**, as they are at the time: `export let count` is seen to change. They are JavaScript's to set, so setting or deleting one is `AttributeError`.
- **What has not been given a value yet is not there yet**, as in a module of Python's that is half way through being imported: `hasattr()` is false, and `dir()` and `from m import *` leave it out. JavaScript throws a
  `ReferenceError` for such a thing. `JSModuleNamespaceObject::isInitializedExport()` asks without.
- **What Python sets on it besides is kept in it, out of JavaScript's sight**: `__name__`, `__spec__`, `__path__`, and the modules below it. They are properties of it like the attributes of any object of Python's. JavaScript
  finds nothing in a namespace object but what is exported and what is keyed by a symbol, so it does not find them, by any means. `[[OwnPropertyKeys]]` used to list them, going by what was in the object where the
  specification says symbols, and no longer does. `vars(m)` is those, and not what is exported.
- `from m import *` leaves out `default`, as `export * from` does.
- **Calling it calls what it exports by default.** Python has no way of writing `import express from "express"`. What it writes is `import express`, and then `express()`. A namespace object cannot be called, so
  there is nothing else that that can mean, and it is asked only where there was nothing left to do but raise (`callWhatOnlyPythonCalls()`), as with a class that is called without `new`. To JavaScript it is still not a function.

`interop/a-module-is-a-module.mjs` looks at one from both sides.

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

**Where it is is not found out until something may ask.** An `Error` goes through the stack when it is made, and what wraps whatever is thrown (`JSC::Exception`) goes through it again. Only making that into a string is put off. Code in Python makes nothing of
either, and raises and catches exceptions as a matter of course, so for what it makes and what it throws (`VM::isPythonCodeRunning()`) both are put off: `ErrorInstance::StackCapture::Pending`, `Exception::StackCaptureAction::CaptureStackWhenItIsSeen`.

- **It is done when the exception comes to code in JavaScript, or is on its way out to whatever is written in C++ and called all this** (`UnwindFunctor::captureStacksThatArePending()`). That is while it is unwinding, when nothing has been done yet to the
  frames that it has been through, so what is found is what would have been found where it was raised. Code in JavaScript that has been inlined in code in Python counts, both here and in telling who is running.
- **If it had been caught before that and thrown again**, by a `finally` or a `raise`, where it had been until then is gone. That is what its traceback had in it when the unwinding began, and it goes first. There was nothing but code in Python there, or
  it would have been seen to already.
- **If it never leaves code in Python, and is handed to JavaScript**, what it says is what its traceback says, which goes no further out than what caught it. One that was never raised says where it was when it was asked.
- What JavaScript makes or throws is as it always was.
- **It is kept as it always was.** The frames are referred to weakly, and are made into strings at the end of a collection in which any of them goes (`reconcileWeakReferencesAtGCEnd()`), whenever it was that they were found. That goes through every
  `ErrorInstance` there is and asks nothing of how it was made. Until they are found there is nothing to keep.

`interop/where-an-exception-has-been.py`

**Nor is where it was in the source.** Each frame that an exception comes to is remembered in its traceback, with how far the frame had got. What line that is takes some working out, and is worked out when it is asked for (`lineOf()`). Until then the entry has
the `UnlinkedCodeBlock` to find it in, which is kept in any case by what the function of the frame is an instance of.

| Python | JavaScript |
|---|---|
| `BaseException` | `Error` |
| `TypeError` | `TypeError` |
| `SyntaxError` | `SyntaxError` |
| `NameError` | `ReferenceError` |
| `ValueError`; `RecursionError` and `MemoryError` for those two | `RangeError` |

Each is an instance of the other's class, whichever language made it. `name`, `message`, `cause` and `stack` are to JavaScript what they are for any
`Error`, being not enumerable, and `AttributeError.name` is Python's. One that JavaScript made is an object of JavaScript's whatever its class, so Python finds those on it, as it finds any property of it. What JavaScript throws that is no `Error` passes through `except`, though not
through a bare one or `finally`.

An `Error` of a kind that is not in the table is a `JSError` to Python, which is derived from `Exception`. Calling `JSError` makes an `Error`, and it is JavaScript that makes it, as it is for any class of JavaScript's.

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

- **There is not one `bytes` with nothing in it**, as there is one empty tuple. `b"" is b""` is false, and so is `x[5:5] is x[6:6]`. JavaScript can give away the buffer of a `bytes`, and if it did so with that one it would have done
  so with every empty `bytes` there is. Otherwise what CPython gives back as it is, is given back as it is: `t[:]`, `t * 1`, `t + ()`, `b.strip()` with nothing to strip, `frozenset(f)`, `format(s)`. `the-same-object-or-another.py`
  goes through them.
- **A `bytearray` can be resized while there is a `memoryview` of it, or while a regular expression is going through it.** CPython raises `BufferError`, and can because the view is released the
  moment the last reference to it goes. Here it would stay locked until the next collection, and programs that are right would fail. A view
  holds no pointer, only where it is looking, and checks each time. For the same reason the `__release_buffer__()` of a class that has one is called when
  the last `memoryview` of it is released, by `release()` or by `with`, and not when it is merely let go of.
- **`tokenize` takes a t-string whose format specification goes over more than one line.** CPython 3.14.7 raises `MemoryError`, though it compiles it. And once `_tokenize.TokenizerIter` has raised something it is over, where
  CPython's goes on from wherever it had got to.
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
- **An `array` can be resized while there is a `memoryview` of it**, as a `bytearray` can, where CPython raises `BufferError`. And one that is made shorter and then longer again has zeros where CPython has whatever was left there.
- **What a socket is receiving into or sending from can be resized meanwhile**, by what sees to a signal. No more is put there than there is then room for, and no more is sent than is then there.
- **What `struct` is packing into or unpacking from can be resized meanwhile**, as what a regular expression is going through can. If it has been made too short by the time that there is something to write or to read, that is
  `BufferError`.
- **A NaN has 50 bits to be told from another by, and not 51.** The engine has other uses for some of what would be NaNs, and JavaScript never sees those, because every number that it reads out of memory is made the one NaN
  first. Python can tell NaNs apart, with `struct`, `marshal` and `memoryview`, so `floatFromDouble()` keeps the sign and all of the rest but bit 50: `purifyNaNKeepingPayload()`, in `runtime/PureNaN.h`, which says why it is that bit.
  Without it, eight bytes of a program's choosing would be a pointer of its choosing.
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
- **A module can await.** `compile("await f()", name, "exec")` is a `SyntaxError` in CPython. See "A module can await".
- **There is no `os.fork()`.** What it would leave in the new process is the one thread, and the collector and the compilers have threads of their own, which would be waited for and never answer. `os` has none on the
  systems where it cannot be had, so a program that can do without looks first, as the library does. `posix_spawn()`, `exec*()` and `system()` are there.
- **`subprocess` does not take `preexec_fn`**, for the same reason: it would be called in the new process. It raises `RuntimeError`. `start_new_session`, `process_group`, `user`, `group`, `extra_groups` and `umask` are there for most of
  what it was used for.
- **`os.closerange()` closes what is open**, which it finds out, where CPython on macOS tries every number in the range.
- **What is written to `sys.stdout` is sent on at once**, as it is by CPython with `-u`, whether or not it is a terminal. Otherwise what Python writes would come out after what JavaScript wrote later. So `sys.stdout.buffer`
  is a `FileIO`, and `write_through` is true. If it is to be kept back so as not to ask the system so often, it is for the host to keep back, in `FileOperations::write`, along with what JavaScript writes.
  `Configuration::buffersStandardStreams` is for a host that would sooner have it as CPython has it.
- **Text is UTF-8 unless it is said to be something else**, whatever the locale: CPython's "UTF-8 mode", which is how it is to be from 3.15 on.
- **`sys.flags.hash_randomization` is 0**, which is so: the hash of a `str` is what it is to JavaScript, and is the same each time that a program is run. `PYTHONHASHSEED` is gone by all the same by what has hashing of its own to do: `PyRealm::hashSecret()`
  is `_Py_HashSecret`, byte for byte, for any seed. `pyexpat` gives Expat sixteen bytes of it.

## Tests

| | |
|---|---|
| `JSTests/python/run-programs.sh <jsc>` | programs whose output is CPython's, byte for byte, each in eight configurations of the engine, and once by way of its syntax tree |
| `JSTests/python/run-interop.sh <jsc>` | the two languages together. There is nothing to compare these with: what is expected was read and found right |
| `JSTests/python/audits/run-audits.sh <jsc> [n]` | not tests but measures of how far there is to go, over everything that is built in |
| `JSTests/python/parser.js`, `symbol-table.js` | the first stages by themselves |

All but the last need to be told where the part of the library that is written in Python is: `PYTHONPATH` is to name the `Lib` directory of CPython 3.14.

**They are to be run with the engine's assertions on as well**: `-DENABLE_ASSERTS=ON`, with everything else as it is for a release, which is quick enough to run all of it. The first time that was done half of the programs stopped at one. Some of what was found
was wrong and had gone unnoticed:

- What is written in C++ has `args[i]` for an argument that there has to be, and it was taken from where it would be had it been given by position. Nineteen functions have one that can be given by name, so `binascii.b2a_hex(sep=":", data=b"ab")` took the separator for
  the data. It is looked for by name now if it was not given by position. `programs/arguments-by-name-in-any-order.py`
- `x << 0`, of an `x` that is a `JSBigInt`, looked at the first digit of a zero, which has none.
- What `functools` marks the keywords in a key with was a cell of one class with the `Structure` of another.
- An instance of a class derived from `_thread.RLock` that has `__call__()` could not be called from JavaScript.
- `continue` in `while 0:` went to a label that was nowhere.
- **There is no such thing as a property that has nothing.** What an object keeps under a name of its own was given the empty value for it to be kept no longer. `putDirectOrRemove()`

And three of JavaScriptCore's own say something else now, for what only Python does. A `Structure` may leave `OverridesGetCallData` out though its class has `getCallData()`: a tuple can be called only if it is of a class that a program has derived, and it is the
`Structure`s of those that say so. A node of which two things come may be one that has to be generated (`PyLoadMethod`), which counts for one more use of it. And a function may have no source at all, being a module with nothing in it.

**A test of what is run often looks at what comes of every run**, and not of the first and the last. What goes wrong on leaving compiled code goes wrong on the run after it was compiled, and it is compiled again less and less often. With assertions on, a
register that is taken not to be needed on leaving is given something that can be mistaken for nothing else (`--poisonDeadOSRExitVariables`), where otherwise it is given `undefined`, which is `None`, and a program may well go on with that.

**What is expected is what CPython 3.14.7 prints**, as it comes, on ARM64: `x.expected` on macOS. Where it prints something else on Linux, that is in `x.linux.expected`, which is what is gone by there. There are twenty or so:
the system has other things in it, calls errors by other names, and its C library works some sums out to a different last digit. A program that is about what only one system has has only the one, and is passed over on
the other. Nothing that is expected may depend on the machine: no name of a directory, no number of a process.

- CPython is to have its modules built in, as they are here, or they say otherwise of themselves: `MODULE_BUILDTYPE=static ./configure --disable-test-modules`.
- It is to be built with clang, as this is. On ARM64 it is up to the compiler whether `a * b + c` is rounded once or twice, and GCC does not choose as clang does.
- It is run with `PYTHONUTF8=1 PYTHONHASHSEED=0 PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1`.

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

What a host does about signals:

- **It is Python code that looks whether a signal has come.** JavaScript that is going round does not, and neither does an event loop that is waiting. So what a program has for a signal is not called until Python is next run.
  `asyncio` has `set_wakeup_fd()` for that. A host with a loop of its own wants to be woken as well, and then to call `checkSignals()`.
- **What is done about a signal is set with `sigaction()`, over whatever the host had set.** A host that lets JavaScript listen for signals wants the two to know of each other.
- **On Linux the engine stops threads for the collector with a signal**, which a program could set something else for.

What follows from the second:

- **A file that is let go of without being closed is not closed, and what has been written to it and not yet sent on is lost.** `open(p, "w").write(s)` is written a great deal, and in CPython it works, because the
  file is finalized as the statement ends. `__del__()` is there to be called, and does what it does in CPython. Nothing calls it.
- What a weak reference refers to is gone when the collector finds that it is, and not when the last reference to it goes, and the callback is called some time after that.
- A socket that is let go of without being closed is not closed either.
- What is warned of when something is let go of while it is open (`ResourceWarning`) is not.
