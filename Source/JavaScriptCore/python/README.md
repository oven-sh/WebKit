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

A module's body, a class's body, a function, a lambda, a generator expression, and what evaluates annotations are each an
`UnlinkedFunctionExecutable`, with a `Python::FunctionInfo` that says which. So there is one place where the languages part:
`generateUnlinkedFunctionCodeBlock`.

Functions are compiled when first called, as JavaScript's are, from their range of the source. What an inner function needs to know
of the blocks around it is worked out when those are compiled, and kept in its `FunctionInfo`. **The whole file is checked when it is
loaded**, since Python reports a syntax error anywhere in a file before running any of it. What is wrong travels in a `ParserError`,
as for JavaScript, which has room for what Python has to say besides.

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

### A class is the prototype of its instances

`instance.[[Prototype]]` is the class, so `type(x)` is a load from `x`'s structure. `class.[[Prototype]]` is its first base, and beyond a
built-in class that JavaScript has too is JavaScript's: `Array.prototype` beyond `list`, `TypeError.prototype` beyond `TypeError`.

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
defined.

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
- Names that JavaScript expects and Python has no use for are provided: `toString`, `Symbol.iterator`, `next`, `toJSON`, `length` or `size`.

**Reading an attribute that the class has nothing to say about is cached inline as for any object.** A class has a watchpoint set,
`instanceAccessIsAsFound()`, which is named in the `PropertySlot`. It stops holding when the class, or one that it is derived from, is given a data
descriptor or a `__getattribute__`, `__setattr__` or `__delattr__` after it was made.

Every kind of cell that can have attributes has these methods (`PYTHON_DECLARE_EXOTIC_METHODS`).

### What Python sees

An object of JavaScript's is an instance of `JSObject`, and a function of `JSFunction`, which is derived from it. Its attributes are its properties,
as JavaScript finds them. `obj[key]`, `len()`, `in`, iteration and `isinstance()` do what they would in JavaScript. `import js` is the global object.

- `obj.f(x)` passes `obj` as `this`.
- **A function that is got from what an object inherits from is bound to the object**, as one got from a class is in Python. One that the
  object has of its own is as it is, as one in an instance's `__dict__` is. So `js.Math.floor` and `js.Array` are themselves.
- **Calling a class makes an instance**, Python having no `new`. That is asked only where there was nothing left to do but throw
  (`callConstructorWithoutNew`), so no call that works pays for it. What can be both called and constructed with is called, and `.new()` constructs.
- JavaScript's methods are not attributes of a `list` or a `str`. `hasattr(x, "keys")` is how Python tells a mapping.

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

- What has a class for its prototype is an instance of it only if it is the kind of cell that its instances are (`typeOf`). Otherwise it is an
  object of JavaScript's that inherits from a class, and what it inherits is what the class itself has.
- Every `Structure` for a cell of Python's has `IsImmutablePrototypeExoticObject`. Assigning to `__class__` sets the prototype, having checked.
- One class of Python's, one class of cell.

## Where it follows the engine

Before something is invented, how the engine already models the nearest thing is looked for, and that is extended, in whatever directory it
is. So far: another language is a `SourceProviderSourceType`, as WebAssembly is. What the host provides is in `GlobalObjectMethodTable`. What is not to
be in a stack trace has `ImplementationVisibility::Private`. What a cached property depends on is a watchpoint set in the `PropertySlot`. An error
with its own idea of a message uses `finishCreationForEmbedderError`. A class of cell that is all of a size has an `IsoSubspace`. An object that is
some values and nothing else is a `JSInternalFieldObjectImpl`. A cell has no vtable, so an `Array` that behaves a little differently is an `Array`
whose `Structure` has another `ClassInfo`.

There is nothing that is per process, nothing that is set after something is made, and nothing that only the shell can do.

## Where it differs from CPython on purpose

- **A `bytearray` can be resized while there is a `memoryview` of it.** CPython raises `BufferError`, and can because the view is released the
  moment the last reference to it goes. Here it would stay locked until the next collection, and programs that are right would fail. A view
  holds no pointer, only where it is looking, and checks each time.
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

Threads, when objects are finalized (`__del__`, and `with`-less file handles), extension modules written for CPython's C API, what
`class D extends C` in JavaScript makes of a class of Python's, and whether JavaScript's operators mean anything for what is Python's.
