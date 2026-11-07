# Python in JavaScriptCore

Python 3.15 as a second language of the engine. JSC parses it, compiles it to JSC bytecode and runs it in the same VM, on the
same heap and the same stack as JavaScript. There is no CPython in here and no translation to JavaScript source: the only thing
taken from CPython is its standard library in Python (`Lib/`), unchanged, which the host (Bun) supplies.

What is not done, and what was decided against, is in [DEFERRED.md](DEFERRED.md). How much memory programs take, and where it goes, is in
[MEMORY.md](MEMORY.md).

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
it behaves as a `Number` object does. `-0` is whole too, so it is the int `0`: JS makes one of `-i` and of `0 * -1`, and `range(-i)` is to
work. Python's own `-0.0` is a cell, like `0.0`. It goes out as `-0` and comes back as `0`; `float(-0)` of `bun:python` is `-0.0`. What
takes the value of a number that may be an int takes it without the sign of a zero (`numberValue`). No fast path of the
interpreter stores a result that is whole, so none can make a `-0.0` that is a number. Nothing converts what JS passes in, so a
`-0` that Python only keeps or passes on is `-0` when JS sees it again; `round()`, `abs()` and `int()` give `0`.
(A decision that can still be changed.)

**str.** A `JSString`, so UTF-16 where it is not Latin-1, while Python counts code points. A text without surrogate pairs is indexed
directly. C++ borrows the text of a str while it works on it (`StrView`, `JSString::view()`) instead of retaining it: the reference
count of a `StringImpl` is atomic, and two such operations cost more than most methods of str. Whether there are pairs is seen by
looking for a unit that is a second half, eight at a time. `StrIndexCache` (16 entries for each runtime, each holding its
`StringImpl`, all emptied after each collection, nothing for texts shorter than 48 units) remembers the answer, and for a text with
pairs their number and the offset of every 64th code point, so that `s[i]` in a loop does not go through the text each time. (A
`StringImpl` has no bit left to remember it in.)

**list.** Any `JSArray` is a list, so a list is not always values in a row. An array that has held nothing but int32s, or nothing but
numbers, is stored as int32s or as doubles: what `[]`, `[1, 2]` and a comprehension make begins so, as in JS, and `JSArray::push`,
`op_py_list_append` and `op_py_set_item` keep it so. (In an array of doubles a whole number is an int. A float that is whole is a cell, and
makes it an array of values.) JS also makes copy-on-write
arrays, ones with holes, sparse, frozen, with accessors for indices, with any length below 2^32; and once `Array.prototype` has an accessor
for an index, *every* array of the realm, also the next one that Python makes, has `ArrayStorage`, and `a[i] = v` and `push()` of JS hand the
value to that accessor where the array has nothing of its own at the index. The items of a list are nobody else's: there they are
defined (`listSetSlow()`, `listPush()`), and a hole is read as `None` without asking. So nothing outside `PyList.cpp`
touches a butterfly. There, what changes a list asks `ListStorage` whether the items are values in a row, which makes them so if they are
numbers in a row. What only reads asks `listValues()`, or `hasItemsInARow()` and then `copyItems()`; from outside, `listCopyTo()`. If they are not, each item goes
through what works for any array. What is done to a whole list asks once, works on the row and has one barrier. A hole, which reads
as `None`, stays a hole where the row is copied in one piece (`a[:]`, `+`, `*`, `extend`, `copy`, `list(a)`) and becomes `undefined` where
items are taken one by one (`a[::-1]`, `b[1:1] = a`): to Python that is the same, JS sees `1 in result` false or true. C++ elsewhere
gets the items one after the other from `Items` (`PyAbstract.h`: any iterable) or all at once from `AllItems` (`PyList.h`).
A new list is made by `newListOf` or `tryNewListFilledBy`, with what `constructArray` uses, and is an array of values. (`PyListObject::create` lays out an empty butterfly, and `listSort`
leaves a mark in one after looking at its shape.) The limit on the length of items in a row is `ListStorage::grow`'s; any other array
may be as long as 32 bits allow, so whatever takes room for each item looks at the length first. The fast paths of the
interpreter look at the indexing type first. `differential/crashers/evil_js_arrays.py` does everything to all of them.

**tuple, dict.** A tuple remembers its hash once it has been asked for. A dict keeps its entries in an array in insertion order, with an
index beside it; it knows where its first entry is, and can leave room before it, so that taking and adding entries is as cheap at the front
as at the back. `collections.OrderedDict` (`PyOrderedDict.cpp`) is that and nothing more. Each entry keeps the hash of its key, whole, so
that `set(d)`, `dict.fromkeys(s)`, `s - d` and the like never ask a key for its hash again, and so that no key is asked whether it equals
another unless all 64 bits agree, which `__eq__` can tell. A dict with none but str keys keeps 32 bits for each, of another hash (below).

**Hashes.** `hash()` of a str, of bytes, and of what is hashed as its bytes (a `memoryview`, a `date`, a `time`, a naive `datetime`, the
code of a compiled pattern) is CPython's (`PyHash.cpp`): SipHash-1-3 over the bytes that CPython has in memory, which for a str is one, two
or four for each code point, however the text is kept here. The key, 128 bits, is drawn when Python first starts in the process, or made
from `PYTHONHASHSEED` as CPython makes it; all threads and VMs of the process have the one. So whoever chooses the keys of a dict, as the
sender of a request does, cannot choose keys that collide. And with one seed a str has the same hash here and there, and a set, whose table
is sized and probed as CPython's is, the same order (but for some displays of constants: DEFERRED.md). `-R`, `sys.flags.hash_randomization`
and `sys.hash_info` say what they say there. A `__hash__` written in C++ returns an `int64_t` (`PY_HASH`). The built-in `hash()` makes an
int of it and nobody else does: few hashes fit a number, and a `JSBigInt` is an allocation.

JS pays nothing for this: not a line of WTF or of JSC outside `python/` has to do with it. A `StringImpl` remembers a hash of its own, 24
bits, without a key, the same in every process. Neither it nor a `JSString` has room for 64 bits more, and none was made: 8 bytes in every
`StringImpl` are 10 to 13 % of a heap of short strings. So the hash of a str is worked out every time it is asked for: some 90 instructions
for up to seven characters, 19 more for every eight, 0.7 cycles a byte for a long text. (bytes, tuples, frozensets and datetimes remember
theirs in the object, as CPython's do.) No table on the side remembers it either. One was tried: 1,024 entries by the address of the cell,
emptied at each collection. It makes `k in s` with the same strs over and over cost what it cost when the hash was the `StringImpl`'s, and a
str that is not in it 25 instructions more. Whole programs (the test suites of pygments, jinja2 and networkx, `python -m rich`, 60 imports)
gained 0.04 to 0.3 %, for in them all the hashing of strs there is comes to 0.15 to 0.5 % of the instructions. What is felt without it is a
long str that is hashed again and again other than as the key of a dict of strs: 1,000 characters take 0.18 µs each time, 100,000 take 17.5.
Thirteen whole programs were counted (below). In twelve of them all the hashing of strs comes to 0.1 to 0.8 % of the instructions, a sixth
of that at most for strs of 64 characters and more. The thirteenth is the test suite of pyparsing, whose cache of results has the whole
input in each key: 1.4 million hashes of strs of 64 to 999 characters and 195,000 of longer ones, by these costs 4 % of its instructions.

**By what a dict places a str.** Most programs hash few strs because dicts do without. A dict that holds nothing but strs (exactly strs:
what CPython has a kind of table of its own for) places them by the hash that the `StringImpl` remembers, which costs a load, and keeps 32
bits with each entry. Four things make it go by `hash()`, for good (only `clear()` makes a dict what it was at first):

- It is given a key that is not exactly a str. Such a key is placed by `hash()`, and may be equal to a str. The table is made anew, as
  CPython's is at that moment.
- It is asked for a key that might say it is equal to a str and hash like it: an object whose `__eq__` or `__hash__` is not written in C++,
  or a `mappingproxy`, which passes both questions on (`TypeFlag::AnswersForAnother`). (Any other key cannot be in a dict of strs and is not
  looked for. An instance of a class derived from str that has str's `__eq__` and `__hash__` is looked for as the str it holds, if the hash
  that comes with it is that of the str: a set may have kept another from before the class was changed.)
- A search gets too long: more than 64 slots, where a slot with another str that has the same 24 bits counts for 17. Chance does that to one
  search in 10^11. Keys that were chosen to collide, which is easy with a hash that is the same everywhere (one core finds such a key every
  70 ms), do it with the fifth. The search for a free slot counts as well, when a key is added and when the table is made anew, larger or
  smaller: nothing that a dict does with keys placed by that hash goes on for longer.
- Its table gets 2^21 slots, which is at 699,051 entries: 24 bits then tell too few keys apart. Up to there going by `hash()` gains nothing;
  a dict of 1.4 million strs is made in 72 % of the time, read in 78 %, asked for what it has not in 56 %.

After the first two the table has room for 64 bits with each entry and keeps the hash of every key whole, strs included
(`keepsWholeHashes()`). After the last two it is still a table of strs, and keeps the 32 bits that `hash()` folds to, until one of the first
two comes.

In the last three cases the entries stay at their places, no key is compared or asked for its hash, and no code of the program runs. So they
can come in the middle of a lookup, of an iteration, of an `__eq__` that looks into the same dict, and what goes by the place of an entry
(iterators, the caches of globals and of attributes of modules, in the interpreter too) is not disturbed. A copy is what its original is.
With 8,000 keys that collide, making a dict took 220 ms and takes 0.7 (ordinary keys: 0.6), looking them all up 187 and 0.5, `json.loads`
447 and 2.1, `Counter` 469 and 1.0, `urllib.parse.parse_qs` 410 and 6.9, `pickle.loads` 233 and 1.3, a set 482 and 0.8; 130 dicts of 60 such
keys each, 4.5 and 1.0.

What it costs: instructions for one operation with the loop around it, before and now. In a dict of strs `d[k]` is shorter than it was, 446
and 377 (24.1 ns and 20.4), `d[k] = v` 412 and 343, because a str no longer goes the way of all keys. In a dict that keeps whole hashes a
str is hashed each time it is looked for: `d[k]` 422 and 518, `d[k] = v` 394 and 505; with strs of 64 characters 416 and 690, of 1,000
characters 412 and 3,273. Other keys gain: `d[i]` 455 and 432, `d[obj]` 533 and 428, `d[(1, 2)]` 990 and 839. Dicts that hold strs and other
keys are rare. Counted in the test suites of attrs, jinja2, pygments, networkx (four directories), sympy (core), pyparsing, more_itertools
and toolz, in black formatting three packages, a small django program (a model, queries, forms, templates), pyflakes, pycodestyle and isort
over jinja2, `python -m rich` and 60 imports: 13.7 million dicts got a table, 2.07 million of them for keys of any kind, and 18,543 came to
hold both (15,030 of these in django, which never looked for a str in them); 24 were asked for a key of the program's while they held strs
only. Of 612 million searches for a str 174,335 went to such dicts, 0.03 %; most in networkx, 0.3 % of its searches, by the costs above 0.02
% of its instructions. The programs as wholes: 0.5 to 1.5 % fewer instructions in seven of them, 0.4 to 1.7 % more in more_itertools,
networkx and sympy (two runs of the last two differ by as much, and sets in another order take other ways through a program), 0.6 % more in
pyparsing, whose dicts gain what its long strs cost (above); `python -m rich` and toolz are too short to tell.

**Where a dict has its keys.** A program can tell which of its `__eq__` and `__hash__` a dict calls, in what order and how often; with keys
whose hashes have the same low bits that depends on where each key lies in the index, and so on everything that has happened to the dict. So
a table with whole hashes is CPython's in all that shows (measured on 3.15.2, by `sys.getsizeof` and by logs of the calls; none of it is in
the language reference):

- The slots a search goes through: `SlotSequence`, the same for all tables here.
- The very object is found where the search comes by it, whatever hash was kept for it; any other key is asked only if all 64 bits agree,
  stored key on the left. After an `__eq__` the search starts again if the table, the key of the entry or the slot is another.
- The size: 8 slots at first; a table takes two thirds of its slots in insertions, whatever has become of the entries (one into a slot that
  a deletion left counts too; `popitem()` gives back the entry, not the insertion), and is then made anew for three times the keys it has,
  which may be smaller. A new key goes into the first slot on its way that is empty or was left by a deletion.
- A key is looked for first. Only if it is then added, and is the first that is not a str, is the table made anew for that.
- `copy()` takes the table as it is, with what deletions have left in it, if at least two thirds of its entries are in use; otherwise the
  keys are put in one by one, and compared. `update()`, `|=`, `dict(d)` and `{**d}` take the table into an empty dict if it has no deleted
  entry and half its size would not do. Otherwise they make room once, for the keys of both, if a table of this size could not hold those of
  the other. Keywords are merged as a dict of them would be, but by `dict()` itself, which stores them one by one. `dict.fromkeys()` makes
  room for all the keys of a dict or a set, and for a set a table for keys of any kind. (So `d.copy()`, `dict(d)` and `{**d}` of 20 keys
  take 970 to 1,170 instructions, where they took 3,400 to 3,700.)

Random programs of 15 to 300 operations on dicts whose keys log these calls: 66,000 on one dict, of which 48,873 logged other calls than
under CPython before and none does now; 23,000 on two dicts, with everything that takes the keys of one into the other or into a set, 13,382
before and 4 now.

A dict that never held or was asked for anything but strs is not where CPython has it, and nothing can tell: no code of the program runs in
its searches, and its order is that of insertion. What is left is in DEFERRED.md, "Which keys a dict or a set asks".

**What a fast path through a dict may assume.** (For whoever writes one, in the interpreter or in a compiler.)

- *The table.* `PyDict::m_storage` is null as long as no room has been made, and after `clear()`. (That there is a table does not say that
  there has been an entry: a display and `dict.fromkeys()` make room first.) A `PyDictStorage` never changes its size or its width. In it
  lie, one behind the other: `entries()`, key and value, `capacity()` of them (two thirds of the slots), in the order of insertion,
  `entryCount()` in use, a deleted one with an empty key; the kept hashes, one for each entry (`hashAt(index)`), `uint32_t` if
  `!hasWideHashes()` and `int64_t` otherwise; the index, one number for each of the `1 << log2Size()` slots (`indexAt(slot)`), of 1, 2 or 4
  bytes by `indexWidth(log2Size())`: the index of an entry, `emptyIndex`, where a search ends, or `deletedIndex`, where it goes on.
- *What is kept*, by the three flags of the dict. `!m_isKeyed`: every key is exactly a str and no rope, the table is narrow, kept is
  `StringImpl::hash()`. `m_isKeyed && !keepsWholeHashes()`: the same, kept is `foldHash(hash(key))`. `keepsWholeHashes()`: the table is wide
  and kept is `hash(key)` for every key. Of these only `m_hasOnlyStringKeys` says something about the keys: a wide table may hold nothing
  but strs.
- *The way.* `SlotSequence(kept, mask)`: the first slot is `kept & mask`; then `perturb >>= 5; slot = (slot * 5 + perturb + 1) & mask`,
  where `perturb` starts as all 64 bits of kept, unsigned.
- *What needs no code of the program:* a search for a str that is no rope in a dict with `hasOnlyStringKeys()`: texts are compared. A search
  for an int32 or a double in a wide table, as long as each entry it comes by has another kept hash, is the same value bit for bit, or is a
  number too. In all other cases a key with the same kept hash has to be asked (`richCompareBool(stored, key)`): leave that to `find()`.
- *What has to be counted:* the steps of a search while `!m_isKeyed`, as `findString()` does. That count is all that stands between chosen
  keys and a search through thousands of slots. A path that gives up after a few slots and calls `find()` need not count.
- *After code of the program may have run* (an `__eq__`, a `__hash__`): read again `m_storage`, the flags, the key of the entry and
  `indexAt(slot)`; if any is another, start again. `find()` itself may replace the table without running any code (the second case above) or
  rebuild the index (the third), so nothing read before a call of it holds after.
- *Adding* goes through `insertNew(vm, key, kept, value)`, with kept for the state the dict is in at that call.
- *Where the 64 bits of a str come from in a wide table:* they are worked out at each `d[k]`, `k in d`, `d[k] = v`: some 90 instructions for
  up to seven characters and 19 more for every eight, so `d[k]` with the loop around it takes 518 instructions where a dict of strs takes
  377, 690 with keys of 64 characters, 3,273 with 1,000. Neither the `JSString` nor the `StringImpl` has room for them (above).

A set has no two ways. It goes by `hash()` from the start, because its order can be seen: whoever sets `PYTHONHASHSEED` to have runs repeat
gets the order that CPython gives. The price is the hashing: `k in s` 447 instructions with the loop, then 487 (25.3 ns, 28.2), `set()` of
20 strs 6,956, now 7,720. (`k in s` is 399 since `in` asks a set or a frozenset itself, in `containsInline()`, without the lookup and the
call of `__contains__`. Of 62 million `in` and `not in` in the thirteen programs above 32 % were asked of dicts and 31 % of sets.) And a
dict of strs has no hashes to hand on: `set(d)` with 20 keys 3,929, now 6,397, `d.keys() | e.keys()` 10,114, now 13,540. That is also what
two ways for sets would save, in whole programs less than the 0.1 to 0.8 % above. `dict.fromkeys(s)` makes a table for keys of any kind, as
CPython does, whatever is in the set (10,117, now 13,906, and a str is hashed whenever it is looked for there).

The tables of `pyexpat` that a document fills (entities, element types, prefixes, the attributes of an element) are WTF's, which left to
themselves go by the `StringImpl`'s hash; they are given `KeyedStringHash`. (CPython salts expat's tables from the same key.) 6,728
attributes with names that collide took 412 ms and take 3.8.

What is not covered are names: see DEFERRED.md, "Names chosen to collide".

**Names that are not exactly strs.** The name of an attribute is a `JSString`, and whatever looks an attribute up takes one: the
instructions and their caches, `getAttr()`, `PyType::lookup()`. In CPython a name is a key of dicts, and `getattr()`, `hasattr()`,
`setattr()`, `delattr()` and the methods behind them may be given an instance of a class derived from str, whose `__hash__` and `__eq__`
then say what it finds. So whoever is given a name as a value asks whether it is exactly a str (`attributeName()` for the text, then
`isString()`), and if not goes another way: `getAttrByKey()`, `setAttrByKey()`, `genericGetAttrByKey()`, `genericSetAttrByKey()`,
`PyType::lookupKey()`. (A `__getattribute__` or `__setattr__` written in C++ calls `genericGetAttrAsGiven()` or `genericSetAttrAsGiven()`;
one that passes the question on to another object, `getAttrAsGiven()` and its kin.) They are the functions for plain names compiled a second
time (`nameIsKey`), so the plain case runs the code it ran. As measured on 3.15.2: the classes are searched with one hash for all of them,
and what the name raises there is lost; the dict of the object is searched with another, and what is raised there comes out; an instance
that has its attributes as properties is given its dict first; `__getattribute__`, `__getattr__`, `__setattr__`, `__delattr__` and the
`__getattr__` of a module get the name itself; it is what `setattr()` stores, and what an `AttributeError` has as its `name`. The name is
hashed before the classes to search are taken, so a `__hash__` that changes `__bases__` is in time and an `__eq__` is not. If the name gives
the object another class on the way, a descriptor hears of the new one and the message names the one that was searched. A `KeyError` out of
the dict of the object, in a store or a deletion, becomes the `AttributeError` for an attribute that is not there, whoever raised it; an
`AttributeError` out of it, under `getattr()` with a default or `hasattr()`, says "not in the dict", and the class is still to come. (Both
hold for a plain name too, where a key in the dict raises.) `operator.attrgetter()` and `methodcaller()` keep such a name, unless it has a
dot in it. `threading.local` asks first whether the name is `'__dict__'`, and unless its class is derived it looks into the dict of the
thread before anything else, so that a `'__class__'` in that dict wins, for plain names too. (That dict is not the `__dict__` of the
object, here as there: to `object.__getattribute__()` and its kin a local has no attributes, and an instance of a derived class those of a
dict of its own. The methods of `threading.local` search and store with `genericGetAttrWithDict()` and `genericSetAttrWithDict()`.)
`type.__setattr__` and `type.__delattr__` take the text. `super()` asks each class with a hash of its own, and what is raised comes out. And
a dict that has got such a key is no dict of strs any more, so a plain name is then looked for in it as any key is, in a module too. The
plain case pays a test of the tag of the value, and gets more back, because `attributeName()` now has its common case inline: instructions
with the loop around them, `getattr(o, 'x')` 696 and 683, `hasattr(o, 'x')` 596 and 580, `setattr(o, 'x', v)` 689 and 655,
`object.__getattribute__(o, 'x')` 539 and 523; `o.x`, `o.x = v`, `C.x`, `module.x`, `o.m()`: the same to the instruction. (`super().m()` 976
and 978, `super().x` 776 and 780: it looks at one byte more in each class it comes by, because it passed over keys that are no strs in the
dicts of classes, which may say that they are equal to the name.) What is left is in DEFERRED.md, "Names that are instances of classes
derived from str".

As the name of a keyword. A call hands over the names of its keywords as a tuple of strs. Where they are the keys of a dict (`f(**m)`) and
some are not plain strs, the tuple has two hidden fields: the keys, and the hashes that the dict remembers for them (`keywordNamesOf()`,
`keywordKeys()`). A function written in Python asks such a key whether it equals each parameter, and its `**kwargs` gets the key. For what
is written in C++ it depends on what CPython hands its counterpart. One that gets names and values stores them one by one, and the key is
asked for its hash again: `addKeywordsTo()`, for `dict()`, the class statement (which takes `metaclass` out of that dict), a call of `type`
itself. One that gets the dict of the keywords copies or merges it with the hashes it has: `mergeKeywordsInto()`, for `d.update()`, the
classes derived from dict, `SimpleNamespace`, `functools.partial`, `threading.local`, `atexit.register`; `str.format()` looks its fields up
in that dict. The key of `lru_cache` has the keys. A node of `ast` asks the key what CPython's asks (is it among the fields; out of the set
of those still to come; `setattr()`). A call of a `partial` with keywords of its own looks each of the caller's up among them, stores it in
a copy if it is there, which looks again, and lets it follow as it is if not. None of this is on the way of a call whose names are plain.

**Built-in types.** A `TypeDef` in the source file of a type says what is particular to it. What follows from how CPython makes the
type is added by `PyType::initializeBuiltin` and `addMethods`, so that `vars(T)`, `help(T)` and what goes by them
(`functools.total_ordering`, `functools.wraps`, `copyreg`) see what they see there. A type with a method for one rich comparison has
methods for all six. The rest goes by the table of CPython's docstrings (`PyDocstrings.inc`, made by `tools/make_docstrings.py`),
which says what each type has in its namespace: the docstrings; a wrapper of the type's own for what it does as `object` does
(`deque.__getattribute__`); and `__module__`, which the types that CPython makes at run time (most of those outside `builtins`)
have there, where their instances find it as well. `TypeFlag::Mutable` is for the few of those whose attributes can be set: the exceptions of
modules (`struct.error`), struct sequences outside `sys`, `ast.AST`.

**Who makes an instance.** As in CPython (`tp_new`) that is state of the type, with a history, and not worked out from the dicts:
`PyType::maker()`, the `__new__` of a built-in type, or `looksUpNew()`, or neither ("cannot create"). A built-in type has its own
`__new__` or that of its base; one derived from `object` that has none has no maker. A class takes what `__base__` has, the base that
decides the layout. Then, and whenever `__new__` is set or deleted through `type.__setattr__` (in the class, and down the classes derived from it
as far as they have none of their own) or `__bases__` is assigned to, `__new__` is looked up: the `__new__` of a built-in type, whichever, means "as it
was"; anything else, "look it up at each call", which stays so whatever is found later; nothing at all (an `mro()` without `object`), "cannot create". So `class C(tzinfo, dict)` is made by `dict.__new__`, and so is
`class C(dict): __new__ = object.__new__`; after `C.__new__ = f; del C.__new__`, `C(1)` is refused by `object.__new__`; what is written into the
dict of a class behind its back is not seen, unless the class looks `__new__` up. `T.__new__(S, ...)`, `Reflect.construct(T, args, S)` and `super(...)` in a class of JS are one function,
`PyNativeFunction::makeInstance`, which has CPython's test for what is "not safe": `Reflect.construct(object, [], D)` is refused for a class D of JS derived from `int`, as `object.__new__(D)` is. (For the two from JS, as where a class is made, the `__new__` of a built-in type
that is found stands for what has come along `__base__`: to T, or after `super(...)` to the class of the instance, where nothing is left to test: the classes on the way that look `__new__` up and find a
constructor of JS are passed over. If one of them finds anything else, the one that was found is asked as Python would ask it.) Nobody else hands the C++ function of a type a class. So it
gets the classes that it came to along `__base__`, and no others, whatever the dicts say. `__init__` has no such state: it is looked up.
The `mro()` of a metaclass may name any class: that is why a type written in C++ whose instances have anything of their own needs `TypeFlag::OwnLayout`: without it, its methods can be
handed an instance of a class that merely names it.

## Code

The compiler emits ordinary JSC bytecode where the semantics agree (moves, jumps, calls, exception handlers, generators through
`BytecodeGeneratorification`) and about two dozen `op_py_*` instructions where they do not (attribute access, operators, name
lookup, iteration, unpacking, argument binding). `op_py_runtime` calls one of a table of runtime functions for everything rare.
Python code runs in the LLInt only; it never tiers up (`shouldJIT`).

A forward jump is written with a distance of 0 and gets its distance when its label is emitted. If the label never were, the jump would lead to itself: a loop that polls nothing and that
Ctrl-C cannot end. So the generator counts the jumps of Python code whose label has not been emitted (`bindLabel()`, `didEmitLabel()`), and a unit at whose end there are any is not run: the
debug build asserts, the release build raises `SystemError` from `compile()`.

**The second page of opcodes.** An opcode is a byte in the instruction stream, and JS and Python together had used 251 of the 254 there are. So there is a prefix, `op_page2`, in the manner of
`op_wide16` and `op_wide32` (and behind them where there is one: `[wide16][page2][number] operands`): the byte that follows it numbers one of up to 256 more opcodes. Their ids in C++ come behind
all the others (`firstPage2OpcodeID`), so the interpreter's three tables serve them too: the handler of `op_page2` is `nextInstruction()` one byte further on, into the same table further down.
Which opcodes are there is a list of names in `bytecode/BytecodeList.rb` (`on_page2`); their definitions stay where they are. The generator gives every struct a constant `prefixBytes`, 0 or 1, which
`decode()`, the setters, the interpreter's six accessors of operands and its three lengths of dispatch add; `Instruction::opcodeID()` and `size()` resolve the prefix out of line. Nothing else knows
which page an opcode is on: not its handler, its slow path, `BytecodeUseDef.cpp`, the dump, or tracing's tables. An instruction of page 2 is one byte longer and takes three machine instructions more.
On it are 17 of Python's opcodes that are rare: the six comparisons that are not followed by a jump, seven of the twelve that are, `py_unary`, `py_floor_div`, `py_store_cell`, `py_spread_now`. None of JS's. What has metadata or
checkpoints stays on page 1 (the id indexes tables), and so does `op_nop`, which fills single bytes. The rule for a new opcode is in the list's comment, with how to put nearly all opcodes on
page 2 for a test of the mechanism.

**Fast paths.** Each `op_py_*` instruction has a slow path in C++ that does all of it (`runtime/PySlowPaths.cpp`), and most have the
common cases in assembly (`llint/LowLevelInterpreter64.asm`, "Python"): arithmetic and comparisons of numbers, comparisons fused
with the jump that follows, truth of the usual values, items of lists, tuples and strs, iteration over ranges, lists and tuples
from the making of the iterator to the end of the loop (the cell is allocated as the AOT interpreter allocates: `allocateCell`),
unpacking, cells, the binding of arguments when they are simply the parameters, appending to the list of a comprehension.

**Inline caches**, in the metadata of the instruction, as for JS:

- _Globals and builtins._ A dict that code has cached from has a number that no other dict, and no other set of keys of the
  same dict, ever has (`PyDict::watchedVersion`). The instruction remembers the number and the index of the entry.
