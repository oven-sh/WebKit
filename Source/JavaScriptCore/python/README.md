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
| JS iterator, generator | `JSIterator`, `JSGenerator` (subtypes of `JSObject`): an iterator, a generator                   |

**int.** Arithmetic that leaves the range of exact doubles continues in `JSBigInt` and comes back when the result fits, so
Python sees one unbounded `int` type. JS sees a number or a BigInt.

**float.** JS has one number type, and which of its two encodings (int32, double) a number is in depends on what computed it,
down to which JIT tier ran. So the encoding cannot carry Python's distinction between `2` and `2.0`. The rule that does not depend
on history is: a number that is whole is an `int`. A `float` that is whole is therefore kept in a cell, `PyFloat`. Where a value is
handed to JS (the result of a call, an argument, an attribute, an operator's result) it is the plain number again
(`valueForJS`), so that floatness is lost in JS and nothing else is. Inside a list, which is shared storage, JS can see the cell;
it behaves as a `Number` object does.

**str.** A `JSString`, so UTF-16 where it is not Latin-1, while Python counts code points. A text without surrogate pairs is indexed
directly. C++ borrows the text of a str while it works on it (`StrView`, `JSString::view()`) instead of retaining it: the reference
count of a `StringImpl` is atomic, and two such operations cost more than most methods of str. For one with pairs, `StrIndexCache` (16 entries for each runtime, emptied after each collection, nothing for texts shorter
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
with the jump that follows, truth of the usual values, items of lists, tuples and strs, iteration over ranges, lists and tuples,
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

**Code objects.** The code is a `FunctionExecutable`; a code object (`PyCode`) is made for it when something asks (`f.__code__`, `frame.f_code`,
`co_consts`), and from then on the executable holds it, so it is one object for as long as the code is there: programs keep weak
references to code objects and tables by their `id()`. JSC drops a `CodeBlock` that has not run for a while and links it again when it is
called. The executables of the functions that Python code defines do not go with it (`FunctionExecutable::pythonFunctionExpression()`): they are
constants of the code, and the functions made after have the same code object as those made before.

**Exceptions** are JSC exceptions whose value is a `PyException`. The unwinder tells the runtime about each Python frame it
passes (`addTracebackEntry`), which is how tracebacks are built. A frame that has not got as far as the program's own code
(`PythonMark::Start`) is not in the traceback: arguments that do not fit, the first iterable of a generator expression.

**Printing them.** As in CPython, `sys.excepthook`, `sys.unraisablehook` and `threading.excepthook` have `Lib/traceback.py` do it
(`traceback._print_exception_bltin`, called by `printException` in `PyFrame.cpp`): carets, suggestions, colours, groups and notes are its
business. What it goes by comes from here: `co_positions()` (columns in bytes of UTF-8; none with `-X no_debug_ranges`), `tb_lasti`, `name` and `obj` of an `AttributeError`
(filled in by `getAttrImpl` when whoever raised it did not), `_suggestions._generate_suggestions`, `SyntaxError._metadata`,
`_missing_stdlib_info`. If it cannot be imported or fails, `formatException` prints the traceback natively, without carets. That is also what the JS view of an
exception shows, and what is used before there is a `sys.stderr`.

## Syntax errors

CPython's parser is generated from a PEG. It goes over the text once; if that fails, it goes over it again with extra rules (`invalid_*`) that
match common mistakes and say what is wrong; if none does, the error is "invalid syntax" at the last token the *first* pass looked at. The
parser here is written by hand and goes over the text once, so type, message, position and text of every error are the same only because it keeps to that
order of events. What that takes (`parser/PyParser.h`):

- _An error nothing explains_ (`invalidSyntax()`, `m_errorIsGeneric`) is only a failure. It can be taken back (`rewind()`), and is worded last of all, in `parse()`: at
  `m_tokens[m_tokensSeen - 1]`, with the line as the lexer has it by then, or as "unexpected indent".
- _Explaining_ (`m_diagnosing`): code that stands for an `invalid_*` rule reads on past the mistake. The tokens it reads do not count as seen. The first
  explanation found stays (`m_errorWasFoundDiagnosing`), whatever is tried afterwards: in CPython raising ends the parse.
- _Prefixes_ (`m_acceptsPrefix`): to a parser that backtracks, `a +` is `a` with a `+` left over. Rules such as `disjunction expression_without_invalid`
  ("Perhaps you forgot a comma?") see such prefixes, so an expression that fails without explanation is parsed again in that mode (`parseExpression()`).
- _Quiet_ (`m_quiet`): under `expression_without_invalid` no `invalid_*` rule is tried, so nothing is explained and nothing is read on that account (`DIAGNOSE()`).
  CPython memoizes what came of a rule at a token, so what was first tried there quietly stays quiet (`m_parsedQuietly`).
- _Rules that are always tried_: a forced token in an ordinary rule (`else` `:`), and the rules about the first item of a dict, raise in the first pass. Nothing is tried
  after them (`errorIsOfARuleThatIsAlwaysTried()`).
- After any error of the parser the rest of the text is lexed, and an error of the lexer there replaces it (`checkRestOfSourceForLexerErrors()`).
- `SyntaxError._metadata` is where the last compound statement began that the second pass got through, for `traceback.py` to look for misspelt keywords
  from there on (`didParseCompoundStatement()`). A `def` does not count, a statement whose optional clause fails does.

The text of the line comes from the source, or from the file when the source is a file or the error is the compiler's (`lineOfFile`, `lineOfProgramText`: CPython's
1000 bytes, the declared encoding). `differential/crashers/fuzz_syntax_errors.py` damages statements of the standard library in one place and compares all of that, and the warnings, with CPython.

## The collector, finalizers and weak references

CPython frees an object the moment its last reference goes. JSC finds out later, in a collection. That cannot be papered over,
so the aim is that everything Python *guarantees* holds, and that what it merely does in practice happens as soon as it soundly can.

- **The collector runs no Python code.** An object with a finalizer (`__del__`, a generator with a pending `finally`, a
  coroutine) is registered. When marking has converged, registered objects that are not marked are moved to a queue and marked,
  with all they reach, and marking goes on (`Collector::runFixpointPhase`). So a finalizer sees live objects only, and may resurrect.
- **The queue is served at a safe point**: a place in Python code where any code may run. The collector sets a trap bit,
  `VMTraps::NeedPythonCalls`; Python's loops (`op_py_check_traps`) and function entries poll it. JS code and C++ do not service it:
  the optimizing JITs take a poll to change nothing. A host may serve it between turns of its event loop (`makeCallsOwed`).
  With reference counts a finalizer runs where its object is dropped; here it runs wherever the program is when the collector has
  come, and it is not to fail for that. So what is called from the queue has 200 frames to itself even at the recursion limit
  (the depth is lowered meanwhile; `sys.getrecursionlimit()` says what the program set), and near the end of the stack itself the
  queue waits for a safe point with more room. Signals are looked at first and between the calls: a long queue does not hold up Ctrl-C.
- **Each finalizer runs once.** An object that is still unreachable at the next collection is freed.
- **An object registers when it is made**, if its class has a `__del__` then. A class that gets one later, by assignment to the
  attribute or to `__bases__`, has the heap searched for its instances (`registerInstancesForFinalization`), once; an object
  that is given such a class by assignment to `__class__` registers then.
- **Finalizers run in the order of reference counts.** A `__del__` that writes to `self.file` finds the file open, and the items of
  a list that is dropped go from the last to the first. When marking has converged, the dead cells are taken for a graph and let go
  of as reference counts would have let go of them (`PyFinalizationOrder.cpp`): first those that nothing dead refers to, each of
  which lets go of what it refers to in CPython's order, depth first. That gives the order of the objects with finalizers,
  and leaves over what reference counts would not have freed: the cycles and what hangs off them.
  - Only those are queued that no other object with a finalizer reaches. The rest wait for the collection that finds those gone, so
    a finalizer that resurrects its object also keeps what the object refers to from being finalized. A structure more than a few
    levels deep is finalized in one round, from the top down, instead of a collection per level.
  - What is in a cycle or hangs off one comes after the rest and is finalized in one go, in the order in which the objects were
    made, whatever refers to what: that is what CPython's collector does with it.
  - A collection that the program waits for (`gc.collect()`, the exit, a thread that is idle: `Runtime::collectAsAskedFor`) looks at
    all that died in it, about 120 ns for a dead cell, if there is something to tell by it: `gc.collect()` wants the count, or two of
    the finalizers are code of the program (what those of built-in types do, closing a file, looks the same in any order). Any
    other collection looks only at what the objects with finalizers reach, and at nothing if there is but one: it does not see
    the list that held them, and takes siblings in the order they were made.
  - Some objects are several cells here that refer to one another: a generator and its frame object, a generator function and the
    function that runs the body, an object of a module written in C++ and its guard. Those are no cycles
    (`wholeThatPartRefersTo`, `TypeFlag::Internal`). A cache that would make one is to be weak.
  - None of this can free or lose anything: the graph marks nothing and decides only about order. Every unmarked object with a
    finalizer ends up queued and marked, or is reached by one that is and stays registered. Dead cells are read only between the
    end of marking and the first sweep, with the world stopped, when they are still whole.
- **Weak references** are cleared when the collection that finds the referent dead ends, before any callback runs; callbacks are
  queued like finalizers, and come first. For an object with a finalizer that is the collection after the one that queued it: first
  `__del__`, then the weak references, as with reference counts. For what is in a cycle or hangs off one CPython has it the other
  way round, and so it is here: the weak references *with callbacks* are cleared before the finalizers run, and stay dead
  if a finalizer brings the object back; those without are cleared afterwards. A callback is called only if the weak reference
  is itself reachable.
- **At exit**, when the threads are joined and before `atexit`'s callbacks, what is unreachable by then and reference counts would
  have freed is finalized: in CPython it would have been long ago. (Only if an object is registered whose finalizer is code of the
  program.) After the callbacks, if there is anything left to finalize, `sys.modules` is emptied as CPython empties it, and
  collections find and order what only the modules held. What is then still held (by JS, by a frame on the stack) is finalized in
  the order it was made. A program that leaves nothing to finalize exits without a collection; a generator that has not begun is
  nothing to finalize.
- **`gc.collect()`** collects, serves the queue, and repeats until nothing is left to do, so `del x; gc.collect()` does what it does in
  CPython. For that to be true the stack scan must not see stale values: before it, the temporaries of Python frames that liveness
  analysis says are dead are emptied (`clearDeadTemporaries`). Variables are not: an object lives while a variable names it.
  And the frames of C++ between the Python code and the collector must not lie over what earlier calls left on the stack, for what
  a frame does not write it still holds: the stack below is wiped before the first of them (`NativeFlag::OnWipedStack`).
  What is wiped lies below the frame of the function that asks for it, and the first thing to lie there again is where the locals of
  the Python function that returned last were. So that function (`callNativeFunctionOnWipedStack` and its like in
  `PyNative.cpp`) is reached by a tail call and has nothing in its frame but its two parameters: in a build that is not
  optimized every variable and temporary of every branch has a slot, written or not. The rule: a frame that is made on wiped stack
  has nothing stale in it for as long as it lives, since what it calls lies below it; a frame that is made after something else
  has been called from the same place has whatever that left. So the stack is wiped before every call on the way to a
  collection that is to find things dead: before every round of `gc.collect()`, for the last round has gone through the very
  objects that this one is to find gone, and at exit (`finalizeInterpreter`, `finalizeAllAtExit`).
- **`gc.collect(0)` and `gc.collect(1)`** are collections of the young generation: of what has been allocated since the last
  collection, whoever made that. There are two generations here, and a program passes 0 or 1 to be quick. What is found
  dead is finalized, weak references to it are cleared and it is counted, as above; what has come through a collection is
  not looked at, as in CPython. There is one round: what is kept for its finalizer is marked, so it is old afterwards, and it
  takes a full collection to find that it is still dead, to clear the weak references to it, to count it and to free it. That
  is sound for the reason any collection of the engine's own is: the marks of the last collection stand, so what is old counts
  as alive, and nothing that is alive is touched. (A young cell that only a dead old cell refers to is kept too.)
- **What `gc.collect()` returns** is what CPython's would: how many objects it found that reference counts would not have freed.
  Of what is left over in the graph above, the cells are counted that are objects which CPython's collector keeps track of
  (`isTracked` in `PyGC.cpp`: tables made from CPython). `DEBUG_SAVEALL` marks them instead and they go to `gc.garbage`. The first
  call in a program also finds the cycles that have piled up since it started, which CPython's own collections would have taken.
  It goes on for another round only if finalizers or callbacks of weak references were owed (`hasPendingFinalizers`), not for
  whatever else is in the queue: a collection that `gc.callbacks` is to be told of is in it too.
- **What the module `gc` hands out** (`get_objects`, `get_referrers`, `get_referents`, `garbage`) is decided in one place, at the top
  of `PyGC.cpp`: a cell is given to the program if it is an object of Python of this realm, of a kind that programs see, whole (no
  tuple that is still being filled), and not a part of something else (the table of a dict, the namespace of a class, the values
  of a frame, the memory of a deque (`TypeFlag::OwnsContainers`), what a module written in C++ keeps under a hidden key or in a slot
  of the runtime). A reference held by a part counts as held by its owner. A generator or coroutine that is running refers to its
  variables, as in CPython, though they are in a frame on the stack meanwhile (`forEachVariableOfRunningGenerators`); the variables
  of a function that is running belong to nobody, as in CPython.
- **The engine collects when it sees fit.** `gc.disable()`, `set_threshold()` and `freeze()` do not change that: the collector is the
  only thing that frees memory. A collection of the engine's own is told to `gc.callbacks` and counted in `get_stats()` at the next
  safe point: "start", then what it found (callbacks of weak references, finalizers), then "stop"; a collection of the young
  generation as generation 0, a full one as generation 2. Not while the collector is "disabled".
- **A thread that waits collects.** A program may wait for what only a finalizer brings about: it has dropped a response without
  closing it, and waits for a server in another thread that waits for the end of the connection. A thread that has waited for
  50 ms collects and finalizes (`Threads::collectWhileIdle`), then less and less often; so does one that would otherwise be told
  that it waits for ever. What it finalizes is what reference counts would have freed, which in CPython would have been finalized
  long ago. What is in a cycle or hangs off one is kept for a collection that the program asks for or brings about by allocating:
  CPython's collector does not run in a thread that waits either, and such a thread may hold the very lock that a finalizer
  wants (`with lock: time.sleep(1)`). Only the one that would otherwise wait for ever finalizes everything. Such a collection stops every thread, and any number of threads may be idle beside one that works: all of
  them together spend at most a twentieth of the time on it, a fiftieth if nothing seems to have been allocated since the last
  one (to drop something takes no allocation). And a wait with less left than twice what the last such collection took is not
  made longer by one. That goes for waiting for another thread
  (`Threads::waitUntil`) and for waiting for the kernel, which is done piece by piece (`IdleWait`): `time.sleep`, everything in
  `select` and every socket call that goes through `waitForEvents` (with a timeout, or on the main thread of a host with an event
  loop), `os.read` and `FileIO` (so `input()` too), `os.waitpid` for one process on Linux.
  - *A piece that ends is not a signal.* A signal that comes between two pieces interrupts neither, so signals are looked at
    after every piece (`IdleWait::didWait`); one that comes between that look and the system call waits for the end of the
    piece, where in CPython it waits for the end of the whole call.
  - *"Nothing to read" is not "`read()` would wait".* A read of no bytes, of the end of a pipe that is open for writing, of a
    socket that listens or has `SO_RCVTIMEO`, of a terminal with `VMIN` 0 returns at once or in its own time. So
    `waitUntilReadable` waits in `poll` only for the kinds of descriptor of which the two are known to be the same
    (`readWaitsAsPollDoes`), and asks only after `poll` has said that there is nothing: a read that finds data costs one
    more system call, no more.
  - *The stack.* A native function that finds that it has to wait is begun again on a wiped stack, for the same reason as
    `gc.collect()` (`NativeFlag::MayWait`, `Threads::willCallFunctionThatMayWait`). That takes a function that waits once, before
    it has done anything, and arguments that can be looked at twice without running code. With other arguments
    (`select.select`: lists) the stack is wiped before the call. A function that may have consumed something when it comes to
    wait (`readall`, the buffered readers) is neither: there the collection happens, but what the frames above hold by
    chance stays.
  - Not covered: `recv` and the like on a blocking socket in a thread other than the main one, writes, `connect`, `flock`,
    `wait()` for any child, `sigwait`.
- **What only a temporary holds is finalized at once**, where that can be known without the collector (`PyTemporaries.cpp`, which has the
  argument in full). `bytes(memoryview(b)[:n]); del b[:n]` and `for x in g(): break` count on it. The compiler leaves marks beside the
  code, no instructions: this call or subscript makes a value that goes in a temporary register, and that instruction is its one
  use (`PythonMark::TemporaryMade`, `TemporaryUsed`).
  - A memoryview looks at the marks when it is made. It is noted as owned by the temporary only if the use is known *then* to keep
    nothing: a method of memoryview that says so (`NativeFlag::KeepsNothing`), a subscript, being passed to a callee that is
    already waiting in a register and says so, being stored in an item of a bytearray with nothing to run in between. To find out
    afterwards is not sound: a use that keeps the object (a function in Python, a store in a list) runs no code that could
    say so, and addresses of frames and offsets of instructions come round again. When the use is over, or has raised,
    the view is released.
  - Generators are made too often for that. A generator function notes the last one it made and for which instruction, and
    `op_py_get_iter`, the next instruction to run there, claims it if the marks say that it is at the head of a `for` over that very
    call. The loop then closes it on every way out but the end: an instruction on the way of `break`, `return` and the `continue`
    of an outer loop; for an exception the unwinder does it, from the marks that tracing has for loops, before anybody is
    told that the exception is caught or leaves the frame, as CPython pops the stack of a frame first.
  - A program can get at an object that it has no reference to through `gc.get_objects()`, `get_referrers()`, `get_referents()` and
    `DEBUG_SAVEALL`, and at a generator through `frame.f_generator`: those end all such ownership
    (`temporariesMayHaveEscaped()`). What traces the program is shown arguments of calls and can assign to variables: that forgets the
    notes of views. The notes are weak: a collection forgets those of views that it has found dead.
  - It is about promptness only. What is not covered (anything that has been in a variable, a parameter, a container or a return
    value; objects made by a class written in Python) waits for the collector as before.
- **The collector marks while the program runs.** Its threads call `visitChildren` on cells that the program is changing at that
  moment. What every class of cell owes them:
  - Whatever `visitChildren` reads that is not a value is set by the constructor and never changes: the length of a tuple, the
    capacity of a table. All the slots are visited, used or not; the unused ones hold no cell. What has to grow is a cell of its own, of
    a fixed size, which its owner replaces (`PyDictStorage`, `PySetStorage`). A list is a `JSArray`, which has JSC's own protocol.
  - Values are at addresses that are multiples of their size. A store that crosses a cache line is not atomic, and the half of an int
    that has arrived looks like the address of a cell. A member is aligned by the compiler; values behind the cell (`this + 1`) are
    if the size of the class is a multiple of 8, which a class with nothing but small members is not by itself:
    `static_assert(valuesMayFollow<T>)`.
  - Values are stored one at a time (`WriteBarrier::set`), or moved with `gcSafeMemmove`, never with `memcpy`, `memmove` or
    `std::copy`. Values that have changed places inside a cell may have passed the collector by: `vm.writeBarrier(cell)` afterwards.
  - `setWithoutWriteBarrier` is for a value that is not a cell, for stores that `vm.writeBarrier(cell)` follows, and for a cell that
    has just been made: nothing refers to it and nothing has been allocated since. After the last store of a cell to a new cell comes
    `vm.mutatorFence()`, or a processor that reorders stores may show the new cell before what is in it. (`finishCreation` does that
    for what the constructor has stored.)
  - Anything else (a `Vector` or a table of the `Runtime`) is read under a lock that the program takes to change it
    (`m_finalizationLock`, `m_moduleTypesLock`).

  `differential/crashers/gc_values_stored_while_marking.py` stores to every kind of cell with values behind it while collections run.
- **Files that are dropped** without being closed are closed by a finalizer, some time later, where CPython closes them at once.
  Two things make up for most of that. Streams as `open()` makes them that hold output are in a list
  (`ioNotePendingOutput`), and `writeOutPendingOutput()`, one load and a branch when the list is empty, writes them out before
  anything that could tell the difference: see its comment in `PyInternals.h`, and call it first in any new function that looks at a
  file or changes one. And a system call that fails for want of descriptors (`EMFILE`) collects, finalizes and is tried again
  (`posixFreeDescriptors`).

## Imports

The import system is `importlib`, unchanged. `importlib._bootstrap` is loaded from the standard library when the interpreter starts
(`startImportSystem`) and imports the rest itself; `zipimport` follows, whose hook is the first of `sys.path_hooks`, then `site`.
`import` in C++ is what it is in CPython: a look in `sys.modules`, and `_find_and_load` if that fails.

**Frozen modules** are the ones that are frozen in CPython (`runtime/PyImp.cpp` has the table: the import system, `os`, `site`,
`codecs`, `io`, `abc`, `runpy` and what else every start needs, and `__hello__` and its like for tests). `FrozenImporter` finds them
before `sys.path` is looked at, their `__spec__.origin` is `frozen`, their code says `<frozen os>`, and `-X frozen_modules=off`,
`PYTHON_FROZEN_MODULES` and `_imp._override_frozen_modules_for_tests()` switch all but the import system's off. Nothing is
compiled in: `_imp.get_frozen_object()` compiles the file in the directory of the standard library, or takes its `.pyc`, and a
module whose file is not there is not frozen. What a host has written in Python for its own use
(`VirtualFileSystem::hostLibrary`) is frozen too, so it can be imported whatever `sys.path` is, on which its directory is not.

**Modules written in C++** are initialized once for each interpreter: that makes their types and functions, and a namespace that no
program sees (`nativeModuleNamespace`). `_imp.create_builtin()` makes a new module object with a copy of it. So a module that is
taken out of `sys.modules` and imported again is a new object without whatever was done to the old one, as in CPython, and state
that belongs to the interpreter (the codec registry, the warning filters) is not lost with a module object.

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
- The code of frozen modules, which importlib does not read from files, is cached in the same files by C++ (`codeOfSourceFile`).
- Nothing can be written beside the files a host has inside itself (`PyVirtualFiles.h`). The `__pycache__` directories there are
  real directories elsewhere: in a cache directory of the user's that the host names, one for each build (`toCachePath`).

**From JavaScript.** A host imports a module by name for JS with `importModuleFromJS` (`PyExec.h`). Bun does for the specifier
`python:json`, as `node:fs` names a module of Node's: in `import`, `import()` and `require()`, with dots for a module in a package
(`python:os.path`). It is `importlib.import_module()`, so the module is the one in `sys.modules`, and whatever Python can import by
name can be imported so, not only the standard library. The default export is the module; the named exports are its attributes at
that time, but for the `__dunder__` names, as for a `.py` file. What JS catches when the import fails is the exception. A
`ModuleNotFoundError` for the module that was asked for, or for a package it is in, has the `code` that programs in JS look
for: `ERR_MODULE_NOT_FOUND`, or `MODULE_NOT_FOUND` from `require()`. Such a specifier resolves to itself. It is never looked for
among files or packages and never installed. `module.isBuiltin("python:x")` says whether `x` is in the standard library
(`isStandardLibraryModuleName`). The bundler leaves it in place, and refuses it for a target other than Bun.

## Codecs

What CPython has in C is in C++ here: UTF-8, 16, 32 and 7, ASCII, Latin-1, the escapes, charmap, the registry and the error handlers
(`runtime/PyCodecs.cpp`). Everything else is `Lib/encodings/*.py`, unchanged.

**Chinese, Japanese, Korean.** `_multibytecodec` (`runtime/PyMultibyteCodec.cpp`) has the objects that `Lib/encodings` derives its
classes from, and all that has to do with errors, pending input and streams. `_codecs_cn`, `_hk`, `_iso2022`, `_jp`, `_kr` and `_tw`
(`runtime/PyCJKCodecs.cpp`) have the 24 codecs, each a rule for writing the codes of one or more of 13 character sets as bytes.

- The character sets are data. `tools/make_cjk_tables.py` asks the codecs of the CPython whose library is shipped about every
  sequence of bytes and every code point, and writes `runtime/PyCJKTables.inc`: 173 KB of tables from code to character, which
  are used where they lie in the executable. (CPython's seven extension modules take 1.1 MB.)
- The way back, from character to code, is made by turning a table round when something is first encoded with it: 30 to 300 µs
  and 10 to 150 KB for a set, which is kept for good. The tool works out for which characters that is not what CPython does,
  47 in all, and the tables say so.
- They are not built on what Bun has. `TextDecoder` (the crate `encoding_rs`) has the WHATWG's encodings, and its tables are its
  own business: there `shift_jis` is code page 932, `euc-kr` is code page 949, `big5` is a mixture with HKSCS, `gbk` decodes as
  GB 18030, and there is no JIS X 0213, Johab, HZ or ISO-2022-KR. No converter of ICU's is linked in.

## The event loop

The event loop is the host's, and there are three ways to it (`PyExec.h`, `PyEventWait.h`).

- **Waiting.** Every wait for a file descriptor or a timeout (`select`, `poll`, `epoll`, `kqueue`, sockets) goes through one function,
  which the host replaces: Bun watches the descriptor and a timer in its own loop and turns that until one of them fires. So
  `asyncio.run()` and any loop a program makes are asyncio's own, unchanged, and timers, promises and I/O of JS go on while they sleep.
- **Coroutines without a loop of their own**: a module that awaits at the top level, a coroutine that JS awaits, an async generator
  that JS iterates over. They are tasks of one more asyncio loop, which never sleeps: the host's loop calls it when a descriptor
  is ready, a timer is due or a callback has been scheduled (`src/python/_bun_asyncio.py` in Bun). To JS they are promises.
  It counts as running (`is_running()`) between its iterations too.
  For what JS calls, that loop is the running loop where no other is: `get_running_loop()` and `get_event_loop()` give it if a
  frame of JS is on the stack (`loopOfHost`), so `gather()`, `ensure_future()`, `Future()` and `loop.call_later()` work from JS.
  (`_get_running_loop()` does not, so `asyncio.run()` can still be called.)
- **`await promise`** in Python, for a JS promise (type `JSPromise`), is a future of the running loop that the promise settles.
  The promise of a coroutine that JS has called is awaited as the task it is, so to cancel who waits cancels the coroutine:
  `wait_for(queue.get(), 1)` from JS, and `ensure_future(queue.get()).cancel()`. An async function of JS cannot be cancelled.
  A `Task` can be made of a promise, which is all that JS ever holds of a coroutine: `create_task(queue.get())`,
  `TaskGroup.create_task(promise)`. It awaits the promise.
- **A promise is a future** to asyncio (`isfuture()`): it has `done()`, `result()`, `exception()`, `add_done_callback()` and the rest, which the
  host supplies. So `gather()`, `wait()`, `shield()`, `as_completed()` and `ensure_future()` take it as it is and put their callbacks on it
  at once. Wrapped in a task, as any other awaitable is, it would be without a handler until the task took its first step, an
  iteration of the loop later, and a rejection in the meantime would be reported as unhandled. It belongs to whichever loop asks.
  `cancel()` cancels the coroutine behind the promise if there is one, and otherwise refuses.
- **Futures and tasks** are native: the module `_asyncio` (`runtime/PyAsyncio.cpp`), which Lib/asyncio takes in place of its own classes.
  A step of a task resumes the coroutine from C++, outside any handler, and `await future` gets the result without a `StopIteration`
  (`PyAsyncio.h`). The loop stays asyncio's: callbacks go through its `call_soon()`. `all_tasks()` reads a table that the collector
  does not visit, so that it keeps no task alive, and out of which the collector itself takes what it has found dead, when marking
  is over and before anything is freed (`Runtime::WeakCellTable`, `Runtime::reconcileWeakReferencesAtGCEnd()`).
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

## ctypes

`_ctypes` is `runtime/PyCtypes*.cpp`; `Lib/ctypes` is CPython's. What a ctypes type knows (size, alignment, format, how its bytes
are read: CPython's `StgInfo`) is a cell, `PyCTypeInfo`, under a hidden key of the type; a type of one of the metaclasses that has
none is abstract. The layout of a structure is worked out in Python, by `ctypes._layout`, as in CPython.

**Memory.** An instance (`PyCData`) has no memory in itself. It refers to one of three things:
- A `PyByteStorage`: memory of the collector, which does not move. That is what an instance made by calling its type has, and
  what a part of it (a field, an item) shares with it. Nothing is freed by hand and no instance has a destructor: the memory lives while
  any instance refers to it. `resize()` beyond what there is room for gives the instance new memory; parts made before go on using the
  old, which stays valid.
- A memoryview (`from_buffer`). The address is looked up through the view at each use, so it is noticed when the view has been
  released or JS has detached or shrunk the memory. While the instance lives, the view counts as an export: a bytearray cannot be resized.
- An address about which nothing is known: `from_address`, `in_dll`, what a pointer points to. Where a pointer points into the memory
  of something that it keeps alive, which is the rule, the instance made from it holds on to that memory instead.

**What keeps what alive** is as in CPython, for programs look at it: `_objects` of the outermost instance (`_b_base_` leads there)
holds, under keys that say which part they are for, what the pointers in the memory point into: the bytes of a `c_char_p`, the
instance a pointer was made from, a capsule for the `wchar_t` copy of a str.

**Safety.** What a program does with instances, fields, items, slices, `value`, `raw`, `resize`, `from_buffer`,
`from_buffer_copy`, buffers and pickles stays within memory that is alive. Each access checks its bounds against the instance (so a
`CField` applied to an instance of another type is an error), and a value is converted, which may run any code, before the
memory is located. A pointer that points into something it keeps alive (`pointer(x)`, `p.contents = x`, an array assigned to a pointer field,
`cast` of an instance) is checked too: `p[i]`, `p[a:b]` and `p.contents` raise `ValueError` if they would leave the memory of that object, which
also holds for a pointer that was given `contents` of another type than its own (which CPython allows, and which is a cast: bytes that are not a
pointer may then be read as one). What follows an address of which nothing is known is not
checked and cannot be: `from_address`, `in_dll`, `string_at` and the like, an int stored in a `c_char_p` or a `c_void_p`, a pointer whose bytes came from
elsewhere (a union, `memmove`, `from_buffer_copy`, a foreign function), and foreign calls themselves. These can crash the process, as in CPython. So
can changing `_objects`, whose docstring says not to.

**`py_object`.** There is no `PyObject*`. The eight bytes are the bits of the `JSValue`, which for most objects is what `id()`
returns, and 0 is NULL. The object is kept alive by `_objects`. Bits read from memory are handed out as an object if they are those of
what `_objects` holds for that place; otherwise only after the heap has confirmed that they are those of a live cell of a kind that programs
see (`objectFromBits()`). `Py_INCREF` and `Py_DECREF` keep a table of objects that are to stay alive. `ctypes.pythonapi` is the
program itself, which has no C API: its functions are missing (`AttributeError`).

**`cast`, `string_at`, `wstring_at`, `memoryview_at`, `memmove`, `memset`** are, in `Lib/ctypes`, function pointers made from addresses
that `_ctypes` exports. The addresses are real and distinct, and a call to one of them runs the C++ function directly.

**Foreign calls** are behind an interface shaped like libffi's, at the end of `PyCtypes.h`: `callForeignFunction()`, `allocateCallbackCode()`,
`releaseCallbackCode()`, `runCallback()`. What is behind it now, `PyCtypesForeign.cpp`, is a stopgap until it is decided whether libffi is vendored. It generates
no code and has no assembly: on x86-64 (System V) and AArch64 the place of an argument depends only on its class and on how many of that class came before
it, so one call through a prototype that fills every argument register and a stretch of stack stands for every signature, and the work is to find
the place of each argument, structures by value included. That is calling functions through prototypes that are not theirs: the calling conventions
define what happens, C++ does not. Callbacks are taken from a fixed table of entry points compiled into the program. Its limits are in DEFERRED.md.

`CDLL` functions run without the GIL, `PyDLL` functions with it. A callback that arrives on a thread that is in a foreign call takes the GIL back for as long as it
runs; on a thread that holds the VM's lock it just runs; on any other thread (one that C started) it is refused: a message on stderr, and zero
for a result. An exception in a callback goes to `sys.unraisablehook`. The entry point of a callback is free again once the collector has found that
nothing refers to the callback; a call that comes after that is refused too.

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
- The instructions belong to the `UnlinkedCodeBlock`, and so does what is known about the places in them: a `CodeMonitor`.
- Nothing is checked when a function is entered. When the events wanted everywhere change, the code of the calls in progress, of all threads, is
  changed at once. All other code that has a `CodeBlock` goes to sleep: only its entrances are replaced, which are its first instruction and, in the body of a generator,
  the instructions it goes on with after a `yield`. When it is entered it wakes: the places in it are looked for, if they never were, and changed. Code that gets a
  `CodeBlock` later begins asleep. So switching a tool on or off costs one byte for each function that is loaded, and only code that runs is ever analysed.
  What a tool itself calls does not wake.
- Whoever else reads a replaced instruction finds the monitor by the address (a table of the ranges of monitored instructions, for all VMs, with a lock: the
  collector's threads ask too), and the monitor knows the opcode.
- `DISABLE` puts the opcode back, if no other tool wants the place. It holds until `restart_events()`, or until the code is entered, or found running, at a time when the tool
  does not want the event: that is when CPython, which changes code as it is entered, forgets. So code with something disabled has its entrances replaced once more after a change.
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
A function that falls off its end returns on the line of whatever ran last. CPython gets there by copying the implicit `return` for each block that leads to it; here a compound statement
that is the last of the function is told so (`isLast`), and each of its ways out returns for itself (`Unit::leaveStatement()`) or marks the line it leaves on (`Unit::lineOfImplicitReturn()`).
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

**Comparing code.** Code objects are equal if their names, flags, first lines, variables and instructions (as the compiler left them) are, and then their text, or, for code made from a tree,
their constants (by type and bits: `0`, `0.0`, `-0.0` and `False` are four) and `co_positions()`. The hash leaves the instructions out: `bdb` keeps code objects in dictionaries while it traces them.

**Frames.** While something is told of an event, `f_lineno` of that frame is the line CPython would show. `f_locals` reads and writes the registers.
`f_lineno = n`, in a trace function, goes where CPython goes and refuses what it refuses, with its messages: the target must be the beginning of a statement whose blocks the
frame is in already, or can take over (a `for` loop's iterator serves another `for` loop). Variables without a value get `None`, with CPython's warning, because code further on
may have been compiled knowing that they have one.

**Threads.** Trace and profile functions, and whether a callback is running, are the thread's (`Runtime::ThreadLocalState`). Tools 6 and 7 want their events
while any thread has a function. The side tables are only changed with the GIL, and those the collector's threads read, with the lock of the table of monitored code as well.

`sys.monitoring._places(code)` shows the places of a code object, and which are replaced right now (offsets as JSC has them).

## JavaScript sees Python objects

- Properties are attributes (items first, for mappings), through the method table, as for any exotic object of JSC.
- A class is called or `new`ed; `instanceof` is `isinstance`.
- **`class Derived extends Base`**, where `Base` is a class of Python's, is `class Derived(Base)` (`runtime/PyJSClass.h`). When the
  methods are in place and before anything can see the class, the bytecode of a class definition with a heritage hands
  constructor, prototype and superclass to `deriveClassFromJS` (if the superclass is an `InternalFunction`, which a `PyType` is), and
  what that returns is the class from then on: a `PyType`, made by the metaclass of `Base` from a namespace with what the
  prototype and the constructor have. So `__init_subclass__`, `__set_name__`, `ABCMeta` and the like run when they would in Python,
  `issubclass()` holds, Python finds the methods and can derive from the class in its turn.
  A method is a `JSMethod`, a function whose first argument is the `this` of the JS function, so `Derived.method(instance)` works as
  `instance.method()` does; an accessor is a `property`; a static method is a `classmethod`; what has a symbol for a key stays
  with JS, which finds it on the instances.
  The constructor is the `__new__` of the class, since it is what makes the instance, and `__init__` does nothing. `super(...)` in it
  constructs `Base` with `new.target` a class derived from it: that is `Base`'s `__new__` and `__init__` for an instance of that class
  (`typeCall` with `derived`). So `class Day extends date { constructor(text) { super(...parse(text)); } }` works, which in Python
  takes a `__new__`. As with any class that has a `__new__`, a class in Python that derives from it and takes other arguments
  has to have one too.
- **`Class.prototype`** (`PyPrototypeView`) has what the class and its bases have. Read for an instance (`super.method`) it binds as
  `super()` does, looking from the class on in the MRO of the instance. Read by itself it gives functions that take the instance
  as `this`: `list.prototype.index.call(array, item)`. What is assigned to it, or defined on it, is a method or a property of the class.
- **A function written in JS that is found in a class is a method**, as one written in Python is (`foreignMethodKind`): what Python
  gets from the instance is a bound method, and to call that passes the instance as `this`. An arrow function has no `this`:
  it is to JS what a lambda is to Python, and takes the instance first. Bound functions, classes and functions written in C++ are
  not methods (nor are built-in functions in Python). Taken from the class, it is the function as it is.
- What a Python function returns to code that is not Python is made fit for it (`resultForCaller`): a whole float is a number, and
  a coroutine is a promise, so an `async def` is an async function to JS. An awaitable can be awaited, an async generator iterated over with `for await`.
- A call follows the rules of the language it is written in. JS may pass a function more than it takes, and callers of
  callbacks do (`array.map(f)` is `f(item, index, array)`): what a function or bound method has no parameter for is dropped, if
  the caller is not Python, be it written in Python or in C++ (`array.map(len)`). What takes `*args` takes it all, and so do
  classes. Too few arguments are a `TypeError` for every caller.
- In Python, to call a class is to make an instance, so Python calling a class of JS is `new`: what `class` declares, and the
  constructors of the engine and the host (`js.Map()`, `js.Date()`, `js.Response("body")`). The constructors of
  primitive values (`String`, `Number`, `Boolean`, `Symbol`, `BigInt`) make one, as they do when JS calls them. A plain `function` is called; `f.new(...)` is `new f(...)` for anything.
- Keyword arguments for a JS function are a trailing object, and JS passes keyword arguments as `kwargs({ name: value })`, last. The two
  are the same thing: a plain object with a mark that JS cannot see (a property whose key is a private name: `isKeywordMarker`).
  As the last argument of a call that JS makes of something of Python's, an object with the mark is keyword arguments. So
  what a function of JS was given it can pass on: `constructor(...args) { super(...args); }`. A copy (`{ ...options }`) has no mark, and
  when it is Python that passes the object it is an object like any other. (Who calls is told as the callee starts, by
  `isBeingCalledByPython`: the frame it returns to does not tell, since `return f(...)` in strict JS code is a tail call.)
- A dict iterates in JS as a `Map` does, by `[key, value]`.
- **Operators.** `a + b`, `-a`, `a < b`, `a == b` and so on ask the classes of the Python objects among the operands
  (`__add__`, `__radd__`, ...), in the slow paths that all tiers share (`OperationsInlines.h`, `JSValue::equalSlowCaseInline`). If
  neither class answers, JS does what it does with any object. `a == null` is false without asking.
  The parser does not make `+a` of `a * 1`, which it did.
  `===`, `Object.is`, `switch`, `includes`, `indexOf` and the keys of a `Map` or `Set` go by identity.
  To the DFG and FTL a Python object has a type of its own, `SpecPyObject`. An arithmetic operator returns a primitive unless an
  operand may be of that type. `==` on two objects is a comparison of pointers behind a check that neither is of that type
  (`DFGFixupPhase.cpp`): `NotPyObjectUse`, or, where a site has seen plain objects only, or arrays, or functions, the check for that
  kind in place of the check for an object. A site that has seen a Python object calls the slow path instead.

## Python sees JavaScript objects

`runtime/PyInterop.cpp`. Properties are attributes and items, those of JS before what the Python type has. Besides:

- `len()` is `length`, or `size`, or the number of own properties. Of what can be iterated over and has neither it is a `TypeError`.
- The items of a `Map` are its entries (`m[key]`, `m[key] = v`, `del m[key]`, `key in m`), so `dict(m)` copies it. `in` asks a `Set`.
  Iterating over them is what it is in JS: pairs, for a `Map`.
- An iterator that the engine makes (of arrays, maps, sets, strings, matches; iterator helpers) is an iterator: `next()`, and
  `__iter__` may return it. A generator is a generator: `send()`, `throw()`, `close()` (which is `return()`), `yield from`, so
  `contextlib.contextmanager` takes a `function*`. Any other object with a `next` method can be iterated over if it has
  `Symbol.iterator`.
- A function has `__name__` and `__qualname__`, which decorators copy. `copy.copy()` and `copy.deepcopy()` give the function itself.
- The working directory is one: `os.chdir()` and `os.fchdir()` tell the host (`setDidChangeDirectoryFunction`), which keeps its own
  copy of it for `process.cwd()` and for resolving modules.

## JavaScript without Python

JSC and WTF work the same whether or not Python has started: there is no switch, for the process or for a VM, that Python flips.
What code outside `python/` asks is one of these, and the answer is the same in every program:

- **What is this cell?** `isPyObjectType()`, a range of `JSType`; `SpecPyObject` in the optimizing JITs. The operators, above.
- **Is this code block Python's?** `UnlinkedCodeBlock::isPython()`: it is never compiled, its call sites are never virtual, unwinding
  tells the Python runtime of each of its frames, its generators are laid out differently.
- **Which runtimes does this heap have?** `Heap::pythonRuntimes()`, empty without Python. They are asked at three points, none of
  which touches a value of JS: before the stack is scanned (dead temporaries in frames of Python code are cleared), when
  marking has converged (Python objects to finalize are kept), and by the host before `console.log` writes (Python's `stdout` is flushed).
- **Is this the embedder's thread?** `Heap::mayRunEmbedderFinalizers()`: false only while a thread that Python started has the VM.
  See Threads. A program without such threads runs on one thread, and the answer is yes.
- **Is a thread parked in the middle of a call?** `JSLock::hasParkedThreads()`: then code is running although the VM has no entry scope,
  and the texts that strings had before they were made atoms are kept (`Heap::clearConcurrentRetainedDataIfPossible()`): a
  `GCOwnedDataScope` on the stack of that thread may refer to one.

One change to the collector was made for Python and is everybody's gain. The helper threads that mark in parallel used to be woken at
the beginning of every collection and waited for at its end, which takes 0.4 ms and 0.9 ms of processor time with seven of
them, on an idle machine, where a collection of the young generation that finds little takes 0.03 ms by itself. Now they are started
when there is work for them (`Collector::runBeginPhase`, `SlotVisitor::donateKnownParallel`, `donateAll`): at once if the
collection expects to visit 16 MB or more; otherwise when what the conductor has marked by itself, plus eight times what it has on
its stack, comes to 131,072 cells (it marks 30,000 to 60,000 in the time that starting them costs); and whenever it leaves
marking to them to let the mutator run. Nothing in the protocol depends on how many run. Termination is "no marker is active, this visitor and the shared stacks are empty", and a
helper that was never started is not active and owns nothing. What is in a shared stack is always taken by somebody: with the
world stopped the conductor ends in `drainFromShared(MainDrain)`, which steals for itself, and every way into the concurrent
phase goes through `donateAll()`. All visitors are set up in the Begin phase, started or not; the start happens once (an atomic
exchange), never in the End phase; tasks of the constraint solver are run by the conductor too and waited for by
reference count. (Programs compiled ahead of time already did this, and also create the threads one by one.) A loop in JS that
allocates 30 million objects of which 1000 are alive at a time went from 680 to 318 ms, and from 1001 to 348 ms of processor time.

One thing that Python needs of WTF is a setting of the process, `Thread::setSurvivesFailedCreation()`: a program may lower its own
limit on threads (`resource.setrlimit`) and go on, and the collector then does without the threads it cannot start. Bun makes that setting
for every process, as it starts (`JSCInitialize`).

What every program pays, with the sizes of a release build for x86-64:

| Where | What |
| --- | --- |
| `VM` (one) | 72 bytes of 110,984: 56 in `Heap` (the list of runtimes, two more owners of finalizers, a subspace pointer), 8 in `VMTraps`, 8 of padding |
| `JSLock` (one per VM) | 72 bytes: the list of parked threads |
| `JSGlobalObject` | 8 bytes: the pointer to the runtime |
| `UnlinkedCodeBlock::RareData`, `UnlinkedFunctionExecutable::RareData` | 8 bytes each, where there is rare data; `FunctionExecutable::RareData` 16. `CodeBlock`, `UnlinkedCodeBlock`, `UnlinkedFunctionExecutable`, `CallLinkInfo`, `Structure`, `JSString`, `JSArray`, `JSFunction`: nothing |
| `JSType`, opcodes, `SpeculatedType`, `UseKind`, `VMTraps` | 19 of the 256 cell types, 55 of the 255 opcodes, 1 of 64 bits, 1 of 64 kinds, 1 of 32 bits |
| `==` on two objects, DFG and FTL | nothing where the site has seen only plain objects (instances of classes are), only arrays or only functions. Otherwise, for each operand not known to be something else: subtract, compare, branch |
| `==`, `<`, `+`, `-`... in the slow paths | for each operand that is a cell: the same test, before the conversion to a primitive; for `a == a` in the slow path, before the answer |
| `Bun.deepEquals()`, `expect().toEqual()` | the same test for each pair of values compared |
| Untyped arithmetic, DFG and FTL | the result is known to be a primitive only if the operands are known not to be Python objects |
| `x * 1`, `1 * x` | a multiplication: the parser no longer makes it `+x` |
| A class definition with `extends` | a move, and a test of the type of the superclass; if that is an `InternalFunction` (`Map`, `Error`, `Promise`), a call of a function that finds it is not Python's |
| A block or large allocation is swept | one load and branch (`mayRunEmbedderFinalizers()`) |
| The incremental sweeper runs | one load and branch (`hasParkedThreads()`) |
| A call is linked, or is virtual and not to compiled JS | is the caller Python code: three loads and a branch |
| An exception passes a frame | the same, twice |
| Bytecode is generated, a code block is linked or asks to be compiled | a look at `isPython()`: for each loop and jump, each call site, each code block |
| `Instruction::opcodeID()` in C++ | a compare with `op_py_instrumented`. The interpreter itself does not look |
| A collection | three walks over an empty list; four more kinds of metadata looked for in each code block that is visited |
| The mutator leaves a collection to the collector's thread | a lock, and a look whether that thread is there |
| No thread can be started | helpers of the collector are done without, and so is its thread; for any other thread, up to 2 s of waiting for room, a line that names it, and the crash that used to come at once |
| The lock of the VM is taken, released | a store; two loads and branches |
| A tick of the event loop that waits, a turn of it | a call that finds no other threads; a load of a flag |
| The executable | about 10 MB of 134: 4.1 the standard library (compressed), the rest code |

## The command line

`runtime/PyCommandLine.h` has the configuration of the interpreter (`Configuration`, what CPython calls `PyConfig`: one for the
process, filled in by the host before Python starts) and CPython's command line: `readCommandLine()` parses the options and the
`PYTHON*` variables before there is a VM, `runCommandLine()` runs the script, `-c`, `-m`, a directory or archive with a
`__main__.py`, standard input or the interactive interpreter, with CPython's `sys.argv`, `sys.path[0]`, messages and exit statuses.
Arguments, names of files and the values of variables are bytes to the system and `str` to Python by way of `surrogateescape`
(`decodeFilesystem`), so none is lost for not being UTF-8.

`sys.path` is what was put first for the program, `PYTHONPATH` (made absolute) and the directory of the standard library: that of
`PYTHONHOME`, else what the host says. A file `python3._pth` beside the executable replaces all of it and isolates the interpreter,
as in CPython. Without a standard library the command ends as CPython's does, with the path configuration on standard error.
A standard stream whose descriptor was closed when the process started is `None` (`Configuration::standardStreamWasClosed`: a
host may have opened something else under the number since, as Bun does).

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

- **Differential tests** (`differential/`, with a README): some 830 scripts, each with what CPython printed beside it. `bun script.py`
  must give the same stdout, the same exit status, the same last line of stderr. `differential/*.test.ts` run them in Bun's test runner,
  without a CPython; `differential/tools/run.py` runs them by hand, shows differences legibly, and asks a CPython again
  (`--update`). `expected-failures.json` lists what is known to differ, and why; a listed script that passes is a failure.
  `differential/command-line/` does the same for about 130 command lines of `python3`.
- **The grid** (`differential/extra/grid/grid.py`): every method of the built-in types and of many others written in C++, every operator, the
  built-in functions, formats, codecs, `struct`, `math`, attributes, exceptions, calls, special methods that misbehave, containers that change
  under an iteration, arithmetic around the sizes where the representation of an int changes: five million calls on ordinary and borderline
  arguments, each of which has to come out as in CPython, message included. What cannot, by design, is left out there with a comment saying
  why. `differential/extra/grid_*.py` are samples of it (each Nth call, digests of the outcomes) small enough for a debug build;
  `tools/grid_compare.py` and `tools/grid_classes.py` make all the calls under both interpreters, in half a minute with a release build, and show
  what differs, call by call or put in classes; `tools/grid_scripts.py` writes the samples again after a change to the grid.
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
