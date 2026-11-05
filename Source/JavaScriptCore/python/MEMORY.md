# Memory

How much memory programs take here, against CPython 3.15 on the same machine (Linux 6.18, x64, 200 GB), where it goes, what has been done
about it and what is left. The numbers are of a release build without link-time optimization, of November 2026. They are there to be
measured again, not to be believed. "anon" is the anonymous resident memory of the process (`RssAnon`): what belongs to it alone.

## What things cost

`tools/memory.py` makes N objects of a kind in a fresh process, keeps them, collects, and divides the growth of the process by N. It
knows 171 kinds and runs under any Python: `python3 tools/memory.py -o here.json`, the same under CPython, then
`python3 tools/memory.py --table here.json cpython.json`. "live" is what the collector counts for the object: its cells, and the memory outside the
heap that they report. The rest of "anon" is slack: free cells in blocks that are partly full, what malloc rounds up, and what
has been freed and not yet given back.

| bytes for one | anon | live | CPython, anon |
|---|---:|---:|---:|
| `object()` | 41 | 32 | 16 |
| instance, no attributes | 92 | 80 | 80 |
| instance, 3 attributes | 91 | 80 | 128 |
| instance, 10 attributes | 125 | 112 | 192 |
| instance, 3 attributes, `__dict__` asked for | 324 | 304 | 192 |
| `__slots__`, 3 | 92 | 80 | 96 |
| dict, empty | 75 | 64 | 64 |
| dict, 5 str keys | 243 | 224 | 225 |
| dict, 100 str keys | 5927 | 4080 | 3358 |
| set, 5 ints | 790 | 688 | 915 |
| list, 3 items | 61 | 48 | 128 |
| list, 100 items | 965 | 896 | 903 |
| tuple, 3 items | 76 | 64 | 112 |
| str, 8 ASCII | 74 | 24 | 64 |
| str, 100 ASCII | 184 | 116 | 160 |
| str, `a + b` of 8 + 8 | 169 | 80 | 64 |
| str, 20 characters cut out of 10,000 | 1779 | 36 | 63 |
| bytes, 100 | 179 | 160 | 145 |
| int below 2^31 | 0 | 0 | 32 |
| float with a fraction | 0 | 0 | 32 |
| int, 2^70 | 92 | 48 | 48 |
| function (`def` in a function) | 263 | 241 | 178 |
| closure, 1 cell | 333 | 321 | 322 |
| bound method | 60 | 48 | 64 |
| bound method of a built-in | 126 | 112 | 80 |
| code object, small function, defined and called | 5116 | 2247 | 895 |
| class, 5 methods | 4160 | 3209 | 3285 |
| exception, raised and caught | 857 | 704 | 693 |
| generator, suspended | 176 | 160 | 224 |
| generator expression, not started | 786 | 756 | 405 |
| `asyncio.Future` | 170 | 160 | 178 |
| asyncio task that waits, 3 awaits deep | 3010 | 1701 | 1836 |
| dataclass, 3 fields | 89 | 80 | 128 |
| `re.Match` | 176 | 160 | 160 |

Data costs about what it costs in CPython, and small numbers cost nothing. Code costs more. The outliers:

- **Code objects.** About 800 bytes of cells (`FunctionExecutable`, `UnlinkedFunctionExecutable`, `UnlinkedFunctionCodeBlock`, `FunctionCodeBlock`, `PyCode`) and 1,300
  bytes in 16 pieces from malloc (instructions, metadata, `CodeInfo`, expression info, rare data) for the smallest function that has been
  called. One that has not been called has no code blocks.
- **`PyFunction`** is 224 bytes (18 fields), CPython's 168. Most fields are empty in most functions.
- **`a + b`** of short strs is a rope that keeps both halves until somebody looks at the text.
- **A generator expression** makes two functions and four strs each time it is evaluated.
- **An object without attributes** has room for six, which is the least that any class gives. `object()` itself is a cell of 32 bytes.
- **An asyncio task** is as large as in CPython, but takes nearly twice that in the process: cells of many sizes, each size in blocks of
  its own, none of them full.
- **`obj.__dict__`**, once asked for, is a dict beside the object for good: 304 bytes against 192. CPython's shares its keys with the class.

## An empty program

`python3 -c pass`: 55 MB resident, against 11 for CPython, and 44.5 for `bun -e 0`, the same executable with an empty program in JS.

| MB resident | |
|---|---:|
| the executable's code | 23.0 |
| the executable's constant data (most of it ICU's) | 20.5 |
| anonymous memory | 9.3 |
| the C library, the stack and the rest | 3.1 |

Four fifths are pages of the executable, which all processes share. How many of them a program touches is a matter of how the executable
is linked: the Bun that is shipped (1.4.4, with link-time optimization) takes 12.4 MB in all for `bun -e 0`. Python's own part is
some 10 MB: 3 MB of live cells (23,000 of them: types, functions, the modules of start-up), the blocks they lie in, 2 MB of
compiled code of those modules, and the code of the runtime that runs once. Starting takes 14.9 ms, CPython 13.7.