- _Attributes_ (`AttributeCache` in `runtime/PyAbstract.h`). The attributes of an instance are properties, so its Structure tells which it
  has and where. The object itself has room for six, or for as many as the methods of its class and of the bases assign to `self`
  (`__static_attributes__`, which the compiler lists; at most 50 with the slots, rounded up to what the cell has anyway): `PyType::createInstanceStructure()`. JSC sizes what `new F()` makes
  by the assignments to `this` it sees (`StaticPropertyAnalyzer`, `ObjectAllocationProfile`). It is a size, read when the first instance is
  made, and names that a slot, a property or another data descriptor takes do not count: a wrong one costs memory (room for names that an instance does not set: `MEMORY.md`) or time (storage on the side, which the
  instruction does not allocate), nothing else. CPython gives every such instance room for 30. A type has a number too, new whenever it or one of its bases changes (`PyType::version`). An instruction
  remembers Structure, version and offset, or the value found in the class, or the method to call. `obj.__dict__` moves the
  attributes into a real dict, for good, and gives the object a Structure that says so (and so do `vars()`, `hasattr(obj, '__dict__')`, `copy.copy()`
  and pickling, of what is copied too, `inspect.getmembers()`); an instance of a class derived from `dict`, `str`, `int` has its attributes in a dict
  from the first, which is a property of it. Of such a dict nothing is remembered, neither an index nor a version: it is asked at each read. What is
  remembered is the half that the class says: that nothing in it comes before the dict, and what the attribute is if the dict does not
  have it (`AttributeCache::dictThenClassValue`, `dictThenUnboundClassValue`). These are two more values of the field that says what kind of thing the
  one entry of a site holds, as JS's `GetByIdModeMetadata` is one entry in one of several modes (`bytecode/GetByIdMetadata.h`); Structure and version
  vouch for them as for the others, and the collector forgets them with the others. The interpreter leaves them to the slow path
  (`findBehindDict()`), which compares the two, sees that the dict has nothing but strs for keys (otherwise a key might run code: the long way) and
  looks the name up: 276 instructions for `obj.v` where the long way takes 425 and CPython 230. Such an object takes the one entry of the site
  like any other: where such objects of two classes come by in turn each costs about 80 more than the long way did, and objects without a dict that
  alternate with them miss too (0 to 0.16 % of all reads in six programs, against 2 to 13 % that are cheaper). Only those two answers of the class are
  remembered. Where it says something else, the read goes the long way, which is 30 to 56 longer for such an object than it was, the part about the class being
  behind a call: a method that is read and not called 691 -> 733, a staticmethod called 762 -> 818, a classmethod 834 -> 889, a value in the dict under a
  name where the class has a descriptor without `__set__` (`functools.cached_property` after the first read) 545 -> 594, an attribute of an exception 497 -> 527. What only reads the attributes
  still does not ask for the dict (`dir()`, `PyInstance::forEachAttribute`).
  One object is remembered. Where objects of two classes come by in turn, or of one class with their attributes set in
  another order, each is looked up anew, and once: `describeAttribute()` finds the attribute and says where it is in the terms of
  the cache, and whether that holds for all objects alike or for this one now (its Structure is its own, its class has no
  number left, what the class has may become another kind of descriptor), or that there is none. The slow path (`findAttribute()`) reads through the description and remembers it if it holds for all, so an
  object of which nothing can be remembered leaves what is remembered as it is. Where the attribute is what a descriptor gives for the object (a
  method that is read and not called, a built-in getter such as `__class__`, a `__get__` written in Python), the description names the descriptor and
  the slow path asks it (`getAttrFromDescriptor()`): nothing of that is remembered, but the walk is not made a second time. (So JS: one
  `getPropertySlot()` gives the value and says whether it may be cached.) The rest goes to `getAttr()`: a `__getattribute__` of the program's, and a
  descriptor in a class that has a `__getattr__`, which is asked if the descriptor raises `AttributeError`.
  The description comes before the lookup, not after one that has succeeded, so it has to keep what the lookup keeps:
  - Whatever `genericGetAttr()`, `getMethod()`, `typeGetAttr()` and `bindDescriptor()` check or raise before they return is checked, or the
    attribute is not described: a slot belongs to the class or a base of it; a method of a built-in type is bound first where the class
    has a `__getattr__`; a data descriptor, a `__get__` written in Python, a class method of a built-in type are named, and asked afterwards.
  - No code of the program has run when the description is there. Only one thing in the walk can run any: a key that is no str in the dict of a class
    (`type('C', (), {key: 1})`) is asked whether it equals the name. `PyType::lookupSlow()` counts that (`PyType::timesLookupRanCode()`), and
    a description during which the count has moved is thrown away: the class, the object or the frame may have changed. (JS's lookups say
    the like through the `PropertySlot`: `disableCaching()`, `isTaintedByOpaqueObject()`.) So for stores and for the methods of `with`.
  - The Structure of the object is read when the last lookup that could run code is over: the compiler may keep no more of it over a
    call than its 32-bit ID, which the collector does not know.
  - The number of the class is taken after the walk, which is right only because no code has run.
  - What is remembered is checked at each use by the handler; what is not remembered is used at once and dropped.
  An instance whose Structure has become a dictionary (many attributes deleted) is flattened first, once, as JSC's own slow path does
  for `get_by_id` (`llint/LLIntSlowPaths.cpp`).
  A store (`op_py_set_attr`) is described first as well, under the same conditions, `genericSetAttr()` being the lookup:
  `describeSetAttribute()` says where the value will go (the offset of the attribute; a new property; the dict of the object), and the slow path puts
  it there. (It does not flatten: nor do JS's stores.) A site keeps what it remembers until it has something else to remember, or the
  collector finds the Structure dead.
  What is found in the class may also be a `staticmethod` or a `classmethod` (what it wraps is read each time, since `__init__` can
  replace it), the field of a named tuple, or a property (below). The object may be a class itself: then its own number is checked, and that of its class.
  The numbers are finite, so a type takes one only when something is to be remembered about it, and at most 1000, as in CPython:
  about a class that keeps changing (`C.count += 1`) nothing is remembered after that.
- _Calls_ are JSC's.

**What a class has under a name** (`PyType::lookup()`: the first class of the MRO that has it) is asked by every slow path above, and by all of the runtime. What was
found last is in one table of 4096 entries for all the types of a runtime (`TypeLookupCache`), names that no class has included, as in CPython. The VM has the like for Structures
(`HasOwnPropertyCache`, `MegamorphicCache`). An entry belongs to a key and a name. A type's key is a number that no type has had before, and it takes a new one whenever it changes,
or one of its bases does, so nothing has to be looked for in the table then. The table keeps no value alive: after each collection it forgets those that are gone
(`Runtime::reconcileWeakReferencesAtGCEnd()`), which only happens to a class whose dict was written to behind its back (`gc.get_referents(C.__dict__)`; CPython crashes). It
does keep the text of a name, until another takes the place: so none of more than 100 characters, as in CPython. Whether an object can be called is a question that JSC takes to have no effects
(`PyObject::getCallData()`), so `__call__` is looked for by its text alone there, and no key of a class's dict is asked whether it equals the name (`PyType::AsksKeys::No`), nor is what such a key has said before gone by: where the only `__call__` of a class lies under a key that is no str, Python calls the object, and to JS it is an `"object"` that cannot be called, before and after. The collector asks
it too, for the frames of the sampling profiler, on whatever thread it runs: what is looked up for it is not remembered, since letting go of a name takes the VM's table of atoms
(`Heap::currentThreadIsDoingGCWork()`, as in `SymbolTable::materializeCachedEntries()`). All 96 KB of it are gone through at the end of a full
collection (15 to 20 µs). An Eden collection frees nothing that has survived a collection, so at the end of one only those runs of 64 entries are gone through that were written
to since the last collection (`Heap::reconcileWeakGCHashTables()` goes by the same): none, in a program that looks up nothing new. (An Eden collection with nothing to do, in
a program of JS that has imported one file of Python: 55 µs; 72 if all of the table were gone through, which is 1.5 to 2 % of a loop that only makes garbage.) More pairs of a class and a name than there are places push each other out, where the 16 places that each type had before grew with the number of
classes: `getattr()` on 300 to 3,000 classes in turn, with 12 names each, takes a fifth to a quarter longer than it did; on 100 classes, or with 40 names each, a tenth to
a quarter less. Programs have more than 16 names for a class. Measured twice: the inner
loops of six programs, over and over in one process, take 2 to 4 % less time (black: the same); whole runs of five of them, with their imports, collections and subprocesses,
take 1 to 5 % fewer instructions (black 0.9, pytest 1.0, django 1.2, sympy 1.5, docutils 4.6).

`object.name(...)` makes no bound method: `op_py_get_method` gives the function and the object, which is passed as the first argument.
`with` finds `__enter__` and `__exit__` with the same instruction (a number stands for the name: they are looked for in the class alone).
Whether there is an object to pass is one `op_jeq_ptr` against the empty value.

`super().name` and `super(type, object).name` make no `super` object, as in CPython and under its conditions (`Unit::isAttributeOfSuper()`: nothing in the source binds `super`;
there is a first parameter and a `__class__` cell). The global `super`, the class, the object and the name go to one function of the runtime (`loadSuperAttribute()`), which walks the MRO
if the global is the built-in and does what is written otherwise; for a call it gives the function and the object, like `op_py_get_method`. It remembers nothing.

**Where values go.** As in `NodesCodegen.cpp`, an expression is given the register its value is wanted in, and writes there last of all: `x = [a, x]`, `x: int = f(x)`, `x = yield x`. Where the
order of evaluation could be told from the order of the writes, a temporary stands in: a variable that `:=` assigns to somewhere in the function is copied when it is read
(`Variable::isAssignedInExpression`; JSC asks the same of the right operand, `rightHasAssignments`); `return x` copies `x` if a `finally`, the end of an `except ... as`, an `__exit__` or the closing of
a generator lies on the way out. `a, b = b, a + b` makes no tuple, and if the targets are variables in registers, the last value is computed in its variable. `a, b = t` and `for a, b in pairs`
unpack into the variables themselves if their registers follow one another and they are written on one line (`Unit::unpack()`): `op_py_unpack` stores nothing before it has all the items.

**Jumps.** JSC emits a jump forward as narrow as its other operands allow, and if the target turns out to be further than the operand can say, puts it in a table beside the code, where the
interpreter looks it up whenever it jumps (a call and a hash table: some 70 instructions). Code of Python has such jumps emitted again, wide enough, when all of it is there
(`widenJumpsWithOutOfLineTargets()`, by `BytecodeRewriter`, which moves handlers, marks and positions along), as `BytecodeOptimizer` does for the code of JS that it emits again.

**Constants.** What CPython's compiler works out, this one does, with the same limits (`Unit::valueCPythonsCompilerWorksOut()`): operations on constants, from the left for as long as they are on
constants (`60 * 60 * x`); tuples of constants; a list or a set of constants that is only searched or iterated over is a constant tuple or frozenset, and a list of anything else that is only
searched or iterated over is built as a tuple (`Unit::exprToIterateOverOrSearch()`). `co_consts` shows what CPython's shows, except that a list display of three constants or more, which
CPython makes from one constant, is made from its items here. A set display of three constants or more is an empty set updated with a frozenset, as in CPython: the order of a set shows
how it was filled. In a `.pyc` file and in the cache a frozenset has its items in the order of what marshal writes for each on its own, which does not go by hashes: the same source gives the same file in every process. It is
CPython's order as far as the items are written alike (there, whether an item is marked as one to refer back to goes by its reference count). What is written once may be compiled several times (a `finally` clause, for each way into it); a function, class or
generator expression in there is compiled once (`Unit::m_compiledCode`) and is one constant, as in CPython, where the copies are equal. Constants of the same type and value
are one constant of a code object, and one object (`Unit::merged()`): tuples, bytes, frozensets, slices, complex numbers, ints and floats that are objects, strs that were worked out.
The order in which CPython's compiler does things shows. It makes a constant of a slice whose parts are literals, before it works out what `-1` is: `x[1:2]` has a slice among the constants and `'abc'[1:]`
is worked out, `x[:-2]` has `-2` and `'abc'[-2:]` is left to the program. (Here the slice of `x[:-2]` is a constant all the same. It is not shown.) It works out `not` of a literal before it
makes tuples or negative numbers: `not 1` is a constant, `not ()` and `not -300` are not. And before all that `'a%sb%5r' % (x, y)` becomes `f'a{x!s}b{y!r:>5}'`, if the format is a literal with nothing
in it but `%s`, `%r`, `%a` and `%%`, with or without flags, a width and a precision of at most two digits, and the tuple is written out, without a `*`, and has as many items
(`Unit::takeFormatApart()`). That shows in the constants (`'a'`, `'b'`, `'>5'`) and in the lines: the pieces of text are on no line, so they are of the line before, and a statement that begins
with one and goes over several lines is first heard of on the line of the first item. At the very beginning of a function the line before is that of the `def`, which is told of then; in a module it is line 0.

`Name(...)`, where the name looks like that of a class, is compiled so that, if it is a class with `object.__new__` and an `__init__`
in Python, the instance is made and `__init__` called without leaving the interpreter (`op_py_prepare_construct`).

**Calls** are `op_call`. Positional arguments are the arguments; keyword values follow them, and `this` is a tuple of their names
(`PyKwNamesType`) or `undefined`. `op_py_bind_args`, the first instruction of a function, sorts them into parameters. The parameters are where the arguments are, so most
of them stay: what was passed by position, and what was passed by name in the order of the parameters right after that, as in `f(x, key=k)`
for `def f(x, key=None)`. Only the other values passed by name are set aside first, being in the way (`bindKeywords()`). The instruction itself does two cases: as many
arguments as parameters, and fewer where what is left out has defaults, both for a function without `*`, `**` and keyword-only parameters and a
call that passes nothing by name. What goes to the tuple of `*args` or the dict of `**kw` is taken out of the frame, whose slots behind the
parameters no code can name: it would live as long as the call. So JS calls a
Python function, and Python a JS function, with the engine's own call sequence and inline caches.
When Python calls what is not Python, the frame is adapted on the way (`prepareForeignCall`, from `linkFor`): keyword arguments
become a trailing object, whole floats become numbers.
A call site is linked to the function that its second call calls. One that is then given something else goes virtual, as in JS, but with a trampoline
of its own (`llint_python_virtual_call_trampoline`), since the VM's would enter a function of JS with the frame as it is. It keeps nothing about the
callees: functions that one `def` or `lambda` makes over and over are all alike to it. It enters functions written in Python (those that have the
Structure they were made with), and functions and types written in C++, and leaves the rest to the slow path: what is foreign, and what C++ has
to call. A bound method of a function written in Python, and an instance of a class whose `__call__` is one (`PyType::m_specials` says so), it
turns into a call of that function: the frame moves down by two slots, as the arity check moves it for a call with too few arguments, the
arguments by one, and `self` goes in front. So there is no frame of C++ between caller and callee, and nothing holds the arguments but the
callee (`self` excepted: the method stays in the caller's register while the call lasts). The slow paths, which make the first calls of a site, and the first call of a function that has no code yet, do the same
(`handleHostCall()`): they find the function (`Python::beginCallWithSelf()`), give it its code, and leave the frame to
`llint_python_call_with_self_trampoline`. The callee in the frame is then the function, not what the instruction called
(`isBeingCalledByPython()`).
`f(x, *a, **k)` is `op_py_call_spread`, which puts the arguments into the frame of the callee from where they are: `x` from its register, the items
from the tuple or the list, the values from the dict. Nothing is made on the way but the tuple of the names. It is JSC's `op_call_varargs` in all but that: the
frame is placed and checked in the same way (`calleeFrameForVarargs()`), two slow paths size and fill it, and the call itself is the same code with the same
call link info. All that allocates, raises or runs code is done by the first of the two, the names included, which wait in a register of the instruction: when the second fills
the frame the stack pointer is at that frame, and C++ runs in what the collector takes for registers of the caller (`clearDeadTemporaries()`). The interpreter does by itself what most such calls are, a function that passes on what it was given: after the `*` a tuple of up to 96 items, a list of up to 128 whose items lie in a row as values or as doubles (beyond that C++ copies faster than it costs to get there), a list with nothing in it, or nothing, after the `**` nothing or
a dict that is empty, none an instance of a derived class. Everything else is `Python::prepareSpreadCall()`, in CPython's order: `k` is read, then `a` is gone through. If an
argument stands before the `*` it is the other way round, CPython having made a list of `x` and `a` by then (`PositionalExtendsList`), and if keyword arguments follow, `op_py_spread_now` goes through `a` before they are
evaluated. That makes a list of whatever is no tuple, a copy if it is a list: unless nothing of the program can run before the call, the keyword arguments being constants and variables of the function, all on one line. A dict of the program's with
nothing but strs for keys is only read, not copied, if no code of the program can run before it has been. The instruction empties its registers when the arguments are in place, so the caller holds neither them nor what they came in.
Arguments before a `*` that is not the last argument, and more than 30 of them, go into a list first, as all do in CPython.
In instructions, with CPython's beside them, for three items, functions that take exactly what they are given, and local variables: `f(*t)` 319 (477), `f(*l)` 316 (651), a list of floats 321 (672),
`f(x, *t)` 333 (1,237), `f(*t, **{})` 321 (682), `f(*t, **d)` 1,286 (1,369), `f(*t, k=x)` 1,961 (1,276: the dict is made
and then taken apart), and `def g(*a, **k): return f(*a, **k)` called with one argument 992 (1,170).
The names that come out of `k` are its keys as they are, not atoms: a program can make any number of them, and an atom is looked
up in a table that all strs of the process share. So whoever takes keywords compares texts where the pointers differ.

**Functions written in C++.** How the arguments of a call get from the frame to the C++ function depends on its signature and on the
kind of function, so it is decided when the `PyNativeFunction` is made (`entryFor`): for a fixed number of parameters there is an
entry that passes positional arguments straight on and leaves every other call (keywords, a wrong count, a `self` of the wrong type)
to the general one. Likewise for the methods that the language itself calls: a type knows of each whether what `lookup(Special)`
finds is a C++ function of its own that takes just the operands (`PyType::isNativeSpecialTaking`), and then `len(x)`, `x[i]`, `a < b` call
that function. These are the slots of CPython's types, found out on first use and forgotten when the type changes. What calling a
built-in type comes to is remembered in the same way (`PyType::Construction`).

**Methods written in Python that the language itself calls** (`__add__`, `__lt__`, `__getitem__`, the getter of a property) are
called by the instruction, not by C++: an entry into the interpreter from C++ costs more than the call, and the frames of C++ would
lie on the stack for as long as the method runs. The frame of the callee goes below the registers of the caller: the stack grows for the call and shrinks
after it, as for the getters that JSC's inline caches call (`InlineCacheCompiler.cpp`), so it costs code that never comes to such a
call nothing: no operand, no register. Only the interpreter makes such a frame, and only when it makes the call
(`LowLevelInterpreter64.asm`, "Calls that instructions make"; `PyBytecode.h` has the layout and the list of the instructions). A
function is called in this way once it has been called in the ordinary way, which makes its code.

- _Which method_ is read out of the type: `PyType::m_specials` is what `lookup(Special)` has found, and its state says whether that
  is a function written in Python. For an operator the interpreter decides by itself when it takes no more than a look: the left
  operand is a `PyInstance` and the right one is of the same class or no cell, or the left one is a number and the right one a
  `PyInstance` (`pyFindBinaryOperatorMethod`, `pyFindComparisonMethod`). No cache, nothing to invalidate.
- _Everything else about operators_ goes to the slow path, which goes through the protocol a step at a time (`askMethods()` in
  `PyOperators.cpp`, `askComparisonMethods()` in `PyAbstract.cpp`). What is not a function written in Python it calls itself. When it
  comes to one, it returns the function in place of where to go on, and how far the operator will have got in place of the frame
  (`operatorSlowPath()`, by way of `Runtime::methodToCall`), and the interpreter makes the call.
- _What the method of an operator returns_ is the result, unless it is `NotImplemented`. Then a second slow path (`..._again`) goes
  on, and may come up with another method. What it goes by is in registers behind the arguments (`RegisterOfOperator`): how far
  the instruction has got (`OperatorProgress`, `ComparisonProgress`) and the two operands, since the method may have given the
  variables they came from other values. The classes it looks up anew, as CPython does: the method that ran may have changed them,
  or the class of an operand. A comparison fused with a jump jumps on `True` and `False` by itself and leaves other results to a
  third slow path (`..._finish`). Between the arguments and those registers is room for two more parameters
  (`def __add__(self, other, context=None)`); a method with more is called by C++.
- _The getter of a property_ is found by the slow path of `op_py_get_attr` in the ordinary lookup (`genericGetAttr()` knows a
  `property`), and returned to the interpreter in the same way. The instruction remembers the property, as it remembers a
  `staticmethod`: while the Structure of the object and the version of its class are the same, it reads the getter out of the
  property (`property.__init__` can replace it) and does without the slow path. Not for a class with a `__getattr__`, which is asked
  when the getter raises `AttributeError`: then C++ makes the call. No `getAttr()` is on the way of what the getter raises, so it is the
  unwinder that tells an `AttributeError` which attribute it was, if it does not say. The object is kept in the frame, behind the
  argument (`RegisterOfGetter`).
- _Nothing is left behind._ Where the frame was, the frames of others will be, and C++ does not write every word of its own: what
  shows through is a reference as far as the collector can tell. So every word of the frame is written when it is made and zeroed
  before it is given up: by the interpreter, by a slow path that finds the frame there and settles the matter, or, if an exception
  comes out of the callee, by the unwinder, which says which frame of code the exception left last (`clearFrameMadeByInstruction()`:
  all but the function and its code block, which the machine code still reads, and which stay until the next call from there).
- _While the method runs_ the caller holds the operands, as any instruction does that has not finished, but not what was in the
  register for the result (`liveLocalsOfFrame()`).

**Generators, coroutines, async generators** are made as JSC makes generators: a function that makes the generator object
(`PyGenerator`), and a body, a function of its own, that the generatorification pass turns into a state machine.

- _To resume a generator is to call its body_, and the body keeps the books. `op_py_generator_enter`, its first instruction,
  notes that the generator is running and goes on where it stopped: the state of a suspended generator is the offset of the
  instruction to go on with. `op_py_generator_suspend` and `op_py_generator_return` note that it no longer runs, and return to
  the caller. If an exception leaves the body, the unwinder does that (`generatorBodyIsUnwinding`), and makes a `RuntimeError` of a
  `StopIteration` (PEP 479), but not for the generator of a lambda: CPython's compiler has left that out since 3.12, so a
  `StopIteration` that `f()` raises in `(lambda: (yield f()))()` leaves the generator as it is, and is the end for `list()`. The caller tells a yield from a return by the state.
- So anybody can resume one: `PyGenerator::resume()` in C++, and two instructions, which make the frame and call the body without
  leaving the interpreter: `op_py_for_iter`, and `op_py_send`, which is a step of `yield from` or `await`. Both enter at
  `llint_python_generator_resume`: the prologue, `op_enter` and `op_py_generator_enter` in one.
- _`throw()` into one that has not started_ starts it: its frame is set up, and there the exception is raised, on the line of the `def`
  and outside of all that the program wrote, so that a `StopIteration` stays one. Tools are told (`PY_THROW`, `RAISE`, `PY_UNWIND`), the
  traceback has the frame, and the frame has the arguments. A generator of JS begins with a test of the resume mode. Here one that is
  started in the usual way runs nothing for it: the switch that the generatorification pass adds, which only a generator that starts
  gets to, comes after what sets up the frame; state 0 goes on with the program's code, and `throw()` passes a state that there is not.
- _The registers_ that are live across a yield, and all the variables (`gi_frame.f_locals` shows them), are kept in slots at the
  end of the generator object, which is as large as its code needs. Runs of registers are saved and restored by one instruction
  (`op_py_generator_registers`; the last run is saved by the suspend itself). Until it starts the first slots hold the arguments.
  While it runs they are empty.
- What the pass saves at a yield is what its analysis finds live, which may be more than is in use: a register above the
  variables may hold the remains of a callee's frame, which the collector must never see in the heap. For Python code the pass
  gives such registers a value where they lose theirs (`findRegistersWithoutValue()`).
- One that may have something to do when it is dropped (a `finally`, a `with`, a coroutine never awaited) is registered for
  finalization when it is made, which is no more than writing it down. Most are done long before the next collection, which
  asks each whether it is (`PyGenerator::finalization()`).

**Code objects.** The code is a `FunctionExecutable`; a code object (`PyCode`) is made for it when something asks (`f.__code__`, `frame.f_code`,
`co_consts`), and from then on the executable holds it, so it is one object for as long as the code is there: programs keep weak
references to code objects and tables by their `id()`. JSC drops a `CodeBlock` that has not run for a while and links it again when it is
called. The executables of the functions that Python code defines do not go with it (`FunctionExecutable::pythonFunctionExpression()`): they are
constants of the code, and the functions made after have the same code object as those made before.
`f.__code__ = code` gives the function another executable (`PyFunction::setCode()`), also while it runs: libraries compile a function when it is first called, from inside it. A call goes on in the code it
began in, so what is about a call (where its variables are, its lines, its monitor, `f_code`) is asked of its `CodeBlock` (`codeInfoOf(callFrame)`, `PyFrame::codeInfo()`), never of the function in the frame; the
generators there are keep the body they were made with, and a generator is made with the body of the code that the call is in (`PyFunction::generatorBody()`). A builtin of JS's that calls the function again and again runs the new code from its next call on if it is linked to the code as a call site is
(`CachedCall`: `sort()`, `replace()`, `new Set(iterable)`), and goes on in the old code until it returns if it has asked once what the function runs (`CallData`: `JSON.stringify(o, f)`, `JSON.parse(text, f)`, `map.set` under `new Map(iterable)`). Either way the arguments are bound by the code that has been entered.

**Exceptions** are JSC exceptions whose value is a `PyException`. The unwinder tells the runtime about each Python frame it
passes (`addTracebackEntry`), which is how tracebacks are built. A frame that has not got as far as the program's own code
(`PythonMark::Start`) is not in the traceback: arguments that do not fit, the first iterable of a generator expression.
An entry holds the code block and the place in the bytecode, like a `StackFrame` of JS; `tb_lineno` and `tb_lasti` are found when something asks (`PyTraceback::findPlace()`). So for
`f_lineno` and `f_lasti` of a frame object whose call has ended (`PyFrame::callIsEnding()`).
Most exceptions are caught by a handler that never looks at them (`except KeyError: pass`), and for those **the entry of the frame that catches is only noted**: the exception, the frame, the code block and
the place, in a small stack that belongs to the thread (`Runtime::NotedEntry`). No traceback object is made and no frame object, so the call returns the short way, and the frame object of its caller is not made either
(`f_back`), nor that one's caller's. The entry is made from the note if anybody could come to look, as an `ErrorInstance` makes `stack` of the raw stack it keeps. That rests on one bit: an exception is *unseen* from where the runtime
makes it in order to raise it (what C++ raises; `raise Error`; `raise Error(...)`, if `Error` turns out to be the class of what is raised; in each case only if no `__new__`, `__init__` or `__del__` of the program's
and no metaclass is involved) until something of the program's could get hold of it. `PyException::willBeSeen()` ends that and makes the entry. It is called by whatever gives out the exception being handled
(`sys.exception()`, `exc_info()`, `_current_exceptions()`, a `raise` without an exception, the `__context__` of what is raised meanwhile) or the pending one (`pendingException()`), by `op_py_catch` for a handler that names the exception or hands it
on (`as`, `with`, `finally`, `except*`) or that can be suspended, by every way to raise an object that is there already, before a tool is told, where the unwinder comes to a frame of JS or to a function of C++ that called
Python, by `traceback()` itself, and when the program is handed objects it had no reference to (`gc.get_objects()`: `temporariesMayHaveEscaped()`). So while there is a note, only the slot, the registers of the handler and the VM refer to the
exception, and when the handler is left (`RuntimeFunction::LeaveHandler`) the note is dropped with it. If the exception goes on instead, the entry is made when the frame is left. A note is gone before its frame is: builds with assertions
check that wherever a frame ends or is put away. Missing a way to get at an exception would cost a line in a traceback, not a pointer to a frame that is gone.
**The caller of a frame that was left is found when it is needed.** A frame object that outlives its call needs `f_back`, and `PyFrame::callIsEnding()` makes the frame object of the caller for that. That one is there from then on: when its
own call ends it takes over its variables and makes the frame object of *its* caller, and so on to the bottom of the stack, 439 instructions for every return, long after the exception is forgotten. (CPython builds the same chain, but the
reference count of the first frame object falls to zero when the handler ends, and the chain is never begun.) So a frame object that is made for the entry of an unseen exception which is leaving the call does not ask for its caller: it is marked as
lacking it (`PyFrame::lacksBack()`). The exception has come up to the caller, which gets an entry of its own, at once or when somebody could look, and then the frame object below is told (`backIs()`); or it catches the exception, nobody looks,
and the note, the exception, its traceback and the frame object go together. Only the exception leads to such a frame object, and the entry is made before the exception is seen.
This is the way of whatever an instruction of Python's code has called: functions, lambdas, methods, static and class methods, `__init__` and `__enter__`, with arguments of any form; and bound methods that were kept and objects with a `__call__`, where what they call is a function written in Python, which the interpreter then enters itself ("Calls"). All else goes the old way: what was called from C++ or from JS, where the exception
is seen as soon as it comes up (properties, special methods, key functions, `map`, `partial`, class bodies, modules, `exec`); generators and coroutines, whose frame object is theirs and has no caller once they have
stopped; a call that has a frame object already (`sys._getframe()`, a tool, an earlier traceback); an exception that has been seen; a thread that is being ended. Where code of the program runs while an exception is on its way, to close what a loop was going through, the exception counts as seen.
Builds with assertions check that no frame object that lacks its caller is given out: by `willBeSeen()`, `traceback()`, `tb_frame`, `f_back`, `PyFrame::of()` (so `sys._current_frames()` and the frames of threads that wait), `gc.get_objects()`, `get_referrers()`, `get_referents()`
and `gc.garbage`. In other builds `f_back` would be `None`: nothing points to a frame that is gone.
A handler begins with `op_py_catch`, which is to Python what `op_catch` is to JS: it stands at the end of the code, as that does, and jumps to the handler. It takes what was thrown, makes it the exception
being handled and keeps what that was before, in one call of C++, and profiles nothing, there being no other tier. Which of the two routines the unwinder goes to is settled when the code is linked
(`HandlerInfo::initialize()`), so a `catch` of JS runs what it always ran.
An exception that goes on past a `finally`, a `with` or `except` clauses that do not match is thrown twice on the way: into the handler, and out of it by one instruction that also puts back
what was being handled before (`RuntimeFunction::ReraiseOutOfHandler`). It stands outside the range of the handler's own clean-up, which is there for what the clauses themselves raise.
CPython raises it inside that range, catches it, and raises it a third time: tools are told of those two steps all the same (`Monitoring::exceptionLeavesHandler()`).
What is left: a frame that an exception leaves costs 1,700 instructions, 1,235 in CPython. Of the 465 between them, more than all is JSC's own unwinder, which JS pays for as well: 218 to copy the callee saves to the
entry frame's buffer for each frame, 267 in `StackVisitor`, `UnwindFunctor` and `CallFrame::bytecodeIndex()`. If exceptions that cross many frames turn out to matter, that is where to look, for both languages.

**Printing them.** As in CPython, `sys.excepthook`, `sys.unraisablehook` and `threading.excepthook` have `Lib/traceback.py` do it
(`traceback._print_exception_bltin`, called by `printException` in `PyFrame.cpp`): carets, suggestions, colours, groups and notes are its
business. What it goes by comes from here: `co_positions()` (columns in bytes of UTF-8; none with `-X no_debug_ranges`), `tb_lasti`, `name` and `obj` of an `AttributeError`
(filled in by `getAttrImpl` when whoever raised it did not), `_suggestions._generate_suggestions`, `SyntaxError._metadata`,
`_missing_stdlib_info`. If it cannot be imported or fails, `formatException` prints the traceback natively, without carets. That is also what the JS view of an
exception shows, and what is used before there is a `sys.stderr`.

**Calls of functions of JS are in the traceback.** Where there is a Python, the unwinder also tells of each call of a function of JS that an
exception of Python's leaves or is caught in (`addTracebackEntryForJS`): those that a stack trace of JS would show, the ones that an optimizing compiler
has made part of their callers too. The entry (`PyTraceback::createForJS`) holds the code block and the place in it. What Python sees is made when something
asks: `tb_lineno` and the file are those of what the programmer wrote (the host's source map, `VM::computeLineColumnWithSourcemap`), `tb_lasti` is -1, and
`tb_frame` is a frame of code that does nothing, whose `co_filename` and `co_name` are the file and the function (`<module>`, `<anonymous>`, `<eval>`), without
locals; its `f_globals` is a dict that the runtime keeps for the file (`namespaceOfFileOfJS`). So `traceback`, `logging` and `linecache` show `File "x.ts", line 3, in f` and the
line itself. A `finally`, and what the compiler makes that works like one (`for`-`of`, `using`), catches and throws again: that is one entry, as in Python; `catch (e) { throw e }` is two, as with
`raise e`. A function that ends in a tail call (`return f()` in strict code) has no frame any more, in a traceback as in a stack trace.
An error of JS has no traceback until it comes up to code of Python's. Then the calls between that code and where the error was made are taken from the
error's own stack (`tracebackOfErrorOfJS`), if the call that Python made is in it: so a `JSError` says where in JS it comes from, also as the `__cause__` or `__context__` of
another exception. From there on the unwinder keeps track as above. So in a program that has started Python, nothing is noted about what JS throws and JS catches. That a value is of that kind is
found out at the first frame of an unwind and goes with the unwind from frame to frame (`m_thrownValueHasNoTraceback`), until a frame of Python's is passed: the other frames cost a call and a test.

**What a `catch` of JS has caught is being handled**, as in an `except`, until the clause is left: `sys.exception()`, `traceback.format_exc()`, `logging.exception()`, a
`raise` without an exception, the `__context__` of what is raised meanwhile, and of an exception of Python's that JS makes and throws there (`willThrowInJS`). Of what is no
exception of Python's it is the `JSError` that an `except` would have got. Nothing in the code of a clause says where it ends, and none of it is changed. The
bytecode generator notes where each clause begins and ends (`PythonMark::CatchOfJS`, at its `op_catch`). The function of C++ that every `op_catch` calls for the exception, in
all tiers, notes the frame, the clause and the value if the realm has a Python (`didCatchInJS`), and that is all a catch costs. (The same catch once more, in a loop or in a function that is called again and
again, with no other noted: only the value is noted anew.) Whoever next asks what is being
handled, or is about to change it, looks at the stack first (`Runtime::putCatchesInJSInSlots`): a catch whose frame is gone, or has gone on to code outside the clause, is over. What is
left is put where an `except` at that place would have put it, so handlers of Python's above and below, and generators, find what they expect, and it is taken out again when
it is over. A `finally` of JS is no handler. A call in tail position takes the frame away while the clause, as written, goes on: `catch { return f() }` in strict code
without a `finally`. By the rules of JS the function has made way for `f`, and nothing is being handled there.
An async function or async generator of JS has another frame each time it is resumed. The mark of a clause says which yield points are in it (an `await` is one), and the
frame of such a function has what keeps its state as its first argument. A catch whose frame is gone while the function is suspended at one of those points is put by, in a
weak map from that object (`m_catchesOfSuspendedGeneratorsOfJS`): what was caught lives as long as the function can go on. While that map has anything in it, whoever looks at the
stack also looks for frames of those functions, and a catch goes on if the frame is in its clause. So behind an `await` in a clause the exception is being handled
again, and in between it is not, as with a coroutine of Python's. A function that has left the clause or come to its end with nobody looking gives up what was put by for it
when it is next seen on the stack, at `gc.collect()`, or when it is collected. These functions are resumed by C++, so they always have a frame of their own.
`next()` of a generator is written in JS, and an optimizing compiler makes the body part of it, with its arguments nowhere to be found (FTL leaves their slots unwritten).
So for a generator that is not async a catch is over at the first `yield` in the clause: a frame of its own says at which yield point it began, and one that began in
the clause did not catch. At the top level of a module an `await` still ends the catch.

**What nobody catches.** An exception of Python's that comes up to the host with nothing left to catch it (a timer, a microtask, a promise nobody handles, the top level of
a module of JS) is given to `sys.excepthook` (`reportUncaughtException` in `PyExec.h`), whichever language the program is in, and so is printed as Python prints it. In Bun, `process.on("uncaughtException")` comes first, and the test
runner shows the exception among its own lines (`stack`). An `Error` of JS whose `cause` is an exception of Python's is printed as JS prints it, followed by the traceback of the cause.

**Warnings** count calls of functions of JS among the levels of the stack (`forEachCallOfEitherLanguage`): `warn()` called by JS, or with a `stacklevel` that leads to JS, is about that file
and line, `skip_file_prefixes` goes for such files, the module is named after the file (as `warn_explicit()` does without a name), and `__warningregistry__` is in the dict the runtime keeps for
the file. What the host has written in Python to serve JS (its frozen modules: `with()` of `bun:python`) does not count, as importlib does not.
The file that a program in JS began with is `__main__`, as the file of a program in Python is, so a `DeprecationWarning` about its code is shown though nobody has set a filter. The host
says which file that is (`setIsMainFileFunction`; in Bun it is `Bun.main`: in a worker the file of the worker, which has an interpreter and a `__main__` of its own, and under
`bun test` the file of tests that is running). (A decision that can still be changed.)

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
  `VMTraps::NeedPythonCalls`; Python's loops (`op_py_jmp_back`, on their way back, which is where CPython looks for signals) and function entries poll it. JS code does not service it:
  the optimizing JITs take a poll to change nothing. Nor does C++, but where it is about to call a function or a class of JS's, or the `next()` of an iterator of JS's (`safePoint()`): a loop in C++
  (`sum()`, `any(map(f, ...))`) relies on what it calls for each item, and a function of Python's polls at its entry. (Not where JS code runs because an object of JS's is looked at: a getter, a trap of a Proxy, `toString()`, `has()` for `in`, `[Symbol.iterator]()` for `iter()`, `[Symbol.hasInstance]()`.) A host may serve it between turns of its event loop (`makeCallsOwed`).
  With reference counts a finalizer runs where its object is dropped; here it runs wherever the program is when the collector has
  come, and it is not to fail for that. So what is called from the queue has 200 frames to itself even at the recursion limit
  (the depth is lowered meanwhile; `sys.getrecursionlimit()` says what the program set), and near the end of the stack itself the
  queue waits for a safe point with more room. Signals are looked at first and between the calls: a long queue does not hold up Ctrl-C. Signals alone are also looked at after every call of a function written in C++, as CPython does after every call, but not after the call of `__enter__` by a `with`: there CPython does not look either, so that what a handler raises is raised in the statement and `__exit__` is called (`checkSignalsAfterCall()`, which knows the call by a mark that the compiler puts at it, `callOfEnterMark`: at both of the calls that a call of a method is compiled to).
  What a handler raises in the middle of one of the calls ends that call, as it would in CPython, but is not reported as "Exception ignored":
  it is thrown where the queue is being run. The program did not make the call. It may be in `time.sleep()`, where CPython runs nothing, and the
  `KeyboardInterrupt` has to come out of that. Only what a handler raised, and only there: where CPython runs the finalizer at the same place
  (`gc.collect()`, a temporary that is finalized at once, exit), it is ignored and reported as in CPython, and so is whatever the finalizer itself raises.
  Nor is the code that happens to run there to suffer for it. A warning looks for the code to blame among the callers, and makes a
  `__warningregistry__` in its globals even if it is ignored, as the `ResourceWarning` about an unclosed file is: that code may be
  going through those globals, as `enum._convert_()` does when `socket` is imported. So the search ends where the calls from the queue
  began (`Runtime::wasInterruptedByPendingCalls()`), unless the program asked for them (`gc.collect()`): the warning is about
  `<sys>`, line 0, as where there is no frame at all, and its registry is the runtime's own.
- **Each finalizer runs once.** An object that is still unreachable at the next collection is freed.
- **What waits for a finalizer does not make the heap grow.** After a full collection the heap lets the program allocate in proportion to what the
  collection marked, before each of the next collections (`Heap::updateAllocationLimits`: twice as much in Bun). What is kept for a finalizer is
  marked like anything else, but it is garbage, and a fixed share of what the cycle allocated: 0.70 for a small object with a `__del__`, 0.93
  for one that holds a list of a hundred. Counted, it made the next cycle 2 x 0.70 times as long, which left as much more to keep, and so on:
  the heap of `while True: [C() for _ in range(20)]` grew by 1.4 with every other collection, to 880 MB after 16 million objects, though every
  finalizer had been called and `gc.collect()` got it all back. (With the factor at 3, 2, 1.7, 1.5 and 1.4 the size after a full collection grew by
  2.11, 1.41, 1.21, 1.10 and 1.03; at 1.3 and below it settled. Every collector that sizes a cycle by what came through the last one has that
  threshold, factor x share = 1: Go, whose factor is 1, is over it with `GOGC=200`, PyPy, with 0.82, with `PYPY_GC_MAJOR_COLLECT=3`. Measured.)
  So the heap is sized by what was marked when marking first converged: all that the program can reach (`Heap::didMarkAllThatIsReachable`). What
  is marked after that takes up room until it is freed and counts for nothing else:
  - the objects that this collection found unreachable and keeps, with all that only they refer to. They are garbage as soon as their finalizers have been
    called, at the next safe point of their thread, and the full collection after that frees them;
  - what earlier collections queued and has not been called yet. So the queues are not visited with the runtime: the collector keeps them at that
    point (`keepUnreachableObjectsToFinalize`; in a heap snapshot for debugging all that is kept there is a root, for the reason `KeptForFinalizers`). Finalizers that allocate bring about collections while the queue is being served, and what those find
    stands behind the rest: with a `__del__` that makes five objects with a `__del__` the queue was two to four times what a cycle may allocate, and with
    that counted as alive the heap still grew without bound;
  - what is unreachable and kept for another reason: cyclic garbage that a collection of an idle thread leaves for later, callbacks of ctypes while finalizers are left.

  An object that its finalizer brings back is reachable at the next full collection, which is the next time the heap is sized, and counts from then on: a
  program all of whose finalizers do that collects as often as one that keeps the same objects in a list. In a collection that lets the program run while it
  marks what it keeps, what the program makes and stores meanwhile may be taken for kept; that also lasts until the next full collection.
  The rule is the heap's and knows nothing of Python: where nothing is marked after marking has converged, as in every program in JS, it changes
  nothing (2,392 collections in 50 benchmarks: not a byte).
  - *Measured.* That loop: 23 MB of heap before a collection (13 without the `__del__`, 585 and rising before), 34 MB more resident than without, flat over
    40 million objects, 2.0 million objects a second as before. Objects that hold a list of a hundred: 22 MB more than without, was 1,150. Three threads whose
    finalizers make five more each, beside a main thread that sleeps: 90 to 140 MB more, flat, and 150,000 to 620,000 calls behind; it was 7 GB after 50 s
    and 87 million calls. The same three and a main thread that ends: 3.4 to 5.1 s and 270 MB, was 8.8 to 13.2 s and 1.4 GB.
  - *The price.* A cycle is as long as the reachable heap warrants, 6.5 MB in a small program, and where most of what is allocated has to be kept every other
    collection is a full one, because only that frees it: 6 ms for a program that has imported nothing, against 0.5 ms. The loop above is none the slower for it; objects of
    1 KB take 15 to 26 % more processor time, suspended generators 4 to 6 %, and 200,000 files that are dropped open, whose buffers of 128 KB count as allocated, 37 us each where it
    was 21 (the heap then took itself to be 5 GB large, and there were 23 collections, not 918).
  - *A queue that stands still* while others allocate (its thread is in foreign code, or in a system call that is not to be interrupted) is another matter. After a collection of the
    young generation the heap asks for a full one if the room that is left is less than a third of the heap, and with cycles that are short for a heap full of what is kept that is so every time. Where
    finalizers are called soon it is as it should be: only a full collection frees what was kept. But a queue that is not served it can only mark again. So the queues from which nothing has been
    taken since a full collection found calls in them (`hasStoodSinceFullCollection`) are kept first, the heap notes how much that was (`Heap::didKeepWhatWaitsForFinalizersIndefinitely`), and in that
    one question it counts as if it were alive: the room that the heap would have granted for it is added on both sides. The cycles stay as short as they are: a queue whose thread has merely not
    had its turn between two full collections looks the same, and if that lengthened the cycles the heap would be back where it was. It ends with the first call that is taken from such a queue.
    With 229 MB queued for a thread that sat in `read()` by way of ctypes, the main thread's 8 million small objects took 3.3 to 3.8 s, with 6 full collections and 100 to 120 of the young generation;
    9 to 10 s without this (48 and 47); 1.5 s while the queue counted as alive in every respect, with 880 MB resident, not 415. (What is left is the walk through the three million entries, in every
    collection.) Queues that are being served are not touched by it: the same collections, time and memory in all the programs above.
  - *Several runtimes in one heap* (a `ShadowRealm` that imports a `.py`). What one of them keeps may lead to what is another's, and the heap's notes are of all of them together. So the collector takes them through the steps side by
    side (`Heap::keepUnreachablePythonObjectsToFinalize`, `Runtime::StepOfKeeping`): the queues that stand still, of all; the heap's note; the other queues; what has died; what those that listen to the collection keep; each step again and again until none of them has marked anything. Only then, once
    for a collection, each is told that no more will be marked (`Heap::didKeepAllPythonObjectsToFinalize`, `Runtime::didMarkAll`). That is as far as it goes. In the step for what has died each runtime works out by itself what of its own has died and in which order, in the same
    round as the others, when what another has just kept has not been followed yet. So an object of one realm that only a dead object of another realm leads to does not wait for that one's finalizer, as it would in one realm: both are called after the same
    collection, at a safe point the runtimes in the order in which they began, in a collection that was asked for the runtime that asked last, and so the one that was held may come first. (Each used to go through all the steps by itself in every round. A runtime without a standing queue had the heap make its note in the round in which
    another had only just put its queue forward: the program above made 15 full collections for 1 as soon as there was a second realm. And a runtime that had nothing left was told that marking was over while another was still keeping: it counted
    two collections for one, called `gc.callbacks` four times, and took out of its lists what the other might yet have marked.)
  - *A runtime ends in the collection that finds its global object dead* (`Heap::didKeepAllPythonObjectsToFinalize`, `Runtime::globalObjectDidDie`), not when that is swept. The first eight global objects of a VM are allocations of their own, which are
    destroyed at the end of that collection if the main thread has the VM then. The ninth lies in a block, and a block is swept when its memory is wanted, perhaps many collections later. Until then the runtime stayed in the heap's list, and the collector went on asking it about cells that had been
    swept and whose memory was in use again: nine plain realms, a tenth that imports a `.py` and is dropped, 20 MB of garbage made by the main realm's Python in one call: a crash. It also kept the signals for as long. Now the heap forgets it there and then, and what
    the world outside knows of it (the signals, `faulthandler`, the C API) ends as well. A realm that is dropped is garbage like any other: what of it has a finalizer is kept and finalized, at a safe point of whatever runs in the VM (`makeCallsOwed` goes through all the
    runtimes), and keeps the realm meanwhile by way of its Structure. So a runtime that ends owes nothing.
- **An object registers when it is made**, if its class has a `__del__` then. A class that gets one later, by assignment to the
  attribute or to `__bases__`, has the heap searched for its instances (`registerInstancesForFinalization`), once; an object
  that is given such a class by assignment to `__class__` registers then.
- **To register is to be written down, and nothing is looked up.** There are two vectors, in the order of registration: the objects
  that a collection has seen, and those registered since. What a collection has seen and left there is old, so a collection of the
  young generation goes through the second only, and moves what lives to the first. Nothing is taken out in between: whether an object
  still has something to finalize is asked of the object, when a collection comes by. A generator knows, and an open file, a
  socket, a future with an exception nobody has looked at have two bits for it (`FinalizerState`: wanted, listed), so closing
  and opening again are a store each. An object must be in the list once. An instance is written down by whoever called the `__new__` of
  a built-in type to make it (`didMakeObject()`), or when it is given a class with a `__del__`, and once in its life: it has a bit
  that says so (the one in the header of a cell that JSC leaves to each class of cells). The lists are whole whenever the program
  can see them, which is also in the middle of a collection: one that the engine began lets the program run while it
  marks what the objects to be finalized keep alive. What has been finalized waits in a vector for the next full collection, which
  finds nearly all of it dead; the rest, which somebody took hold of, goes to a set (`gc.is_finalized()`).
- **Finalizers run in the order of reference counts.** A `__del__` that writes to `self.file` finds the file open, and the items of
  a list that is dropped go from the last to the first. When marking has converged, the dead cells are taken for a graph and let go
  of as reference counts would have let go of them (`PyFinalizationOrder.cpp`): first those that nothing dead refers to, each of
  which lets go of what it refers to in CPython's order, depth first. That gives the order of the objects with finalizers,
  and leaves over what reference counts would not have freed: the cycles and what hangs off them.
  - A cell says what it refers to as that lies in memory. Where CPython's object lets go in another order, the edges are put in
    that order: a list, a tuple and a frame go from the end; the attributes of an instance in the order of their places, which is
    the order in which they were set unless one has been deleted, though those beyond the sixth lie outside the cell; a deque from
    its left end, which is somewhere in the middle of its memory.
    The table that a dict or a set has left for another (it grew, or was made anew) is dead, still refers to what it held, and nothing
    refers to it. With reference counts it was freed on the spot and let go of nothing. It says what it is (`wasLeftForAnother()`) and is
    not asked what it holds: or an item would go when the second of its holders does. The table that `clear()` leaves is not one of
    these: it is how the container let go of its items, and takes its turn among what the program has dropped, like any other thing
    that nothing refers to. A deque that grows or shrinks empties the memory that it leaves.
  - Only those are queued that no other object with a finalizer reaches. The rest wait for the collection that finds those gone, so
    a finalizer that resurrects its object also keeps what the object refers to from being finalized. So it goes level by level, each
    level in the order of reference counts: of `[N('x', N('x1')), N('y', N('y1'))]`, dropped, `y x y1 x1`, where CPython, which knows at
    once whether `y` has come back, has `y y1 x x1`. A structure more than a few levels deep is finalized in one round, from the top
    down and depth first, instead of a collection per level. So is all that dies when the modules have gone at exit.
  - What is in a cycle or hangs off one comes after the rest and is finalized in one go, in the order in which the objects were
    made, whatever refers to what: that is what CPython's collector does with what no collection has seen alive. (See "At exit" for what one has.)
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
- **Which thread runs a finalizer.** With reference counts it is the thread that drops the last reference, at once. Nobody knows here
  who that was, but programs count on it: a `__del__` that calls JS, closes an event loop or a connection of sqlite3 has to run in the thread that
  made the object. So every call that is owed is owed by one thread, and only that thread makes it (`Runtime::CallsOwed`, one queue
  for each thread that owes):
  - An object is its maker's: the thread that registered it. The callback of a weak reference belongs to the thread that made the
    weak reference. (Under CPython, in 18 suites of packages with other threads alive, the maker was the one to finalize in 84 of 100 cases,
    93 of 100 for `__del__`. The free-threaded build of 3.15 hands an object whose last reference another thread drops to its maker, too.)
  - What dies because something else died follows that: an object that waits for another (see the order above) goes to the
    thread that finalizes the other, as the thread that frees the parent frees the child. That is worked out among what ONE collection finds dead. A collection of the young that takes a young parent does not
    look at a child that is old: that one comes up alone in a later full collection, and is its maker's.
  - What has to happen in one order is not split: in a round that takes parents and children at once, an object and all that its going frees go to the maker of that object; each piece of
    cyclic garbage that hangs together goes to the maker of its oldest, with the callbacks of the weak references to it. CPython's collector
    calls those finalizers one after the other, in whichever thread it happens to run. What hangs together does so by objects of the
    program (`isCellOfTheProgram`), not by what the engine shares among them: the `Structure` of the instances of a class that have the same
    attributes dies with the last of them, and all the dead of that shape refer to it.
  - **A collection that the program waits for** (`gc.collect()`, a wait that nothing else can end) is served by
    the caller: all that it finds, and all that any thread owes and nobody has begun, before it returns (`Runtime::ServesOtherThreads`). A collection of the engine's
    own only hands out. A thread that collects because it is idle makes the calls that are its own. The retry after `EMFILE` is in between: its collection hands out, so the makers are told, and the caller makes
    what any thread owes and nobody has begun, beside them.
  - **What the caller's own collections find is the caller's to make; when nothing can move any more it gives back what it has not begun, and tells the makers** (`Runtime::ServesOtherThreads`). Nothing changes hands for
    that (unless the caller leaves early: below): a call lies in the queue of its thread until somebody begins it (`CallsOwed::taken`), and the caller takes it off at that moment, whichever queue it is
    (`Runtime::takeNextCall`; in which order: below). But the thread is not woken for what is taken, and leaves it alone, woken or running, told or not: the program has asked the caller, and another thread may be in no state for a
    finalizer: it may wait holding a lock that the finalizer wants. What another collection has handed out, the engine's own or that of a thread that is idle, before the caller came or since, is its maker's as
    ever, who has been told; the caller makes it if it comes to it first. (Taken as well, a call that its maker was on the way to waited for the caller: whose finalizer spun for what that call does.)
    - What such a caller's collection found used to go into the caller's own queue, and at exit the main thread took the queues of the others whole. Then a call lay behind one that waits, in the queue of a stranger,
      while the thread that could have made it sat idle: two threads have each dropped an object, the finalizer of the one waits for what that of the other does: with reference counts each thread finalizes its own
      and that ends; here `gc.collect()` never returned, and the program never exited.
    - Nobody can see what a finalizer waits for. But if it is what a call does that the caller has taken, and nobody else brings it about, there comes a standstill: every thread of Python's in the VM, the caller among
      them, waits for another thread (`Threads::waitUntil`: a lock, an event, a condition, a thread), without a deadline, and nobody has let it go on (`Threads::allOthersWaitForGood`). That is a state, which the last
      thread that goes to sleep, or ends, finds with the GIL in its hand. Nothing but a call that is owed can end any of those waits then. So what is taken and not begun goes into the other half of its maker's
      queue, and the threads that owe are told (`Runtime::giveBackWhatIsTaken`): from then on it is what another collection has handed out. (Not a permission that lasts as long as the caller's wait: a thread
      that is told looks once, whenever it gets to it, and the caller may be awake just then, in the handler of a signal on top of its wait.) If nothing is taken, nobody is woken. It is what a single
      thread does when it waits for a lock without a deadline ("only a finalizer can bring about what is waited for"), said for several. What cannot be given back is what lies in the caller's own
      queue: it made the object itself, or the maker has ended and the main thread is the heir.
    - A caller that leaves early (the handler of a signal raises out of `gc.collect()`) keeps what it serves of what is still taken, whoever else serves: it goes into the caller's own queue, and is made at
      its next safe point, like the rest of a group that it has in its hand. (Left to the makers: a thread that waited with a lock in its hand ran the finalizer that wanted it, after a Ctrl-C in the main
      thread.) So something is taken only while somebody serves.
    - In any other wait of the caller's nobody is told: whoever can move may end it. A thread can move that runs, asks for the GIL, has been started and has not run yet, is in a system call or in foreign code, sleeps,
      waits with a deadline, or waits for descriptors, which somebody outside may end; so can the main thread in the host's event loop. (Told whenever a finalizer of the caller's let go of the GIL for a wait that
      somebody else can end, the threads that sat idle shared the calls with the caller as soon as a finalizer started a thread, which waits for an event: of 200 such finalizers they made 83 to 104, one was not
      over when the callbacks of `atexit` ran in 3 runs of 4, and one that sat there with a lock in its hand ran the finalizer that wanted it. Told at every system call: anything from 0 to 200 of 200 were
      finalized when `gc.collect()` returned.)
    - So the cure holds for programs in which everything waits for threads. One thread anywhere that polls (`q.get(timeout=1)` in a loop), sits in `accept`, `select` or `recv`, sleeps or computes, and there is no
      standstill: `gc.collect()` does not return, as before. Nor is there one if the finalizer itself waits with a deadline (which then runs out), for a descriptor, polls with `time.sleep`, or spins. And what
      may end a standstill without a call that is owed is what has not happened yet: a signal that comes later, whose handler runs on top of the main thread's wait; a waiting thread's own later look for
      garbage. There the makers have got their calls back for nothing. (A thread that somebody has sent for already, whoever and whatever for, can move: `Sleeper::wake` counts, and the thread notes up to
      which wake it has come back, `ThreadState::wakesAnswered`.) With several runtimes in a VM: a thread looks at the calls of the runtime in whose wait it is, so the cure does not reach from one to another.
  - **What has to happen in one order is begun as one**: in a round that takes parents and children at once, an object with all that its going frees; all the finalizers of the cyclic garbage that a
    collection gives a thread. Such a group (`CallOwed::continuesGroup`) is taken off the queue whole by whoever begins it, and made call by call, each over before the next begins; what lies behind it in the
    queue is open to the next: while a caller of `gc.collect()`, or the main thread as it ends, has the rest of a thread's group in its hand, the thread itself, if it looks, goes on with what lies behind the group. (With two that serve
    one queue, the thread itself, woken, and a caller of `gc.collect()`, a chain of six was finalized by both at once, a child before its parent.) A group is no larger than that:
    whatever is in one group with a finalizer that waits lies in the hand of a thread that is blocked, and nobody takes from a hand. Callbacks are single calls. Cyclic garbage is the
    exception, as it is in CPython, whose collector calls all the finalizers that it finds in one thread, one after the other: two cycles that have nothing to do with each other are in one group if one thread
    gets both. (A child that two such trees hold goes with the one that lets go of it last. The other holder may be in another thread's hand: the two calls are then in no order.) The price: a caller goes on
    while another thread is in the middle of a group. A daemon has begun a chain of 200 slow finalizers when the main thread ends: none is over when the callbacks of `atexit` run; the main thread makes
    the other 199 behind those, in order; the one that the daemon was in is cut off, as any finalizer is that a daemon is in the middle of at the end. (When the queue was open call by call, the main thread
    made the 199 before the callbacks, the first of them while its parent's finalizer was still running.) Nothing but to wait for that thread would help: while a call of a group runs nobody may begin the next.
    Of the same kind, without a group: what has been given back at a standstill the caller shares with the makers, call by call, to the end, and the last may be a maker's: of 200 finalizers of 1 ms that the
    main thread had given back at exit, one was not over when the callbacks of `atexit` ran, in 10 runs of 20 (with `gc.collect()`, at its return: 1 of 20). (To take them again as soon as the caller goes on:
    its next finalizer polled for what one of them does, while the maker was still busy with another: the maker left it alone, and the program never exited.)
    And as long as a thread is stuck in the middle of a group there are finalizers left to be called, whoever collects: so the code of the `ctypes` callbacks that the program has dropped is kept.
  - **Which call next** is what it was when each thread had one queue and what such a caller's collections found went into the caller's (`Runtime::takeNextCall`, `CallOwed::comesBefore`): callbacks before
    finalizers; of two callbacks the one whose weak reference was made later (so the references to an object are told latest first, and a reference that a callback has made, to what it then drops, comes before
    all that wait); finalizers as they were queued. Each half of a queue is kept in that order, and lists are merged by it. One that makes the calls of all threads takes first what other collections have handed
    out to other threads, thread by thread, in the order in which the threads first came to owe or looked (`CallsOwed::rank`): that was found earlier than what its own collections have found. Its own
    `calls` and every `taken` that it may begin are one queue to it. And the rest of a group stands, for the thread that has it in its hand, where it stood: at the head of the finalizers of the queue that the
    group was taken from (`CallsOwed::groupsInHand`); if another group from the same place is begun in the middle of it, the two are one from then on, by their numbers (cyclic garbage of several makers that
    `gc.collect()` has found is numbered in the order of making, so by turns, and is finalized in that order). So wherever one thread makes the calls their order has not changed, but for this:
    - A caller that leaves early in the middle of a group of another thread's goes on with it at its next safe point; it used to lie in the other's queue until somebody served that.
    - What other threads owe when the main thread comes to its end: in the order in which it was found, as reference counts would have it. (The queues used to be taken over whole, one behind the other.)
    - A round that takes parents and children at once, handed out: each tree lies in the queue of the maker of its top, so a caller that comes later goes maker by maker. (The whole round used to go to one
      thread, in the order of reference counts.)
    - What a thread leaves when it ends, and what the threads that are stopped at exit have in their hands, is merged by number into the queue of the heir, or of the thread that held it. The rest of a group of
      such a thread's in another's hand stands at the heir's from then on; if the heir has it itself, in its one queue, so behind what other threads owe.
    What that order is not: the order of reference counts. The callbacks for one object are parted by a later reference to another; a callback for cyclic garbage comes before that of an earlier reference to
    what reference counts would have freed long before; what a call drops comes behind what waits, unless its reference is later. See DEFERRED.md.
  - **No such thread any more**: the main thread. A thread makes the calls it owes before it ends. When the other threads have been stopped for good at exit, everything is the main thread's.
  - **A thread that waits is woken** (`Threads::wakeToMakeCalls`: what it sleeps on, the host's event loop, or the signal that gets it out of
    a system call), makes its calls where it would look at signals (`checkSignals()`, as `PyErr_CheckSignals()` runs the collector in CPython), and
    goes on waiting for the time it has left. One that only waits for the GIL finds out when it has it: the flag that the safe points
    look at is the running thread's. In foreign code: when it comes back. A signal that comes just before the system call does nothing, so it
    is sent again, after 1 ms, 2 ms, 4 ms and so on up to a second, until the thread is back (`Threads::interrupt`, on the queue from which `VMTraps`
    sends its signals again and again). Once it is back none is sent, and none is on its way. The main thread gets a signal that is
    nobody else's (`wakeMainThreadFromSystemCall()`), the others `SIGURG`. A collection tells a thread once, however much it leaves for it: to
    tell is a system call. (Once for each callback of a weak reference, 200,000 of them took 145 ms, 100 ms of that in the kernel; now 42 ms.)
  - **And by nothing else.** The collector stops every thread with a signal to scan its stack, and a signal ends a sleep in the kernel. A thread that has given up the GIL to wait for another thread
    (`Threads::waitUntil`), to sleep (`time.sleep`) or to wait for descriptors (`blockingWait`: `select`, `poll`, `epoll`, `kqueue`) goes on for the time that is left, without the GIL in between, unless somebody has sent for it (`AllowThreads::wasInterruptedForNothing`). Whoever sends for a thread says so
    before the signal: `Threads::interrupt` by a flag, the end of the VM by another, everybody else by `Sleeper::wake`: a signal of the program's (which is for the main thread, wherever it was delivered), the traps
    that another thread has left to the main one; whoever ends what the main thread runs, for the host, does both (`Threads::wakeToHandleTrap`). (Each collection sent every thread that waits through the GIL: it asked for it, whoever ran had to give it up after a switch interval, and the thread made the
    calls in its queue, told or not. And a collection looks at the stacks twice: the second time it found the thread waiting for the GIL, in frames that had come to lie over what the finalizers which it had run
    on top of its wait had left there: of a chain of 100 objects that a waiting thread had finalized, 23 stayed alive until the thread went on, held by one word that no frame had written.) One thing that those wake-ups did by chance is gone with them: a thread
    that a collection had just given a call was asking for the GIL before the collection was over, and mostly began the call before a main thread that went straight on to its end took the queues of the others
    upon itself (DEFERRED.md, "At exit, a finalizer that waits for the finalizer of another thread's object"). Where there is no other thread the GIL is not given up, and every signal ends the wait as before: it may be the engine's own, for a trap that the host has set (a time
    limit of `node:vm`, `Worker.terminate()`). The other system calls that may take long (`read`, `recv`, `accept`, the `wait` family, `flock`, a semaphore) still come back for the GIL when a signal ends them.
  - **The signal is sent only where it loses nothing** (`AllowThreads::LosesNothing`, which the call site says; but for what the host ends: `Threads::wakeToHandleTrap()`): a call that transfers nothing
    before it returns and is made again when it was interrupted: `read`, `readv`, `recv` and its kin, `accept`, `poll`/`select`/`epoll`, the
    `wait` family, `flock`, `lockf`, `open` (of a FIFO), a semaphore, a sleep. CPython sends a thread no signal at all, and one that comes in the
    middle of `write`, `send`, `sendmsg`, `sendfile` or `recv(MSG_WAITALL)` makes the call return the part it has done, which a program that
    trusts a blocking call never looks at; a time limit that the kernel keeps (`SO_RCVTIMEO`) begins anew; `signal.pause()` returns. There, and
    in `connect`, `close`, `tcdrain`, `sigwait` and foreign functions, the calls are made when the system call returns.
  - The price: **a thread that waits runs Python code, and it may wait holding a lock.** W makes an object, hands it on, takes a lock and sleeps; another
    thread drops the object; its `__del__` wants the lock. In CPython the other thread waits until W lets go. Here the finalizer
    runs in W, on top of the sleep, and never gets a lock that is not re-entrant. It takes four things at once: the maker is not
    the one who drops, the maker waits, holding a plain lock, which the finalizer takes without a timeout. The data model warns of
    locks in `__del__` for the like reason ("may deadlock"), and none was found in the standard library or in 262 distributions.
    Not with what a caller of `gc.collect()`, or the main thread as it ends, has taken: that one runs the finalizer, and waits for the lock as in CPython.
- **At exit**, when the threads are joined and before `atexit`'s callbacks, what is unreachable by then and reference counts would
  have freed is finalized: in CPython it would have been long ago. (Only if an object is registered whose finalizer is code of the
  program.) After the callbacks, if there is anything left to finalize, `sys.modules` is emptied as CPython empties it, and
  collections find and order what only the modules held. What is then still held (by JS, which has imported a module, by a frame on the stack) is finalized in
  the order it was made, a holder before what only it holds. A program that leaves nothing to finalize exits without a collection; a generator that has not begun is
  nothing to finalize.
  - A function makes the namespace of its module a cycle, so nearly all that a program still holds when it ends hangs off a cycle. In the order of making, the raw file of
    `f = io.BufferedWriter(Raw())` was closed before the writer was asked to write what it held, which was lost, and of 10,000 objects held in all sorts of ways 78 % of those with one holder went before it. So what dies
    when the modules go and that reference counts would not have freed is finalized in the order in which the objects were made, but an object that is reached from the namespaces of the modules only
    through others goes after those (`DeadGraph::candidatesWithHoldersFirst`). That is what CPython's exit does in every version, however it is started and whatever the size of the program; the rest of its
    order depends on where the generations of its collector happen to be cut, and differs between two installations of one version in 35 to 48 % of all pairs.
  - Where such objects hold one another, the circle is opened where the names lead into it. A reference does not hold what it refers to if that leads back to where the reference comes from by a way on which
    nothing is nearer to the names than it is itself; nearer: fewer objects with finalizers on the way from a namespace; at the same distance what has no finalizer; of two objects the younger, which is the wrapper
    if one of them is. So of `raw.owner = f` the writer goes first if the program has `f`, or has both in a list, and a tree with links to the parents goes from its root down. A reference that is on no circle
    holds (unless it is to a namespace, or to an object with a finalizer that has a name of its own), what holds has no circle, and nothing depends on the order in which the collector comes to the cells (as far as
    the bound below is not reached). (A search from the names that took the first way into a circle for the way in
    depended on that order, and let what lay behind the first of two holders go before the second.) All of it is finalized in one round: what has waited a round stands in the lists in the order that the
    round before has given it. Among what is still held when all else has gone (by JS, by a frame) only what the objects with finalizers lead to is looked at: a list with a name that holds them is not
    in it, and a circle that no way leads to is entered at its youngest.
  - The two ends of a reference that does not hold count as one cell if they are equally near, and so do all the cells of a circle that is not opened, or that has no object with a finalizer in it: they are free to go,
    together, when nothing holds any of them. So a document over a tree of plain nodes with links to the parents goes before the older resource of a leaf (`res = T(); root = Node(); leaf = Node(); root.child = leaf; leaf.parent = root;
    leaf.res = res; doc = T(root); del res, root, leaf`), and what holds one list of a cycle of lists holds what hangs off the other. That makes it a law: an object goes after every object with a finalizer through which all the ways to it
    lead, from the names and from what nothing refers to (garbage that dies in the same collection is a way in, too). CPython keeps that law in every state. It holds as far as the bound below is not reached (with a coarser
    "nearer" the two may fall together, and go the older first; so may a writer and its raw file), and as far as the graph goes: beyond 4,000,000 cells what is reached is taken to refer to nothing, and where only what the objects with finalizers lead to is looked at (see above) a list with a name that holds one of them is not
    in it. Not covered: two plain
    cells that are equally near and lead to one another only by way of objects with finalizers stay two: in `x = T(); y = T(); a = [x]; b = [y]; x.r = b; y.r = a; ha = T(a); keep = [T(b)]; del x, y, a, b` `x` goes before
    the holder of `b`. (A search stops when it has accounted for every reference to its cell, so it does not know all that the cell reaches, and all the ways of opening a circle are to work out the same.)
    It takes no looks: which cells are one is noted where a reference is found not to hold. The holders are counted cell by cell as before, and a bit in the count says that a cell is one with others: where nothing is, that is
    all that is paid. Memory: 12 bytes for each cell that is in any circle, opened or not, from when two cells of a circle that is opened are found to be one; if it is not to be had they stay two, and the law does not hold
    for them.
  - What leads to no object with a finalizer is left out where the holders are counted and the cells are let go, which in most heaps is nearly all of it: it lies on no way from one such object to another, so the order is the
    same without it. That is one bit a cell, worked out as the circles are found: a circle, or a cell that is in none, is found after all that it leads to.
    With both, the function by itself, compiled apart, before and now, in ms, medians of three. 4,000,000 cells that lie in memory as they hang together, and lead to no object with a finalizer: nothing in a circle 215, 191; a
    tree of plain cells with links to the parents 236, 238; pairs of plain cells that know each other 229, 207. The same where they lie scattered: 2,646, 1,770; 1,640, 1,572; 1,473, 1,276. Where nearly every cell leads to one
    (each plain cell refers to two earlier cells at random, every hundredth cell is an object with a finalizer) 1,198, 1,348, scattered 2,548, 2,541. All of them objects with finalizers: pairs that know each other 348, 372,
    scattered 1,824, 1,875 (nothing becomes one there: the cells of each small circle are numbered once more, and each circle is looked at once more at the end); a doubly linked list 349, 358; tangles of 64: 1,099, 1,101; two
    tangles 4,209, 4,449. It costs more where cells become one. A large circle of plain cells that is opened: 1,500,000 cells: a doubly linked list between two objects 304, 330, scattered 1,197, 1,275; a tree with an object
    for its root, in a list, 366, 431, scattered 1,020, 1,173; 1,000,000 plain cells each of which knows the next and two others, in a ring with one object: 736, 1,020, scattered 1,091, 1,355. Very many small circles, each of
    which is counted and let go as one, in the order of its holders and not where its cells lie: 4,000,000 cells in circles of one object and three plain cells 366, 477, scattered 2,810, 3,079; in pairs of plain cells that
    know each other, one of each pair in a list that one object with a finalizer holds, each pair with such an object of its own: 219, 262, scattered 998, 1,908; in pairs of plain cells that know each other, each of which
    refers to one object with a finalizer that has no name: 239, 294, scattered 1,560, 2,778.
  - That is worked out with the world stopped. **A circle of up to 64 cells is always opened, and as the rule says, whatever else is in the heap**: from each cell what it reaches among what is no nearer, as
    a set of bits: two looks at each reference of its cells (a third if it has two cells without a finalizer, to see which are one), some 64 steps for a cell, no memory but what cells that are one take. So a writer and its raw file never go short. Larger circles share a bound: 16,000,000 looks at their cells and
    the references inside them in all (`FinalizationQuestions::mostLooksAtInnerReferences`), the smallest first, by the number of those. Where such a circle has more than eight degrees of nearness, first by
    searches from each cell, the nearest first, which in a list, a tree, a ring or a class that keeps its instances are over after a few cells each; if they run out of looks (four for each cell and
    reference), or there are fewer degrees, by division, which takes three looks at each reference for each halving of the number of degrees, whatever the shape. All the ways work out the same. (Where
    assertions are on they are compared: in every small circle; in a larger one of up to 10,000 cells that is opened, if the searches come to an end. That is done to the side: the circle is then opened as in a
    build without, on the same account.) If the looks do not suffice for all the halvings, neighbouring degrees count as one: the same rule with a coarser "nearer". A circle for which there are not twice as
    many looks left as it has cells and inner references, or not eight for each cell, so any of more than 2,000,000 cells, is not opened, and costs no looks: it counts as one cell, and when nothing holds any of it its objects go in the order of making. The looks
    suffice for a tangle of 90,000 objects each of which knows three others, and for a list, a tree or a ring of 1,500,000. As long as they do, nothing depends on the order in which the collector comes to the
    cells or their references; behind that, on the order of a cell's own references, which the object fixes: so the order is the same from run to run. (`-X finalization_order_looks=N` lowers the bound: for tests.)
    All this is done only at exit, from where the modules have gone: by every collection that comes then, the engine's own too, each with a bound of its own; the first finds nearly all.
    The function by itself, compiled apart, on a busy machine, before circles were opened and with that, in ms, for 100,000 / 1,000,000 / 4,000,000 cells: nothing in a circle 6, 5 / 50, 49 / 205, 205; a tree of plain cells with links to
    the parents 5, 6 / 59, 62 / 236, 234; the same with an object with a finalizer for its root, in a list, 4, 40 / 81, 273 / 335, 325; all of them objects with finalizers: pairs that know each other 6, 8 /
    76, 92 / 367, 384; a doubly linked list 5, 20 / 53, 223 / 206, 342; a tree 6, 24 / 66, 326 / 271, 391; a class that keeps its instances 9, 25 / 112, 334 / 519, 317; a doubly linked list that was not
    made in the order of its links 13, 51 / 437, 1,694 / 2,885, 4,444; each knows three others at random 11, 242 / 202, 2,172 / 1,711, 4,158. The worst that has been found, at 4,000,000 cells, in s: two
    tangles of 2,000,000: 1.1, 6.0; two lists out of order: 2.1, 5.5; tangles of 64, all of which are opened: 0.6, 1.2 where each lies together in memory, 2.8, 7.8 where it lies scattered. So up to 21 times
    what it was, and at its worst 7.8 s where it was 2.8 s: for 4,000,000 objects with finalizers, the calls of which take longer than that. (Without the bound: 120 s.) In the engine, the whole end of a
    program with 1,000,000 objects takes 0.2 to 0.3 s longer for a list, a tree, or plain cells in a circle with one object with a finalizer; 2.1 to 3.2 s longer for a list out of order or a tangle (which
    is as much again); no longer where no object with a finalizer is in a circle: a large cache beside an open log file. (Those are the figures of when the circles came to be opened. For what has come since see above.)
    Memory for the circle in hand: 20 bytes for a reference inside it, 68 for a cell: 300 MB at the most, asked for before anything is done: if it is not to be had the circle is not opened.
  - Daemon threads are still running during the first of these steps, and in CPython they finalize what they drop themselves. So the step begins
    an era (`Runtime::m_era`, which each object with a finalizer is registered with; weak references have serial numbers): the main thread
    is a caller as above that serves only what is of an earlier era, whoever made it. It takes more: beside what its own collections find, what the others owe when it comes, and again before each further
    collection of its own. What another collection hands out while the step lasts is its maker's as ever, who is told; the main thread makes it if it comes to it first, where it stands at the head of that
    thread's queue; to the main thread it is part of its one queue. (Taken, whichever collection finds it: the main thread's finalizer polled for what the finalizer of an object does that its maker dropped
    meanwhile: the maker's own look for garbage found the object, and the maker left it alone: the program never exited.) So a maker may make such a call at exit, too: one that waits with a lock in its hand
    that the finalizer wants runs it on top of that wait, as at any other time. What a thread has made since is for that thread alone, as at any other time; but all the cyclic garbage that one collection gives a
    thread goes by its oldest. Behind something younger at the head of a queue the main thread does not look: that thread has made something since, so it is awake; if it never comes back, what is older is
    taken before the next collection of the step, or, if there is none, made behind the callbacks of `atexit`. It ends when a collection leaves nothing that the main thread may begin.
    (It used to pay until no thread owed anything, and each of its collections took all garbage upon
    it: beside two daemons that keep making objects with a `__del__` the process ended after 1 to 60 s or not at all, with millions of
    finalizers called by the main thread; now in half a second, most of it for what the daemons had left when they were stopped.)
  - What it costs, measured: black formatting its own 24 sources in one process (7 s, 330 MB) makes one full collection at exit, 99.9 ms of
    marking and 13.2 ms of sweeping; from its last line to the end of the process 112 to 142 ms, against 28 to 31 ms with `os._exit()`.
    It is made for one object: the tokenizer's generator for the last file, dropped while suspended inside `try` / `except`. To
    close it evaluates the `except` clauses, as CPython does when it is dropped. Whether one old object is dead takes a full marking:
    a collection of the young does not find it (and takes 18 to 27 ms), and to leave out the sweep would save 0.2 % of the run.
- **What a daemon thread still holds at exit is never finalized**, as in CPython, where the frames of a thread that never runs again
  never let go: its variables and temporaries, the arguments of the built-in functions it is in, and by way of its functions their
  globals, with all that those lead to (`whatOtherThreadsHold`). A file among that is not closed, and what was written to it and
  not flushed is lost. A finalizer would see what no code was meant to see: a thread stopped half way through `close()` has
  closed the descriptor and not yet said so. The frames are walked, not scanned: Python code runs in the LLInt, where every value
  of a frame is in a register that the code block knows of. As CPython empties the modules that are still referred to once
  `sys.modules` is gone, and `sys` and `builtins` at the very end, the walk does not go into a module object that it meets, nor into its
  dict. What belongs to the thread and not to its frames goes too: what it kept in a `threading.local`, the exception it is handling.
- **`gc.collect()`** collects, serves the queue, and repeats until nothing is left to do, so `del x; gc.collect()` does what it does in
  CPython. For that to be true the stack scan must not see stale values: before it, the temporaries of Python frames that liveness
  analysis says are dead are emptied (`clearDeadTemporaries`). Variables are not: an object lives while a variable names it.
  And the frames of C++ between the Python code and the collector must not lie over what earlier calls left on the stack, for what
  a frame does not write it still holds: the stack below is wiped before the first of them (`NativeFlag::OnWipedStack`).
  What is wiped lies below the frame of the function that asks for it, and the first thing to lie there again is where the locals of
  the Python function that returned last were. So that function (`callNativeFunctionOnWipedStack` and its like in
  `PyNative.cpp`) is reached by a tail call and has nothing in its frame but its two parameters: in a build that is not
  optimized every variable and temporary of every branch has a slot, written or not. And what wipes is part of the LLInt
  (`llint_python_wipe_stack_below`): a function of C++ cannot wipe its own frame. The rule: a frame that is made on wiped stack
  has nothing stale in it for as long as it lives, since what it calls lies below it; a frame that is made after something else
  has been called from the same place has whatever that left. So the stack is wiped before every call on the way to a
  collection that is to find things dead: before every round of `gc.collect()`, for the last round has gone through the very
  objects that this one is to find gone, at exit (`finalizeInterpreter`, `finalizeAllAtExit`), and before finalizers and handlers
  of signals are called at a safe point (`makeCallsOwed`), whose frames of C++ stay while those run.
  Nothing is wiped before an ordinary call of a function written in C++. If that calls back into Python code (`max(l, key=f)`, a class
  with an `__init__`) and a collection is made there, its frames may lie over what an earlier call left, and an object that was dropped
  just before may still be seen. The slow path of a call clears 256 bytes below the frame (`linkFor` in `LowLevelInterpreter.asm`). The first
  two calls of a site come by it, and every call of what C++ has to call; a site that is linked, or has gone virtual, does not. A
  program that has `x` in 60 variables of a recursion, drops it, and asks in `f([1], key=check)` whether it is gone hears yes 0 to 2 times
  out of 18, for `max`, `min`, `sorted` and classes with `__init__` or `__new__`, whether the site has had one callee or several. (While a
  site with several callees took the slow path each time, it was 1, 18, 6, 0 and 18 for these; with one callee, as now.)
  One such word is made by the compiler of C++ in the engine's own code: a `bool` in a callee-saved register whose other seven bytes are left over from the address of a cell. The collector
  takes an address inside a cell for the cell, so the word stands for the neighbour in front of that cell whenever the last byte of the address was less than the size of the cells: 160 in 256 for the
  keys and values of a small dict. `test_contextlib.test_nokeepref` meets it in the first resume of a generator (`Interpreter::executeCallImpl()`), and fails in two runs of three, on some builds and not on others.
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
  The collection that is looked at is the one asked for, from beginning to end (`PreventCollectionScope` in `collectAndFinalize`, as for `get_referrers()` and for buffers): what was under
  way or had been asked for is finished first, and no other begins until what was found has been put where the collector sees it. What that earlier one frees is neither counted nor saved, whichever
  generation is asked for (ten young cycles, a collection that has been asked for and is not waited for, `gc.collect(0)`: 0). But it ends inside the call, so it is one that the caller waits for (`Runtime::WaitForCollection`), of whichever kind
  it is: it looks at all that has died, orders the finalizers as reference counts would and hands them to the caller, as the one asked for would have done (`x = [A(1), A(2), A(3)]; del x; gc.collect()`: 3, 2, 1). A window
  stays: a collection of the engine's own that begins after the drop and ENDS before the call takes the items for siblings, in the order in which they were made. Only an allocation between the two can set one off: none in 120,000 rounds. What looks on (`Census`) notes cells by their addresses
  and has them marked for one collection. A full collection of the engine's own that was under way when `gc.collect()` was called used to be taken for the one asked for; that one followed, freed
  the cells, and with `DEBUG_SAVEALL` their addresses went to `gc.garbage`.
- **What the module `gc` hands out** (`get_objects`, `get_referrers`, `get_referents`, `garbage`) is decided in one place, at the top
  of `PyGC.cpp`: a cell is given to the program if it is an object of Python of this realm, of a kind that programs see, whole (no
  tuple that is still being filled, no frame object that lacks its caller), and not a part of something else (the table of a dict, the namespace of a class, the values
  of a frame, the memory of a deque (`TypeFlag::OwnsContainers`), what a module written in C++ keeps under a hidden key or in a slot
  of the runtime). A reference held by a part counts as held by its owner. A generator or coroutine that is running refers to its
  variables, as in CPython, though they are in a frame on the stack meanwhile (`forEachVariableOfRunningGenerators`); the variables
  of a function that is running belong to nobody, as in CPython.
  `get_objects()` and `get_referrers()` find what nothing of the program's refers to, so they also pass over all that is left of an exception that nobody has seen, and all that leads there (`HeapView::findWhatIsOutOfReach`; below, "So what walks
  the heap does not count on it"). `get_referents()` begins at what the program has. That may be a generator that is running in another thread and refers to the exception that it is handling. So all three, before they look, have the exceptions
  that any thread is handling made fit to be seen (`temporariesMayHaveEscaped`; `get_objects(0)` and `get_objects(1)` give nothing and look at nothing), and that behind their audit hooks: a hook that lets go of the GIL lets the other threads raise new ones.
  `get_objects()` and `get_referrers()` begin with a collection (`collectBeforeLookingAtHeap`), so that what is dead and has not been swept does not come back. It calls nothing. What reference counts would
  have freed is gone in CPython: its finalizer is owed from then on, by the thread that made it, and until that has been called it is not given, nor does it count as a referrer. (It used to be given, and was
  then finalized in the hands of whoever had got it: a generator taken out of `get_objects()` while suspended was closed a moment later.) Nor is what only such an object leads to, which is kept with it:
  of a chain of six objects with a `__del__`, each of which holds the next in a list, the five lists were given, whose items were then finalized in the program's hands. So that collection is listened
  to (`WhatIsGoneInCPython`): when all that is reachable is marked and nothing has been kept yet, whatever waits for a finalizer, since whichever collection and in whichever runtime of the heap, is dead
  like all that it leads to, and the dead cells are let go of as reference counts would (`PyFinalizationOrder.cpp`). What of that is marked in the end has been kept, by whatever way, and is passed over. So is whatever is in a
  queue and held all the same: a weak reference, which lives until the finalizer has been called, has given it back to the program. It costs nothing if nothing that has something to finalize has died or waits. What is in a cycle, or hangs off one, is still there in CPython until a
  collection comes: what of that has a finalizer is left as it is (`leavesCyclicGarbageForLater`) and is given alive, and being held it is no garbage any more.
- **The engine collects when it sees fit.** `gc.disable()`, `set_threshold()` and `freeze()` do not change that: the collector is the
  only thing that frees memory. A collection of the engine's own is told to `gc.callbacks` and counted in `get_stats()` at the next
  safe point: "start", then what it found (callbacks of weak references, finalizers), then "stop"; a collection of the young
  generation as generation 0, a full one as generation 2. Not while the collector is "disabled". Nor is the program told of one that comes between "start" and "stop", in whichever thread: it is
  counted, and what it finds is called before "stop" with the rest. That goes for `gc.collect()` in another thread too, which does nothing there in CPython: here it collects, untold. In CPython nothing is collected in between, and a callback that brings about a collection whenever it is called would never
  let the program go on. One place knows who is between the two (`Runtime::m_threadBetweenStartAndStop`): a thread that tells of a collection of the engine's own, or one in `gc.collect()`. Until it has told "stop" nobody tells
  anything: of two collections that wait to be told, the second waits for the "stop" of the first, whichever thread comes by. `gc.collect()` first makes the calls that its thread owes, and so tells of what the engine has collected
  before: there need not have been a safe point on the way. And it tells "stop", and counts, on whatever way it is left: what the handler of a signal raises between two finalizers goes on to the caller behind that. `gc.collect()` does nothing when one of `gc.callbacks` calls it, as in CPython, where a callback runs within the collection. Not so in a finalizer that is called between
  "start" and "stop": in CPython most of these run outside any collection, so there the call is put off until the calls that are owed have been made, as at any other time. Where the runtime collects as a means to an end and CPython collects nothing (`get_objects()`, `get_referrers()`, buffers that nothing can reach any more), that is
  neither told nor counted (`gc.collectsForItself`).
- **A thread that waits collects.** A program may wait for what only a finalizer brings about: it has dropped a response without
  closing it, and waits for a server in another thread that waits for the end of the connection. A thread that has waited for
  50 ms collects and finalizes (`Threads::collectWhileIdle`), then less and less often; so does, at once, one whose wait nothing
  else can end (no time limit, no other thread). One short wait after another counts as a long one (`while ref() is not None:
  time.sleep(0.01)`), as long as the thread runs for no longer between two waits than the first of them lasted. What it finalizes is what reference counts would have freed, which in CPython would have been finalized
  long ago. What is in a cycle or hangs off one is kept for a collection that the program asks for or brings about by allocating:
  CPython's collector does not run in a thread that waits either, and such a thread may hold the very lock that a finalizer
  wants (`with lock: time.sleep(1)`). Only the one that would otherwise wait for ever finalizes everything. Such a collection stops every thread, and any number of threads may be idle beside one that works: all of
  them together spend at most a twentieth of the time on it, a fiftieth if nothing has been allocated since the last one, as far as can
  be told, and no object with a finalizer has been made (to drop something takes no allocation). The time of all processors
  counts: the collector marks on several, and a process that only waits is to use next to none. But they are never blind for longer
  than 5 s, unless that came to more than a tenth of the time. A collection that is due when the share is used up is put off until
  there is some again, not left out. And one thread collects at a time: finalizers run in the middle of it, and other threads
  meanwhile. While such collections find something to finalize, whose thread finalizes it and so
  uncovers more, the threads that wait look again after 50 ms and may spend half the time, but only if that thread waits too: one
  that works leaves something to finalize all the time, and has to stop for every collection. Not for what the collecting thread itself
  has finalized: it goes on until it finds no more of its own. And eight times at most, and then not again until two collections in a
  row have found nothing. For a thread that polls and drops something each time round (a response, a generator with a `finally`)
  also leaves something to finalize every time, which no finalizer has uncovered.
  (Without the condition that the thread waits too, two idle threads
  of a process pool that nobody had shut down made the tests of black take 39 % more instructions and 97 s instead of 65. A thread counts
  as waiting by what it is in, an `IdleWait` or a system call that loses nothing when it is interrupted, not by whether it holds the
  GIL: a thread that hashes 32 MB at a time does not, and beside two idle threads it saw 63 full collections in 3 s and took 32 %
  longer; now 2, and no longer.) And a wait with less left than twice what the last such collection took is not
  made longer by one: the next wait begins with it. That goes for waiting for another thread
  (`Threads::waitUntil`) and for waiting for the kernel, which is done piece by piece (`IdleWait`): `time.sleep`, everything in
  `select` and every socket call that goes through `waitForEvents` (with a timeout, or on the main thread of a host with an event
  loop), `os.read` and `FileIO` (so `input()` too), `os.waitpid` for one process and `os.spawnv(os.P_WAIT, ...)` on Linux.
  - *What that comes to, measured.* With 2,000,000 objects alive, a loop that drops one object with a `__del__` and sleeps for 1 ms used 91 % of a
    processor (266 full collections in 30 s, half as many rounds); now 6.5 %, 14 collections, 97 % of the rounds. Naps of 0.1 s: 65 % and now
    5.0 %. With 1,000 objects alive: 18.7 % and now 6.3 %. Two threads, one of which drops what the other makes: 93 % and now 9.9 %.
    How long an object waits for its finalizer when it is dropped while every thread waits, whatever went before (a poller,
    such a pair, trees across threads, allocation between naps): at most 4.4 s with 2,000,000 objects alive, 0.3 s with 1,000; twenty
    times what the last collection cost. Before, a collection that was refused was left out, and the next came four times
    as late: 4.3 s and 17 s.
  - *The list of objects with finalizers holds complemented addresses.* Three threads that each wait for the finalizers of trees three levels
    deep, beside those 2,000,000 objects, were done after 1.1 s, but in 14 runs of 320 after 6, 11 or 17 s: collections found nothing,
    because the objects were marked. The stack of another thread that waited had their addresses, 16 bytes apart, each with the
    number of a thread beside it: a piece of the list. `memmove` copies with vector registers (on that machine `zmm16` to `zmm23`), which
    nothing else uses: a thread that has collected, and so copied the list, goes on waiting with up to 512 bytes of it in them (seen in
    a debugger). A collection suspends every thread with a signal, and the kernel saves all registers on the thread's stack, below what is
    in use, those registers on top. When the thread wakes while another collects, it waits for the GIL, which takes more frames than it had:
    they come to lie on what the kernel wrote, and what they leave unwritten, and the 128 bytes below the stack pointer, which the
    collector reads too, is still the list. (The addresses agree to the byte.) Threads that wait wake together since a
    collection that is refused is put off to the moment the share allows. Now what lies there is no address: 1 run of 320, and
    0.8 s. (In such a run the registers held addresses 8 bytes apart: some other list that a collection copies. Nothing is done
    about those.) The rule: a table that refers weakly to cells and is copied in bulk holds complements. That what it holds is no
    reference is what such a table means, and nothing is wiped for it.
  - *A piece that ends is not a signal.* A signal that comes between two pieces interrupts neither, so signals are looked at
    after every piece (`IdleWait::didWait`); one that comes between that look and the system call waits for the end of the
    piece, where in CPython it waits for the end of the whole call.
  - *"Nothing to read" is not "`read()` would wait".* A read of no bytes, of the end of a pipe that is open for writing, of a
    socket that listens or has `SO_RCVTIMEO`, of a terminal with `VMIN` 0 returns at once or in its own time. So
    `waitUntilReadable` waits in `poll` only for the kinds of descriptor of which the two are known to be the same
    (`readWaitsAsPollDoes`), and asks only after `poll` has said that there is nothing: a read that finds data costs one
    more system call, no more.
  - *The stack.* The collection is not made from inside the function that waits. The frames of C++ between the code in Python and the
    collector are full of words that look like references: what earlier calls left where they lie now, and what they have
    written themselves (a `bool` stored over the last byte of a slot that held the address of a cell leaves the address of a
    neighbour of the cell, which is what was made just before or after it: the very object that the program has dropped). To wipe
    the stack before the call takes care of the first kind only. So a native function whose wait has come to the point where it is
    to collect gives up (`NativeFlag::MayWait`, `IdleWait`): how far the wait has come is noted, something is thrown that
    nobody but the caller of the function sees, its frames go, the stack below is wiped, the collection is made and the
    finalizers run from there (`callNativeFunctionAgainAfterCollecting`), and the function is called again. Its wait takes up the
    deadline and the interval of the one that gave up, so a timeout does not start anew. Nobody hears of a second call: the
    events of a call are its caller's, and `time.sleep` tells the audit hooks when it begins, not when it goes on.
    The collections of other threads look at the stack of a thread that waits too. So a wait that is about to block gives up once in
    the same way, without a collection (`IdleWait::willBlock`): for as long as it lasts its frames lie on wiped stack.
    - *Whose wait it is.* Code in C++ calls functions written in C++ without a frame, so the frame on top of the stack does not tell the
      wait of the function from that of something it calls, or of what runs while it waits: a lock of the interpreter
      (`ThreadLock`), the finalizer of a socket, a callback of the host's loop. Were one of those to give up, its caller would take what
      is thrown for a failure. So the function says which wait is its own (`isWaitOfCall`), that wait takes the note that
      the call may begin again out of the thread's state and keeps it for itself, and while the collection runs there is none.
    - *What it takes.* A function that has done nothing that must not be done twice when it waits, that waits in one place and keeps no
      time of its own (a socket call that waits again after a false alarm goes by the deadline that the wait gives back), and
      arguments that can be looked at again without running code: none of them is a cell, but for the object whose method it is
      (`Threads::willCallFunctionThatMayWait`). `time.sleep` while there are audit hooks says so itself
      (`mustNotBeginCallAgain`). With other arguments (`select.select`: lists; `time.sleep(x)` with an `x` that has `__float__`) the
      stack is wiped before the call and the collection is made from inside, which is second best. A function that may have
      consumed something when it comes to wait (`readall`, the buffered readers) has not even that: the collection happens, but
      what the frames above hold by chance stays. So does what the frames of another thread hold that sits in such a function.
    - *The frames under which a thread waits have little room that is not written to.* That is the rule for the C++ on the way to a system call that may take long
      (`fileIOSystemRead`, `waitForEvents`, `blockingWait`, `socketRecv`, `posixRead`, `Ctypes::callFunction`): no buffer, no inline vector and no `AllowThreads` (1.5 KB, written as the GIL is given
      up) in a frame that waits for something else first. What needs room has a frame of its own beside the wait (`NEVER_INLINE`), or the room is written when it is made (`RoomForArguments`, `WaitRequest::asked`;
      a struct that lies there has no padding: `PY_BEGIN_WITHOUT_PADDING`). It makes what stays by chance rare, not impossible: padding, slots of branches not taken and the frames of the callers
      remain. Not yet so: a socket with a timeout, `recvmsg`, `accept`, a text file's frames above the buffered one, `StreamUse<&bufferedRead>::call`, `os.read` of what `poll` does not speak for, `os.readv`, a function of a `PyDLL`.
      Nothing guards the shape but the compiler's word on padding: how many objects stay is a matter of one kind of build (with assertions, or optimized at link time, the frames are others), so no test counts them.
      `tools/kept_by_a_waiting_frame.py all SYMBOLS`, run by the `bun` in question, says for some ways to use an object what is still alive while the thread waits, what a heap snapshot says of it, and in whose frame the word lies.
    - What runs between giving up and beginning again (finalizers, and other threads while those wait) finds the function not begun: a
      `poll` object is not in use, a lock that a finalizer takes for this thread and keeps is this thread's.
    - *What JSC's own entry leaves unwritten.* Every entry into the VM (`vmEntryToJavaScript`, `vmEntryToNative`) makes room on the stack
      for a `VMEntryRecord`, and `doVMEntry` does not write its `calleeSaveRegistersBuffer`: that is filled when something is thrown
      (`copyCalleeSavesToEntryFrameCalleeSavesBuffer`). Until then it holds what earlier calls have left there, for as long as the entry
      lasts. Seen in a debugger: the scope of a function of JS that had returned, with no edge to it and no root in five heap
      snapshots, and its address in `calleeSaveRegistersBuffer[0]` of the entry that ran the test; in 4 runs of 450 it kept a generator of
      Python alive that the test had dropped. On x86-64 there is one more such word: `vmEntryToJavaScriptWith1Arguments`, `With3` and `With5`, the last of which resumes a
      generator for whatever is written in C++ (`next`, `list`, `send`, `a, = g()`, `in`), make room for a slot behind the arguments that keeps the stack aligned, and do not write
      it. (On AArch64 it gets the last argument once more, by a store of a pair; the general entry fills it with `undefined`.) Seen in `a, = g(A(), A())`, where the generator lets
      go of its arguments and collects: the second `A` was alive, a cell with no edge to it and no root in the heap snapshot, its address in that slot, 88 bytes below the frame pointer
      of the entry, last written by the prologue of a function that ran while the `A` was made (a watchpoint says so). When the statement is over it is gone. Nothing is done about
      either: a store at every entry is a store that JS would pay for. A test that asks whether something has been collected asks it of several objects, and not of all of them, or
      asks when the entry is over and something else has been on that part of the stack.
  - *`flock` cannot be made piece by piece.* `flock(open(p, "w"), LOCK_EX)`, the file dropped, and the same again: with reference counts the
    first file is closed by then. Here it would hold its lock while the second call waits for it, for ever if there is no other thread; and with
    `LOCK_NB` ("is another instance running?") the second call would be told that somebody has the lock. So every call that asks for a lock asks
    without waiting first, and if the lock is taken and this very process has one on the same file (by `fstat` of the descriptors it has
    locked), it collects and asks again. Whatever the share of the time that such collections have, if one of those locks has not been through
    such a collection yet, and it finalizes what reference counts would have freed whichever thread made it, since the answer is wanted now; nor
    does it count against that share. If all of them were in use the last time, this may be a program that polls, or whose threads contend for
    the lock: then within the share; if that is refused, a call that waits blocks, and a thread that waits looks within the 5 s, and one that
    does not wait is refused though the file may have been dropped since. A call that would wait in the only thread there is collects in any
    case. A lock that another process holds costs one or two more system calls. A copy of the descriptor (`dup`, a child) keeps the lock as it
    does in CPython. A file that hangs below a cycle of objects without finalizers (`l = [f]; l.append(l)`) looks like plain garbage to such a
    collection, and gives back a lock that it keeps in CPython.
  - Not covered: `recv` and the like on a blocking socket in a thread other than the main one, writes, `connect`,
    `wait()` for any child, `sigwait`, `sigtimedwait`.
- **What only a temporary holds is finalized at once**, where that can be known without the collector (`PyTemporaries.cpp`, which has the
  argument in full). `bytes(memoryview(b)[:n]); del b[:n]`, `open(p, "w").write(s)` and `for x in g(): break` count on it. The compiler leaves marks beside the
  code, no instructions: this call or subscript makes a value that goes in a temporary register, and that instruction is its one
  use (`PythonMark::TemporaryMade`, `TemporaryUsed`).
  - A memoryview looks at the marks when it is made. It is noted as owned by the temporary only if the use is known *then* to keep
    nothing: a method of memoryview that says so (`NativeFlag::KeepsNothing`), a subscript, being passed to a callee that is
    already waiting in a register and says so (`bytes`, `bytearray`, `memoryview`), being stored in an item of a bytearray
    with nothing to run in between. To find out afterwards is not sound: a use that keeps the object (a function in
    Python, a store in a list) runs no code that could say so, and addresses of frames and offsets of instructions come
    round again. When the use is over, or has raised, the view is released.
  - `open()` does the same with the stream it returns, which nobody else has seen: `open(p).read()`, `open(p, "w").write(s)`, and so with the
    other methods that read, write, move or ask, which keep nothing of the stream they are called on. When the method has returned or raised, the stream is
    finalized as the collector would do it, `ResourceWarning` included, on the line where CPython gives it. Of several in one call
    (`open(w, "w").writelines(open(p))`) the last made goes first, as CPython lets go of them. Not `json.load(open(p))` or
    `for line in open(p)`: what a function written in Python does with its argument is not known when the file is opened.
  - A generator function looks at the marks too, and so does what makes the generator of a generator expression. It makes a note of the generator if its use is
    the head of a `for` loop or of a loop of a comprehension, to be searched by `in` or `not in`, to be taken apart (`a, b = g()`), or to be
    passed by position to a built-in function or type that is already waiting in a register and says that it goes through such an argument and keeps
    nothing of it (`NativeFlag::ClosesGeneratorArgument`: `next`, `any`, `all`, `sum`, `min`, `max`, `sorted`, `functools.reduce`, `str.join`, `bytes.join`, the `extend` of `list`,
    `bytearray` and `deque`; `TypeFlag::GoesThroughArgument`: `list`, `tuple`, `set`, `frozenset`, `dict`, `bytes`, `bytearray`, `deque`). The use takes the note up:
    `op_py_get_iter`, `op_py_compare` or `op_py_unpack`, the next instruction to run there, or the function when it begins (`GeneratorArgumentScope`), which forgets the
    notes that were made for its call and are of something else (`next(it, g())` may return the generator). There is room for four notes; a fifth pushes out the oldest, whose
    generator waits for the collector.
  - `for i, x in enumerate(g())`, `any(map(f, g()))`, `list(islice(g(), 3))`, `for a, b in zip(g(), h())`: the generator is in an iterator that lets nobody have what it was made
    from, and the iterator is where the generator is above. Such a type says so (`TypeFlag::HasIteratorsToItself`: `enumerate`, `zip`, `map`, `filter` and, in `itertools`, `accumulate`, `batched`, `chain`,
    `compress`, `cycle`, `dropwhile`, `filterfalse`, `islice`, `pairwise`, `starmap`, `takewhile`, `zip_longest`; not what derives from them). To be passed to it is a use that notes are made for, and
    when the iterator is made, the notes that were made for the call are about the iterator and its use from then on (`Runtime::iteratorWasMade()`), if that is such a use again. `iter()` passes its argument
    on in the same way. So it goes on for as many levels as are written. The iterator remembers that there may be something in it (`hasGeneratorsToItself`), and whoever closes what it owns looks for
    the generators in such iterators only (`forEachGeneratorBehind()`), in the order in which CPython lets go of them: the last item of a tuple first. An argument that is passed by keyword is not
    noted: `accumulate(x, initial=g())` and `zip_longest(x, y, fillvalue=g())` hand it out. What others pay: one test for "is there a note" where a type is called, and a look at the type and at that
    mark where a loop over such an iterator ends.
  - The loop closes a generator it owns on every way out but the end: an instruction on the way of `break`, `return` and the `continue`
    of an outer loop; for an exception the unwinder does it, from the marks that tracing has for loops, before anybody is
    told that the exception is caught or leaves the frame, as CPython pops the stack of a frame first. The function closes it when it
    returns or raises; if a profiler is waiting to hear of that, after it has. `in` closes it when it has found the item, or `==` has raised; the assignment, when there were too many
    items. A loop that ends closes what it owns too, for `zip(g(), h())` ends with the first that does. An iterator that lets go of what it went through before it goes itself, as in
    CPython (`islice` when it has given what was asked for; `batched`, `cycle`, `pairwise`, `zip_longest` and `chain` at an end), closes what was owned with it then. A generator that the
    collector would leave alone (no handler in it, nothing in its frame to close) is left alone here too, unless a tool listens to
    its code, which is then told of the `GeneratorExit`, as in CPython.
  - `(x for x in g())`: what a generator expression goes through is not in a temporary but in a variable of its frame, `.0`, and goes when the frame goes. The use of `g()` is
    the call that makes the generator expression, which takes the note up (`Runtime::didMakeGenerator()`). Where the generator expression is closed or dropped while it is
    suspended, the frame is gone at once, for nobody gets to see the `GeneratorExit` whose traceback has it: whoever closes a generator closes what its frame owned
    afterwards (`GeneratorsOwnedByFrame`). So `next(x for x in g())`, `any(p(x) for x in g())` and a `sum()` that raises run the `finally` clauses of `g` before the next statement. An
    exception that comes out of the generator expression keeps the frame, as in CPython, and the collector finds out when that is over. What `f_locals` shows of `.0`
    is owned no longer. If the program has asked for the frame object of the generator expression (something it called, by `sys._getframe(1)`; `gi_frame`, `f_back`, `tb_frame`), whoever has that has the
    variables, and nothing is closed (`PyFrame::wasAskedFor()`). A trace function is handed the frame object unasked, and what tells tools of events makes one for notes of its own: that does not count, or nothing would be closed where
    CPython closes it as soon as a program is traced.
  - The unwinder reads the iterator from its register, and no instruction shows that use. Nor does any show that a trace function may send the frame
    back to the head of a loop from where the code only leaves it (`frame.f_lineno = line`). So whoever asks which registers are live adds
    what the blocks that are open hold (`BlockPiece`): the collector, before it empties dead temporaries, and what decides
    which registers a generator saves at a `yield`. In CPython those values are on the stack of the frame for as long.
  - A program can get at an object that it has no reference to through `gc.get_objects()`, `get_referrers()`, `get_referents()` and
    `DEBUG_SAVEALL`, and at a generator through `frame.f_generator`: those end all such ownership
    (`temporariesMayHaveEscaped()`). What traces the program is shown arguments of calls and can assign to variables: that forgets the
    notes of views, and to be told of a call forgets that of a generator.
  - The notes are weak: nobody visits them, and a collection forgets those of objects that it has found dead. To forget them all
    whenever a collection begins will not do: it is allocation that sets a collection off, and in `bytes(memoryview(b))` it is the
    use that allocates, as raising an exception does in a loop.
  - It is about promptness only. What is not covered (anything that has been in a variable, a parameter, a container or a return
    value; objects made by a class written in Python) waits for the collector as before.
- **An exception that has been caught is gone** as soon as the program lets go of it, with its traceback, the frames in that and their locals.
  - *The VM remembers the last exception thrown* (`VM::lastException()`, a root) when it has long been handled: until the next throw, or the next entry from native code. It is there for a
    host that has caught a value and wants to report it with the stack of the throw. So a function that had raised, once, kept its locals alive for as long as the thread raised nothing else and
    called nothing from native code: a local with a `__del__` was not finalized after four million allocations, in seven of eight ways of catching. A collection that the program asks
    for made the VM forget, but only the exception of the thread that asked: a thread without the GIL has its own with it (`JSLock::park`), and four calls of `gc.collect()` in the main thread
    did not finalize what another thread's last exception held, until that thread ended. Now whoever catches for Python makes the VM forget: `op_py_catch`,
    `clearPendingExceptionIfMatches`, `takePendingException`, and `dropPendingException`, by which native code swallows what code of the program has raised. (`tryDropPendingException` where the end of
    what runs is to go on to the caller.) What still clears an exception by itself throws it again, or another, or has to do with the end of what runs. And a thread that gives up the GIL takes none along (`Threads::release`). Nothing changes where JS catches. Nobody misses it: it is read by the collector, by a function of `$vm`, and by the host where it has a value to report and no exception
    (`reportException(JSGlobalObject*, JSValue)`), and every entry from native code clears it, so it could not be counted on beyond the return of the call that failed. That report was wrong
    before: an event listener whose `handleEvent` cannot be called, after `hasattr()` had caught an `AttributeError`, was reported as that `AttributeError`.
  - *What a thread has on its stack while it is without the GIL has no room that is not written.* The state that a thread takes along (`Runtime::ThreadLocalState`) lies in the frame of whatever
    gave up the GIL, where the frames of the thread's last calls lay, and stays there for as long as the thread waits; the collector takes every word of it for a reference. It had a vector with room for
    eight items of its own, which nothing writes while the vector is empty, and a `bool` with seven bytes of padding. A thread that did nothing but raise and catch had the cell that carries a thrown
    exception (`JSC::Exception`) in that vector's room every time it was made to give up the GIL: the throw had left the address there. (A heap snapshot had the cell without an edge or a root; the
    word stood at the same offset from the stack pointer in every run, beside a return address into `VM::throwException`.) And in the padding: of a word with one byte written the rest still points into the block
    of the old cell, which holds nothing but such cells, and the scan also takes what points just behind a cell for a reference to it. `gc.get_objects()` in another thread found the frame of
    the function that had raised in every call, one for each such thread; now in none of 40. Nothing is wiped for that: there is nothing left to wipe.
    The frame has room for the states of four runtimes. If more are alive in the VM, the rest is in a block of its own, all of it written when it is made, of which the collector is told, and which
    it scans as it scans the stack, at the same time and by the same rules (`Threads::gatherConservativeRoots()`). Handles made when a state is put there would not do: other threads write
    into a state that is set aside (`threading.setprofile_all_threads()`). In a heap snapshot a cell that only such a block keeps looks like one that only a word of a stack keeps: no edge leads to
    it and it is no root. The word is then to be found from `ThreadState::saved`.
  - *What is left* are the words that the compiler leaves unwritten in frames that are not ours, on the way into the wait (`Threads::takeGIL`, `Condition::waitFor`, `ParkingLot`). Other shapes of that loop
    (a raise twelve calls deep, `hasattr()`, several threads) have the address of the carrier, or of the exception itself, in one of those in up to 15 looks of 40, for as long as the thread stays at that very place.
    And the 128 bytes below the stack pointer, which are no frame's: on x86-64, and on arm64 under Darwin, the scan of another thread's stack takes them in because the ABI lets the innermost function use them (`osRedZoneAdjustment`). On a loaded machine a thread that gives
    up the GIL loses the processor inside the system call by which it wakes the next one (`pthread_cond_signal`, on the way into the wait of `Threads::dropGIL`). After `hasattr()` had swallowed an `AttributeError`, an entry of the traceback
    stood 64 bytes below the stack pointer there in 33 looks of 240, until the thread ran again. (What was seen, in two ways. A heap snapshot that the other thread took in place of its look has that entry, or the exception that leads to it, without
    an incoming edge and without a root. And a build that stops the process while the collector has the thread suspended: in the debugger the thread stands behind the `syscall` in `pthread_cond_signal`, whose frame begins at the stack
    pointer; the address of the entry is at `rsp - 64`, in no frame; and the collector's own log, when the process goes on, names the word at that offset as the one that led it to the entry. In the 207 looks in which the
    frame was not found there was no such word.)
  - *So what walks the heap does not count on it.* An exception that nobody has seen is in the heap like any other, and mostly by references that are meant: while another thread is raising it, the VM's, which a thread without the GIL has with it on its stack (`JSLock::ParkedThread`); while that thread is handling it, the slot
    of the thread or, in a generator that is running, of the generator (`PyGenerator::m_handledException`), and the registers of the handler. (Not the thread's notes, `m_notedEntries`: only those of the thread that runs are visited. Those of
    a thread that waits for its turn are in memory that nobody scans.) Or the thread has caught it and is about to drop it, or has such a word on its stack. The runtime
    counts on it that nothing of the program's has it, nor the entries of its traceback, nor the frame objects made for those, the last of which lacks its caller (`PyFrame::backIs`). `gc.get_objects()` and `gc.get_referrers()` pass over
    all of that (`HeapView::findWhatIsOutOfReach`): the exception, its entries, their frame objects; and, whatever is left of the exception, all that leads to a frame object that lacks its caller: the frame objects of the calls it made, the
    entries of those, the exception. (They used to pass over that one frame object only. Three threads that raise twelve calls deep and catch, 40 looks: a frame without its caller was reached 660 times from other frames, 715 times from
    tracebacks, 715 times from exceptions; with assertions on, `f_back` of such a frame ended the process.)
