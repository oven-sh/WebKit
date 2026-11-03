# Python in JavaScriptCore

Python 3.15 as a second language of the engine. JSC parses it, compiles it to JSC bytecode and runs it in the same VM, on the
same heap and the same stack as JavaScript. There is no CPython in here and no translation to JavaScript source: the only thing
taken from CPython is its standard library in Python (`Lib/`), unchanged, which the host (Bun) supplies.

What is not done, and what was decided against, is in [DEFERRED.md](DEFERRED.md).

## Layout

| Directory   | What                                                                                                        |
| ----------- | ----------------------------------------------------------------------------------------------------------- |
| `parser/`   | Lexer and hand-written recursive-descent parser for the 3.15 grammar, with CPython's error messages. AST.   |
| `compiler/` | Symbol table, and the compiler from the AST to JSC bytecode through `BytecodeGenerator`.                    |
| `runtime/`  | The object model, the built-in types, `builtins`, `sys`, the import system, the modules CPython has in C.   |
| `tools/`    | Scripts that compare the parser with CPython's over whole source trees, and that make generated tables.     |

`PyBytecode.h` is what the compiler and the runtime share (operator numbers, the functions of `op_py_runtime`).
`runtime/PyHooks.h` is everything the rest of JSC calls. `runtime/PyExec.h` is what a host calls, and through
`runtime/PyVirtualFiles.h` it supplies files that are on no disk: Bun has the standard library inside its executable.

## Values

A Python value is a `JSValue`, and wherever JS has the same thing, it is the same thing. Nothing is wrapped or converted when a
value goes from one language to the other.

| Python                 | JSC                                                                                              |
| ---------------------- | ------------------------------------------------------------------------------------------------ |
| `None`                 | `undefined` (`null` from JS is `None` too)                                                       |
| `True`, `False`        | the booleans                                                                                     |
| `int`                  | a number that is a whole number with magnitude below 2^53; beyond that a `JSBigInt`              |
| `float`                | any other number; a float that is a whole number is a `PyFloat` cell (see below)                 |
| `str`                  | `JSString` (indexed by code point; UTF-16 underneath)                                            |
| `list`                 | `JSArray`                                                                                        |
| function               | `PyFunction : JSFunction`, whose executable holds Python bytecode                                |
| class                  | `PyType : InternalFunction`                                                                      |
| function written in C++| `PyNativeFunction : InternalFunction`                                                            |
| instance of a class    | `PyInstance`: its attributes are the properties of the JS object, laid out by Structures         |
| anything else          | `PyObject : JSNonFinalObject`, with a pointer to its `PyType`                                    |
| any other JS object    | an instance of the Python type `JSObject` / `JSFunction`: properties are attributes and items    |

**int.** Arithmetic that leaves the range of exact doubles continues in `JSBigInt` and comes back when the result fits, so
Python sees one unbounded `int` type. JS sees a number or a BigInt.