## Programs

| | here | CPython |
|---|---:|---:|
| **pytest, 20,000 parametrized tests**: highest resident size | 570 - 580 MB | 156 - 169 MB |
| live heap at the end | 163 MB | 93 MB in tracked objects |
| capacity of the heap at the end | 309 MB | |
| dicts, lists, sets alive at the end | 266,311, 222,229, 20,734 | 266,478, 221,833, 20,488 |
| **`import sympy`**: anonymous | 122 - 132 MB | 41 MB |
| resident in all | 171 - 182 MB | 50 MB |
| **`django.setup()`** with the admin: anonymous | 96 - 103 MB | 34 MB |
| resident in all | 144 - 151 MB | 47 MB |
| **`json.loads` of 500,000 records**, then a pass over them: highest resident size | 811 MB | 511 MB |
| anonymous after a collection | 536 MB (366 live) | 417 MB |
| **asyncio, 5,000 connections**, both ends in the process: anonymous | 139 - 155 MB (55 live) | 68 MB |

The pytest run took 1,161 to 1,198 MB before the changes below (60 MB of that were ICU's data, which was linked in another way then), with 297 MB
live. It keeps the same objects as CPython now. What is left of the difference: the collector's policy (309 MB of blocks for 163 MB of
cells), what `gc.collect()` itself needs while it runs (75 to 82 MB here; pytest calls it at the end), and the floor.

After an import it is code that takes the room. `import sympy` leaves 26 MB of cells (strs 4.9, `PyFunction` 4.2, dict storage 3.6, `FunctionExecutable` 2.5,
`PyType` 2.2, `UnlinkedFunctionCodeBlock` 1.9, `UnlinkedFunctionExecutable` 1.9) in 48 MB of blocks, some 16 MB from malloc (strs 5, `CodeInfo` 6, code blocks 5), and
**46 MB of copies of `.pyc` files**: a function is decoded when it is first called, so what marshal was given is kept as long as any function of
the module has not been called. CPython's `.pyc` files for the same modules are 17 MB, and it keeps none of them.

Memory that a collection frees goes back to the system, but only a moment later (malloc gives pages back with a delay): a million
objects dropped and collected leave 372 MB anonymous, and 36 MB half a second and a little work later (CPython: 39).

## The collector's policy

The engine collects when the heap has grown by a factor since the last full collection. On a machine with 16 GB or more the factor is
about 4 for a small heap and falls towards 1 as the heap nears the size of the memory (`Heap::updateAllocationLimits`, `proportionalHeapSize`; the memory is what
the container allows, if there is one). So a program with 160 MB of live data may hold 300 to 600 MB. That buys speed. With the
factor at 2 (`BUN_JSC_heapGrowthMaxIncrease=1`; measured before the changes below) the pytest run peaked at 846 MB, not at 911 to 1,197, in the same wall time and 2%
more CPU time, and none of the Python benchmarks lost time, but a program in JS that churns objects took 1.5 times as long. It is the engine's policy for JS and Python alike and
has been left alone. `BUN_JSC_forceRAMSize=1073741824` makes a process behave as on a machine with 1 GB.

## What has been done

- **Structures of objects that lose properties** (a bug in JSC, in plain JS too). A transition that deletes a property was filed under
  attributes 0 and looked for under the attributes of the property. For a property that is not enumerable, as every attribute of a
  Python object is, it was never found: each object that lost such a property got Structures of its own, 112 bytes each, for good.
  `Structure::removeNewPropertyTransition` files it where it is looked for. The pytest run had 1,023,598 Structures alive at the end, and has 2,496.
- **`obj.__dict__`** moves the attributes of an object into a dict for good (`PyInstance::ensureDict`). It deleted them one by one. Now the object goes
  straight to one Structure that all such instances of its class share, and the keys are the one str cell that the VM has for each name
  (`vm.atomStringToJSStringMap`), not a new one for each object: 890 bytes -> 324. pytest, attrs, `copy`, `pickle` and `vars()` all ask for `__dict__`.
- **Parts of strs.** `jsSubstring()` shares the text of the whole, so one word of a file kept the file alive: 10,016 live bytes for 20 characters of
  10,000, now 36. Slices, `split`, `partition`, `strip`, `removeprefix` and the groups of `re` go through `partOfStr()`: a part shares only if it has 256
  characters or more and is at least half of what it would keep alive. A slice of 20 characters takes as long as before, one of
  1,000 half as long again (106 ns; CPython 86), and `s = s[1:]` in a loop copies what is left each time it has eaten half. JS's substrings are as
  they were.
- **`gc.collect()`** looks at how the dead cells refer to one another (`PyFinalizationOrder.cpp`), and kept a table of 4 KB for every 16 KB block with a dead
  cell in it: a quarter of the heap, on top of the heap, while it ran. The table now has an entry for each cell of the block, not
  for each 16 bytes. With a million objects alive and a hundred thousand dead the call needs nothing to speak of, not 55 MB. Where most of the
  heap is dead it still needs 64 bytes for each dead cell, much of it in the old buffers of vectors that grew.