- **The collector marks while the program runs.** Its threads call `visitChildren` on cells that the program is changing at that
  moment. What every class of cell owes them:
  - Whatever `visitChildren` reads that is not a value is set by the constructor and never changes: the length of a tuple, the
    capacity of a table. All the slots are visited, used or not; the unused ones hold no cell. What has to grow is a cell of its own, of
    a fixed size, which its owner replaces (`PyDictStorage`, `PySetStorage`). A list is a `JSArray`, which has JSC's own protocol.
  - Values are at addresses that are multiples of their size. A store that crosses a cache line is not atomic, and the half of an int
    that has arrived looks like the address of a cell. A member is aligned by the compiler. What is behind the cell is reached through
    `storageBehind<T>(this)`, which does not compile unless the size of the class is a multiple of what `T` needs: a class with
    nothing but small members has to say `alignas`.
  - Values are stored one at a time (`WriteBarrier::set`), or moved with `gcSafeMemmove`, never with `memcpy`, `memmove` or
    `std::copy`. Values that have changed places inside a cell may have passed the collector by: `vm.writeBarrier(cell)` afterwards.
  - `setWithoutWriteBarrier` is for a value that is not a cell, for stores that `vm.writeBarrier(cell)` follows, and for a cell that
    has just been made: nothing refers to it and nothing has been allocated since. After the last store of a cell to a new cell comes
    `vm.mutatorFence()`, or a processor that reorders stores may show the new cell before what is in it. (`finishCreation` does that
    for what the constructor has stored, `ObjectInitializationScope` for a new array: `newListOf`.)
  - Anything else (a `Vector` or a table of the `Runtime`) is read under a lock that the program takes to change it
    (`m_finalizationLock`, `m_moduleTypesLock`). It is visited with the global object. So after the program has put a cell in it comes
    `vm.writeBarrier(globalObject)`: a collection of the young generation passes the global object over, and one that is under way
    may have been there already. (What the collector puts there, with the world stopped, it has marked.)

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
up on disk. Such an object is no module to Python: it has no `__name__`, `__file__` or `__spec__`, and `importlib.reload()` refuses it. What it was imported as the
host remembers (`spec_of()` in that module), and `from helper import nothing` asks, to name the module and its file in the `ImportError`.
Only files of program text are found by `import`. What else Bun can load (JSON, TOML, YAML, text) is not: such files lie about in directories that are
on `sys.path`, and a `requests.json` is not to hide `requests`. `bun.require("./data.json")` loads them.

