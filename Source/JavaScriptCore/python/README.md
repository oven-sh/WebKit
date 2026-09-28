# Python in JavaScriptCore

A second front end. Python source is scanned, parsed and compiled to JavaScriptCore bytecode, and runs in the same interpreter and
compilers, on the same heap, with the same values as JavaScript. There is no CPython here, and no boundary: a Python list *is* a
JavaScript array, and a Python function *is* a JavaScript function.

The language is Python 3.14. CPython is the specification: `Grammar/python.gram`, `Parser/Python.asdl`, `Python/symtable.c`,
`Python/codegen.c`, and its test suite.

## The pipeline

| | | checked against CPython by |
|---|---|---|
| `PythonLexer` | source to tokens. It alone reads the source | |
| `PythonParser` | tokens to `PythonAST`, by recursive descent | `$vm.pythonAST`: every file of `Lib`, and 150,000 damaged programs |
| `PythonSymbolTable` | what each name refers to | `$vm.pythonSymbolTable`: the same files, and 60,000 meddled programs |
| `PythonCodeGenerator` | the tree to bytecode, through `BytecodeGenerator` | running programs |
| `runtime/` | the object model and the built-in types | running programs, and `Lib/test` |

The tree and the symbol table are CPython's own, node for node and flag for flag. The `ast` and `symtable` modules have to give
them out, and it means each stage can be compared with CPython's by itself.

## Decisions

### Which language a piece of source is in

`SourceProvider::language()`. Everything that has code can reach its provider, and functions inherit it from what they are in. It
is not a `SourceProviderSourceType`, which says script or module: that is another question. It is part of the code cache's key.

### All Python code is function code

A module's body, a class's body, a function, a lambda, a generator expression, and what evaluates annotations are each an
`UnlinkedFunctionExecutable`, with a `Python::FunctionInfo` that says which. So there is one place where the languages part:
`generateUnlinkedFunctionCodeBlock`.

Functions are compiled when first called, as JavaScript's are, from their range of the source. What an inner function needs to know
of the blocks around it is worked out when those are compiled, and kept in its `FunctionInfo`: which of its names are an enclosing
function's, and the class that private names are mangled for. **The whole file is checked when it is loaded**, since Python
reports a syntax error anywhere in a file before running any of it.

### `BytecodeGenerator` is shared

It has a constructor for a `Python::ScopeNode`, which is a `ScopeNode` whose `emitBytecode` walks a Python tree. Registers, labels,
constants, calls, `try` and `finally`, scopes and generators are `BytecodeGenerator`'s, unchanged.

### Values

| Python | is |
|---|---|
| `None` | `undefined`. `null` is `None` too: `x is None` asks whether it is either |
| `True`, `False` | `true`, `false` |
| `int` | an int32, or a double that could have been one; past 2**31, a BigInt |
| `float` | any other double, or a whole float (see `TaggedArithmetic.h`) |
| `str` | a `JSString` |
| `list` | a `JSArray` |
| function | a `JSFunction` |
| `tuple`, `dict`, `set`, `bytes`, `range`, `slice`... | cells of their own |
| an instance of a class | a `PyInstance`: an object with inline properties, like a `JSFinalObject` |
| a class | a `PyType` |
| unbound, deleted | the empty value, as for JavaScript's `let` before it is initialized |

### A class is the prototype of its instances

`instance.[[Prototype]]` is the class, and `class.[[Prototype]]` is the next class in its method resolution order. So

- `type(x)` is a load from `x`'s structure;
- an attribute of a class is one property, found by `C.x` and by `instance.x`;
- looking up a method is a prototype chain walk, which the inline caches and the compilers already know all about;
- `C.prototype` is `C`, so `instance instanceof C` is true in JavaScript.

With multiple inheritance the order is not always the chain of the first base. Then what comes after the class in the chain are
*links*: objects that stand for a class at that place in the order and hold a copy of its attributes, which the class keeps up to
date. Writes to a class's attributes all go through `PyType::put`.

What Python does on the way out of a lookup (binding a function to `self`, calling a property's getter) is not what `[[Get]]` does.
Python's own opcodes read the properties directly. For JavaScript, `PyType::getOwnPropertySlot` does it when the receiver is not the
class itself, so that `instance.method` in JavaScript is a bound method.

### Names

| | |
|---|---|
| local | a register. Empty when unbound, and `check_tdz` raises `UnboundLocalError` |
| cell, free | a variable of a `JSLexicalEnvironment`, found by `resolve_scope` as JavaScript's are |
| global | a property of the module's *namespace object*, by `get_by_id` and `put_by_id` |
| in a class body, or under `exec` | by name in a mapping, then global |

**The namespace object's prototype is the builtins' namespace object, whose prototype raises `NameError`.** So `LOAD_GLOBAL` is
`get_by_id`, with its inline caches, and the compilers fold a global function or a builtin to a constant as they do for
JavaScript's. Nobody sees a namespace object: `globals()` and `module.__dict__` are dicts that are views of it.

A function finds its namespace object in a variable named `.globals` of the outermost environment of its scope chain.

### Calls

`f(a, b)` is `call`, with `undefined` for `this`. Python's parameters are JavaScript's, in order, so either language calls the
other's functions with no adapter.

- **A method call does not make a bound method.** `py_load_method` gives the function and `self`, or the callable and nothing, and
  there is a `call` for each case on the same argument registers.
- **Keywords, `*args` and `**kwargs` are bound by the caller**, in `pyCallKeywords` and `pyCallSpread`. They call the function with
  every parameter filled in, `*args` already a tuple and `**kwargs` already a dict, and a marker for `this` that says so. So a
  function has two ways in and both are ordinary control flow: it starts with "if `this` is the marker, skip binding".
- Otherwise a function binds what it was given itself, in bytecode: checks the count, fills in defaults, makes the `*args` tuple.
- A built-in function written in C++ gets keywords as extra arguments, and a tuple of their names for `this`.
- Given keywords, a JavaScript function gets them as an object, as its last argument.

### Opcodes

There is room for 58 more (an opcode is a byte). Python gets its own where its semantics are its own *and* it matters how fast they
are: arithmetic, comparison, truth, attributes, subscripts, iteration, unpacking. One opcode serves a family, with the operator as an
operand, which every tier but the interpreter reads at compile time. Everything else is an existing opcode, or a call to a function of
the runtime. Those are the properties of one object, which is a link time constant.

### Exceptions

`raise` is `throw`, and `try` is JavaScriptCore's. An exception is an instance of a class as any other. `except` compares classes. A
JavaScript `Error` is an instance of a Python class too, by its kind.

### Where the library is written

Data structures and what the language itself needs, in C++. The long tail of methods, in Python, compiled by this front end.

## What is not decided

Threads, when objects are finalized (`__del__`, and `with`-less file handles), and extension modules written for CPython's C API.