- **What was looked up in a class** was remembered in 16 places in each `PyType`, 544 bytes of 1,776. It is one table of 96 KB for the runtime now
  (`TypeLookupCache`, from malloc, so each Worker has one), which is less from 181 types on; an empty program has 316. It keeps the text
  of up to 4,096 names of up to 100 characters, and nothing else. An empty program takes 15 page faults fewer than before.

## Tried and put aside: mapping the `.pyc` files

Instead of a copy, the file itself, mapped privately and read-only. It was built and measured. After `import sympy` 45 MB of anonymous memory
became pages of the file cache, which all processes share and the system can have back; 31 MB after `django.setup()`, 2 MB at start-up.
**The resident size did not fall**, and import took as long as before. Less than half of the pages are ever read, but the
kernel maps more than is asked for: 64 KB around each fault, and, since it keeps files in large folios, whole folios. On Linux 6.18 one byte
read in every 256 KB of a cached file makes all of the file resident, whatever `madvise()` is told. Against that stands a way to
fail that CPython does not have. To replace or remove a mapped file is harmless, but one that is overwritten in place (`cp -r` of a tree with its
`__pycache__` over one that is in use) is other code from then on, and one that is cut short kills the process with `SIGBUS`. The first can be found out
before each function is decoded, by looking at the header of the file, which has a checksum of the rest, and be made an `ImportError`. The
second cannot. (A checksum for each function does not fit the format: a code block shares arrays with identical ones written earlier, strs lie where
they first occurred, expression info is shared at the end of the file.) A test in Bun's `bytecode-cache.test.ts` holds what is promised today: a program goes on
working whatever becomes of the file.

## What is left, largest first

1. **The collector's policy** (above): the largest part of the difference in every program with much data. Not Python's to change.
2. **The copies of `.pyc` files**: 46 MB after `import sympy`, 2.7 times the size of CPython's files. Two things make them smaller without any risk.
   - **The text** of the module is 27% of a file (13.6 of 49.9 MB for sympy) and the last thing in it (`PythonCodeCacheEntry::encode`), so the copy can stop
     short of it. It has few readers. `codeStartColumn()` and `codePositions()` (`PyFunction.cpp`) look for the starts of lines, which the payload has (`lineStartsForBytecode()`), and
     the length. `sourceLine()` (`PyFrame.cpp`), for tracebacks, can read the `.py` as CPython does. What writes code out again (`marshal.dumps`, `code.replace()`,
     `co_linetable`), `toString()` from JS (`PyInterop.cpp`) and `SourceProvider::hash()` need all of it and are rare: `CachedPythonSourceProvider::source()` can fetch it when first
     asked, from the `.pyc` if its header is still what was read, else from the `.py` if its time and size are those in that header. Where neither
     is to be had, a traceback has no source line, as in CPython without the `.py`, and the code cannot be marshalled. The provider has to
     know the length without the text: offsets into it are kept everywhere.
   - **The records.** `def f(): pass` takes 394 bytes against CPython's 99: its text 17; its name twice, as an atom and as a plain str (`m_name`,
     `m_qualname`), 32; the module's own instructions for the `def` 21 to 59; what it takes to make the function 132, of which `CachedPythonCodeInfo` 92 (four slots for
     strs, seven ints of four bytes, five vectors of eight: nearly all zero or "none"); its code block 152, of which `CachedCodeBlockRareData` 112 (nine
     tables of eight bytes, all empty but `m_pythonMarks`, which every code block of Python's has: two marks of 16 bytes). A generator function is two such
     functions, a class is one and a call. JS's own records in the same file (`CachedFunctionExecutable`, `CachedFunctionExecutableRareData`) have a word of presence bits,
     then only what is there, with numbers as varints. Written the same way: about 220 bytes, 10% of the files.
3. **Code in memory**: what a function costs that has been defined (`PyFunction` 224 + `FunctionExecutable` 128 + `UnlinkedFunctionExecutable` 96 bytes of cells, `CodeInfo` and its
   vectors from malloc once it is called or asked about itself), and once called. The fields of `PyFunction` that are rarely used could move out of it.
4. **What `gc.collect()` needs**: 64 bytes for each dead cell. Counting the dead first (the mark bits say how many) and reserving the vectors would
   leave no old buffers behind.
5. **Ropes**: `a + b` of short strs should be flat.
6. **Room for attributes**: at least six in every instance, more where the methods of the class assign more to `self` (README, "Inline caches"). The
   list could also say that there are fewer: an object without attributes would take 32 bytes, not 80. Cells come in sizes: 80 bytes
   for up to 6 names, 112 for 10, 160 for 16, 224 for 24, 320 for 36, 432 for 50 and more. Names that are assigned only on paths rarely taken
   count like the others, and an object keeps its size when `obj.__dict__` has moved its attributes out.
7. **Generator expressions** make their two functions anew each time.
8. **The compiled standard library** could be in the executable, where all processes would share it and it would need no cache directory.
