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
them out, and it means each stage can be compared with CPython's by itself.

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
- `co_consts` is what is written out in the source and the code of what is defined in it, as they are first come to. It is not CPython's to the last item: it has no tuples that
  were folded, and has the small ints that CPython has an instruction for.
- What is never come to, `if 0:` and `while 0:` and what `__debug__` rules out, is compiled, since that is how it is found what is wrong with it, and jumped over. It has no lines and no constants.

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

### Modules, and what is up to the host

`import` finds a module in `sys.modules`, among those that are written in C++, or in a file on `sys.path`. (The last is to be `importlib`, which
is written in Python.) An ES module can import a Python file: it is a synthetic module record, made when it is needed as for CommonJS, that
exports each global by name and the module as the default.

What is up to the host is asked of it as JavaScript asks it, through `GlobalObjectMethodTable`: `configurePython` for `sys.argv`, `sys.path` and the
like, and `createPythonBuiltinModule` for modules that only the host can provide. `posix` is one, and everything that touches a file goes
through it, `import` and `print()` included.

### Where the library is written

Data structures and what the language itself needs, in C++. What is easier said in Python, in Python: `lib/*.py` are compiled by this front
end when a realm is made, and are in no traceback.

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
- **A set is in the order in which it was added to**, and not in the order of a hash table's slots.
- **One NaN is another.** A float is a value and not an object, so `x is y` is true of two NaNs, and a set has room for one.
- **In a `__dict__`, keys that are strings come before those that are not**, and before those that JavaScript would take for an index.

## Tests

| | |
|---|---|
| `JSTests/python/run-programs.sh <jsc>` | programs whose output is CPython's, byte for byte, each in five configurations of the engine |
| `JSTests/python/run-interop.sh <jsc>` | the two languages together. There is nothing to compare these with: what is expected was read and found right |
| `JSTests/python/audits/run-audits.sh <jsc> [n]` | not tests but measures of how far there is to go, over everything that is built in |
| `JSTests/python/parser.js`, `symbol-table.js` | the first stages by themselves |

## What is not decided

Threads, when objects are finalized (`__del__`, and `with`-less file handles), and extension modules written for CPython's C API.
