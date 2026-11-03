# Work that has not landed

`generators-in-the-interpreter.patch`: generators and coroutines resumed without leaving the interpreter (see "Generators without
leaving the interpreter" in ../DEFERRED.md), against this tree as of the commit that added this file, or a little before. Tested
in release builds only. It also holds two later changes that have never been built: smaller C++ frames under `call()`, and `with`
without bound methods. To be deleted when it lands.

`ordereddict-tuple-hash-signatures.patch`: a native `collections.OrderedDict`, tuples that cache their hash, signatures of native
functions, and more places at which buffered output is written out. Syntax-checked only: never built, never run.