**Compiled code is kept in `.pyc` files**, and it is importlib that keeps it there, as in CPython: `__pycache__/os.bun-315.pyc` beside
the source, checked against the time and size of the source (or its hash), `-B`, `PYTHONPYCACHEPREFIX`, `py_compile`, modules
without source. What the runtime supplies is `marshal` for code objects (`runtime/PyCodeCache.h`):

- The code is written by JSC's own serializer of bytecode (`runtime/CachedTypes.cpp`, `encodePythonCode`), which has learnt the
  constants Python code has and what a code object knows besides its bytecode (`CodeInfo`). It is read in place: loading a
  module makes the code object of the module, and a function in it is decoded when it is first called. A `def` makes the function
  object and nothing else: the `CodeInfo` stays in the file until the function is called or asked about itself, and the strs of
  `__name__`, `__qualname__` and `__doc__` are made when one of them is asked for (`PyFunction::codeInfo()`, `takeNamesFromCode()`).
  Only the thread that runs the program can take a `CodeInfo` out of the file, so it is taken out when a `CodeBlock` is made
  (`didCreateCodeBlock()`): the collector and the unwinder only ask about code that they find in frames.
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
  `python3 -c pass` reads 22 of them, 1.6 MB. Each is read once, straight to where the code stays (`readFileToKeep`, three bytes in,
  so that the payload lies at a multiple of 8): memory that is written for the first time is a page fault for every 4 KB, and page
  faults are most of what a start costs. (In a small environment, with ICU linked statically: 12.5 ms, 4.5 of them in the
  program and 8 in the kernel, 2,360 faults; CPython 10.6 ms, 7.0 and 3.5, 930 faults. That was on a virtual machine, where a fault
  costs 3 µs. Four fifths of the faults are first touches of `malloc`'s memory. When each file was read into one buffer and the
  code copied to another: 13.6 ms, 2,550 faults.)
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