**float.** JS has one number type, and which of its two encodings (int32, double) a number is in depends on what computed it,
down to which JIT tier ran. So the encoding cannot carry Python's distinction between `2` and `2.0`. The rule that does not depend
on history is: a number that is whole is an `int`. A `float` that is whole is therefore kept in a cell, `PyFloat`. Where a value is
handed to JS (the result of a call, an argument, an attribute, an operator's result) it is the plain number again
(`valueForJS`), so that floatness is lost in JS and nothing else is. Inside a list, which is shared storage, JS can see the cell;
it behaves as a `Number` object does.

**str.** A `JSString`, so UTF-16 where it is not Latin-1, while Python counts code points. A text without surrogate pairs is indexed
directly. For one with pairs, `StrIndexCache` (16 entries for each runtime, emptied after each collection, nothing for texts shorter
than 48 units) remembers the number of pairs and the offset of every 64th code point, so that `s[i]` in a loop does not scan from
the start each time.

**tuple, dict.** A tuple remembers its hash once it has been asked for. A dict keeps its entries in an array in insertion order, with
an index beside it; it knows where its first entry is, and can leave room before it, so that taking and adding entries is as cheap at
the front as at the back. `collections.OrderedDict` (`PyOrderedDict.cpp`) is that and nothing more. Each entry keeps the hash of its
key, whole, so that `set(d)`, `dict.fromkeys(s)`, `s - d` and the like never ask a key for its hash again. The hash of a str fits 32
bits, so a dict with none but str keys keeps 32 bits for each; the first key of another kind widens them to 64.

**Built-in types.** A `TypeDef` in the source file of a type says what is particular to it. What follows from how CPython makes the
type is added by `PyType::initializeBuiltin` and `addMethods`, so that `vars(T)`, `help(T)` and what goes by them
(`functools.total_ordering`, `functools.wraps`, `copyreg`) see what they see there. A type with a method for one rich comparison has
methods for all six. The rest goes by the table of CPython's docstrings (`PyDocstrings.inc`, made by `tools/make_docstrings.py`),
which says what each type has in its namespace: the docstrings; a wrapper of the type's own for what it does as `object` does
(`deque.__getattribute__`); and `__module__`, which the types that CPython makes at run time (most of those outside `builtins`)
have there, where their instances find it as well. `TypeFlag::Mutable` is for the few of those whose attributes can be set: the exceptions of
modules (`struct.error`), struct sequences outside `sys`, `ast.AST`.

**Which `__new__` makes an instance.** As in CPython: if what the MRO finds is the `__new__` of a built-in type, it stands for "what
the class inherits", which is that of `__base__`, the base that decides the layout. So `class C(tzinfo, dict)` is made by
`dict.__new__`, and `tzinfo.__new__(C)` is refused as not safe (`typeCall`, `PyNativeFunction::invokeNew`).

## Code

The compiler emits ordinary JSC bytecode where the semantics agree (moves, jumps, calls, exception handlers, generators through
`BytecodeGeneratorification`) and about two dozen `op_py_*` instructions where they do not (attribute access, operators, name
lookup, iteration, unpacking, argument binding). `op_py_runtime` calls one of a table of runtime functions for everything rare.
Python code runs in the LLInt only; it never tiers up (`shouldJIT`).

**Fast paths.** Each `op_py_*` instruction has a slow path in C++ that does all of it (`runtime/PySlowPaths.cpp`), and most have the
common cases in assembly (`llint/LowLevelInterpreter64.asm`, "Python"): arithmetic and comparisons of numbers, comparisons fused
with the jump that follows, truth of the usual values, items of lists and tuples, iteration over ranges, lists and tuples,
unpacking, cells, the binding of arguments when they are simply the parameters.

**Inline caches**, in the metadata of the instruction, as for JS:

- _Globals and builtins._ A dict that code has cached from has a number that no other dict, and no other set of keys of the
  same dict, ever has (`PyDict::watchedVersion`). The instruction remembers the number and the index of the entry.
- _Attributes_ (`AttributeCache` in `runtime/PyAbstract.h`). The attributes of an instance are properties, so its Structure tells which it
  has and where. A type has a number too, new whenever it or one of its bases changes (`PyType::version`). An instruction
  remembers Structure, version and offset, or the value found in the class, or the method to call. `obj.__dict__` moves the
  attributes into a real dict, for good, and gives the object a Structure that says so.
- _Calls_ are JSC's.

`object.name(...)` makes no bound method: `op_py_get_method` gives the function and the object, which is passed as the first argument.
`with` finds `__enter__` and `__exit__` with the same instruction (a number stands for the name: they are looked for in the class alone).

`Name(...)`, where the name looks like that of a class, is compiled so that, if it is a class with `object.__new__` and an `__init__`
in Python, the instance is made and `__init__` called without leaving the interpreter (`op_py_prepare_construct`).

**Calls** are `op_call`. Positional arguments are the arguments; keyword values follow them, and `this` is a tuple of their names
(`PyKwNamesType`) or `undefined`. `op_py_bind_args`, the first instruction of a function, sorts them into parameters. So JS calls a
Python function, and Python a JS function, with the engine's own call sequence and inline caches.
When Python calls what is not Python, the frame is adapted on the way (`prepareForeignCall`, from `linkFor`): keyword arguments
become a trailing object, whole floats become numbers.

**Generators, coroutines, async generators** are made as JSC makes generators: a function that makes the generator object
(`PyGenerator`), and a body, a function of its own, that the generatorification pass turns into a state machine.

- _To resume a generator is to call its body_, and the body keeps the books. `op_py_generator_enter`, its first instruction,
  notes that the generator is running and goes on where it stopped: the state of a suspended generator is the offset of the
  instruction to go on with. `op_py_generator_suspend` and `op_py_generator_return` note that it no longer runs, and return to
  the caller. If an exception leaves the body, the unwinder does that (`generatorBodyIsUnwinding`). The caller tells a yield from
  a return by the state.
- So anybody can resume one: `PyGenerator::resume()` in C++, and two instructions, which make the frame and call the body without
  leaving the interpreter: `op_py_for_iter`, and `op_py_send`, which is a step of `yield from` or `await`. Both enter at
  `llint_python_generator_resume`: the prologue, `op_enter` and `op_py_generator_enter` in one.
- _The registers_ that are live across a yield, and all the variables (`gi_frame.f_locals` shows them), are kept in slots at the
  end of the generator object, which is as large as its code needs. Runs of registers are saved and restored by one instruction
  (`op_py_generator_registers`; the last run is saved by the suspend itself). Until it starts the first slots hold the arguments.
  While it runs they are empty.
- What the pass saves at a yield is what its analysis finds live, which may be more than is in use: a register above the
  variables may hold the remains of a callee's frame, which the collector must never see in the heap. For Python code the pass
  gives such registers a value where they lose theirs (`findRegistersWithoutValue()`).
- One that may have something to do when it is dropped (a `finally`, a `with`, a coroutine never awaited) is registered for
  finalization, but not when it is made: most are done long before the next collection. It is written down
  (`noteFinalizableGenerator`), and those still unfinished when a collection comes are registered then.

**Exceptions** are JSC exceptions whose value is a `PyException`. The unwinder tells the runtime about each Python frame it
passes (`addTracebackEntry`), which is how tracebacks are built.

## The collector, finalizers and weak references

CPython frees an object the moment its last reference goes. JSC finds out later, in a collection. That cannot be papered over,
so the aim is that everything Python *guarantees* holds, and that what it merely does in practice happens as soon as it soundly can.

- **The collector runs no Python code.** An object with a finalizer (`__del__`, a generator with a pending `finally`, a
  coroutine) is registered. When marking has converged, registered objects that are not marked are moved to a queue and marked,
  with all they reach, and marking goes on (`Collector::runFixpointPhase`). So a finalizer sees live objects only, and may resurrect.
- **The queue is served at a safe point**: a place in Python code where any code may run. The collector sets a trap bit,
  `VMTraps::NeedPythonCalls`; Python's loops (`op_py_check_traps`) and function entries poll it. JS code and C++ do not service it:
  the optimizing JITs take a poll to change nothing. A host may serve it between turns of its event loop (`makeCallsOwed`).
- **Each finalizer runs once.** An object that is still unreachable at the next collection is freed.
- **An object registers when it is made**, if its class has a `__del__` then. A class that gets one later, by assignment to the
  attribute or to `__bases__`, has the heap searched for its instances (`registerInstancesForFinalization`), once; an object
  that is given such a class by assignment to `__class__` registers then.
- **An object is finalized before what it refers to**, as with reference counts: a `__del__` that writes to `self.file` finds the
  file open. The collection ranks the unreachable objects that have finalizers by how they reach one another through what else is
  unreachable (`PyFinalizationOrder.cpp`), and queues those that no other reaches. The rest wait for the collection that finds
  those gone, so a finalizer that resurrects its object also keeps what the object refers to from being finalized. Objects in a
  cycle are finalized together, in no order, as in CPython. A structure more than a few levels deep is finalized in one round,
  from the top down, instead of a collection per level.
- **Weak references** are cleared when the collection that finds the referent dead ends, before any finalizer or callback
  runs; callbacks are queued like finalizers.
- **At exit**, if there is anything left to finalize, `sys.modules` is emptied as CPython empties it, and collections find and
  order what only the modules held. What is then still held (by JS, by a frame on the stack) is finalized in the order it was made.
- **`gc.collect()`** collects, serves the queue, and repeats until nothing is left to do, so `del x; gc.collect()` does what it does in
  CPython. For that to be true the stack scan must not see stale values: before it, the temporaries of Python frames that liveness
  analysis says are dead are emptied (`clearDeadTemporaries`). Variables are not: an object lives while a variable names it.
  And the frames of C++ between the Python code and the collector must not lie over what earlier calls left on the stack, for what
  a frame does not write it still holds: the stack below is wiped before the first of them (`NativeFlag::OnWipedStack`).
- **A thread that waits collects.** A program may wait for what only a finalizer brings about: it has dropped a response without
  closing it, and waits for a server in another thread that waits for the end of the connection. A thread that has waited for
  another for 50 ms collects and finalizes (`Threads::collectWhileIdle`), then less and less often; so does one that would
  otherwise be told that it waits for ever. A native function that finds that it has to wait is begun again on a wiped stack, for
  the same reason as `gc.collect()` (`NativeFlag::MayWait`, `Threads::willCallFunctionThatMayWait`). A thread that is blocked
  in a system call does not collect.
- **Files that are dropped** without being closed are closed by a finalizer, some time later, where CPython closes them at once.
  Two things make up for most of that. Streams as `open()` makes them that hold output are in a list
  (`ioNotePendingOutput`), and `writeOutPendingOutput()`, one load and a branch when the list is empty, writes them out before
  anything that could tell the difference: see its comment in `PyInternals.h`, and call it first in any new function that looks at a
  file or changes one. And a system call that fails for want of descriptors (`EMFILE`) collects, finalizes and is tried again
  (`posixFreeDescriptors`).

## Imports

The import system is `importlib`, unchanged. `importlib._bootstrap` and `_bootstrap_external`, which CPython has compiled in, are
loaded from the standard library when the interpreter starts (`startImportSystem`), then `site`. `import` in C++ is what it is in
CPython: a look in `sys.modules`, and `_find_and_load` if that fails.

A host adds what it can import through a module of its own (`setImportSystemModule`), which also decides what module a file is that JS imports by path (one file, one module; the name Python would import it by, if there is one). Bun's (`src/python/_bun_import.py`) makes
JavaScript and TypeScript files importable wherever Python files are, packages in `node_modules`, and `node.*` and `bun.*`. What
is imported is what `require()` returns, a JS object like any other. A name that Python code tries to import is only ever looked
up on disk.

**Compiled code is kept in `.pyc` files**, and it is importlib that keeps it there, as in CPython: `__pycache__/os.bun-315.pyc` beside
the source, checked against the time and size of the source (or its hash), `-B`, `PYTHONPYCACHEPREFIX`, `py_compile`, modules
without source. What the runtime supplies is `marshal` for code objects (`runtime/PyCodeCache.h`):

- The code is written by JSC's own serializer of bytecode (`runtime/CachedTypes.cpp`, `encodePythonCode`), which has learnt the
  constants Python code has and what a code object knows besides its bytecode (`CodeInfo`). It is read in place: loading a
  module makes the code object of the module, and a function in it is decoded when it is first called.
- The text of the module goes with the code, so tracebacks, `co_positions()` and comparing code objects are the same whether
  the code was compiled or loaded, and nothing reads the source.
- Bytecode means something only to the build that wrote it. What marshal writes starts with a number made from the build ID of
  the executable (`codeCacheStamp`), and the magic number of `.pyc` files is made from it, so importlib takes the files of another
  build for out of date and replaces them.
- A checksum tells code that is damaged. `_io.open_code()`, through which importlib reads the files, refuses such a file, which
  to importlib is a file that is not there: it compiles the source and writes the file anew. (`marshal.loads()` raises
  `ValueError`. A module without source fails with `OSError`.) A file with the right header that holds no code at all is left to
  importlib, which reports it as CPython does (`EOFError`, `ImportError`): CPython's tests ask for that. Against a file that was
  made to do harm there is no defence, no more than in CPython: whoever can write it can write the `.py` as well.
- The two modules of importlib that are loaded before importlib exists are cached in the same files by C++ (`codeOfSourceFile`).
- Nothing can be written beside the files a host has inside itself (`PyVirtualFiles.h`). The `__pycache__` directories there are
  real directories elsewhere: in a cache directory of the user's that the host names, one for each build (`toCachePath`).

## The event loop

The event loop is the host's, and there are three ways to it (`PyExec.h`, `PyEventWait.h`).

- **Waiting.** Every wait for a file descriptor or a timeout (`select`, `poll`, `epoll`, `kqueue`, sockets) goes through one function,
  which the host replaces: Bun watches the descriptor and a timer in its own loop and turns that until one of them fires. So
  `asyncio.run()` and any loop a program makes are asyncio's own, unchanged, and timers, promises and I/O of JS go on while they sleep.
- **Coroutines without a loop of their own**: a module that awaits at the top level, a coroutine that JS awaits, an async generator
  that JS iterates over. They are tasks of one more asyncio loop, which never sleeps: the host's loop calls it when a descriptor
  is ready, a timer is due or a callback has been scheduled (`src/python/_bun_asyncio.py` in Bun). To JS they are promises.
- **`await promise`** in Python, for a JS promise (type `JSPromise`), is a future of the running loop that the promise settles.
- **Futures and tasks** are native: the module `_asyncio` (`runtime/PyAsyncio.cpp`), which Lib/asyncio takes in place of its own classes.
  A step of a task resumes the coroutine from C++, outside any handler, and `await future` gets the result without a `StopIteration`
  (`PyAsyncio.h`). The loop stays asyncio's: callbacks go through its `call_soon()`. `all_tasks()` reads an array that the collector
  does not look at, so that it keeps no task alive; a task in it is registered for finalization, which takes it out before it is freed.
- **Calls owed** (finalizers, signal handlers) are also made between turns of the host's loop, which is woken up for them.
  `SystemExit` and `KeyboardInterrupt` end the program wherever they surface, a callback of JS included.

## Threads

A Python thread is a thread of the system, and one of them at a time runs in the VM: the one that holds the global interpreter
lock (`runtime/PyThreads.h`). To hold it is to hold the VM's `JSLock`, with what the VM and the runtime know about the running code
(top frame, entry frame, exception state, stack limits) being that thread's: a thread that gives up the lock takes that with it
(`JSLock::park`) and puts it back when it has the lock again. The collector scans the stacks of all of them.

- The lock changes hands where a thread is about to wait or to make a long system call (`AllowThreads`: sleep, locks, `join`, queues, `select`
  and its kin, sockets, files, `waitpid`), and where another thread has waited for longer than `sys.getswitchinterval()`: that
  fires `VMTraps::NeedPythonCalls`, and the running thread hands over at its next safe point.
- Until a program starts its first thread none of this happens, and while there is no other thread blocking calls keep the lock.
- **JavaScript stays on the main thread.** Only that thread turns the host's event loop; using a JS object or function from another thread
  raises `RuntimeError`. While the event loop idles, the lock is free.
- **So does everything else that is the host's.** `Heap::mayRunEmbedderFinalizers()` is false while another thread has the VM. Then
  cells with destructors of the host's, or with dead weak handles whose finalizers are not known to run anywhere
  (`Heap::addFinalizerForAnyThread()`), are not swept: the allocator passes over their blocks, `gc.collect()` does not sweep. The main
  thread is woken and makes up for it at its next safe point. Requests to end what runs (`node:vm` timeouts, `Worker.terminate()`, the
  watchdog) are put aside for the main thread too, and the VM counts as idle (`VM::isIdle()`) when the main thread is outside it,
  whatever Python frames other threads are parked in.
- While there are other threads, a `Buffer` that C++ holds of memory of JS pins the `ArrayBuffer` (`runtime/PyBuffer.h`): JS, which may run
  while the thread is in a system call, cannot detach or shrink it.
- A thread that waits for others for more than 50 ms, having allocated since it last did so, collects and runs finalizers: the
  others may be waiting for what a dropped object holds open, and in CPython it would have been closed when it was dropped.
- At exit the threads that are not daemons are joined, unless the wait is interrupted; daemon threads never get the lock again.
- Signals are handled by the main thread, whichever thread the system delivers them to.

## Tracing

`sys.monitoring` (PEP 669) is the mechanism; `sys.settrace` and `sys.setprofile` are two of its tools (numbers 7 and 6, with callbacks in
C++ that do what CPython's `legacy_tracing.c` does), and `pdb`, `bdb`, `trace`, `profile`, `cProfile` (`_lsprof`, a tool like any other) and
coverage tools are CPython's own code on top. It is all in `runtime/PyMonitoring.{h,cpp}`.

**Code that nobody asks about pays nothing.** No instruction is there for the sake of tracing and none tests a flag.

- _Events at places in the code_ (a function starts, returns, yields, is resumed; a line begins; a call; a jump or a branch; an instruction). When a
  tool asks for them the instructions are changed where they stand: the opcode at each such place is replaced by
  `op_py_instrumented`, operands untouched. That calls the runtime, which tells the tools and hands back the opcode that was
  there, and the interpreter goes on to the code for it with the same PC. Python is only ever interpreted, so there is no other
  copy of the code. Whoever else reads instructions sees what was there (`Instruction::opcodeID()`), and `marshal` is given the
  instructions as the compiler left them.
- The instructions belong to the `UnlinkedCodeBlock`, and so does what is known about the places in them: a `CodeMonitor`, made when the code is
  first changed. When the events wanted everywhere change, all code that has a `CodeBlock` is changed at once, running code included,
  and code that gets one later is changed then. So nothing is checked when a function is entered. (The price: switching a tool on or
  off takes time in proportion to the code that is loaded. See DEFERRED.)
- `DISABLE` puts the opcode back, if no other tool wants the place.
- Some instructions have to be carried out by the runtime to be told of: a branch (which way it went is known afterwards), a call of
  something that is not a Python function (`C_RETURN`, `C_RAISE`), the end of a `yield from` (`STOP_ITERATION`). `Name(...)` is an ordinary call while calls are told of.
- _Events to do with exceptions_ (`RAISE`, `RERAISE`, `EXCEPTION_HANDLED`, `PY_UNWIND`) come from the unwinder, which looks at one flag for each
  Python frame an exception passes.
- What a tool, a trace function or an audit hook (without `__cantrace__`) does is not told of (`Runtime::tracingDepth`).

**Lines.** The compiler leaves marks (`PythonMark`, in the rare data of the `UnlinkedCodeBlock`; what they say is in `PyBytecode.h`): here begins line _n_; what follows is
of no line; here the program's own code starts; here a block begins or ends that holds something in a temporary register; the jump that comes next is not the program's. They follow CPython's
compiler location by location, artificial code included: operations on constants that it works out are of one line, statements that cannot be reached are of none. A line of which no instruction came (`pass`, `try:`, `x = (` over `1)`) has a mark on
whatever comes next, which costs nothing; only where that can be reached from elsewhere is there an `op_nop`, as in CPython.
`f_lineno`, tracebacks, `co_lines()` and `LINE` events all go by the marks.
A line is news if the frame was on another line before. For nearly every mark that is plain from the code (`CodeMonitor::findPoints`).
Where the frame may come from the same line or from another, the jumps and branches that lead there say which: those from other lines tell of the line
(the head of a `for` loop), or those from the same line say that it is not news (`return (a and` over `b)`). In what is left, one code object in 200 of the
standard library, the frame object remembers the line it was last on, and all places where lines begin stay changed while a tool wants any of them.

**Jumps and branches.** The layout is not CPython's, so some of the jumps here are none there, and are not told of (`Unit::hiddenJump()`): over
an exception handler, which CPython moves out of the way; between the two ways a method call is compiled. A jump to a jump is told of as one. A `for` loop that goes on, and an
`except E:` that matches, go on "in their own last byte", which is of their line, where CPython has an instruction to store the item or drop the exception.

**Offsets.** What a program is given for a place in the code (`f_lasti`, `tb_lasti`, `co_lines()`, `co_positions()`, `co_branches()`, the arguments of callbacks) is twice the offset
in JSC's instructions (`shownOffset()`): in CPython code is made of units of two bytes, and `traceback.py` and `dis` count on it. `co_code` is an outline in those
units: a `NOP` for each byte, and `RESUME` before the program's own code, which is how `pdb` finds the first line of a function.

**Frames.** While something is told of an event, `f_lineno` of that frame is the line CPython would show. `f_locals` reads and writes the registers.
`f_lineno = n`, in a trace function, goes where CPython goes and refuses what it refuses, with its messages: the target must be the beginning of a statement whose blocks the
frame is in already, or can take over (a `for` loop's iterator serves another `for` loop). Variables without a value get `None`, with CPython's warning, because code further on
may have been compiled knowing that they have one.

**Threads.** Trace and profile functions, and whether a callback is running, are the thread's (`Runtime::ThreadLocalState`). Tools 6 and 7 want their events
while any thread has a function. The side tables are only touched with the GIL; the table of replaced opcodes, which the collector's threads read and all VMs of the process share, has a lock.

`sys.monitoring._places(code)` shows the places of a code object, and which are replaced right now (offsets as JSC has them).

## JavaScript sees Python objects

- Properties are attributes (items first, for mappings), through the method table, as for any exotic object of JSC.
- A class is called or `new`ed; `instanceof` is `isinstance`.
- What a Python function returns to code that is not Python is made fit for it (`resultForCaller`): a whole float is a number, and
  a coroutine is a promise, so an `async def` is an async function to JS. An awaitable can be awaited, an async generator iterated over with `for await`.
- A call follows the rules of the language it is written in. JS may pass a function more than it takes, and callers of
  callbacks do (`array.map(f)` is `f(item, index, array)`): what a function or bound method written in Python has no parameter
  for is dropped, if the caller is not Python. Too few arguments are a `TypeError` for every caller.
- In Python, to call a class is to make an instance, so Python calling a class of JS is `new`: what `class` declares, and the
  constructors of the engine and the host (`js.Map()`, `js.Date()`, `js.Response("body")`). The constructors of
  primitive values (`String`, `Number`, `Boolean`, `Symbol`, `BigInt`) make one, as they do when JS calls them. A plain `function` is called; `f.new(...)` is `new f(...)` for anything.
- Keyword arguments for a JS function are a trailing object. A dict iterates in JS as a `Map` does, by `[key, value]`.
- **Operators.** `a + b`, `-a`, `a < b`, `a == b` and so on ask the classes of the Python objects among the operands
  (`__add__`, `__radd__`, ...), in the slow paths that all tiers share (`OperationsInlines.h`, `JSValue::equalSlowCaseInline`). If
  neither class answers, JS does what it does with any object. The DFG and FTL used to know that an arithmetic operator returns a
  primitive and that two objects are `==` only if identical; they now know it only while
  `VM::operatorOverloadingWatchpointSet()` is valid, and starting Python fires it. A VM that never runs Python compiles what it
  always did.

## The command line

`runtime/PyCommandLine.h` has the configuration of the interpreter (`Configuration`, what CPython calls `PyConfig`: one for the
process, filled in by the host before Python starts) and CPython's command line: `readCommandLine()` parses the options and the
`PYTHON*` variables before there is a VM, `runCommandLine()` runs the script, `-c`, `-m`, a directory or archive with a
`__main__.py`, standard input or the interactive interpreter, with CPython's `sys.argv`, `sys.path[0]`, messages and exit statuses.

Bun is that command when the name it is started under, or that of the file that was executed, is `python`, `python3` or `python3.15`
(`src/runtime/cli/python_command.rs`);
its own options, `BUN_OPTIONS`, `bunfig.toml` and `.env` files then do not apply. `sys.executable` is the name it was started under.
For `bun main.py` it is a link named `python3` that Bun makes in a directory of its own under `/tmp`
(also `sys._base_executable`), so `subprocess`, `multiprocessing`, `venv` and the like find an interpreter. The directory is not put on `PATH`.

On a terminal the interactive interpreter is `_pyrepl`, otherwise a loop in C++ that behaves as CPython's basic one. `await` works
at the top level of both, as in scripts, `-c` and standard input.

## Writing a module in C++

Tables of `NativeDef` (`PY_FUNCTION`, `PY_METHOD`, `PY_SLOT`, ...) with signatures in the form of `__text_signature__`; the
argument parser is generated from the signature at compile time. `TypeDef` describes a type; `Runtime::typeFor(def)` makes it on
first use. A module is `void initializeXModule(JSGlobalObject*, PyModule*)` listed in `PyNativeModules.cpp`.

## Testing

Everything is tested through Bun, against CPython 3.15 as the oracle. The tests are in Bun's repository, under `test/js/bun/python/`.

- **Differential tests** (`differential/`, with a README): some 580 scripts, each with what CPython printed beside it. `bun script.py`
  must give the same stdout, the same exit status, the same last line of stderr. `differential/*.test.ts` run them in Bun's test runner,
  without a CPython; `differential/tools/run.py` runs them by hand, shows differences legibly, and asks a CPython again
  (`--update`). `expected-failures.json` lists what is known to differ, and why; a listed script that passes is a failure.
  `differential/command-line/` does the same for about 130 command lines of `python3`.
- **Bun's own tests**: `*.test.ts` (`cli.test.ts` is the command line).
- **CPython's test suite** (`Lib/test`, which is not in the executable: point `BUN_PYTHON_STDLIB` at a checkout's `Lib`).
  `python3 -m test test_x` is regrtest unchanged, `-j N` with worker processes included. Tests that need `_testcapi`,
  `_testinternalcapi` and other extension modules skip themselves.
- **The scoreboard** (`cpython-suite/score.py`) runs each of the 676 test modules in a process of its own, several at a time, with a release build, and
  writes `score.md` and `score.json`: totals, tests that kill the interpreter or hang, failures grouped by cause, what changed since
  the last run, and how many tests CPython itself passes in each module on the same machine. A module in which a test crashes or hangs is run
  again without that test, so each such test is found and the rest of the module counts. A full run takes about ten minutes.
  `cpython-suite/TRIAGE.md` has every crash and hang found that way; their reproductions are differential tests now.
- **Builds.** The debug build (ASAN, assertions) takes 2.6 s to start Python and 20 s to import `asyncio`; the test suite needs a release
  build (0.1 s and 0.25 s). A release build with assertions (`--profile=release-local --assertions=on`) is a third tree; it is half as fast as the release build, and finds what only `ASSERT` finds. In a release
  build of Bun `malloc` is mimalloc inside the executable only: memory that the C library allocated (`realpath(p, NULL)`,
  `std::filesystem::canonical`, `strdup`, `getline`) must not be passed to `free`. Debug builds do not notice.