A file is Python if its name ends in `.py` or the import says so: `import rules from "./rules.txt" with { type: "python" }`. Python's own import would not look at such a file, so it is a
module under a name of its own (`rules_<hash of the path>`), one module however often it is imported.

A `.py` file that JS imports is the module that Python's own `import` of it gives (Bun: `import_file` in `src/python/_bun_import.py`): `a/mod.py`
is `mod`, and a file in a package (a directory with `__init__.py`) is `package.mod`. For that its directory, or the one the outermost package
lies in, is appended to `sys.path`: at the end, where it hides nothing, and there it stays. So files that JS imports from several
directories share one `sys.path` and one `sys.modules`, as all of a Python program does, and names go by Python's rule: `import helper` in
`b/mod.py` finds `a/helper.py` if both directories have one and JS imported something from `a/` first. A relative import, in a package,
says which one is meant. Two files that JS itself imports are two modules though they have one name: if the name means something else to Python
already (`a/mod.py` came first, the standard library has a `mod`), the file is a module under a name of its own, as above. Not so a file in
a package: of two packages with one name Python knows only the first, and to import `b/package/mod.py` after `a/package/mod.py` is an `ImportError`.

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
  That loop is the running loop where no other is, for everything on the main thread that runs in the host's event loop:
  `get_running_loop()` and `get_event_loop()` give it (`loopOfHost`), so `gather()`, `create_task()`, `Future()` and `loop.call_later()`
  work in what JS calls and in what was handed to a timer, a promise or a server. Everything runs there but what a program in Python
  does itself, from its first statement to its last or to its first `await` at the top level, and the handlers at exit: there,
  as in CPython, no loop is running (`Runtime::eventLoopPositions`). What such a program calls JS for, and what the host's loop
  calls while it waits, is in the loop again. (`_get_running_loop()` never gives it, so `asyncio.run()` can still be called.)
  The loop holds on to no task: what is to wake a task up does, as in asyncio. A coroutine that JS called and that nothing can wake up
  any more is collected like an async function of JS's in that position. Its `finally` runs, and nothing is said of a task that
  was destroyed: the program made none, it has dropped a promise.
- **`await promise`** in Python, for a JS promise (type `JSPromise`), is a future of the running loop that the promise settles.
  The promise of a coroutine that JS has called is awaited as the task it is, so to cancel who waits cancels the coroutine:
  `wait_for(queue.get(), 1)` from JS, and `ensure_future(queue.get()).cancel()`. An async function of JS cannot be cancelled.
- **A promise is to asyncio what a coroutine object is** (`iscoroutine()`): what the call of an async function gave, to be awaited or made
  a task of. It is all that JS ever holds of a coroutine. So `create_task(queue.get())`, `TaskGroup.create_task()`, `ensure_future()`,
  `gather()`, `shield()`, `as_completed()`, `run_until_complete()` and `run()` make a task of it, on whichever loop, and `wait()`
  refuses it as it refuses a coroutine. The task awaits the promise, and has a handler on it from the moment it is made: an iteration
  of the loop later, at its first step, a rejection would already have been reported as unhandled. To cancel the task cancels the
  waiting. A future a promise is not: that belongs to one loop, and can be cancelled. `send()`, `throw()` and `close()` are those of
  the iterator of `await promise`, for whoever drives coroutines by hand.
- **Futures and tasks** are native: the module `_asyncio` (`runtime/PyAsyncio.cpp`), which Lib/asyncio takes in place of its own classes.
  A step of a task resumes the coroutine from C++, outside any handler, and `await future` gets the result without a `StopIteration`
  (`PyAsyncio.h`). The loop stays asyncio's: callbacks go through its `call_soon()`. `all_tasks()` reads a table that the collector
  does not visit, so that it keeps no task alive, and out of which the collector itself takes what it has found dead, when marking
  is over and before anything is freed (`Runtime::WeakCellTable`, `Runtime::reconcileWeakReferencesAtGCEnd()`).
- **Where a program ends.** A program in Python that awaits nothing at the top level has run to its end when the host gets it back:
  the microtasks it queued run then, as at the end of a script of JS, and the first step of a coroutine that JS started is one.
  When the loop has nothing left to do: the threads that are no daemons are joined and `atexit`'s callbacks run
  (`runExitHandlers()`), then the host's own handlers (`process.on("exit")`), which may be functions of Python's, and last the
  interpreter ends (`finalizeInterpreter()`): finalizers, flushing. All of that is code of the program, and a host that
  terminates it meanwhile (`Worker.terminate()`) ends it there: nothing of the rest runs.
- **Calls owed** (finalizers, signal handlers) are also made between turns of the host's loop, which is woken up for them.
  `SystemExit` and `KeyboardInterrupt` end the program wherever they surface, a callback of JS included.

## Threads

A Python thread is a thread of the system, and one of them at a time runs in the VM: the one that holds the global interpreter
lock (`runtime/PyThreads.h`). To hold it is to hold the VM's `JSLock`, with what the VM and the runtime know about the running code
(top frame, entry frame, exception state, stack limits) being that thread's: a thread that gives up the lock takes that with it
(`JSLock::park`) and puts it back when it has the lock again. The collector scans the stacks of all of them.
To let other threads run moves references from where the collector finds them to where it finds them, so it takes access to the heap. Bun gives that
up before it waits for events while a collection that it has asked for is under way; `Threads::release()` takes it back for as long as it needs it.

- The lock changes hands where a thread is about to wait or to make a long system call (`AllowThreads`: sleep, locks, `join`, queues, `select`
  and its kin, sockets, files, `waitpid`), and where another thread has waited for longer than `sys.getswitchinterval()`: that
  fires `VMTraps::NeedPythonCalls`, and the running thread hands over at its next safe point. It does not have the lock back before one of those
  that were waiting for it has had it. In CPython it is enough that somebody else has had it. Those that wait are asleep, though, and no match for
  a thread that is awake: a thread that collected again and again had the lock back each time before any of them ran, because the main thread,
  which it wakes to finalize what it has found, came later and had it for a moment in between. Nobody else is kept waiting: whoever finds the lock
  free takes it. One turn is all that is promised: it may be short, and the thread that handed over, awake by then, may well be next.
  `Bun.serve` beside threads that collect and threads that compute answers a quarter to a half more requests for it, and those threads get
  done what they did. Why is not understood. The price: with many threads that compute, a thread that has handed over waits a little longer
  before it asks again (beside eight others 1.3 -> 1.5 ms on average, the longest 7 -> 34 ms). The lock is not idle for longer, and the main
  thread is as fast as before. The manager tests of `multiprocessing`, a process each, 32 at a time on 28 CPUs, took 0.6 % less time with it
  (+- 1.1, 17 pairs of runs) when measured on one machine and 2 % more (two pairs of runs) on another. Neither has been explained.
- Until a program starts its first thread none of this happens, and while there is no other thread blocking calls keep the lock.
- **JavaScript stays on the main thread.** Only that thread turns the host's event loop; using a JS object or function from another thread
  raises `RuntimeError`. While the event loop idles, the lock is free.
  Another thread may look, since it has the lock: to read an attribute of an object of JS gives a property that is a value, or says that there is
  none, if that can be seen without running any code of JS's or the host's (`inquireProperty` in `PyInterop.cpp`). So what goes through `sys.modules` and
  asks every module for `__warningregistry__` or `__file__` (`unittest`, `warnings`, `inspect`) works in a thread. A getter, a proxy, an export of a
  module, a property that the engine or the host makes when it is first asked for (`js.WeakRef`) and an object of a class with a way of its own
  to look properties up are a `RuntimeError` as before, and so are calls, writes, items and iteration. `type()` and `repr()` work; `repr()` does without the name of the class.
- **So does everything else that is the host's.** `Heap::mayRunEmbedderFinalizers()` is false while another thread has the VM. Then
  cells with destructors that may be the host's, or with dead weak handles whose finalizers are not known to run anywhere
  (`Heap::addFinalizerForAnyThread()`), are not swept: the allocator passes over their blocks, `gc.collect()` does not sweep. (Whose a destructor is the engine knows only in a block of a space for
  one class, where such a thread allocates none but the engine's. Anywhere else, so in a large allocation and in the first few cells of any class, which are allocations of their own, every
  destructor counts as the host's.) The main
  thread is woken and makes up for it at its next safe point. Until then such a cell stands there dead, while what it refers to is swept. In a block the engine has always had that, sweeping
  being lazy, and whatever comes to a cell by anything but a reference asks whether it is alive. A large allocation, or one of the first few cells of a class, used to be swept by the collection that
  found it dead, so three places did not ask. The scan of the stacks (`ConservativeRoots::genericAddPointer`): a stale word marked such a cell again, and the marker went on into what had been swept.
  `HeapUtil::isPointerGCObjectJSCell()`, by which `ctypes` takes bits for an object and the sampling profiler a word for a callee: `ctypes.cast(address, ctypes.py_object).value` gave such a cell
  out, and whatever then held it had it marked. The search for weak references with callbacks (`Runtime::weakRefsWithCallbacksToUnmarkedObjects`), which looked into every one that stood. They ask now. Requests to end what runs (`node:vm` timeouts, `Worker.terminate()`, the
  watchdog) are put aside for the main thread too, and the VM counts as idle (`VM::isIdle()`) when the main thread is outside it,
  whatever Python frames other threads are parked in.
- While there are other threads, a `Buffer` that C++ holds of memory of JS pins the `ArrayBuffer` (`runtime/PyBuffer.h`): JS, which may run
  while the thread is in a system call, cannot detach or shrink it.
- A thread that waits for others for more than 50 ms, having allocated since it last did so, collects and runs finalizers: the
  others may be waiting for what a dropped object holds open, and in CPython it would have been closed when it was dropped.
- A wait without a time limit while there is no other thread can be ended by a finalizer, so the thread collects at once, or by a
  signal. A program of Python sleeps until one comes, as in CPython: `threading.Event().wait()` until Ctrl-C. Where Python is not the
  program (JS has called into it) and no realm of the VM has a function for any signal, the wait is a `RuntimeError` ("deadlock: ..."): nothing of JS
  runs while the thread waits, so it would only hang.
- At exit the threads that are not daemons are joined, unless the wait is interrupted; daemon threads never get the lock again.
- **A VM that goes away while the process goes on takes its threads with it** (`stopThreads()`, `Threads::stopAll()`): a worker of the host's
  that ends, or is terminated. A thread that merely never ran again would be left with frames that refer to the VM, and may have
  handed the kernel memory that dies with it (`recv_into()`). So each thread is woken wherever it waits (its sleeper; `SIGURG` again and
  again for system calls, which Python's threads cannot block and which gets a handler only where it was ignored), gets the lock as ever,
  and at its next safe point, or where it looks at what interrupted it (`checkSignals()`), meets the exception that ends the code of
  a terminated worker. Nothing catches that: no `finally`, no `__exit__`, no more Python. The frames of C++ unwind as for any
  exception. The main thread waits until the last one is gone, so something long in C++ that looks at nothing (a name being
  resolved) holds up the end of the worker.
- Signals are handled by the main thread, whichever thread the system delivers them to.

## ctypes

`_ctypes` is `runtime/PyCtypes*.cpp`; `Lib/ctypes` is CPython's. What a ctypes type knows (size, alignment, format, how its bytes
are read: CPython's `StgInfo`) is a cell, `PyCTypeInfo`, under a hidden key of the type; a type of one of the metaclasses that has
none is abstract. The layout of a structure is worked out in Python, by `ctypes._layout`, as in CPython.

**Memory.** An instance (`PyCData`) has no memory in itself. It refers to one of three things:
- A `PyByteStorage`: memory of the collector, which does not move. That is what an instance made by calling its type has, and
  what a part of it (a field, an item) shares with it. Nothing is freed by hand and no instance has a destructor: the memory lives while
  any instance refers to it. `resize()` beyond what there is room for gives the instance new memory; parts made before go on using the
  old, which stays valid. There are at least 16 bytes of it, a multiple of 16, and all of it is zeroed: in CPython a small instance has 16 zeroed
  bytes in itself, and programs pass a `c_wchar` or a full `(c_char * 3)` where C looks for the end of a string.
- A memoryview (`from_buffer`). The address is looked up through the view at each use, so it is noticed when the view has been
  released or JS has detached or shrunk the memory. While the instance lives, the view counts as an export: a bytearray cannot be resized.
- An address about which nothing is known: `from_address`, `in_dll`, what a pointer points to. Where a pointer points into the memory
  of something that it keeps alive, which is the rule, the instance made from it holds on to that memory instead.

**What keeps what alive** is as in CPython, for programs look at it: `_objects` of the outermost instance (`_b_base_` leads there)
holds, under keys that say which part they are for, what the pointers in the memory point into: the bytes of a `c_char_p`, the
instance a pointer was made from, a capsule for the `wchar_t` copy of a str. During a call, what the arguments were converted to lives until the
result has been read. What a callback returns a pointer into (a `c_char_p`, a `c_wchar_p`) is never let go, with the warning that CPython gives.

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

**`py_object`.** The eight bytes are a `PyObject*` as the C API has it (see there): what `id()` returns, for every object but a str, and 0 is NULL. The object is kept alive by `_objects`. Bits read from memory are handed out as an object if they are those of
what `_objects` holds for that place; otherwise only after the heap has confirmed that they are those of a live cell of a kind that programs
see (`objectFromBits()`). `Py_INCREF` and `Py_DECREF` count as C's do (see "The C API"). `ctypes.pythonapi` would be the
program itself, which has little of the C API yet. Bun takes the name away when `ctypes` is imported, as it is absent on PyPy: that is how programs find out.

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
nothing refers to the callback; a call that comes after that is refused too. Nothing in the heap says who calls a callback, though: a library has its
address, and finalizers call it by way of the library (they close what it opened, and it says so). A package keeps its callbacks in its modules,
which CPython lets go after the program's own at exit; here all that dies is found at once. So a callback that is found dead is kept while there is
a finalizer left to run, and comes up again in the next collection.

## The C API

`capi/` is CPython's C API for extension modules, in the one form in which C sees the layout of no object: **abi3t** (PEP 803). `PyObject`,
`PyVarObject`, `PyTypeObject` and `PyModuleDef` are incomplete types there, `Py_INCREF`, `Py_TYPE` and `Py_SIZE` are calls, and what C knows of an object is its
address. Nothing in `capi/` is CPython's text: what a function does is taken from the documentation, the list of the stable ABI, and from running CPython. The
functions are exported from the executable. (So far there is what it takes to hold objects, and no way to load a module: a library loaded with `ctypes.PyDLL` can call it.)

**A `PyObject*` is the 64 bits of the `JSValue`**, and NULL the empty value (`toJS()`, `toC()` in `capi/CApi.h`). The address of a cell stands for the cell. None, the
bools, and the ints and floats that are no cells are addresses at which there is no memory, and C cannot tell. So `==` in C is `is`, nothing is allocated or looked
up for a value to cross, and a reference that is borrowed from a container is the value that the container holds, good for as long as it holds it: CPython's rule.
Three kinds of value are spelled otherwise for C than in the heap, where one object has several spellings or C's was fixed beforehand:
- A built-in type that C names by a symbol (`&PyLong_Type`; `PyExc_ValueError`, which is a variable) has an address that was fixed when the executable was linked. These symbols lie in one
  section. A pointer that comes in is tested for that range, and the symbol holds the cell; a type that goes out is asked for its symbol (`PyType::symbolForC()`).
- An int that is no cell may be tagged as an int32 or as a double, as the arithmetic left it (`(2**40 + 5) - 2**40`). It goes out in the one way in which `jsNumber()` tags it.
- `null`, which only JS makes and which is None too, goes out as `undefined`.

**One function says which object a value is**: `identityBits()`, in `runtime/PyType.h`. `toC()` is that function; `id()` is its result as an int; the hash that `object` and `type` give is
made of it, as in CPython (`id()` turned right by four bits); the eight bytes of a `py_object` are it. (Only the `id()` of a str goes by something else, its characters: two cells with the same ones are one str to `is`. C has the cell.) It needs no API and
asks for no interpreter: a type has been told its symbol when its interpreter made it (`CApi::didMakeTypes()`), in every interpreter, and a symbol says which it is (`StaticObject::number`), so
the way back (`objectOfSymbol()`) gives each interpreter its own type. So the `id()` of `int` is `&PyLong_Type`, as it is in CPython, in every interpreter, from the start; and to write a type
into memory takes no API. No cell can have such an `id()`: the section is part of the executable. (An `id()` is unique within an interpreter: the `int` of another realm of the VM is another
cell with the same `id()`. A `ShadowRealm` lets no object through, so no program there has both.) What it costs: for a cell, one comparison of its `JSType`, which sends
everything but types away.

**`PyExc_ValueError` never changes.** A library may read it as it is loaded, whoever loads it and whenever (JS by `bun:ffi` before there is any Python, a Worker before main has a use
for the API), and a constructor runs once in a process. So the variable is a constant that the linker fills: the address of a symbol like those of the types, which has no name of its own. What
stands behind a symbol is there while the API is set up. Before and after, the symbol holds nothing, and nobody gets as far as looking: every function that would refuses the caller first.
That goes for what such a constructor *calls*: it runs once, so what it was refused (an object that it meant to make and keep) it never gets.

So a value that is its own 64 bits is one object however often it is made, where CPython makes a new object each time. `PyLong_FromLong(x) == PyLong_FromLong(x)` holds for every
int that is no cell (below 2**53 in size; CPython: from -5 to 256), and `PyFloat_FromDouble(x) == PyFloat_FromDouble(x)` for every float that is no cell (1.5, 1e308; a float with a
whole value, 0.0 and nan are cells, and differ as there). These are the rules of `is` in a program here, and they hold however the number is tagged. For C it means that an address used as a key finds an equal number, not
"the same allocation": harmless, since the objects are immutable, and nothing may be concluded from two addresses being equal but that the values are. There is no cell to count on:
`Py_INCREF` and `Py_DECREF` of such a value do nothing, and its `Py_REFCNT()` never changes.

**References are counted in a table**, cell -> how many C owns (`capi/CApiCounts.h`). A function that returns a new reference adds one, `Py_DECREF` and a function that
steals one take one away, and with the last the entry goes (as long as anything is let go of: see below). Nothing else happens then: an object dies when a collection finds that nothing refers to it, as everywhere here.
What is not a cell is not counted, nor are the types with symbols. `Py_REFCNT()` is the count and two more, for whoever lent C the reference it asks with and for whatever else
refers to the object: nobody knows how many do. It rises and falls by one as C takes and gives back, and it is never 1, which would tell C that nobody else can see the object,
not even for an object that C has just made: code that changes in place what it alone owns takes its other way.

- **When a collection has marked all it will, every key of the table is marked.** No key is ever the address of a freed cell, whatever an extension does or
  fails to do. That is asserted when marking ends.
- What C holds is reachable as what a global variable holds is, and the collector learns of it as of an embedder's other roots: by a marking constraint
  (`Heap::addMarkingConstraint()`, which Bun uses for its own). JSC evaluates constraints with the world stopped.
- The program changes the table with the GIL, and the collector reads it only in the constraint. The thread that runs the program is never stopped from outside: it stops
  where it allocates in the heap or gives up its access to it, and no operation on the table does either. So there is no lock, and no barrier. A thread
  without the GIL counts as stopped, so `Py_INCREF` and `Py_DECREF` look who calls: in CPython such a call spoils a count, here it would spoil the table. It is refused: the
  count stays as it was, with a line on stderr the first time. What was to be given back is then held for ever. What was to be taken is a reference that C believes in and
  has not got, and nobody can tell which `Py_DECREF` will give it back: so from then on nothing is let go of, and all that C owns or comes to own is held for ever. A leak, and
  nothing that C rightly owns dies under it. The line says so. The counts go on falling, for they are asked about (`Py_REFCNT()`, `PyTuple_SetItem()`): it is the entry that stays when the count has
  come to 0, and a key of the table is what the collector marks.
- A thread that runs C for another interpreter (`CRunsFor`: its `ctypes` call, a library that it loads or lets go of) is refused as well, with a line of its own, and that is all: it costs
  the interpreter that has the API nothing (but for the one case at the end of this item). What `ctypes` hands to C there are that interpreter's own objects, and that C takes a reference to one of them says nothing about the
  objects of this one. No function hands such a thread an object of this interpreter. What it can name is what a library that both have loaded keeps in a variable of its own: to share an
  object between two interpreters in that way is an error in CPython too, and here it is not defended against. And it can call the code of a callback of this interpreter's, if it has got the
  address as a number: that gives a reference with its result, as to any C, and this C cannot give it back, so the object is held for ever.
- A thread of another VM that gets into C in another way (a Worker's JS by `bun:ffi` or N-API, a destructor of a key of pthreads as the Worker's thread ends) is a thread without
  the GIL like any that C has started, and is treated so. It has no objects of its own, so a cell to which it takes a reference can only be one of this interpreter's, got by a variable:
  that is the case which "nothing is let go of" is for. The types, the exceptions, None and the numbers are not counted, whoever asks.

**The error indicator is not the VM's pending exception.** C goes on calling the API while an exception is set, and the functions of the runtime expect none to be pending
when they are entered. So the indicator is a reference of its own, one for each thread, counted like C's other references and kept alive by that (for ever, if a thread ends
with an exception set). A function
of the API that finds the runtime to have raised moves the exception there (`Entry::failed()`); whoever has called C throws what is there (`throwWhatCRaised()`), or drops it if the
call ends otherwise: it does not outlive the call that set it. It is the thread's, not the interpreter's, and two interpreters may run on one thread; so nothing gets into it, or out of it,
but by a caller who may come in (`PyErr_SetRaisedException()`, `PyErr_GetRaisedException()` and `PyErr_Clear()` ask too), and only the interpreter that has the API looks at it after C
has run. What ends the
program (a termination) is never put in the indicator: it stays pending, and while it does every function of the API fails at once.

**One interpreter of a process has the API: the first that there is** (`CApi::runtimeIsBeginning()`, in `Runtime`'s constructor). The functions are not told for whom they are called,
and a symbol such as `PyLong_Type` stands for one object. Nothing that a program does changes whose the API is: for `bun main.py` it is main's, whatever its Workers do. In any
other interpreter what needs the API is an `ImportError`; and when that one has ended nobody has the API any more, for what was loaded stays loaded, with pointers into its heap.
(CPython's extensions have to say that they can serve more than one interpreter, and few do.) It ends when its `Runtime` does (`runtimeIsEnding()`): for the interpreter of a realm to which
nothing refers any more that is in the collection that finds the global object dead, with the world stopped, not when that cell is swept. C that JS calls on that thread could come in
meanwhile. Nobody is in the middle of a call then (a function that has entered has the global object on its stack), and while C owns an object the realm does not die (the object holds it by
its Structure; nor does it ever die if C has owned one since a reference was refused, see below). What C may still own are cells of no realm, a str or a large int, and what a thread's indicator still points to: nothing follows those pointers any more, for every function refuses.

It is **set up** when that interpreter first has a use for it (`ensure()`, `tryEnsure()`): before it loads a library with `dlopen()`, because what a library does as it is loaded may
call; before a function of C's in a `PyDLL` is called (after its arguments have been converted: a call that fails before it gets to C fails as
it does anywhere; not for the prototypes that `ctypes` makes of its own functions or of a Python callable, which have the same flag); and when C is handed a reference. `import ctypes`
has a `dlopen()` in it.

**Who may come in.** Every function that looks at an object, at the table or at the interpreter asks first whether the calling thread holds the GIL of the interpreter that has
the API, and does not run C for another (`interpreterToEnter()`). If not (a thread that C made and that has not attached itself, a thread between `Py_BEGIN_ALLOW_THREADS` and
`Py_END_ALLOW_THREADS`, a thread of another
interpreter, any thread before the API is set up or after its interpreter has ended) the function fails: it returns what it returns when it fails and sets no exception, with a
line on stderr the first time if there is an interpreter. In CPython all of that is undefined. The question is `JSLock::isHeldBy()` with the thread's own `Thread`, which each thread
keeps at hand, with a reference, from its first call to its end. What the question reads stays when the interpreter goes: the object that `s_interpreter` points to is never freed,
and it holds a reference to the lock. `Py_None` and the other constants that are no cells, `Py_Is*()` and `Py_IsInitialized()` ask nothing and look at nothing that could go away:
the symbols of None and the bools say what they stand for from the time the executable is loaded. `Py_IsInitialized()` says whether the interpreter that the API serves is there.

The lock is the VM's, and a VM may have several interpreters, on one thread: each realm (`ShadowRealm`) that imports Python has its own. So **whoever runs C says for whom** (`CRunsFor`, around
a foreign call, `dlopen()` and `dlclose()`): that is the thread's, and the innermost counts, so C that one interpreter calls, that calls back, and so gets to C that the other calls, is
told apart at every level. It costs who is let in nothing: while a thread runs C for an interpreter that the API is not for, it does not have its `Thread` at hand, so the
question above fails and the long way finds out why. C that JS runs (`bun:ffi`, N-API) runs for nobody in particular: on a thread that holds the GIL of the interpreter that has the
API it is served, from whichever realm it was called, unless that is inside C which runs for another.
The question is for C, and only C is asked. What the runtime does for itself (it throws what C raised, gives up the reference that came with a result, hands C one with the result of a callback;
`_ctypes.Py_INCREF()`) goes by whose call it is: code of the interpreter that is served may run deep inside C that another one runs, and its calls come with references and exceptions like any. A
function that has asked does not ask again for the references that it takes and gives (`retain()` and `release()` with `current`).

**Quiet and noisy.** Every function that C calls says in its first line whether it can run code of the program or put a reference where the program finds
it (`CAPI_ENTER()`, `CAPI_NOISY()`) or not (`CAPI_QUIET()`); all three ask who calls, once, and have the answer at hand (`current`). `CAPI_QUIET_AND_ASKS_NOBODY()` is for the quiet ones that have nothing to ask for.
`tools/check_capi.py` looks for the first lines, and that a function which asks nobody does not name the interpreter. A function is noisy unless it is known to be quiet. Nothing
counts the noisy calls yet: that is for the order in which instances of extension types are destroyed.

**`ctypes`.** A `py_object` is a `PyObject*`: the same bits. A function of a `PyDLL` is called with the GIL; what C raised is thrown when it returns, whatever it returned (also for a
function of a `CDLL` that got in because no other thread wanted the GIL: CPython crashes there). What a library raised as it was loaded, or as it went, is the cause of a `SystemError` from
`dlopen()` or `dlclose()`, as in CPython. Code that
returns a `py_object` returns a reference with it: a function of C's does, and the reference is given up once the object is in hand, or when the call ends in another way; a callback
does, and C owns what it gets (a prototype over a Python callable that is called from Python does both). Whether a reference goes with it depends on whose the code is, the
caller's interpreter for C's and the callback's own for a callback, and on nothing that changes with time. For a callback it is also the callback's own prototype that says whether the result is
an object: one that returns a number, called by its address with a prototype that says `py_object`, gives no reference with the number. (In CPython that caller gives up a reference that nobody gave.)
`ctypes`' own functions (`string_at()`, `cast()`) are no code in that sense: nothing is counted. `_ctypes.Py_INCREF()` and
`Py_DECREF()` count in the same table. In an interpreter that the API is not for, C can own nothing: `Py_INCREF()` keeps a table of its own, and what a callback returns lives while the
program holds it.

**`PyTuple_SetItem()`** writes only into a tuple that C is filling. CPython asks for a reference count of 1, which here nobody knows; what is known is asked: `PyTuple_New()` has made
the tuple, and there is no more than one reference to it among those that are known of: the ones that C owns, and the ones that C has handed on or seen taken (`didGetHolder()`: a container
takes one over, or takes one of its own; the program gets one with a result, or with what C raised, and nobody counts what it does with it, so that is the end). The tuple keeps that: none, one, or
more. So the one reference may be a container's: "put it in,
then fill it" (`PyList_SetItem(list, i, pair)`, then `PyTuple_SetItem(pair, ...)` by the pointer that C still has) is how extensions keep their error paths short. Into any other tuple it is a
`SystemError`. Nobody sees what the program does. What passes here and not there is a tuple that somebody has got hold of without a reference changing hands under the eyes of the API,
and kept: out of the container, or as the argument of a callback. What passes there and not here: a tuple whose other holder has let go of it again, for holders are only ever counted up (the container
has died, the program has dropped the result; also where it is the API that puts something else in the tuple's place, `PyList_SetItem()` or `PyTuple_SetItem()`); and a tuple that the program has made
and of which C has the only reference (a callback has made it and returned it). The hash that the tuple has kept is
forgotten (CPython 3.15 goes on answering with it).

## decimal

`_decimal` is `runtime/PyDecimal*.cpp`, and `Lib/decimal.py` takes it in place of `Lib/_pydecimal.py`, as in CPython. It is written from the
General Decimal Arithmetic Specification and from what CPython's module and the library under it (libmpdec) do: they are the reference for
every result, flag, limit and message, and nothing of theirs is in it.

**The arithmetic** (`PyDecimalNumber.h`, `PyDecimalLimbs.h`, `PyDecimalNumber.cpp`, `PyDecimalFunctions.cpp`, `PyDecimalText.cpp`,
`PyDecimalTransform.cpp`) knows nothing of the engine or of Python: numbers and a context in, a number and conditions out. With
`-DPY_DECIMAL_STANDALONE` it compiles by itself, which is how it was compared with the C module, operation by operation, before there were objects
(`tools/decimal_arithmetic.cpp`, `tools/decimal_arithmetic_fuzz.py`). A `Number` is a sign, an exponent and a coefficient in limbs of 10^19, eight of
them in the object itself, which lives on the stack. What the operations give is determined: `exp`, `ln` and `log10` are correctly rounded (the C
module always asks its library for that), so how they are computed does not show; a power is the same sequence of rounded steps as there, because the
steps do show. Products are schoolbook, Karatsuba from 28 limbs, and from about 190 limbs number theoretic transforms modulo three primes; quotients
are Knuth's algorithm D, and Burnikel and Ziegler's recursion on top of the products; a square root is Newton's iteration on integers, begun from the
root of the leading half. No operation can be interrupted, as in CPython.

**The objects** (`PyDecimal.h`). A `Decimal` is one cell with the limbs behind it, 64 bytes for up to 38 digits, and never changes. An operand is
read where it is (`PyDecimal::lend()`), an int is converted on the stack, the result is computed on the stack and copied into a cell of
its size: one allocation for an operation. A `Context` is a cell with the limits. Its traps and flags are in a small cell of their own, which the
mappings `context.traps` and `context.flags` read and write as well: the context refers to them, they do not refer to it, and so a context is
not part of a cycle.

**The current context** is the value of a `ContextVar`, as in CPython, so threads, tasks and `contextvars.Context.run()` each have theirs. Finding it
costs a load and a comparison: `Runtime::contextVariablesVersion()` changes whenever a context variable may have another value than before (one
was set, a context was entered or left, another thread took over), and the module remembers which context it found at which version.

**Memory.** An operation that needs more than the machine has gives `MemoryError`, not a dead process: what is asked for in one piece is compared with
the size of the memory first. Division and square root then try again with the precision that the operands call for, as in libmpdec. A logarithm or a
power whose result is sure to be longer than that says so at once: they are computed in steps, each with about twice the digits of the one before, and
it would be hours until a step found out (in CPython it is). The series of `exp` ends at the first term for which there is no memory, not after
billions more. A text is no longer than a str can be, and while a number is formatted no number is longer than that text (`LimbLimit`).

`test/js/bun/python/differential/mod_decimal/engine.py` makes random operations of eight kinds, one line of output each (the result and the flags, or
the exception and its message); `tools/compare.py` there runs it under two interpreters, as many cases as one likes. What still differs is in DEFERRED.md.

## Tracing

`sys.monitoring` (PEP 669) is the mechanism; `sys.settrace` and `sys.setprofile` are two of its tools (numbers 7 and 6, with callbacks in
C++ that do what CPython's `legacy_tracing.c` does), and `pdb`, `bdb`, `trace`, `profile`, `cProfile` (`_lsprof`, a tool like any other) and
coverage tools are CPython's own code on top. It is all in `runtime/PyMonitoring.{h,cpp}`.

**Code that nobody asks about pays nothing.** No instruction is there for the sake of tracing and none tests a flag.

- _Events at places in the code_ (a function starts, returns, yields, is resumed; a line begins; a call; a jump or a branch; an instruction). When a
  tool asks for them the instructions are changed where they stand: the opcode at each such place is replaced by
  `op_py_instrumented`, operands untouched (of an instruction of page 2 it is the prefix that is replaced). That calls the runtime, which tells the tools and hands back the opcode that was
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
  A tool that answers `DISABLE` to a `CALL` is still told how that call ends, as in CPython, which knows only the tools of the code object, and not those of each instruction, until
  an event at a place in the code has two tools while the code is changed (`CodeMonitor::hadEventWithSeveralTools()`). From then on it is not told.
- Some instructions have to be carried out by the runtime to be told of: a branch (which way it went is known afterwards), a call of
  something that is not a Python function (`C_RETURN`, `C_RAISE`), the end of a `yield from` (`STOP_ITERATION`). `Name(...)` is an ordinary call while calls are told of.
- A branch told of like that is carried out by the runtime for as long as one of its ways is still wanted, and the way out of a loop is wanted until the loop ends. Under a tool that
  disables every place once it has seen it, as coverage.py does, each turn of each loop would go through the runtime. So a way is told of _where it leads_, if nothing else
  leads there: `BRANCH_LEFT` at the instruction behind the branch, `BRANCH_RIGHT` at its target (`Point::leftIsToldOfAt`). The branch is left as the compiler emitted it, and a way that
  is disabled, or never taken, costs nothing. (CPython has an instruction behind every branch for the first of the two, `NOT_TAKEN`.) As far as a tool can tell, the frame is still at the branch.
  A `for` loop that goes on and an `except E:` that matches are told of at the branch: they have done more than branch by the time the next instruction runs.
  Jumps may lead to the target as well, as a `break` does to where its loop ends: they are replaced for as long as the way of the branch is wanted, and say as they go that it is they who
  come (`Point::leadsWhereBranchJumps`). A `break` is taken once where the branch at the head of the loop is taken at every turn.
  Where more than that leads to the target (`if` without `else` and with something behind it, `a and b`), `BRANCH_RIGHT` is told of at the branch, which stays replaced until it
  has jumped once. It may never: while that is all that is wanted there, the runtime only takes the branch (`Monitoring::takeBranch()`).
- _Events to do with exceptions_ (`RAISE`, `RERAISE`, `EXCEPTION_HANDLED`, `PY_UNWIND`) come from the unwinder, which looks at one flag for each
  Python frame an exception passes. The places they name are of the lines they are of in CPython. `EXCEPTION_HANDLED` is at the `op_py_catch` of the
  handler, which is of no line, as what a handler begins with is there: they stand behind all the rest of the code, and the generator marks them. The
  handler of a `with` or an `async for` is of the line of the statement from its first instruction, so for those (`handlerOfLineMark`) the place is
  where the handler goes on. What `ReraiseOutOfHandler` saves running, the handler around a handler, is of no line either, and so is where the frame
  is left from then. So is the handler that all of a generator is in, in CPython: the body of a generator has a place for it, an instruction that is
  never run, right before the code of the program. What raises an exception again on its way out of a `with` has two marks: the line of the `with` for
  tools, no line for the frame, which stays on the line the exception came from.
- `throw()` and `close()` of a generator that delegates go to the innermost one first in CPython, and the outer ones are resumed only if something
  comes back. Here every one on the way is resumed, from the outside in, to hand on what was thrown (`op_py_send`), so it says nothing while
  it does (`PyGenerator::throwIsWithDelegate`): no `PY_THROW` as it is resumed, no `PY_YIELD` if the inner one yields again. What CPython
  tells of when the exception comes back (`PY_THROW`, `RAISE`, the handler that drops the delegate, the line once more, `RERAISE`), or when the inner one returns, is told of then.
- What a tool, a trace function or an audit hook (without `__cantrace__`) does is not told of (`Runtime::tracingDepth`).

**Lines.** The compiler leaves marks (`PythonMark`, in the rare data of the `UnlinkedCodeBlock`; what they say is in `PyBytecode.h`): here begins line _n_; what follows is
of no line; here the program's own code starts; here a block begins or ends that holds something in a temporary register; the jump that comes next is not the program's. They follow CPython's
compiler location by location, artificial code included: operations on constants that it works out are of one line (but for `not`, whose operand is of its own); whatever makes a display with `*` or `**` in it, or a long one, adds to it or extends it is of the line the display begins on; statements that cannot be reached are of none.
What cannot be reached is told from the code that was emitted (`Unit::cannotGetHere()`: it ends in something that never goes on, and no jump leads here). It is compiled all the same, for the errors in it, ends in `op_unreachable`, and its constants, code objects among them, are not in `co_consts`; a `finally` clause has no copy for a way that does not exist. So it is with parts of statements: what a constant in a condition rules out (`if a and 0:`, `b` in `a or 1 or b`, the message of `assert a or 1, m`; `Unit::deadCode()`), and where a `break` was going through a `finally` clause that returns. The constants of a condition are gone through one by one, not worked out together (`0 or 1`): each may be on a line of its own, which the frame is on when it gets there. Where `and` and `or` are taken for their value, only what is written as a constant settles anything (`1`, `None`, `__debug__`; not `not 1`, not `()`), and what comes after the operation it is an operand of can still be reached, as in CPython, which folds a constant and the test behind it only within a block (`Unit::boolOperand()`). A test that settles two operations at once (`(a or b) or c`, `a` true) is of the line of the outer one. No jump is emitted where the code cannot get, and no label that nothing jumps to, or the target would look as if it could be reached. A line of which no instruction came (`pass`, `try:`, `x = (` over `1)`) has a mark on
whatever comes next, which costs nothing; only where that can be reached from elsewhere is there an `op_nop`, as in CPython.
`f_lineno`, tracebacks, `co_lines()` and `LINE` events all go by the marks.
A function or a module that falls off its end returns on the line of whatever ran last. CPython gets there by copying the implicit `return` for each block that leads to it; here a compound statement
that is the last of the function is told so (`Last`), bodies and handlers of `try` statements included, and each of its ways out returns for itself (`Unit::leaveStatement()`) or marks the line it
leaves on (`Unit::lineOfImplicitReturn()`). A test written over several lines returns from the line it fails on, and so does a pattern of the last `case`. A `return` that runs a `finally` clause returns on the line of the clause that ran last.
The body of a class ends in more than a `return` (`__static_attributes__`, `__classcell__`), which cannot be compiled before all of the body has been: there is one copy of that behind the body for each line the body is left on (`Unit::endOfClassBodyForLine()`).
So does the end of a `finally` clause that an exception led to: each way to it raises the exception again for itself (`Last::exception`), and that is the line the frame is left on.
The same goes for the jump at the end of the body of a loop, which CPython's compiler has every jump to it replaced by: the last statement of the body is told so, and each of its ways out jumps back for itself, on its own line.
A test that fails there would be a branch backwards, which CPython does not have: the branch is turned round and jumps over a jump (`Unit::jumpIfOrLeave()`), so left and right are as in CPython.
That is for a statement which the jump back comes right after, an `assert` included. Where another jump comes first, over an `else`, over the handlers or over the other cases, CPython's compiler has a jump to that jump go where it goes, but not a branch: the branch
stays as it is written and leads to a jump back of its own, on the line and of the place of the branch (`Last::isBehindJump`). Either way the code that runs is as long.
CPython has two kinds of jumps to the end of a statement, and the compiler here says of which kind each is (`JumpIs`). Over the other cases of a `match`, out of a `with`, `break`: tools are told of it whichever way it goes, a branch to it goes where it goes, and each way that leads to it has a copy of it, on its own line. Over an `else`, over the handlers, out of a handler or a `finally` clause: tools are told of it only if it goes forwards, a branch to it stays a branch to it, and where several ways meet in front of it, it is of no line.
Whether there is a jump at all depends on how CPython lays the code out, which is not how it is laid out here: what only an exception leads to comes behind everything else, in the order it was written. That is worked out where the places of the events are (`CodeMonitor::findPoints()`, which knows what can be reached without an exception; `jumpAsLaidOutMark`, `jumpForwardsAsLaidOutMark`). Out of a handler to what is no handler is backwards; a jump with nothing in between that is laid out with it leads to what comes next, and is dropped: the jump over the handlers. In a handler, where all is laid out as it was written, the same jumps are jumps: a `try` statement in a handler has both.
Where such a jump is dropped, or where the statements of a `try` or a `with` end, CPython is left with something that does nothing. It stays, for its line, if it is where a branch or a jump goes and that is the only way there: then it is of the line of that (`Unit::lineOfOnlyWayHere()`). So the way out of `while a:` at the end of a `try` is said to lead to the line of the `while`. Here that is the jump itself, which is not told of (`jumpHasLineToItselfMark`), or an `op_nop` (`Unit::endOfProtectedStatements()`), only then. Behind the statements of a handler comes what puts back the exception that was being handled before, which is of no line in CPython either and takes the line in the same way. A jump there from a jump counts as no way if nothing but a jump is there: it has a copy. The same is left of a jump that a copy of the end of the function has replaced, in front of the copy.
`except*` is laid out so that the same holds for it. Where a clause does not match, the branch leads to an `op_nop` on the line of the clause, which is where CPython drops the `None` that stands for no match; the end of a clause, and the way of an exception that a clause raises, is a jump to the next clause, or behind the last to what is of no line; what is being handled is put back before those jumps, not behind them. At the end the branch is to what raises the rest again, and what goes on leaves the statement as any other does. The jump over the clauses to an `else` leads to the `else`: whether it is replaced by a copy is counted from there. The statements of the `try` end as those of a `try` with `except` do (`Unit::bodyOfTry()`), and so do those of a clause, in front of the jump to the next clause. The `else` does not come where `tryExcept()` has it, in front of the handlers, but behind the clauses, as in CPython's code: where the clauses stay in their place, in a handler, there is a jump over them in front of the `else` and none behind it.

What a clause catches comes first in CPython's code, of `except` and of `except*`. So where it begins on a line of its own (`except (` / `E):`; a tuple begins with its parenthesis) that line is told of first and the line of the `except` after it, with the test. Only the first clause of `except*` has something on its line before that: the list of what the clauses raise.
So all that goes back in a loop is a jump, and that jump, `op_py_jmp_back`, is where the loop looks for signals and for the calls the runtime owes, as `JUMP_BACKWARD` does: a handler, and the traceback of Ctrl-C, find the frame on the line it goes back from.
Where the ways out of a compound statement meet, what comes next, if it is not a statement, is of no line (`Unit::endOfStatement()`). What no way leads to is not emitted (`Unit::cannotGetHere()`).
A line is news if the frame was on another line before. For nearly every mark that is plain from the code (`CodeMonitor::findPoints`), which leaves out what cannot be reached.
As in CPython, no line begins where the instruction before, as the code is laid out, is of the same line and no jump leads there; the end of a `for` loop is neither a jump nor an instruction, what cannot be reached is not laid out, and handlers are laid out behind all the rest, so that none stands in front of what is no handler. That is why the line of `with open(p) as f:` is not told of again when a loop in it runs out that has a `return`: what the `return` does to leave the statement stands in front.
The first line of an exception handler is news unless the exception was raised on that line (`with a: b()`): the runtime remembers the line of the last `raise`, while the code is told of lines.
The handler of a comprehension compiled in place begins, in CPython, with instructions of no line, so its line is news in any case (`afterNoLineMark`).
The jump over the other value of a conditional expression is of no line in CPython if more than one way leads to it, which is so after `a or b`: a branch does. What comes after the
expression is then a line that begins, be it the line the frame has been on all the time: `x = (a or b) if c else d` is told of twice, and so it is here (`Unit::ifExp()`). The mark of that line is
at the end of the expression, in front of whatever moves the value to where it is used (`m_whereLineBegins`).
It is not so where what comes after the expression is at most four instructions up to the end of the function, as CPython's code generator emits them: `return`, `x =` and `return x`. Those its compiler
copies to where the jump was. Here the jump stays, and tools are not told of it; to know which, the compiler counts as CPython's does (`Unit::isCopiedWhatComesAfter()`).
The same goes for every jump to the end of a statement that a short end of the function comes after: `if a: x = 1` / `else: x = 2` / `return x` has no jump (`Unit::isCopiedWhatComesAfterStatement()`). What is copied is one run of instructions, which ends where another way leads into it: at the end of an `if`, say. Nothing else leads to the end of a loop without a `break`, so what is in its `else` is one run with what comes behind the loop (`StatementsAfter::goOnBehindEnclosing`). Where a jump leads to a jump, and that to the end of the function, CPython's compiler goes over the code again and again, and each time whatever ends in a jump takes in what the jump leads to: another jump of no line with whatever is dropped before it, or a short end of the function. What has taken in the end of the function may not be short any more, so whether the first jump ends up as a copy depends on how many there are and on what comes with each. `Unit::isReplacedByCopy()` plays that through; `Last` has the numbers. A jump that does not end up as a copy goes as far as it gets there: to the jump that has taken in the end of the function and has become too long.
Where the copy has taken the place of a jump, there is no jump for a branch to go through: the branch leads to what is left of the jump, which does nothing, and which is of the line of the branch if that is the only way there (`while a:`, `assert a`, a pattern) and gone if the statements of an `if` come there too (`Unit::jumpIfOrLeave()`, `Unit::endOfStatement()`). Behind a `for` loop it is on the line of the loop, as what drops the iterator in front of it is, and goes for that; not at the end of an `else` or of the last `case _:`, where CPython's code generator had the other ways through the statement jump, so that it is the first of a block (`Unit::endOfForLoopWhereJumpsLed()`).
A test that ends in a constant (`assert a or True`, `while True:`) is a branch until CPython's compiler works it out, which is after all that and after the lines have been worked out for the first time: until then it counts as a way to where it leads (`Unit::jumpOfConstantLeadsHere()`).
Code written on the line of its `def`, `lambda` or `class` is on that line from the start. A trace function is told of the line all the same, and tools are if one of them is told of `PY_START` there: that is what CPython does.
Where the frame may come from the same line or from another, the jumps and branches that lead there say which: those from other lines tell of the line
(the head of a `for` loop), or those from the same line say that it is not news (`return (a and` over `b)`). In what is left, one code object in 200 of the
standard library, the frame object remembers the line it was last on, and all places where lines begin stay changed while a tool wants any of them.

**Patterns.** A pattern may be written over several lines. CPython's code keeps what is still to be looked at, and what has been captured, on its stack, and whatever puts a value there or takes it
off is of the line its pattern begins on: so the line of `case {` comes again after the last of the values, that of `(` after the alternatives, that of a name where it is put underneath
what is there already, and the first line of the pattern where it fails with something on the stack. Here nothing is on a stack, so the compiler counts what would be
(`PatternContext::stillToLookAt`, the captures) and marks the lines where CPython has such an instruction. On one line that tells too: a branch or a jump in front of what drops something is said to lead
there, to the line of the pattern, and not to the line that comes next. So where CPython drops something there is a mark for the line although the code is on it already (`Unit::dropWhatPatternLookedAt()`):
the line has the first byte of whatever instruction comes next, as any line of which no instruction comes. A pattern has two places to fail, with something to drop (`failWithMoreToDrop`) and without; if both are used, the first gets an instruction that
does nothing, and the body of the last case jumps over it, as it jumps over what drops the values in CPython. A jump that comes next is not told of as one with a jump that leads there
(`jumpHasLineToItselfMark`). Where the last case fails at the end of the body of a loop, with nothing to drop, the branch would go backwards: it is turned round, to jump over a jump
(`Unit::failUnless()`). In CPython's code a jump to the end of the statement comes behind the body of every case but a `case _:` that is the last of several: also behind the last, where it has nothing to jump over, and behind a `case _:` that is the only one. So whatever a body ends with is in front of a jump (`Unit::lastBeforeJump()`), and the pattern of a last case that fails in front of a jump goes through it as a test does. The keys of a mapping pattern, if they are constants, are one constant tuple, on the first line.

**Jumps and branches.** The layout is not CPython's, so some of the jumps here are none there, and are not told of (`Unit::hiddenJump()`):
between the two ways a method call is compiled, in place of a copy, or where CPython has dropped one (see above). A jump to a jump is told of as one, unless the second is a statement, `break` or `continue`, or has a line to itself for another reason: then both are. A branch to a jump that tools are not told of and that has no line to itself
is said to go where the jump goes, in events and in `co_branches()`. A `for` loop that goes on, and an
`except E:` that matches, go on "in their own last byte", which is of their line, where CPython has an instruction to store the item or drop the exception.
Every test of the program's is a branch of Python's (`op_py_jtrue` and the like), also where the value is known to be a bool (`x in y`, patterns). `x is None` is JS's `op_jundefined_or_null`, which counts as one of them.
The other branches of JS are those the compiler adds for itself, and so is one of these if it is marked (`hiddenJumpMark`). An `async for` loop has no instruction that branches: two `op_nop`, marked, stand for the two ways. Code that cannot be reached has no branches, nor have its handlers, which the compiler marks (`deadHandlerMark`): what a handler covers does not tell, for an empty range is in no table.
CPython's compiler has a branch to a jump go where the jump goes if the jump is of the kind that ends a `case` or the body of a loop, and not if it is of the kind that goes over an `else` or over handlers, whatever that one comes to lead to (`Last::firstJumpIs`, `Unit::canBranchTo()`).

**Generator expressions.** `all()`, `any()`, `tuple()`, `list()` and `set()` of a generator expression are compiled as in CPython: if the name is bound to the builtin, the code goes through the generator itself
(`Unit::callOnGeneratorExpression()`). No call is told of, the loop is, and `all()` and `any()` close the generator they leave early, there and then.

**Instructions.** `INSTRUCTION` events, and the `opcode` events of a frame with `f_trace_opcodes`, are told of JSC's instructions, the hidden jumps left out. There are fewer of them than in
CPython, where a call is followed by something that drops or stores its result. Here the call does that itself, and nothing of the statement is left. `breakpoint()` counts on there being something: it asks for
opcodes in the frame of its caller and stops at the first, which is on the line it was written on. So a call after which another statement begins, or the end of a `with` or of a handler (`Point::isLastOfStatement`),
owes an event: whoever is told of instructions when the call is over, or asks for them while it is under way, is told of one more at the call, on its line, before anything else (`PyFrame::callWithResultToTake`).

**Where a frame is said to be.** Some events are told of from an instruction that is not where CPython has them: the line at the head of a loop from the jump that goes there, the start of a call from its first
instruction. While the tool runs, `f_lineno` and `f_lasti` are what they would be in CPython (`Monitoring::Shown`). The frame object holds them too: a debugger stops the thread in the callback, and
it is another thread of the debugger's that asks the frame (`pydevd`).

**Offsets.** What a program is given for a place in the code (`f_lasti`, `tb_lasti`, `co_lines()`, `co_positions()`, `co_branches()`, the arguments of callbacks) is twice the offset
in JSC's instructions (`shownOffset()`): in CPython code is made of units of two bytes, and `traceback.py` and `dis` count on it. `co_code` is an outline in those
units: a `NOP` for each byte, `RESUME` before the program's own code, which is how `pdb` finds the first line of a function, and CPython's instructions for what decides where the code goes on,
with the distances they have here: `JUMP_FORWARD`, `JUMP_BACKWARD`, `JUMP_BACKWARD_NO_INTERRUPT` (one that is not the program's), `POP_JUMP_IF_TRUE`, `POP_JUMP_IF_FALSE`, `FOR_ITER`, `RETURN_VALUE`, `YIELD_VALUE`, `RAISE_VARARGS`, `RERAISE`.
`coverage.py` follows them from where a branch went to the next line, to tell which of the branches in the source that was. Code that cannot be reached, which CPython drops, is all `NOP`.
`co_exceptiontable` has JSC's handlers in CPython's form, for the same units: ranges in order that do not overlap, the innermost handler for each; the depth of the stack is 0.

**Places.** Columns come from JSC's table of where expressions are, which has an entry for what may raise; what has none is of the place before it if that is on its
line. A jump of the program's has one: the place of whatever CPython's compiler emitted last (`Unit::m_place`), which is where its jumps get theirs. It matters for jumps that go back: they look for signals, so a
`KeyboardInterrupt` in a busy loop is raised there, and the traceback marks the place. As in CPython no jump leads to a jump: what would fall off the end of a statement into
a jump (the end of the body of an `if` that has an `else`, of a `case`, of a `try` with handlers) goes where that goes (`Last::BeforeJump`), as it returns if the function ends there and goes round if the body of a loop does.

**Comparing code.** Code objects are equal if their names, flags, first lines, variables and instructions (as the compiler left them) are, and then their text, or, for code made from a tree,
their constants (by type and bits: `0`, `0.0`, `-0.0` and `False` are four) and `co_positions()`. The hash leaves the instructions out: `bdb` keeps code objects in dictionaries while it traces them.

**Frames.** While something is told of an event, `f_lineno` of that frame is the line CPython would show. `f_locals` reads and writes the registers.
`f_lineno = n`, in a trace function, goes where CPython goes and refuses what it refuses, with its messages: the target must be the beginning of a statement whose blocks the
frame is in already, or can take over (a `for` loop's iterator serves another `for` loop). Variables without a value get `None`, with CPython's warning, because code further on
may have been compiled knowing that they have one.

**Threads.** Trace and profile functions, and whether a callback is running, are the thread's (`Runtime::ThreadLocalState`). Tools 6 and 7 want their events
while any thread has a function. The side tables are only changed with the GIL, and those the collector's threads read, with the lock of the table of monitored code as well.

A monitor belongs to what is known about the code (`CodeInfo`), which belongs to the executable, and goes with it. It refers to the code weakly: the executable keeps its code for as long as it has a monitor
(`UnlinkedFunctionExecutable::returnCodeToCache()` leaves it alone), the two die in the same collection, and are swept in no particular order. So code that a program makes, runs under a tool and drops goes like any other.
The collector is told the size of the side tables, which are several times the size of the code.

`sys.monitoring._places(code)` shows the places of a code object, and which are replaced right now (offsets as JSC has them).

## JavaScript sees Python objects

- Properties are attributes (items first, for mappings), through the method table, as for any exotic object of JSC.
- A class is called or `new`ed; `instanceof` is `isinstance`.
- **`class Derived extends Base`**, where `Base` is a class of Python's, is `class Derived(Base)` (`runtime/PyJSClass.h`). When the
  methods are in place and before anything can see the class, the bytecode of a class definition with a heritage hands
  constructor, prototype and superclass to `deriveClassFromJS` (unless the superclass is a `JSFunction`, which a `PyType` is not), and
  what that returns is the class from then on: a `PyType`, made by the metaclass of `Base` from a namespace with what the
  prototype and the constructor have. So `__init_subclass__`, `__set_name__`, `ABCMeta` and the like run when they would in Python,
  `issubclass()` holds, Python finds the methods and can derive from the class in its turn.
  A method is a `JSMethod`, a function whose first argument is the `this` of the JS function, so `Derived.method(instance)` works as
  `instance.method()` does; an accessor is a `property`; a static method is a `classmethod`; what has a symbol for a key stays
  with JS, which finds it on the instances.
  `__new__`, `__init_subclass__` and `__class_getitem__` are a static method and class methods without being told, as in a class statement.
  A class without a constructor, and without fields or private methods, which a constructor would have to install, has neither
  `__new__` nor `__init__`: it is `class Derived(Base): pass`, hides nothing as a mixin, and can have an `__init__(a, b) {}` as in Python.
  Otherwise the constructor is the `__new__` of the class, since it is what makes the instance, and `__init__` does nothing.
  So `class Day extends date { constructor(text) { super(...parse(text)); } }` works, which in Python takes a `__new__`. As with
  any class that has a `__new__`, a class in Python that derives from it and takes other arguments has to have one too.
  What can stand among the bases of a class can stand behind `extends`: `Generic[T]`, `list[int]` (`__mro_entries__`).
- **`super` goes by the MRO**, as `super()` does, so that a class written in JS cooperates with whatever it is combined with
  (`class C(Derived, Other)`). The home object of the methods and the constructor get prototypes of their own for it:
  `super.method()` is what follows `Derived` in the MRO of `this` (`PyPrototypeView::createSuper`; in a static method, of the class
  that is `this`), and `super(...)` is the `__new__` and then the `__init__` that follow `Derived` in the MRO of `new.target`
  (`PySuperConstructor`, `typeCall` with `afterType`). If the class has an `__init__` besides its constructor, `super(...)` is `__new__`
  alone, and the `__init__` calls those of the bases. What it gives is `this`, which JS takes for an object: if a `__new__` returns 5, `None`
  or a str, it throws the TypeError of a constructor that returned no object. (`new P()` for the class of Python itself gives the 5. So does a `super(...)` that
  gets to that class by another way than `extends`, `Object.setPrototypeOf(F, P)` or a Proxy: see DEFERRED.md.)
- **`Class.prototype`** (`PyPrototypeView`) has what the class and its bases have. Read for an instance
  (`Reflect.get(Class.prototype, name, instance)`) it binds as `super()` does, looking from the class on in the MRO of the instance.
  Read by itself it gives functions that take the instance as `this`: `list.prototype.index.call(array, item)`. What is assigned
  to it, or defined on it, is a method or a property of the class.
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
  primitive values (`String`, `Number`, `Boolean`, `Symbol`, `BigInt`) make one, as they do when JS calls them.
  A function of the host that can be constructed but has no `prototype` for instances (a mock function of `bun:test`) is called.
  A `function` is a class if somebody has given its instances something to inherit, as was done before there was `class`
  (`EventEmitter`, `stream.Readable`): its `prototype` has a property of its own besides `constructor`, or inherits from something else than
  `Object.prototype` (`isClassWrittenInJS`; it looks at the two objects and runs no code). Any other `function` is called. A host with classes that are
  meant to be called says which (`setClassIsCalledFunction`): in Bun those whose binding says what a call does, so `expect(x)`.
  `f.new(...)` is `new f(...)` for anything. (A decision that can still be changed.)
- Keyword arguments for a JS function are a trailing object, and JS passes keyword arguments as `kwargs({ name: value })`, last. The two
  are the same thing: a plain object with a mark that JS cannot see (a property whose key is a private name: `isKeywordMarker`).
  As the last argument of a call that JS makes of something of Python's, an object with the mark is keyword arguments. So
  what a function of JS was given it can pass on: `constructor(...args) { super(...args); }`. A copy (`{ ...options }`) has no mark, and
  when it is Python that passes the object it is an object like any other. (Who calls is told as the callee starts, by
  `isBeingCalledByPython`: the frame it returns to does not tell, since `return f(...)` in strict JS code is a tail call.)
- A dict iterates in JS as a `Map` does, by `[key, value]`.
- `JSON.stringify()` makes of a `datetime.date`, `time` or `datetime` what it makes of a `Date`: the text by ISO 8601, which is what `isoformat()` gives
  (`toJSON`, `jsViewIsDateOrTime`). No other type of the standard library has a `toJSON` of its own: a `Decimal`, a `UUID` or a `Path` would lose
  what it is without a word. (A decision that can still be changed.)
- A context manager is no disposable: `using` would skip `__enter__`. In Bun, `bun:python` has the statement as a function, `with(manager, body)` and
  `asyncWith(manager, body)`, and for `using` there is `enter(manager)`: it calls `__enter__` and gives `{ value, [Symbol.dispose]() }`, which calls
  `__exit__(None, None, None)`, once. `await enterAsync(manager)` is the same with `__aenter__`, `__aexit__` and `Symbol.asyncDispose`, for `await using`.
  JS tells a disposable nothing of the exception that leaves the block, so the manager does not hear of it and cannot swallow it: a
  transaction commits, `suppress()` suppresses nothing. Where that matters, `with()` is the full form. `__aenter__` and `__aexit__` are two tasks, and the
  block is in neither: `asyncio.timeout()` and `TaskGroup` need `asyncWith()`. (A decision that can still be changed.)
- What all objects of Python's inherit in JS (`toString`, `Symbol.toPrimitive`, `Symbol.iterator`, `then`, `toJSON`, what `console.log` asks)
  is about the object of Python's also where `this` is a `Proxy` around it or was made of it with `Object.create()` (`jsViewThis`).
  The operators do not see through those.
- **Memory.** What exports its memory in Python (`bytes`, `bytearray`, `memoryview`, `array.array`, `mmap`, a class with `__buffer__`) is a
  buffer to the code of the engine and the host that takes a typed array or an `ArrayBuffer`: `WebAssembly.Module(bytes)`, and in
  Bun `new Response(bytes)`, `Bun.write()`, `fs.readSync(fd, bytearray)` and so on. ("For the host" in `runtime/PyBuffer.h`.) It is
  never made a string of there. Such code keeps no `Buffer` on its stack but a value that it roots while it uses the memory, which
  may be long after the call: `memoryForHost()` gives it that value, the holder. Of a `bytes` it is the object itself; of anything else a
  `memoryview` that the program cannot get at, which is an export like any other for as long as a root refers to it or the host has
  pinned it. So while a thread of the host reads or fills a `bytearray`, to resize it is a `BufferError`, as it is while Python's own
  `readinto()` runs in another thread; afterwards the view is dropped and found to be so when the exporter next asks.
  Memory that is read-only says so, and what would write to it refuses it (`fs.readSync(fd, bytes)` is a `TypeError`).
  A view that was released, or whose bytes do not follow one another, raises what it raises in Python (`ValueError`, `BufferError`).
  An instance of a class whose `__buffer__()` is written in Python gives its memory by running code of the program. The host takes it
  where it converts its arguments before it does anything else, as it calls `toString()` there (`new Response(o)`, `Buffer.concat([o])`).
  To code that has looked at its own state already and asks a value for its bytes in passing (`hash.update(o)`, a parameter of an SQL
  statement, `crypto.randomFillSync(o)`) it is no buffer, and is refused like any other object: `getBufferRunsCode()`.
  To code written in JS these objects are no typed arrays (`ArrayBuffer.isView()`, `instanceof Uint8Array`): see `DEFERRED.md`.
- **Operators.** Where an operand is an object of Python's, `a + b`, `-a`, `~a`, `a < b` and so on are what they are in Python
  (`binaryOperatorForJS` and the like in `PyHooks.h`), in the slow paths that all tiers share (`OperationsInlines.h`,
  `JSValue::equalSlowCaseInline`). The type of the other operand is asked too, whatever it is: `"%s: %d" % pair`, `[0] * index`,
  `2 * fraction`, `10n + fraction`. What neither has an answer to is Python's `TypeError`: `timedelta(1) + 1`, `-object()`, `a < null`.
  An object that has `__float__` or `__index__` and no `__mul__` is not made a number of for `a * 2`, no more than in Python: `+a * 2`.
  One exception: `+` with a string of JS on either side. There only the class of the object of Python's is asked (`__add__`,
  `__radd__`), and if it says nothing JS concatenates, as with any object: `"total: " + decimal`. (Decisions that can still be changed.)
  `a == b` asks `__eq__`, and goes on as JS does if there is no answer; `a == null` is false without asking. Unary `+` and `>>>` are JS's.
  The parser makes `+a` of `a * 1`, as it always did, and the `op_to_number` says what was written (`ToNumberForm`): its slow paths ask `__mul__`.
  `===`, `Object.is`, `switch`, `includes`, `indexOf` and the keys of a `Map` or `Set` go by identity.
  To the DFG and FTL a Python object has a type of its own, `SpecPyObject`. An arithmetic operator returns a primitive unless an
  operand may be of that type. `==` on two objects is a comparison of pointers behind a check that neither is of that type
  (`DFGFixupPhase.cpp`): `NotPyObjectUse`, or, where a site has seen plain objects only, or arrays, or functions, the check for that
  kind in place of the check for an object. A site that has seen a Python object calls the slow path instead.
- **What writes properties behind the object's back.** Code of the host that would store a property straight into an object has to ask first whether the properties are Python's
  (`propertiesArePythons()` in `PyHooks.h`: an object, a class, `Class.prototype`, a function, a list) and go through `defineOwnProperty` then, which is `setattr()` or `[]=`. `spyOn()` of `bun:test` does: the spy
  is where Python looks too, `mockRestore()` puts back what was there, and what cannot be stored (an attribute of a tuple) is the error that Python raises.

## Python sees JavaScript objects

`runtime/PyInterop.cpp`. Properties are attributes and items, those of JS before what the Python type has. Besides:

- **`import js`** gives the global object. It is in `sys.modules` from then on, not before: a program that has nothing to do with JS finds nothing
  of JS's there. `js` is a module written in C++ that puts the global object in its own place in `sys.modules` when it runs
  (`NativeModuleDef::execute`), so what `importlib` writes into a new module (`__spec__`, `__loader__`) does not end up in the global object.
  Code of the host's that is not to be seen importing it has `_js.globalThis`.

- A name that is a keyword of Python cannot stand behind a dot, so it is written with an underscore behind it: `js.Array.from_(x)`,
  `promise.finally_(f)`, `o.class_ = 1`, `del o.in_`. Only for the keywords (`keyword.kwlist`, not `match` or `type`), and only if the object has no
  property of the name with the underscore (`propertyKeyForAttribute`). (A decision that can still be changed.)
- **`js.null`** is `null`. `None` goes out as `undefined`, and some functions tell the two apart: `stream.push(js.null)`,
  `js.Object.create(js.null)`. It is no object of a type of its own but the value `null` itself, which to Python is `None` wherever it comes
  from (`js.null is None`, its `repr()` is `None`) and stays `null` for as long as it is only passed on. A list, a dict or an attribute is shared with JS,
  which reads what is in it without Python having a say, so only `null` itself is `null` there too. `pickle` makes `None` of it.
  (A decision that can still be changed.)
- **What a `JSError` has not is asked of what was thrown**, if that is an object: `e.code`, `e.message`, `e.cause`, `e.errors` are
  `e.value.code` and so on (`JSError.__getattr__`). Only its properties, and only to read. Only for Python: JS, which has `error.value`, sees the
  exception as it sees any other (`attributeOfExceptionForJS`). (A decision that can still be changed.)
- **A `TypeError` is a `TypeError` in both languages.** What stands for a thrown object that is an `instanceof TypeError` is a
  `JSTypeError`, a class derived from `JSError` and `TypeError`: `except TypeError` catches a call of what is `undefined` or an
  `ERR_INVALID_ARG_TYPE` of Node, and so does `except JSError` (`exceptionFromThrownValue`). To JS an instance of Python's `TypeError`, or of
  a class derived from it, is an `instanceof TypeError`: behind it comes `TypeError.prototype` where the other exceptions have
  `Error.prototype`. All exceptions have one `Structure`, and an object can change its class, so the prototype is asked for
  (`PyException::getPrototype`). (A decision that can still be changed.)
- **An error of the system is an `OSError`.** Node and Bun throw an `Error` with the name of the error number as its `code` (`"ENOENT"`) and a number as
  its `errno`. What stands for one is of the class that `OSError(errno, ...)` makes and a `JSError` as well: `JSFileNotFoundError(JSError,
  FileNotFoundError)`, `JSPermissionError`, or `JSOSError` where there is no class for the number (`osErrorFromThrownValue`; the classes are made when
  they are first wanted, also by `from _js import JSFileNotFoundError`; `IOError` and `EnvironmentError` are other names for `OSError` in `builtins` and no classes, so there is no `_js.JSIOError`, unless JS has a global `IOError`: then it is the class of that, and no `OSError`). `e.errno` is the number that goes with the name on this system, positive as in Python,
  whatever the `errno` of JS is (Node's are negative, and libuv's own on Windows); `e.strerror` is `os.strerror()` of it, `e.filename` the `path`, `e.filename2` the
  `dest`, and `str(e)` reads as for any `OSError`. What JS said is still there: `e.message`, `e.code`, `e.syscall`, `e.value`. A `code` that names no error number
  (`ENOTFOUND`, `ERR_...`) makes no `OSError`. The other way round an `OSError` of Python's has, for JS, a `code` (by `errno.errorcode`), a `path` and a `dest`, which is what code written for
  Node asks; its `errno` is Python's attribute, positive, and it has no `syscall`. What a `JSError` stands for has a field of the exception to itself, so the
  fields of `OSError` are free. No other class of errors is linked. (A decision that can still be changed.)
- **A class of JS can stand in an `except` clause**, alone or in a tuple: `except js.RangeError`, `except (ValueError, AppError) as e`. It catches an exception that stands
  for what JS threw, if that is an instance of the class. Any class will do (`isClassOfJS`, the test that decides whether a call constructs), since JS can throw
  anything; what is no class is the `TypeError` it always was. It goes by the prototypes and runs no code (`standsForInstanceOfJS`), as CPython's matching runs none: a
  `Symbol.hasInstance` or a proxy is not asked, and it works in any thread. It catches nothing of Python's, though to JS every exception of Python's is an
  `Error`: `except js.Error` is not to swallow a `KeyboardInterrupt` or a `CancelledError`. The same for `except*`, and for `split()` and `subgroup()` of a group, where
  a class of JS is a class and any other function a predicate. `isinstance(e, js.RangeError)` asks about what was thrown too (by `instanceof`).
  An exception of one of these classes that was made in Python (`JSRangeError("x")`) stands for nothing: there its class says what it is. (A decision that can still be changed.)
- **The classes of what JS threw mirror those of JS.** What stands for a `RangeError` is a `_js.JSRangeError`, a class derived from `JSError`, and for
  `class ValidationError extends RangeError` it is a `JSValidationError(JSRangeError)`. `JSError` itself stands for `Error`, and `JSTypeError` for `TypeError`. So what goes by `type(e)` sees what was thrown: a
  traceback, `type(e).__name__`, and `issubclass(type(e), js.RangeError)`, which is what `contextlib.suppress(js.RangeError)` asks (`jsFunctionSubclassCheck`; like `except`, it runs no code and works in any thread, also for the exceptions of Python's own, which `suppress()` asks about as well). What wants a class of Python's, as `assertRaises()` and
  `pytest.raises()` do, gets it from `_js.exception_class(js.RangeError)`, or for a global of JS's by `from _js import JSRangeError` (`exceptionClassNamed`; the classes that go with those of Python's come first: `_js.JSTimeoutError` is
  `(JSError, TimeoutError)`, whatever `globalThis.TimeoutError` or `builtins.TimeoutError` is; then the errors of the engine, whatever the program has made of the globals, and these two kinds are there in every thread; then the globals, which on a thread other than the main one is a `RuntimeError` where the thread cannot see whether there is one, and an `AttributeError` where it sees that there is none).
  There is one class for each object on the chain of prototypes between what was thrown and `Error.prototype` (`exceptionClassForPrototypeOfJS`), so classes made the old way are mirrored
  too (`Mine.prototype = Object.create(Error.prototype)`), and so are the prototypes that Bun puts between an `ERR_...` error of Node's and `TypeError.prototype`: `type(e)` is then a class derived from `JSTypeError`, of the same name. It is named `JS` and the `name` of the constructor whose `prototype` the object is, or else the `name` that the object has itself, or else, and where that is no name for a class (nothing at all, or nothing that is seen: blanks, and what is to be ignored by default; a control character or the end of a line, which a traceback would print; a character that is not seen and changes what is, as those that turn the direction of the text, but for U+200C and U+200D, which the grammar of JS takes within a name; a lone surrogate; more than 1,000 characters: `canNameClass`, which the name of a class of Python's that derives from one of JS's goes through too), as its base. The classes are made when something of the kind is first
  thrown or asked for, by looking only: no code of the program's runs. Nothing can be set in them, as in `JSError` (`TypeFlag::Immutable`), so there is no `__init_subclass__` of the program's to call (who gets at the dict of `JSError` through `gc.get_referents()` and puts one there finds it not called); a name that has no UTF-8 never reaches `type()`, which would ask the handler of errors that the program may have registered as `'strict'`; and the VM is told to let nothing in meanwhile (`DisallowVMEntry`, as for a look at a property with `VMInquiry`), which with assertions is a crash. They are kept in a `WeakMap` by the prototype, and die with it. Where there is no stack or memory to make one, what was thrown is for now of the nearest class that could be made, and nothing is kept of that, neither the class for the prototype nor the exception for the object, nor one whose `cause` it is: whoever catches it further up, where there is stack, gets another (`IfClassCannotBeMade`). So an error whose `toString()`, or whose getter for `errno`, uses up the stack or the memory wherever it is called is never kept: every handler has another exception for it. (A class for a prototype that cannot be made for another reason never can: the nearest stands in for good, also for who asks for the class.) Where a thread other than the main one cannot see what the class is called (the host computes `DOMException.prototype.constructor`) the nearest class stands in too and is not kept for the prototype, but the exception is kept for the object: what the thread catches, changes and raises again stays the same object, also for the main thread, which then finds it as the thread has made it: with that class and with the message and the `errno` that the thread could see. Such a thread runs nothing of JS's, so the message is made of the `name` and `message` that are there to be seen, and `errno`, `code` and `path` count if they are (`messageOfErrorForAnyThread`). To who asks for the class itself, `exception_class()` or `from _js import`, the `RecursionError`, `MemoryError` or `RuntimeError` is raised. A termination waits while they are made. Every class has an MRO of its own, so what a chain of prototypes costs grows with the square of its length, where to JS it is so many objects (`Object.create()` in a loop). So a chain is mirrored until the MRO has 256 classes (`longestMROOfMirror`), and the prototypes further down have the class of the last that is mirrored: `type(e)` and `exception_class()` give that, `issubclass(type(e), js.FarDown)` says no, and `except js.FarDown` and `isinstance()` go by the object as ever. A class stays as it was made: who gives a prototype another prototype later (`Object.setPrototypeOf(Mine.prototype, ...)`) changes what
  `issubclass(type(e), js.RangeError)` says, which goes by the prototypes, and not the bases of the class: if the new parent is `TypeError.prototype`, `except js.TypeError` catches the next one that is thrown, and `except TypeError` does not. An error of the system whose class extends `Error` is of a class derived from the mirror
  and from the `OSError`, `JSFileNotFoundError(JSDiskError, FileNotFoundError)` (`osErrorClassFor`). The `Error` of another realm (`node:vm`) is an error and no `js.Error`: its class derives from `JSError`, and that of its `TypeError` from `JSTypeError` too: `except TypeError` and `except JSTypeError` catch it, `except js.TypeError` does not, as `instanceof` says no. So two classes can have one name, as two of JS's can: that of the other realm's `Error` prints as `<class '_js.JSError'>` and is not `_js.JSError`, and a traceback shows the name alone.
  What was thrown and is no `Error` by its prototypes (`throw 1`, `throw {}`, a `Proxy`) is a `_js.JSThrownValue(JSError)`, which mirrors nothing: `except JSError` catches all that JS throws,
  `except js.Error` and `suppress(js.Error)` the errors. (`except js.Object` catches a thrown `{}`, going by the object. `suppress(js.Object)` does not.) `issubclass(E, f)` says no for every class `E` of exceptions of Python's own and every function `f`, `js.Object` too, in any thread, as `except f` catches none of them: it is what `suppress()` asks. (So `issubclass(KeyError, object)` and `issubclass(object, js.Object)` hold and `issubclass(KeyError, js.Object)` does not. It is no also where JS has made `f` of the class itself, `KeyError.bind(null)` or a `Proxy` of it.) To JS they are `Error`s all the same, and `isinstance(e, js.Error)` is `e instanceof Error`, which is true. What JS puts under `E.prototype` changes none of this: the instances do not inherit from it.
  A mirror can be instantiated and derived from, as `JSError` can. To JS such an instance is an exception of Python's and an `instanceof RangeError`: its prototype inherits from `RangeError.prototype`
  (`prototypeForInstancesOfExceptionClass`), as that of a `TypeError` of Python's from `TypeError.prototype`; where a class derives from both, the mirror says what it is. That is what the instance answers when it is asked (`PyException::getPrototype`), not what is stored in it. What only looks at a chain of prototypes goes by what is stored, so `Object.create(e)`, thrown, is an `instanceof RangeError` to JS, and to Python an error that `except js.RangeError` does not catch.
  `pickle` names a class by its module and name, for another process to find, so an exception is pickled as of the nearest class that `_js` has under its name in every process (`classToPickleAs`, `exceptionClassNamedEverywhere`): the errors of the engine and what goes with the classes of Python's, so `JSRangeError` for a `ValidationError extends RangeError`, global or not, and `JSError` for a `DOMException`. `copy.copy()` and `copy.deepcopy()` stay in the process and keep the class: they do what they do for a class without `__copy__()` (`copyreg.dispatch_table`, `__reduce_ex__(4)`, else `__reduce__()`, else `copy.Error`; so a `__reduce__()` of the program's counts) and put the class back where it was `JSError.__reduce__()` that named another: not where the program has registered a function for the class or given the instance a `__reduce_ex__` or `__reduce__` of its own that is not `None` (`copyOfJSError`). The rest of `JSError.__reduce__()` is `super().__reduce__()`: that of `OSError`, or of what else a class of the program's derives from (`class PluginError(JSError, ImportError)`). (A decision that can still be changed.)
- **`raise js.RangeError("x")`** throws that very object: JS catches what it expects, and Python the `JSError` that stands for it. `raise js.RangeError` makes the
  instance. Only an `Error`, or a class of them; anything else is the `TypeError` it was. So after `from`: `raise ValueError() from js.Error("x")`. And
  `new Error(message, { cause })` is `raise ... from cause`: if the `cause` is an own property and an `Error` or an exception of Python's, it is the `__cause__` of
  the `JSError`, from when that is made. (A chain of causes is gone down in a loop, to its end: how far does not depend on how much stack there is. That has its price where a program makes a very long one, at the first catch: some 2 µs and 330 bytes a link, so a fifth of a second and 33 MB for 100,000, where it used to be cut at some 18,000. The host can end it. Nothing of it is kept before all of it has been made, so that one that is kept has causes that are kept: the `toString()` of one link is code of the program's, which may catch another link meanwhile. What a link has got in that way is what stands for it, and takes the place of what was being made.) `raise js.Error("x") from e` sets the `__cause__` of the `JSError` and leaves the object of JS as it is.
  (A decision that can still be changed.)
- **What the items of an object are** is said in one place (`itemsOf`), and iteration, `len()`, `bool()`, `in`, `keys()` and the like go by it: those of a mapping by its methods (below) are what `keys()` gives; else they are
  what JS iterates over, if it does; nobody can say what is in a `WeakMap` or a `WeakSet`; of anything else they are its own enumerable properties, by their names. It is made of three questions, whether the object is a `Map`, whether JS iterates over it (`methodOfIteration`) and
  whether it is a mapping by its methods then (`itemsOfIterable`), and `len()` and `in`, which often know enough before the last, ask them one by one. A `Map` is a mapping whatever has been done to it: it is known by its cell, and where that takes a method that is gone
  (`Object.setPrototypeOf(map, null)`), it raises. JS iterates over anything else that has a function as its `Symbol.iterator`: what is there and is no function is as good as nothing. (A `Set` that the engine itself goes through without calling anything, `isIteratorProtocolFastAndNonObservable()`, is not asked. One that has lost its `Symbol.iterator` is an object
  that JS does not iterate over, with properties and no `len()`; `in` still says what is in it.) **The `prototype` of a class** (an object with a `constructor` of its own whose `prototype` it is: `isPrototypeOfConstructor`) has `Symbol.iterator`, `has()` and the like for the instances and is none of them: `Set.prototype.has(1)` throws. JS does not iterate over it, and
  its items are its properties: `'union' in js.Set.prototype`, `'get' in js.Map.prototype`, `js.Headers.prototype['get']`. That is so also where the methods would work on it, because it is a collection besides: `String.prototype` is an empty `String` object, and had a `len()` of 0; `Sub.prototype = new Super()`; `F.prototype = arguments`. Not so for what is a `Map` or a `Set` by its cell, which is known before the question comes up (`F.prototype = new Set([1])`). (A mapping that can be changed, like any object: `js.Headers.prototype.clear()`, `pop()`, `popitem()` and `del` take the methods away from all `Headers` of the realm, as `delete Headers.prototype.get` does in JS, where the host's methods are configurable. So it was for `Blob.prototype`, which was never taken for a collection.) A `constructor` that is a getter is asked if it is of the kind that only the engine and the host can make, written in C++ (`Iterator.prototype`, `Headers.prototype`). An accessor that JS can make is never asked, whatever its getter is. **JS has no mark for a prototype**, so what serves as one and has no `constructor` of its own is not known for one: the prototypes of the iterators (`Object.getPrototypeOf([].values())`),
  `(function* () {}).prototype`, `Object.create(Set.prototype)`, a `Proxy` of `Set.prototype`, what a class of Python's that derives from `Set` is to JS. It inherits `Symbol.iterator` or `has()`, so it is asked as an instance would be, and `in`, `len()` and iteration raise what `for...of` throws over it in JS. `hasattr()` is the test that works for all. (The engine knows which objects have been made the prototype of something, `Structure::mayBePrototype()`. To go by that would
  change what a `Map` is when somebody calls `Object.create(map)`.) **A `Proxy` of an object of Python's**: what such an object contains, how much and in which order is what its type says, and to JS its items, its attributes and
  what JS wants of it (`length`, `Symbol.iterator`) are all properties (`{'size': 7}` has a `size`). So `len()`, `bool()`, `in` and iteration of the `Proxy` are those of the object (of a class, what its metaclass says: `len(js.Proxy(Color, {}))` of an `Enum`), all four as soon as its type has one of `__len__`, `__contains__`, `__iter__` and `__getitem__` (one with `__len__` alone is not iterable behind a `Proxy` either; one with `__getitem__` alone is gone through by 0, 1, 2 and so on, as Python does with it; a `__bool__` alone decides `bool()` alone), whatever the handler has, as `typeof` and `Array.isArray()` are those of the target (`objectOfPythonBehindProxy`). An instance of a class that has none of them is an object with properties, its attributes. The rest goes through the `Proxy`: `p[k]`, `p.get(k)`, `p.keys()`, attributes. So its items are not its properties: it has the methods of a mapping that the object has and no others (no `popitem()` for what has a `__len__` alone), and `vars(p)` is not `p`: it is the `__dict__` of the object, which is a property of the `Proxy` like any attribute, and for an object without one the mapping of the properties of the `Proxy`, which are all the attributes that the object and its type have. (What is to run nothing asks the same by looking only, and says no where it cannot tell: `itemsAreProperties` for `__dict__` and `match`,
  `givesWayToGetOfMappings` for what `o.get` is. So `vars(js.Set.prototype)` is the object, which lists what is enumerable as any object does, so none of the methods (`'add' in vars(js.Set.prototype)` and `dir()` have them), and a mapping pattern takes the `prototype` of a class that iterates, if that is a plain object: the class extends nothing. `case {**rest}` then calls its `keys()`, if it has one, as it does for a prototype that does not iterate.)
  (`o[k]` goes by the methods of a mapping by its methods and else by properties, also where JS iterates over the object: see below. A typed array is a sequence by its type, which has its own `[]`, `in`, `len()` and iteration: see below.)
- `len()` is how many items iteration gives, of what is a container to Python (`numberOfItems`). That is, first, what JS iterates over and says how many that gives: by its `size`, or else by its `length`. (The other way round for what the engine itself makes like an array, an `arguments` object and a `new String()`. Of that the `length` counts halves
  of pairs, as its `[]` does, where iteration gives characters.) It is the object that says it: a class derived from `Map` with a `keys()` of its own and `Map`'s `size`, or an order of shirts that JS iterates over and that has a `size`, says something else than iteration gives. Not asked is what the engine goes through without calling anything: a `Set` whose `Symbol.iterator` is `Set`'s own, and a `Map` whose `keys()` is `Map`'s own, of whichever realm, if that gives an iterator that nobody has touched there. So it is for an instance of a class derived from them that has left these alone (one of Python's: and has no `__iter__` of its own). They are counted as they are, which is what iteration gives, whatever has been made of their `size`. (The method is looked at, not asked for. Where it is a getter the object is not counted so: it says its `size`, and a `Set` is first asked for its `Symbol.iterator`, as anything else is. A `Set` that has a `get()`, a `has()` and a `keys()` is a mapping by its methods, which iteration goes through by its `keys()`: it is not counted so either, unless that `keys()` is `Set`'s own, and `len()` asks that with the function that iteration asks it with, `itemsOfIterable`, once. That one asks for `get`, `has` and `keys`: a getter for one of them runs at `len(s)` and at `if s:` as it does at `for x in s:`, and what it throws is raised there.) A mapping by its methods that has neither has as many as its `keys()` gives. What else JS iterates over has no `len()`: a `TypeError`, as for a generator.
  Then a plain object, whose items are its properties: it is the number of those that iteration lists. There `length` and
  `size` are properties like any other, data as likely as not (`len({ size: 3, a: 1 })` is 2, and so is `len(o.__dict__)`), and what is inherited is no item. Anything else is no container, as an instance of a class in Python is none: it is true whatever it has, and has no `len()`.
  So `len(function)` is not the number of its parameters, which is `function.length`, and `len(blob)` is not its `size` in bytes, which is `blob.size`: both are a `TypeError`. So are `len(vars(function))`, since `vars(o)` is `o`, and the `len()` of what has
  `Object.prototype` for its prototype and is no plain object to the engine: a `Proxy` of one, `process.env`, `Bun.env`, `require.cache`. (Their properties can be iterated over all the same, and `keys()`, `items()` and `dict(o)` give them: `len(list(js.process.env))`.)
  What has a `len()` is false when that is 0, and nothing else is false, also where the `__len__()` is that of a class of Python's that derives from one of JS's (`jsObjectLen`, `jsObjectBool`). (`isinstance(o, collections.abc.Sized)` goes by the type, and says yes for every object of JS's.)
- `x in o` asks about the items too: of a `Map`, a `Set`, a `WeakMap` and a `WeakSet` it is `has(x)` (a `Set` without a `has()` is looked into); so of what else JS iterates over, if there is a `has()`, which a mapping by its methods has, and what is like a `Set`. For what else JS iterates over it is whether iteration gives something equal to `x`, as for an iterable of Python's
  (`1 in js.eval("(function () { return arguments })(1, 2)")`). Where it stops early, because it has found `x` or an `__eq__()` has raised, the iterator that it has made is told, as by a `for...of` that is left: its `return()` is called. Not so an iterator that the program has: of a generator `in` takes what it looks at, and the program goes on with the rest. Between two items there is a safe point (`safePoint()`), which code of JS's has not, as in `sum(o)` and `list(o)`: so Ctrl-C ends it where the iterable has no end, and other threads of Python's have their turn. Where a class of Python's that derives from one of JS's has an `__iter__()` of its own and the object no `has()`, it is what that gives, as for a class of Python's without a `__contains__()`. For an object whose items are its properties, it is whether it has such a property itself (`jsObjectContains`). A symbol is no item of these last two, also where iteration gives one: `js.Symbol.iterator in o` is the `in` of JS, and takes nothing from a generator.
- **`async for` is `for await`**, also where it is left before the end: by `break`, `return` or an exception (a cancellation too), in a
  statement or a comprehension. Then the iterator's `return()` is called and awaited, which is what an object of JS counts on to
  clear its timer or to cancel and unlock its stream (`getAIterOfLoop`, `leaveAsyncFor`). The handler that does this for an exception is one that CPython's code has
  not, so it is a `HandlerType::SynthesizedCatch`, as what JSC's compiler adds for itself is, and tools are told neither that it catches nor that it raises again. It is called and not awaited by a coroutine
  that is being closed, and where it is the wait for the next item that was cut short: an async generator of JS answers
  `return()` after the `next()` that it still owes. Only for the iterator that the loop itself has made of an object of JS: one that the program made with `aiter()`
  is the program's, and has `aclose()`. For everything of Python's the loop does what it does in CPython, which is nothing.
- **Truth.** What is a container to Python is false when it is empty: what has a `len()`, as above (a `Map`, a `Set`,
  a typed array, a mapping by its methods, a plain object). Everything else is true, as an instance of a class in Python is: a function, a class, a
  promise, an `Error`, a `Date`, a `Response`, an iterator. So `if error:` and `if callback:` do what they say.
- An object that JS does not iterate over is a mapping of its own enumerable properties: `keys()`, `values()`, `items()`, `get()`, `pop()`,
  `popitem()`, `setdefault()`, `update()`, `clear()` (those of `collections.abc.MutableMapping`), unless it has properties of these
  names; so `dict(object)`. What JS iterates over has none of them, unless it is a mapping by its methods: `dict(map.entries())` takes the pairs.
- **The items of an object are its own properties**, unless JS iterates over it or it is a `WeakMap` or a `WeakSet`. `k in o`, `o[k]`, `o.get(k)`, `o.pop(k)`, `o.setdefault(k)` and `del o[k]` see what the object has itself, as iteration, `keys()` and
  `dict(o)` do: what it inherits is for attributes, which follow the chain as ever. So `"toString" in o` and `"constructor" in o` are false for `{}`, as they are for a dict, and what is read of data
  with such a key is not a method (`itemOfObjectOfJS`, `jsObjectContains`, `jsObjectDelItem`). That is not the `in` and `[]` of JS:

  | | JS | Python |
  |---|---|---|
  | an own enumerable property | `"a" in o`, `o["a"]` | the same |
  | an own property that is not enumerable (`f["name"]`) | there, and not listed | the same: `"name" in f`, `f["name"]`, and not in `list(f)` |
  | what is inherited (`"toString"`, a method or a getter of the class) | `"toString" in o` is `true`, `o["toString"]` the function | `"toString" in o` is `False`, `o["toString"]` a `KeyError`, `o.get("toString")` `None`; `o.toString` is the method |
  | `delete` of what is inherited | `true`, and nothing happens | `del o["toString"]` is a `KeyError` |
  | a name that is not there | `o["x"]` is `undefined` | a `KeyError` |
  | `o[k] = v` | the assignment | the same. So a setter that the object inherits takes it, or the `set` of a `Proxy`, and what is inherited and read-only refuses it: after it `k in o` may still be false |
  | `"__proto__"` | `o["__proto__"]` is the prototype, and `o["__proto__"] = x` sets it | `o["__proto__"] = x` sets the prototype, as in JS, or does nothing if `x` is no object. `o["__proto__"]` reads the own property: a `KeyError`, unless `JSON.parse()` has made one |
  | a `Symbol` | follows the chain | the same, also for `del`: a symbol is no item, and `o[js.Symbol.iterator]` is the one way to write that read |
  | an `int` | `1 in o` is `"1" in o` | the same |
  | a `Proxy` | `has`, `get` | `getOwnPropertyDescriptor` says whether there is an item, and then `get` what it is. So one that makes up its properties with `get` and `has` alone has no items, as it has none to iterate over (`ownKeys`): that is meant. Its attributes are what `get` says (`p.a`, `getattr(p, "x-y")`) |
  | a binding of a module that is not initialized yet (a cycle of imports) | `"x" in namespace` is `true`, `Object.hasOwn(namespace, "x")` throws | `"x" in namespace` raises the `ReferenceError`, as the read does |

  But for `in`, which asks about the items, it holds for every object whose `[]` goes by its properties, also one that JS iterates over and that is no mapping by its methods: `typedArray["length"]` is a `KeyError`, `arguments[0]` is the first, and `0 in arguments` asks whether one of them is 0. `dict(o)` of an object with a `keys()` of its own
  asks `o[k]` for each of those: a `KeyError` where one names what the object inherits. (A decision that can still be changed.)
- **A plain object**, one whose prototype is `Object.prototype` or `null`, has a type of its own, `_js.JSPlainObject`, which derives from `JSObject` and adds
  nothing to it but what it says to others: it is registered as a `collections.abc.MutableMapping` (when the interpreter starts, `registerInteropTypes`: the
  type has instances before any module is imported that could do it), a mapping pattern of `match` takes it, unless JS iterates over it (`TypeFlag::Mapping`, `plainObjectIsMapping`; of an object of JS's
  the pattern calls the `get(key, default)` of its type, which every mapping has, not `o.get`, which may be a property: `matchKeys`), and
  `json.dumps()` writes it as it writes a dict, wherever it is found (also a `Map`, and a `_js.JSProperties`). It is no `dict`: `isinstance(o, dict)` is false, and `o == {...}` goes by identity.
  So `json.dump()` and `JSONEncoder.iterencode()`, whose encoder is the library's own in Python and asks `isinstance(o, dict)`, take it only with
  `default=dict`. `typeOf()` tells by the type of the cell and one comparison of its prototype. (A decision that can still be changed.)
- **A mapping by its methods.** An object that is not plain, that JS iterates over and that has `get()`, `has()` and `keys()` is a mapping by those: a `Map`,
  `Headers`, `URLSearchParams`, `FormData`, `CookieMap`, a cache from a package (`itemsOf`, asked at each use). `o[k]` is `get(k)`, or a
  `KeyError` if not `has(k)`; `k in o` is `has(k)`; `o[k] = v` is `set(k, v)`; `del o[k]` is `delete(k)`; iteration is over `keys()`; `len()` is
  `size` or the number of keys; an empty one is false. So `dict(o)` and `**o`. `items()`, `pop()`, `popitem()`, `setdefault()` and `update()` are
  those of a mapping, made of the above. `o.get(k, default=None)` is Python's too, where the `get()` of JS's is written in C++ and declares one parameter, as that of a `Map`, of `Headers`
  and the like: it knows no default and ignores a second argument. Then `o.get` is no function of JS's but a method written in C++, as `{}.get` is, with the parameters of `collections.abc.Mapping.get`
  (`attributeFromProperty`, `givesWayToGetOfMappings`, `jsMappingGet`). Its `__self__` is the `get()` that was found, bound to `o`: that is what it calls, so `super().get(k, default)`, in a class with a `get()` of its own, calls the one behind that. JS's is asked, and where a default was given, it gives `undefined` or `null` and `has(k)` says
  no, the default is the answer. A `get()` written in JS says itself what its arguments mean (`get(key, options)` of a cache) and is left alone; so is one that declares
  no parameter or several (the functions of an addon declare none), and `Map.prototype.get`. To read `o.get` runs and raises nothing but what the read of the property does, and nothing is remembered: what the object is goes by what can be
  seen of its `Symbol.iterator`, `has` and `keys` without running code of the program's, so an object that has getters for those, or is a `Proxy`, keeps its `get()`. It goes by the name: the same function under another name is JS's.
  As that of a dict, it does not go by the `__getitem__` that a class of Python's derived from the object's may have. The default is for Python: what JS passes beyond the key is dropped, as its own `get()` ignores it, so
  `array.map(o.get)` is what it is in JS (`OptionalParametersAreNotForJS`). What else the object has itself comes first as ever:
  `o.keys()`, `o.values()` and `o.entries()` give iterators of JS. Its properties are attributes only. A key that is there twice
  (`URLSearchParams`) is iterated over twice, and `[]` gives what `get()` gives, the first.
  (A decision that can still be changed.)
- **A `Map`** is the mapping of JS whose keys can be anything, and they are compared as JS compares them: `1` and `"1"` are two keys, and so are two equal tuples. It is iterated
  over by its keys, as every mapping of Python's is, not by pairs as in JS: `for k, v in m.items()`, or `m.entries()`. It has a type of its own, `_js.JSMap`, which
  like that of a plain object adds nothing to `JSObject` but what it says to others: it is registered as a `MutableMapping`, and a mapping pattern of `match` takes it. (`typeOf()`
  tells by the type of the cell. `class Cache(js.Map)` derives from it.) Where a method is `Map`'s own there is no need to call it (`mapWithItsOwnMethods` looks the
  method up and sees what function it is); a class derived from `Map` with a `get()` of its own is asked. `json.dumps()` writes it as it writes a dict (`json.dump()` and `iterencode()`: as for a plain object, only with `default=dict`),
  under the rule for the keys of one: a `str`, or an `int`, `float`, `bool` or `None`, which are written as strings; any other is a `TypeError`, or left out with `skipkeys`.
  A `Set` is as it was: iteration over its members, `in`, `len()`. A `WeakMap` is a mapping that cannot be iterated over: `w[o]`, `w[o] = v`, `del w[o]` and `o in w` go by
  its `get()`, `set()`, `delete()` and `has()`, as those of a `Map`; there is no `len()`. `o in w` is `has(o)` for a `WeakSet`, which like a `Set` has none of the methods of a mapping. (A decision that can still be changed.)
- **What can be disposed of is a context manager.** `with o:` is `using`: it gives `o`, and calls `o[Symbol.dispose]()` at the end. `async with o:` is
  `await using`: it awaits `o[Symbol.asyncDispose]()`, or calls `o[Symbol.dispose]()` if there is no such method. JS tells a disposable nothing of the
  exception that leaves the block, and whatever the method returns the exception goes on.
  An object without these methods is a `TypeError`. It is the statement that finds them (`contextManagerMethodOfJS`), and `with()` of `bun:python`: the class of an
  object of JS cannot say whether the object can be disposed of, and what has an `__exit__` in its class is a context manager to all who ask
  (`ExitStack.push(f)` takes a function without one for a callback). So `hasattr(o, "__enter__")` is false, and `ExitStack.enter_context(o)`, which looks in
  the class, does not take it: `stack.callback(o[js.Symbol.dispose].bind(o))`. (A decision that can still be changed.)
  `vars(object)` and `object.__dict__` are the object itself, so `functools.wraps(f)` takes a function of JS for the wrapper. Where its items are not its
  properties (what JS iterates over, a `WeakMap`, a `WeakSet`, a `Proxy` of a container of Python's: `itemsAreProperties`) they are the mapping of its own properties, `_js.JSProperties`, see below. So they are where
  it would take a getter for `Symbol.iterator` to tell: to read `__dict__` runs no code of the program's. (A `Proxy` is asked, as every read of an attribute of one runs its traps. Whether the object is the `prototype` of a class of the host's takes the host's getter for `constructor`, written in C++, which only the main thread calls: `vars(js.Headers.prototype)` is the object there and the mapping of its properties in another thread.)
- A function that is read off an object is bound to it, so `f = o.method; f()` works as it does in Python. It is one bound function for
  each object and function, remembered weakly (`bindIfFunction`): `o.method is o.method`, and `emitter.off(name, o.method)` finds what
  `on()` was given. It is bound as `Function.prototype.bind()` binds it, so it has the `name` and `length` that gives (`bindToObject`): they are asked for at once of what
  may say something else later, a callable `Proxy` or a function that was given another name, and what a trap or a getter throws comes out of the read. (The `get()` of a mapping by its methods: see there.) Not bound: classes, what a module exports (read off its namespace), and what is read off the `prototype` of a
  constructor, which is how JS spells an unbound method (`js.String.prototype.toUpperCase.call("abc")`).
- A `Proxy` has an answer to every read. Where that is `undefined` it is asked whether it has the property (`has`): if not, `p.name` goes on to what every
  object of Python's has, or is an `AttributeError` (`readProperty`). For an item it is first asked whether it has the property itself (`getOwnPropertyDescriptor`): if not, `p[key]` is a `KeyError`.
- `weakref.ref()`, `proxy()`, `finalize()` and the weak containers take objects and functions of JS.
- A function written in JS has a signature for `inspect.signature()`, made of the text of its parameters (`__text_signature__`):
  `(a, b=None, /, *rest)`. They go by position; a default is `None`, since to pass `undefined` gives the default; a pattern has no name
  (`arg1`). An async function is a coroutine function to `inspect.iscoroutinefunction()`. `__module__` is `js`. There is no `__code__`.
- **`class Derived(Base)`, where `Base` is a class of JS** (`runtime/PyJSClass.h`): `class Lines(stream.Readable)`, `class Bus(EventEmitter)`,
  `class Cache(js.Map)`. `Base` stands among the bases for a class of Python's made for it (`JSFunction.__mro_entries__`,
  `typeForClassOfJS`), which derives from that of the class `Base` extends, and in the end from `JSObject`. An instance is an object of
  JS that `Base` itself has made, `Reflect.construct(Base, arguments, Derived)`: it has Base's fields, private names and internal
  state. Its prototype is `Derived.prototype`, behind which comes `Base.prototype`: by that Python knows its class
  (`Runtime::typeOf`, `typeByPrototype`), and JS finds what `Derived` defines, so the code of `Base` calls `this._read()`.
  `super().method()` goes on into `Base.prototype`; properties with setters work from both languages; static members of `Base` are
  attributes of `Derived`, with `Derived` for `this`, and to JS `Derived` inherits from `Base` (`Symbol.species`). JS can extend `Derived` in turn.
  The constructor of a class of JS makes the object, so it is `__new__` that takes its arguments. `Base.__init__()` does nothing, and
  objects if it is given other arguments than the constructor got: `def __init__(self, name): super().__init__()` would have
  constructed `Base` with the name. Such a class defines `__new__(cls, name)` and calls `super().__new__(cls)`.
  The attributes of an instance are its properties, and there is no other place for them: no class in the line adds a `__dict__`
  (`TypeFlag::AttributesAreProperties`, which `JSObject` has and all that derives from it inherits). An attribute is searched for as
  Python does it, with the properties where another instance has its `__dict__`: a `property()`, a slot or another data descriptor of
  a class of Python's, wherever in the MRO, comes first, `__class__` among them; then the instance's own property; then what the classes
  have, in the order of the MRO, where `Base` stands for its prototype (so a setter of `Base` is called, unless a class of Python's has
  something for the name that is a data descriptor or comes before `Base`). Classes of JS's that extend one another stand together,
  where the first stands. JS sees the classes that come before `Base`. What a class of Python's has does not pass through JS on the way:
  `whole = 1.0` in the class is a float, and a thread other than the main one can have it. `del` deletes an own property, and
  what is inherited is an `AttributeError`. `object.__getattribute__()`, `__setattr__()` and `__delattr__()` do the same, for any object
  of JS's, so a `__setattr__` of the class's own may hand on to `object`'s, and `@dataclass(frozen=True)` works.
  `instance.__dict__` and `vars(instance)` are the mapping of its own properties (`_js.JSProperties`, a `MutableMapping` and no dict,
  with the `copy()`, `|`, `reversed(mapping)` and `popitem()` from the end of a dict; the order is that of the properties, in which names that are array indices, `'2'`, `'10'`, come first and by value): not the object itself, which may be a `Map`, and whose
  `[key] = value` is the assignment of JS, which asks what the object inherits. The elements of a typed array are not in it. To store there
  defines the property, past whatever the class has for the name. It comes from the `__getattribute__` that the class inherits from
  `Base`; `object.__getattribute__(instance, '__dict__')` is an `AttributeError`, because who asks that way expects a dict
  (`inspect.getattr_static()`, and so `isinstance()` with a protocol). `__dict__` cannot be assigned or deleted, of any object of JS's: no
  other mapping can take the place of the properties. Nor can `__class__`: the prototype says what the object is an instance of.
  `copy.copy()` and `pickle` refuse such an instance. Of its `__dict__` they make the dict that it stands for (`jsPropertiesReduce`), so the `copy.deepcopy(self.__dict__)` of a
  `__deepcopy__()` works.
  `str(instance)` is what JS makes of it, by `toString()` or `[Symbol.toPrimitive]()`. Where it has no other than
  `Object.prototype.toString()`, which is to JS what `object.__str__()` is to Python, it is `repr(instance)`: a `__repr__()` of the class,
  or of `@dataclass`, serves `print()` too.
  An object is an instance of `Derived` by its prototype, if it is what the methods of `Derived` count on: a `Map`, a promise or a typed
  array where `Base` extends that. `Object.create(Derived.prototype)` is no `Uint8Array`, and the prototype of a class of Python's that
  derives from no class of JS's makes an object nothing but an object with that prototype: `Object.create(list.prototype)` is no list.
  The prototypes are made with the class, so `Derived.__bases__` cannot be assigned.
  One class of JS among the bases, and no built-in type of Python's beside it. Not `Array` (an array is a list), `Function`, nor the
  wrappers of primitive values. An `Error` of JS is no exception of Python's, so such a class cannot be raised.
- What JS forbids is refused as in strict code: to assign to or delete a property of a frozen object is a `TypeError` of JS.
- `dir()` lists the names of the properties, inherited ones too, once each; not `arguments` and `caller` of `Function.prototype`, which throw.
- `str()` is `String()`; of what JS can make no string of (an object without a prototype, the namespace of a module), `repr()`.
- `in` asks a `Set`, and to iterate over one is what it is in JS.
- An iterator that the engine makes (of arrays, maps, sets, strings, matches; iterator helpers) is an iterator: `next()`, and
  `__iter__` may return it. A generator is a generator: `send()`, `throw()`, `close()` (which is `return()`), `yield from`, so
  `contextlib.contextmanager` takes a `function*`. Any other object with a `next` method can be iterated over if it has
  `Symbol.iterator`.
- **A typed array is a sequence** of a fixed length (`_js.JSTypedArray`, which derives from `JSObject`; `runtime/PyJSBuffers.cpp`), besides
  exporting its memory: `a[-1]`, `a[1:3]` and `a[::-1]` (copies, made as `%TypedArray%.prototype.slice` makes them, so that of a
  `Buffer` is a `Buffer`), `a[1:3] = items` (as many as there were), `len(a)`, `x in a`, `index()`, `count()`, sequence patterns. An index, `len(a)`, `bool(a)`, `x in a` and iteration go by the memory and the length that the array has, and ask the object nothing: not for its `length`, nor for its `Symbol.iterator`. Nor does iteration go by the `__getitem__()` of a class of Python's that derives from one, as that of a list or a bytearray does not (`_js.typed_array_iterator`). (So one that is detached has no items, where JS throws. `len()` and `bool()` answer on any thread, as `memoryview(a)` does.) `==` is that
  of `memoryview(a)`, so `Uint8Array.of(1, 2) == b"\x01\x02"`, and like every sequence that can change it has no hash. A key that is
  neither an index nor a slice names a property. An int stored in a `BigInt64Array` is a BigInt. The class that stands for `Uint8Array`
  among the bases of `class Bytes(js.Uint8Array)` derives from it, and a slice of a `Bytes` is a `Bytes`.
- A function has `__name__` and `__qualname__`, which decorators copy. `copy.copy()` and `copy.deepcopy()` give the function itself.
- The working directory is one: `os.chdir()` and `os.fchdir()` tell the host (`setDidChangeDirectoryFunction`), which keeps its own
  copy of it for `process.cwd()` and for resolving modules.
- The environment is one. `os.environ` begins as what the host has (`HostEnvironment::read`; in Bun `process.env`, with what the program
  wrote there before Python started and with the variables of `.env` files), and from then on each hears of what changes in the other.
  `os.putenv()` and `os.unsetenv()`, which `os.environ` calls, tell the host (`didChange`; for another thread the main one does it, as soon as it
  has the VM again). A write to `process.env`, or a `delete`, changes the dict in which `os.environ` keeps the variables
  (`hostDidChangeEnvironment`). It does not change the environment of the process, with Python no more than without: `setenv()` is not safe while
  other threads call `getenv()`, and Bun has such threads. Python's own writes go on calling it, as in CPython (`TZ` and `time.tzset()`, `LANG` and
  `locale` need it). So the two can differ, and where they do is kept (`Threads::variablesOfHost()`: what the host has for a name, or that it has nothing; an entry goes when
  Python writes that variable, or the host what the process has). A program that Python starts or gives way to without being told its environment inherits that
  of the process in CPython. Here it is given that with the differences applied (`PosixChildEnvironment`), on whatever thread, without asking the host:
  `subprocess`, `os.system()`, `os.popen()`, `os.spawnv()` and its kin, `os.posix_spawn(..., None)`, `os.execv()` and its kin, `multiprocessing`. While there is
  no difference nothing is built, and `os.system()` is `system(3)`. `os.reload_environ()` gives the same. `time.tzset()` and `locale.setlocale(category, "")` tell
  the C library to read variables again: the process is first given what the host has for just those (`TZ`; `LC_ALL`, the six `LC_*` of POSIX, `LANG`), as if Python had
  written it (`posixGiveProcessVariablesOfHost`). What is left: whatever else calls `getenv()` does not see what JS wrote, as in Bun without Python.
  To JS names and values are text, with U+FFFD for what is not UTF-8. Such bytes stay what they are in
  `os.environb` until JS writes another text. A name that no environment can hold (`A=B`) stays in `process.env` alone.
  In a worker `process.env` is a copy, or what `new Worker(file, { env })` gave it, and `os.environ` is the same as that, and so is what the programs get that Python
  starts there. What JS writes there goes no further; what Python writes there also changes the environment of the process, as in a subinterpreter of
  CPython, and no other thread is told. (A decision that can still be changed.)

## JavaScript without Python

JSC and WTF work the same whether or not Python has started: there is no switch, for the process or for a VM, that Python flips.
What code outside `python/` asks is one of these, and the answer is the same in every program:

- **What is this cell?** `isPyObjectType()`, a range of `JSType`; `SpecPyObject` in the optimizing JITs. The operators, above.
- **Is this code block Python's?** `UnlinkedCodeBlock::isPython()`: it is never compiled, its call sites are never virtual, unwinding
  tells the Python runtime of each of its frames, its generators are laid out differently.
- **Which runtimes does this heap have?** `Heap::pythonRuntimes()`, empty without Python. They are asked at three points, none of
  which touches a value of JS: before the stack is scanned (dead temporaries in frames of Python code are cleared, and what a thread that is parked
  has no room for on its stack is scanned with it: `Threads::gatherConservativeRoots()`, which finds nothing unless more than four realms have a Python), when
  marking has converged (Python objects to finalize are kept), and by the host before `console.log` writes (Python's `stdout` is flushed).
- **Is this the embedder's thread?** `Heap::mayRunEmbedderFinalizers()`: false only while a thread that Python started has the VM.
  See Threads. A program without such threads runs on one thread, and the answer is yes.
- **Has this realm a Python?** `JSGlobalObject::pythonRuntime()`, a pointer that is null until Python starts there. Asked where an
  exception is thrown or caught (`throw`, `op_catch` in every tier) and when the process or a worker exits; the unwinder asks it of the
  VM (`Heap::pythonRuntimes()`), once for an unwind. What follows a yes is Python's own bookkeeping (tracebacks, what is being handled,
  exit handlers); nothing that JS can see depends on it.
- **Is a thread parked in the middle of a call?** `JSLock::hasParkedThreads()`: then code is running although the VM has no entry scope,
  and the texts that strings had before they were made atoms are kept (`Heap::clearConcurrentRetainedDataIfPossible()`): a
  `GCOwnedDataScope` on the stack of that thread may refer to one.
- **Does the embedder's thread wait where it polls no traps?** `JSLock::ifEmbedderThreadWaits()`, asked by whoever asks for what runs to be ended
  (`VMTraps::fireTrap()` of a termination or of a check of the watchdog, from any thread), beside the line that wakes a thread in `Atomics.wait()`.
  The thread says so itself: while it is parked, and while a call of Python's has it in the host's event loop. Only then is Python told. Without
  Python it never says so.

One change to the collector was made for Python and is everybody's gain. The helper threads that mark in parallel used to be woken at
the beginning of every collection and waited for at its end, which takes 0.4 ms and 0.9 ms of processor time with seven of
them, on an idle machine, where a collection of the young generation that finds little takes 0.03 ms by itself. Now they are started
when there is work for them (`Collector::runBeginPhase`, `SlotVisitor::donateKnownParallel`, `donateAll`): at once if the
collection expects to visit 16 MB or more; otherwise when what the conductor has marked by itself, plus eight times what it has on
its stack, comes to 131,072 cells (it marks 7,000 to 24,000 in the time that starting them costs); and whenever it leaves
marking to them to let the mutator run, which is after 0.3 ms at the least: 4,500 to 15,000 cells. Nothing in the protocol depends on how many run.
Termination is "no marker is active, this visitor and the shared stacks are empty", and a
helper that was never started is not active and owns nothing. What is in a shared stack is always taken by somebody: with the
world stopped the conductor ends in `drainFromShared(MainDrain)`, which steals for itself, and every way into the concurrent
phase goes through `donateAll()`. All visitors are set up in the Begin phase, started or not; the start happens once (an atomic
exchange), never in the End phase; tasks of the constraint solver are run by the conductor too and waited for by
reference count. (Programs compiled ahead of time already did this, and also create the threads one by one.) A loop in JS that
allocates 30 million objects of which 1000 are alive at a time went from 680 to 318 ms, and from 1001 to 348 ms of processor time.
A collection of the young generation that is over before the conductor would let the mutator run (4,000 cells in a list, 8,000 to 9,000
in arrays or a tree, 14,000 old cells that are looked at again) takes a tenth to a half of the time it took; a larger one takes what it
took, with two exceptions. One that finds 20,000 to 40,000 cells, a third of them in one array and the others hanging from those, takes
11 to 22 % longer: 7,000 to 13,000 cells on the stack do not start the helpers, so they come after 0.3 ms. A lower number cures that
(32,768: 0.97 to 1.05) and costs more than it gives: an array of 4,000 to 9,000 cells that have no descendants, or as many old cells to
look at again, then takes 0.55 to 0.8 ms in place of 0.13 to 0.35. And a collection that the program asks for and waits for (`edenGC()`)
and that finds 30,000 to 60,000 cells in a tree takes a quarter to a third longer, whatever the number: the stack of the visitor stays
small, the helpers come after 0.3 ms, the thread that waits marks no more, and the first helper to wake takes most of the tree.
Collections that come by themselves do not show it (a splay tree of 8,000 to 400,000 nodes: 0.99 to 1.03).

One thing that Python needs of WTF is a setting of the process, `Thread::setSurvivesFailedCreation()`: a program may lower its own
limit on threads (`resource.setrlimit`) and go on, and the collector then does without the threads it cannot start. Bun makes that setting
for every process, as it starts (`JSCInitialize`).

What every program pays, with the sizes of a release build for x86-64:

| Where | What |
| --- | --- |
| `VM` (one) | 128 bytes of 111,040: 112 in `Heap` (the list of runtimes, two more owners of finalizers, a subspace pointer, what a collection marked once all that is reachable was marked and how much of that waits indefinitely), 8 in `VMTraps`, 8 of padding |
| `JSLock` (one per VM) | 72 bytes: the list of parked threads, which of them is the embedder's, and how deep that one is in the host's event loop |
| `JSGlobalObject` | 8 bytes: the pointer to the runtime |
| `UnlinkedCodeBlock::RareData`, `UnlinkedFunctionExecutable::RareData` | 8 bytes each, where there is rare data; `FunctionExecutable::RareData` 16. `CodeBlock`, `UnlinkedCodeBlock`, `UnlinkedFunctionExecutable`, `CallLinkInfo`, `Structure`, `JSString`, `JSArray`, `JSFunction`: nothing |
| `JSType`, opcodes, `SpeculatedType`, `UseKind`, `VMTraps` | 19 of the 256 cell types, 42 of the 254 opcode numbers of one byte (17 are left; 17 more of Python's opcodes are on a second page), 1 of 64 bits, 1 of 64 kinds, 1 of 32 bits |
| `==` on two objects, DFG and FTL | nothing where the site has seen one kind of object that optimized code has a check for: plain objects (instances of classes are), arrays, functions, derived arrays, `Map`, `Set`, `WeakMap`, `WeakSet`, `Date`, `RegExp`, `Promise`, `Proxy`, `DataView`, `String` objects. Otherwise (errors, typed arrays, several kinds, an object or `null`), for each operand not known to be something else: load, subtract, compare, branch |
| `==`, `<`, `+`, `-`... in the slow paths | the same test, put where JS is about to convert an object to a primitive. `==`: for an operand that is an object, and for `a == a` before the answer. `a - b`, `*`, `/`, `%`, `**`, `<<`, `>>`, `&`, `\|`, `^`, `~`, `++` and `--`: inside `JSValue::toNumeric()` and `toBigIntOrInt32()`, for an operand that is an object or a symbol; numbers, BigInts, strings, booleans, `undefined` and `null` never get to it. `+` and `-a`: for an operand that is an object. `<`, `<=`: for an operand that is an object or a symbol. (What is no object is no longer handed to `JSCell::toPrimitive()` to get it back, and an object goes straight to `JSObject::toPrimitive()`: these paths are shorter than they were before the branch.) `b = a++`: the same in `op_to_numeric`, which leaves a Python object as it is for the `++` that follows. `a = a++` on a local, where JSC left the `++` out because nothing could see it, has it: a move and an `op_inc` on a temporary. (`>>>` is not overloaded.) |
| `Bun.deepEquals()`, `expect().toEqual()` | the same test for each pair of values compared |
| Untyped arithmetic, DFG and FTL | the result is known to be a primitive only if the operands are known not to be Python objects |
| `+x`, `x * 1`, `1 * x` | one byte more in each `op_to_number` (2 and 4 in the wide forms): what it was written as. A number: nothing. Anything else: in the slow paths a load and a compare, and for `x * 1` the test of the type. The result of `x * 1` is not known to be a number while bytecode is generated, so `x * 1 + y` checks that in Baseline; to the DFG and FTL it is one unless `x` may be a Python object |
| A class definition with `extends` | a move, and a test of the type of the superclass; unless that is a `JSFunction` (so for `Map`, `Error`, `Promise`, a proxy, `null`), a call of a function that finds it is not Python's (175 instructions, once for each time the definition is evaluated). 29 bytes of bytecode, the metadata of an `op_call` and two constants |
| A `catch` clause | 16 bytes in the rare data of the unlinked code block, in a `FixedVector`: where the clause begins and ends, which the table of handlers does not say (`PythonMark::CatchOfJS`) |
| A metadata table, unlinked or linked | 12 bytes: six more kinds of instruction have metadata (24 where the offsets are of 32 bits). A cached code block with rare data: 8 bytes |
| `ArrayBuffer.prototype.resize()` | refuses to shrink a buffer that is pinned, as it refuses to detach one |
| A block or large allocation is swept | one load and branch (`mayRunEmbedderFinalizers()`) |
| A word on a stack, or a callee that the sampling profiler has sampled, points to a large allocation or to one of the first few cells of a class | is that alive (`PreciseAllocation::isLive()`): two loads and branches at most |
| The incremental sweeper runs | one load and branch (`hasParkedThreads()`) |
| A call is linked, or is virtual and not to compiled JS | is the caller Python code: three loads and a branch |
| An exception is thrown | whether there is a Python in the VM is asked once for an unwind (`UnwindFunctor::m_tellsPython`: a load and a compare) and the frame it starts in is noted (`m_frameToldOfLast`: a store), and the answer is looked at twice for each frame the exception passes. All that is done on Python's account is out of line behind it. `throw` and `catch`: has the realm a Python, a load and a branch each |
| Bytecode is generated, a code block is linked or asks to be compiled | a look at `isPython()`: for each loop and jump, each call site, each code block |
| `Instruction::opcodeID()`, `size()` in C++ | nothing: `op_wide16`, `op_wide32`, `op_py_instrumented` and `op_page2` are the last four ids of one byte, and a narrow instruction is known by one compare, where upstream has two. The interpreter itself does not look |
| The interpreter's three tables of handlers | 578 entries each for 338 ids, so that the byte behind `op_page2` is its own bound, as an opcode is; in the 16 KB that are set aside and protected for them anyway |
| A collection | three walks over an empty list; five more kinds of metadata looked for in each code block that is visited; the visitors' counts are added up when marking has converged, not only at the end |
| The mutator leaves a collection to the collector's thread | a lock, and a look whether that thread is there |
| No thread can be started | helpers of the collector are done without, and so is its thread; for any other thread, up to 2 s of waiting for room, a line that names it, and the crash that used to come at once |
| The lock of the VM is taken, released | a store; two loads and branches |
| What runs is to be ended (`worker.terminate()`, the timeout of `node:vm`, the watchdog) | a call that takes a lock that nobody else has, to find that nobody waits |
| A tick of the event loop that waits, a turn of it | a call that finds no other threads; a load of a flag |
| The executable | about 10 MB of 134: 4.1 the standard library (compressed), the rest code |

## The command line

`runtime/PyCommandLine.h` has the configuration of the interpreter (`Configuration`, what CPython calls `PyConfig`: one for the
process, filled in by the host before Python starts) and CPython's command line: `readCommandLine()` parses the options and the
`PYTHON*` variables before there is a VM, `runCommandLine()` runs the script, `-c`, `-m`, a directory or archive with a
`__main__.py`, standard input or the interactive interpreter, with CPython's `sys.argv`, `sys.path[0]`, messages and exit statuses.
Arguments, names of files and the values of variables are bytes to the system and `str` to Python by way of `surrogateescape`
(`decodeFilesystem`), so none is lost for not being UTF-8. When a script or standard input ends, with an exception or without, the standard
streams are flushed before the traceback is written; after `-c`, `-m` and a directory the traceback comes first, and what is left in
`sys.stdout` when the interpreter ends. It shows where both go to one file.

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

Importing `readline` is what gives `input()` and the basic loop line editing and a history, as in CPython. The runtime has no such
module. It looks for one among the imported modules when a line is to be read from the terminal, and calls its `_read_line(prompt)`
(`readLineWithEditor()`): a str, `NotImplemented` for "read it yourself", `EOFError`, `KeyboardInterrupt`. The host brings the module.

Without it, `input()` on a terminal (`sys.stdin.fileno()` is 0, `sys.stdout.fileno()` is 1, both are terminals) flushes `sys.stdout` and writes the prompt
to descriptor 2 itself, in the encoding of `sys.stdout`, as CPython does. What `flush()` raises is dropped, there as here, and a stream looks at
signals after it has written: a prompt that went through it would take a Ctrl-C with it.

## Writing a module in C++

Tables of `NativeDef` (`PY_FUNCTION`, `PY_METHOD`, `PY_SLOT`, ...) with signatures in the form of `__text_signature__`; the
argument parser is generated from the signature at compile time, and so is what it takes to intern the name: which of the names
in `PyNames.h` it is, or the hash of its text (`KnownOfName`, `Runtime::nameIn()`). `TypeDef` describes a type; `Runtime::typeFor(def)` makes it on
first use. A module is `void initializeXModule(JSGlobalObject*, PyModule*)` listed in `PyNativeModules.cpp`.

### Every call that can raise is looked at

JSC can verify that no exception is overlooked (`BUN_JSC_validateExceptionChecks=1`, in builds with assertions; Bun's CI sets it). A
function that can throw declares a `ThrowScope`. When that goes, the VM notes that there *may* be an exception now, whether there is one
or not, and the next scope that is made or goes aborts unless somebody has asked `scope.exception()` in between. So a path on
which a check is missing is found the first time it runs, not the first time something fails on it. The runtime passes, and the wrappers
that run the tests on a debug build have the option on. What that takes:

- After a call that can raise: `RETURN_IF_EXCEPTION(scope, { })`.
- Many functions here say in what they return that they have raised: false, null, an empty value. `if (failed(scope, f(...))) return { };`
  (`PyErrors.h`) tests the result, tells the verification, and asserts that result and exception go together. Only for a function that
  returns such a value *exactly* when it has raised. Where empty also means "there is none" (`getAttrIfPresent`, `dict->get`,
  `next`): `RETURN_IF_EXCEPTION`, then the test. For an outcome with more than two values:
  `EXCEPTION_ASSERT(!!scope.exception() == (outcome == Outcome::Raised))`.
- The last call of a function: `RELEASE_AND_RETURN(scope, f(...))`, or `scope.release()` before a call that returns nothing. Nothing that
  can raise comes after a release, and only one such call is inside it: `RELEASE_AND_RETURN(scope, g(h()))` with two that can
  raise is a missing check between them.
- `return raiseTypeError(globalObject, scope, ...)` needs nothing: the functions that raise take the scope.
- A function without a scope of its own that only hands on what it got (`return f(...)`, `if (!f(...)) return false;`) needs none.
  As soon as it makes two calls that can raise it needs one.
- A loop checks in every round: `listAppend` twice in a row without a check is an error, also when it cannot fail in practice.
- A function that never leaves an exception (it reports it, stores it, or there can be none): `DECLARE_TOP_EXCEPTION_SCOPE`. That one
  verifies at both ends and notes nothing when it goes, so its callers need no check.
- A release does not spare the caller the check: a `ThrowScope` that goes notes that there may be an exception, released or not. A
  function whose callers go on without a look after one of its results (`tryOverloadedOperator` that returns false,
  `bytesOfPythonMemory` that returns the bytes), or a block in the middle of a function that goes on, has a `TopExceptionScope`
  too: `if (scope.exception()) return ...;` with the result after which the callers do look. The exception stays.
- **Callbacks of C libraries** (expat, SQLite, BoringSSL, `qsort` through ctypes): the library does not look for exceptions, and calls again.
  Each callback has a `TopExceptionScope`, leaves the exception pending or puts it aside, sets a flag that makes the library stop, and does
  nothing if it is entered with the flag set. Whoever called the library looks at the flag.
- To put an exception aside and raise it again later: `takePendingException()`, `throwPythonException()`. It leaves what ends the program
  (a termination) where it is and returns empty: `RETURN_IF_EXCEPTION` after it. To raise again in a function with a
  `TopExceptionScope`: an inner block with a `ThrowScope`, then `EXCEPTION_ASSERT_UNUSED(scope, scope.exception() == pending)`.
- Code that runs while an exception is on its way (trace functions, `__del__` during unwinding): `ExceptionSetAside` (`PyErrors.h`),
  which also sets aside that somebody has yet to look, and drops what is pending when it goes. `SuspendExceptionScope` itself puts
  back what it saved over whatever is pending by then: only where nothing of the program's runs meanwhile. What the unwinder
  itself calls has no `ThrowScope`.
- **Terminations.** The host ends what runs here (`worker.terminate()`, a timeout or SIGINT of `node:vm`) with an exception that
  nothing catches. Any `RETURN_IF_EXCEPTION` can throw it, because it handles traps, and it is not for the runtime to call off.
  - Where an exception is dropped because it is of no consequence what went wrong: `dropPendingException()`, not
    `clearException()`. It asks for a termination again, and the next safe point throws it. Where a caller can stop instead, the
    termination is left pending: `tryClearException()`.
  - `reportUnraisable()`, `writeToStderr()` and `printException()` leave a termination pending. Whoever calls them in a loop stops
    there, and leaves what it has not done to be done another time: the host may withdraw the termination, and then the program goes on.
  - What cannot fail, and says so, and runs nothing of the program's: `DeferTerminationForAWhile`. After the exception has been set
    aside, not before.
  - Code of the program that is called where terminations are deferred (by the unwinder, while the interpreter starts):
    `TerminationNotDeferred`, or it cannot be ended.
  - A stream or a generator that nothing refers to any more is closed at once where the runtime can tell. Not while the host is
    ending what runs here (`vm.hasPendingTermination()`): the collector finds it, if the program goes on. So it does a stream whose
    finalizer the host has ended, which is registered again (a file, a buffered or a text stream; an instance of a class that the
    program derives from `io.RawIOBase` and the like is finalized once, as any instance is). A `close()` that the host ends in the
    `close()` of what the stream wraps gives nothing up, so that it is whole when it is closed again.
  - What the runtime has taken apart for as long as it calls code of the program goes back together whatever ends the call (the items
    of a list that is being sorted): `ExceptionSetAside`, then `DeferTerminationForAWhile`.
  - What has to be given back (a descriptor) belongs to its object before the next `RETURN_IF_EXCEPTION`, not after it.
  - No code of a frame that a termination leaves runs any more, not even the compiler's own. What that would have put back, the unwinder
    does: the exception that was being handled (`Runtime::handlersAreLeftByTermination()`).
  - A thread that waits polls nothing. Out of a system call the engine's signal gets the thread that is in the VM, and what follows the
    call has a look (`checkSignals()`: `Threads::throwIfHostEndsWhatRuns()`). With a second thread of Python's alive the main thread
    is not in the VM while it waits: it has parked. And in the host's event loop, which turns while it waits for a descriptor, it goes back to sleep.
    So the thread says where it is, and whoever fires the trap asks (`VMTraps::fireTrap()`, beside the line that wakes `Atomics.wait()`;
    `JSLock::ifEmbedderThreadWaits()`, from any thread). If it waits, Python is told (`mainThreadIsToHandleTrap()`): the loop is woken if the thread
    is there, and a main thread that is parked in the middle of calls is woken from what it sleeps on and got out of its system call as one that owes
    calls is (`Threads::interrupt()`). Whatever the call: what a signal makes it lose, it loses to code that is ended when it returns. (A foreign
    function goes on after a system call of its own has failed with `EINTR`, and is ended when it returns.) (For the watchdog, which may find that it is not time yet, only where nothing is lost. For a termination not
    while the thread defers them: it would only make the call again.) The thread looks at the traps itself once it has parked, or has said that it
    waits in the loop (`waitForEvents()`), so whoever fires either finds it or has fired before it looks: `JSLock::ifEmbedderThreadWaits()`, whose lock
    orders the two.

To find what is missing without dying at the first: `BUN_JSC_uncheckedExceptionsLog=FILE` with the option above writes each missing
check to the file, once, with both stacks, and goes on; `Tools/Scripts/unchecked-exceptions FILE` puts them in order by the function
to change.

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
- **Dicts that become keyed early.** In a build with assertions `-X dict_keyed_after_steps=N` and `-X dict_keyed_from_log2_size=N` lower the
  two limits for all dicts of the process, those of the interpreter included (0 and 3: nearly every dict is keyed at once). Any test can be
  run that way; other builds, and CPython, take no notice of the options. `differential/mod_hash/dict_modes_early.py` does it to the scripts
  on dicts there.
- **Which keys a dict asks.** `differential/mod_hash/dict_calls_of_keys.py` runs random programs on dicts whose keys log every call of their
  `__eq__` and `__hash__`, and prints digests of the logs. Whoever touches a search of `PyDict` or `PySet`, how a table grows or what is
  copied, a fast path in the interpreter included, runs it with larger numbers against CPython (its header says how): `one 0 20000 40` and
  `two 0 10000 60` take a minute.
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
