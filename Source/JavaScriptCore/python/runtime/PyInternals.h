/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSLock.h"
#include "PyAbstract.h"
#include "PyDescriptors.h"
#include "PyUnicode.h"
#include <wtf/FastBitVector.h>
#include <wtf/HashSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/text/StringBuilder.h>

namespace JSC {
namespace Python {

// What one file of the runtime needs of another and nothing outside it does.

class ParseTree;
class ParserHost;
class PyBytes;
class PyFrame;
class PyGenerator;
class PyModule;
class PySlice;
struct SyntaxErrorInfo;

// ---- object and type (PyTypeObject.cpp) ----

// The C++ functions behind these methods, by which the interpreter knows a type has not replaced them.
JSValue objectGetAttribute(JSGlobalObject*, JSValue self, JSValue name);
JSValue objectSetAttr(JSGlobalObject*, JSValue self, JSValue name, JSValue value);
JSValue objectDelAttr(JSGlobalObject*, JSValue self, JSValue name);
JSValue objectNew(JSGlobalObject*, JSValue cls, const CallArgs&);
JSValue objectInit(JSGlobalObject*, JSValue self, const CallArgs&);
JSValue objectRepr(JSGlobalObject*, JSValue self);
JSValue objectStr(JSGlobalObject*, JSValue self);
JSValue objectHash(JSGlobalObject*, JSValue self);
int64_t identityHash(JSGlobalObject*, JSValue self); // The same, not yet an int.
JSValue objectEq(JSGlobalObject*, JSValue self, JSValue other);
JSValue typeGetAttribute(JSGlobalObject*, JSValue self, JSValue name);
// For pickle and copy (PyReduce.cpp).
JSValue objectReduce(JSGlobalObject*, JSValue self);
JSValue objectReduceEx(JSGlobalObject*, JSValue self, JSValue protocol);
JSValue objectGetState(JSGlobalObject*, JSValue self);
// The __dict__ attribute of the instances of a built-in type that have one.
// CPython's text for "module NAME", "type TYPE", "TYPE.NAME" or "MODULE.NAME"; None if it has none. (tools/make_docstrings.py)
JSValue docstringFor(JSGlobalObject*, const String& key);
std::span<const NativeGetSet> dictGetSet();
std::span<const NativeGetSet> weakrefGetSet();
// __eq__ and so on, as they are for a type that has nothing to say: NotImplemented, and for __ne__ the opposite of __eq__.
std::span<const NativeDef> defaultComparisons();
// What object does about attributes, equality and hashing.
std::span<const NativeDef> repeatableSlotsOfObject();
// "<method 'append' of 'list' objects>"; what belongs to object is "<method '__reduce__'>".
String descriptorRepr(JSGlobalObject*, ASCIILiteral kind, StringView name, PyType* owner);

// type.__getattribute__. With `suppress`, a missing attribute is an empty value and no exception.
JSValue typeGetAttr(JSGlobalObject*, PyType*, JSString* name, bool suppress = false);
// type.__call__: makes an instance. With `derived`, a class derived from the type: an instance of that, made and initialized by the
// __new__ and the __init__ that are found in its MRO from the type on (Reflect.construct(type, arguments, derived)) or, with
// `afterType`, behind the type (`super(...)` in the constructor of the type, which is written in JS).
JSValue typeCall(JSGlobalObject*, PyType*, const CallArgs&, PyType* derived = nullptr, bool afterType = false);
// An instance of the type with nothing set: what object.__new__ makes, given how the type's instances are laid out.
// `layout`: the built-in type that decides the layout, if not object. (One whose instances differ from object's in CPython only.)
JSValue allocateInstance(JSGlobalObject*, PyType*, PyType* layout = nullptr);
// For whoever has called the __new__ of a built-in type, or done what it does, to make an instance of the type: if the type has a
// __del__, the runtime is told of the object. Once for an object.
void didMakeObject(JSGlobalObject*, PyType*, JSValue object);
// For PyGenerator::resume().
JSValue callBodyOfGenerator(JSGlobalObject*, PyFunction* body, std::span<const JSValue> arguments);
// What gc.collect() does: collects, runs the finalizers of what was found, and so on until there is nothing left to do. Not from
// a finalizer. Throws nothing but the host's termination.
// Of the young generation only (what has been allocated since the last collection, whoever made that): once. What is kept for its
// finalizer is old afterwards, so that it takes a full collection to find that it is still dead, to clear the weak references
// to it and to free it.
void collectAndFinalize(JSGlobalObject*, Runtime::MarkingObserver* = nullptr, CollectionScope = CollectionScope::Full, bool leavesCyclicGarbageForLater = false, Runtime::CallsOf = Runtime::CallsOf::AllThreads);
// In CPython a file that is dropped is closed at once, and programs count on it: open(path, "w").write(text), and whoever opens the
// file next finds the text. Here it would be closed after some collection. So the streams over files that hold output are kept in
// a list, and what they hold is written before the program, or anyone it starts, can look at a file, and before it does anything
// to a file or a descriptor that would come out differently afterwards (truncating, closing, writing). (When a buffer is written
// is up to the implementation, so this is never wrong.) Not sys.stdout and sys.stderr, which nobody drops. Does not raise.
void writeOutPendingOutputSlow(JSGlobalObject*);
ALWAYS_INLINE void writeOutPendingOutput(JSGlobalObject* globalObject)
{
    if (runtime(globalObject).mayHavePendingOutput()) [[unlikely]]
        writeOutPendingOutputSlow(globalObject);
}
// See PyFinalization.cpp.
#if ENABLE(C_LOOP)
void wipeStackBelow(JSGlobalObject*, size_t = 128 * KB);
#else
extern "C" void SYSV_ABI llint_python_wipe_stack_below(JSGlobalObject*, size_t); // LowLevelInterpreter64.asm
inline void wipeStackBelow(JSGlobalObject* globalObject, size_t size = 128 * KB) { llint_python_wipe_stack_below(globalObject, size); }
#endif
// Room for the frames from a native function down to where it sleeps.
#if ASAN_ENABLED || ASSERT_ENABLED
constexpr size_t stackBytesWipedForWaiting = 64 * KB;
#else
constexpr size_t stackBytesWipedForWaiting = 16 * KB;
#endif
// Room for the frames from makeCallsOwed() down to the finalizer or the handler of a signal that it calls.
#if ASAN_ENABLED || ASSERT_ENABLED
constexpr size_t stackBytesWipedForCallsOwed = 64 * KB;
#else
constexpr size_t stackBytesWipedForCallsOwed = 8 * KB;
#endif
// If all that callee(...) does is self = object.__new__(callee), then callee.__init__(self, ...), and that is a function written
// in Python: the function, and a new `self`. Otherwise the empty value.
JSValue prepareConstruct(JSGlobalObject*, JSValue callee, JSValue& self);
// __build_class__(function, name, *bases, **keywords)
JSValue buildClass(JSGlobalObject*, const CallArgs&);
// The same for a body that is not a function of Python's: it is given the namespace to fill, and returns None, or nothing if it raises.
// Of the arguments only the keywords count.
JSValue buildClass(JSGlobalObject*, JSValue name, PyTuple* bases, const CallArgs& keywords, const ScopedLambda<JSValue(JSValue ns)>& body);
// The most derived of the metaclass given and those of the bases. Null: an exception.
PyType* calculateMetaclass(JSGlobalObject*, PyType* metatype, PyTuple* bases);
// "module.qualname", or the qualname alone for a built-in.
String qualifiedTypeName(JSGlobalObject*, PyType*);
// The same, without the module for a class of __main__ either: what messages call a type (%T in CPython's formats).
String typeNameForMessage(JSGlobalObject*, PyType*);
// A number that tells the object from all others alive: id().
JSValue identityOf(JSGlobalObject*, JSValue);
String addressForRepr(JSGlobalObject*, JSValue);

bool isInstanceWithoutHook(JSGlobalObject*, JSValue object, JSValue cls);
bool isSubclassWithoutHook(JSGlobalObject*, JSValue derived, JSValue cls);

// The __class_getitem__ of the built-in containers.
JSValue classGetItem(JSGlobalObject*, JSValue cls, JSValue item);
// list[int]
JSValue makeGenericAlias(JSGlobalObject*, JSValue origin, JSValue arguments);
// int | str
JSValue makeUnion(JSGlobalObject*, JSValue left, JSValue right);

// What a type written in C++ keeps with an instance that has no field for it (a dict subclass, say): a property of the
// object that JS cannot see. `index` is less than Runtime::numberOfHiddenKeys - 1 (the last is taken: see PyWeakRef.cpp); each
// type decides what its indices mean, and a type must not use those of its bases. Empty: not set.
inline JSValue hiddenField(JSGlobalObject* globalObject, JSValue object, unsigned index)
{
    return asObject(object)->getDirect(getVM(globalObject), runtime(globalObject).hiddenKey(index));
}
inline void setHiddenField(JSGlobalObject* globalObject, JSValue object, unsigned index, JSValue value)
{
    asObject(object)->putDirect(getVM(globalObject), runtime(globalObject).hiddenKey(index), value, PropertyAttribute::DontEnum | 0);
}

// The hidden field of a tuple of keyword names that has the keys of the dict they came from, if some are not plain strs.
static constexpr unsigned keywordKeysField = 0;

// The members of int | str. Null: the value is not a union.
PyTuple* unionArguments(JSGlobalObject*, JSValue);
// object.__getstate__(), as copy and pickle call it: one the class has overridden counts.
JSValue getObjectState(JSGlobalObject*, JSValue);

// ---- Descriptors ----

JSValue memberDescriptorGet(JSGlobalObject*, PyMemberDescriptor*, JSValue object);
void memberDescriptorSet(JSGlobalObject*, PyMemberDescriptor*, JSValue object, JSValue value);
// For super() without arguments: the class being defined and the first argument of the function that is running.
bool findSuperArguments(JSGlobalObject*, JSValue& type, JSValue& instance);

// ---- Arguments ----

// The values in a buffer the collector knows of. They move when the buffer grows.
inline JSValue* valuesOf(MarkedArgumentBuffer& buffer) { return reinterpret_cast<JSValue*>(buffer.data()); }
inline std::span<const JSValue> spanOf(MarkedArgumentBuffer& buffer) { return { valuesOf(buffer), buffer.size() }; }

// Apart, so that the frame of whoever uses withFirstArgument() has no room for a buffer that it seldom writes: see callJSFunction().
template<typename Function>
NEVER_INLINE JSValue withFirstOfManyArguments(JSValue first, const CallArgs& args, const Function& function)
{
    unsigned total = args.totalCount() + 1;
    MarkedArgumentBuffer buffer;
    buffer.ensureCapacity(total);
    buffer.append(first);
    for (unsigned i = 1; i < total; ++i)
        buffer.append(args.values()[i - 1]);
    return function(CallArgs(reinterpret_cast<const JSValue*>(ArgList(buffer).data()), args.size() + 1, args.keywordNames()));
}

// function(CallArgs) with `first` before the positional arguments.
template<typename Function>
ALWAYS_INLINE JSValue withFirstArgument(JSValue first, const CallArgs& args, const Function& function)
{
    unsigned total = args.totalCount() + 1;
    static constexpr unsigned inlineCapacity = 12;
    if (total > inlineCapacity) [[unlikely]]
        return withFirstOfManyArguments(first, args, function);
    JSValue buffer[inlineCapacity];
    buffer[0] = first;
    for (unsigned i = 1; i < total; ++i)
        buffer[i] = args.values()[i - 1];
    return function(CallArgs(buffer, args.size() + 1, args.keywordNames()));
}

// The name as a str that is not a rope and is the one of its kind. Null: a TypeError, "attribute name must be string, not 'x'".
JSString* attributeName(JSGlobalObject*, JSValue);
// False: a TypeError, "f() takes no keyword arguments".
// As VM::isSafeToRecurseSoft(), that much earlier: for a function that recurses by way of others that make the check themselves,
// and that has something more precise to say than they have.
inline bool isSafeToRecurseWithMargin(VM& vm, size_t margin)
{
    return std::bit_cast<uintptr_t>(currentStackPointer()) >= std::bit_cast<uintptr_t>(vm.softStackLimit()) + margin;
}

bool rejectKeywords(JSGlobalObject*, ASCIILiteral function, const CallArgs&);
// For the __new__ and the __init__ of a built-in type `base` that takes no keyword arguments. A class derived from it may be
// called with some if the other of the two methods is its own, and so takes them.
bool rejectKeywordsInNew(JSGlobalObject*, ASCIILiteral function, PyType*, PyType* base, const CallArgs&);
bool rejectKeywordsInInit(JSGlobalObject*, ASCIILiteral function, JSValue self, PyType* base, const CallArgs&);
// False: a TypeError, "f expected at most 2 arguments, got 3" and the like.
bool checkPositional(JSGlobalObject*, ASCIILiteral function, const CallArgs&, unsigned min, unsigned max);

// The keyword arguments of a function that takes any number of positional ones: values[i] is set for names[i] if it is given.
// False: a keyword that is none of the names, a TypeError.
bool parseKeywordArguments(JSGlobalObject*, ASCIILiteral function, const CallArgs&, std::span<const ASCIILiteral> names, JSValue* values);

// The arguments of a function written in C++ whose messages name it otherwise than its definition does. `keywords` names the
// parameters; the first maxPositional of them may be given by position. With hasVarPositional, more positional arguments
// than that are the caller's business. No parameter is positional-only, and no keyword-only one is required.
bool unpackKeywordArguments(JSGlobalObject*, ASCIILiteral functionName, std::span<const ASCIILiteral> keywords, unsigned minPositional, unsigned maxPositional, bool hasVarPositional, const CallArgs&, JSValue* values);

// ---- Numbers ----

// The __format__ of int, float, complex and str. `self` may be an instance of a subclass.
JSString* formatInt(JSGlobalObject*, JSValue self, JSString* spec);
JSString* formatFloat(JSGlobalObject*, JSValue self, JSString* spec);
JSString* formatComplex(JSGlobalObject*, JSValue self, JSString* spec);
JSString* formatStr(JSGlobalObject*, JSValue self, JSString* spec);
// For an argument that CPython takes as a C string. True: there is a null character in it, and a ValueError has been raised.
bool hasEmbeddedNull(JSGlobalObject*, JSString*);
// The module _string.
JSValue formatterParser(JSGlobalObject*, JSValue formatString);
JSValue formatterFieldNameSplit(JSGlobalObject*, JSValue fieldName);
JSValue intFromByteSpan(JSGlobalObject*, std::span<const uint8_t>, bool littleEndian, bool isSigned);
// int(text, base). Empty with no exception: it is not the text of an int.
JSValue intFromText(JSGlobalObject*, StringView, int base);
// float(text). nullopt: it is not the text of a float.
std::optional<double> floatFromText(StringView);
// How many characters at the start are the text of a float. 0: none.
size_t scanDouble(std::span<const Latin1Character>, double& value);
// ASCII, with the whitespace at the ends gone, any decimal digit as an ASCII one, and the underscores between digits gone.
// False: there is something no number has.
bool normalizeNumberText(StringView, Vector<Latin1Character, 64>&);
// (a + bj) ** (c + dj)
JSValue complexPower(JSGlobalObject*, double baseReal, double baseImaginary, double exponentReal, double exponentImaginary);
void initializeComplexType(JSGlobalObject*);
// The operations of float, which complain of a zero divisor.
JSValue floatBinaryOperation(JSGlobalObject*, BinaryOperator, double, double);
// 0-9, or -1: any decimal digit Unicode knows.
int decimalDigitValue(char32_t);
// 0-35, or 36 if it is neither an ASCII digit nor an ASCII letter.
inline unsigned alphanumericDigitValue(char32_t character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'z')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'Z')
        return character - 'A' + 10;
    return 36;
}

// ---- Text ----

inline bool isPythonWhitespace(char32_t character) { return character < 0x80 ? character == ' ' || (character >= 0x09 && character <= 0x0d) || (character >= 0x1c && character <= 0x1f) : isWhitespace(character); }
// format % arguments
JSValue strFormatPercent(JSGlobalObject*, JSString* format, JSValue arguments);
JSValue bytesFormatPercent(JSGlobalObject*, std::span<const uint8_t> format, JSValue arguments, bool makeByteArray);
// str.format, str.format_map
JSValue strFormatMethod(JSGlobalObject*, JSValue self, const CallArgs&);
JSValue strFormatMap(JSGlobalObject*, JSValue self, JSValue mapping);
// str.join
JSString* strJoin(JSGlobalObject*, JSString* separator, JSValue iterable);
// str.encode, bytes.decode. A null encoding or handler is the default.
JSValue strEncode(JSGlobalObject*, JSString*, JSString* encoding, JSString* errors);
JSValue bytesDecode(JSGlobalObject*, JSValue bytesLike, JSString* encoding, JSString* errors);

// bytes(object), for anything but a str or an int.
JSValue bytesFromObject(JSGlobalObject*, JSValue);

// ---- Containers ----

// The index for a sequence of that length, from an index that may be negative. False: an IndexError with the message.
bool normalizeIndex(JSGlobalObject*, JSValue index, int64_t length, ASCIILiteral message, int64_t& result);
// Whether the type has a table of the methods of sequences in CPython (tp_as_sequence), be it an empty one, on which some messages and
// the order of some checks depend: every class written in Python has, and every built-in type that has a length, items or `in`.
bool hasTableOfSequenceMethods(PyType*);
// As the start and end arguments of str.find() and list.index(): None or absent is the default, and what is out of range is clipped.
bool sliceBounds(JSGlobalObject*, JSValue start, JSValue end, int64_t length, int64_t& from, int64_t& to);
// The same in two steps, for a sequence that can change: the first may call __index__ methods, the second is for the length after that.
bool unpackSliceBounds(JSGlobalObject*, JSValue start, JSValue end, int64_t& from, int64_t& to);
void adjustSliceBounds(int64_t length, int64_t& from, int64_t& to);
// dict.update(other), for `other` a mapping or an iterable of pairs.
void dictUpdate(JSGlobalObject*, PyDict*, JSValue other);
// The same for what must be a mapping: other.keys() is called whether it is there or not.
void dictUpdateFromMapping(JSGlobalObject*, PyDict*, JSValue mapping);
// {**a, **b} in a call: a key twice over is a TypeError that names the function.
void dictMergeForCall(JSGlobalObject*, PyDict*, JSValue mapping, JSValue function);
// dict[key], with __missing__ for a subclass.
JSValue dictSubscript(JSGlobalObject*, PyDict*, JSValue key);
JSValue makeMappingProxy(JSGlobalObject*, JSValue mapping);
void setUpdate(JSGlobalObject*, JSValue set, JSValue iterable);
bool setContains(JSGlobalObject*, PySet*, JSValue key); // key in set. (False if it has thrown.)
// The __getitem__ of these types. That of bytes is bytearray's too.
JSValue listGetItem(JSGlobalObject*, JSValue self, JSValue key);
JSValue tupleGetItem(JSGlobalObject*, JSValue self, JSValue key);
JSValue strGetItem(JSGlobalObject*, JSValue self, JSValue key);
JSValue bytesGetItem(JSGlobalObject*, JSValue self, JSValue key);
// `item in container`, as the __contains__ of these types has it.
bool listContains(JSGlobalObject*, JSArray*, JSValue item);
bool tupleContains(JSGlobalObject*, PyTuple*, JSValue item);
bool strContains(JSGlobalObject*, JSString*, JSValue item);
int64_t hashOfTuple(JSGlobalObject*, PyTuple*);
PyType* orderedDictType(JSGlobalObject*); // collections.OrderedDict (PyOrderedDict.cpp)
JSValue makeRangeIterator(JSGlobalObject*, JSValue range);

// ---- Random bytes (PyRandom.cpp) ----

// From the operating system, not from WTF's generator, which keeps a key stream in the memory of the process. On Linux the kernel is
// asked every time; elsewhere it is what WTF::cryptographicallyRandomValuesFromOS() gives, which ends the process if it cannot.
// For a program, as os.urandom() has them: waits until the system has gathered enough to begin with; false is an exception.
// For a seed: never waits and never raises, so that `import random` works early in the start of a machine; false is "none to be had".
enum class SystemRandomBytes : bool { ForProgram, ForSeed };
bool systemRandomBytes(JSGlobalObject*, std::span<uint8_t>, SystemRandomBytes);

// ---- Signals, the thread (PySignal.cpp, PyThread.cpp) ----

// Calls the handlers of the signals that have arrived. False: one of them raised. (PyErr_CheckSignals)
bool checkOnlySignals(JSGlobalObject*);
// For a function that waits, in a system call say, when that fails with EINTR: if this returns true, it tries again (PEP 475).
// Before the handlers it makes the calls that the thread owes, which may be what it was woken for. They are code of the
// program: whoever calls this holds nothing that only a variable of C++ refers to. See Runtime::releaseUnreachableBuffers().
bool checkSignals(JSGlobalObject*);
// Whether a signal may end a wait that nothing else can end. Where Python is the program, always: `threading.Event().wait()` sleeps
// until Ctrl-C raises KeyboardInterrupt, or until a signal ends the process. Elsewhere, if there is a function for any signal.
bool signalMayEndWait(JSGlobalObject*);
// As if the signal had arrived. Nothing happens if it is ignored or has its default effect. False: there is no such signal.
// (PyErr_SetInterruptEx)
bool simulateSignal(JSGlobalObject*, int number);
// For a program in Python: SIGINT raises KeyboardInterrupt, SIGPIPE is ignored. May raise.
void installSignalHandlers(JSGlobalObject*);
// When the interpreter exits, before what is left is finalized: the signals it had functions for have their default effect again.
void finalizeSignals(JSGlobalObject*);
// For the end of the interpreter. (PyThread.cpp) What the callbacks raise is reported.
void runAtexitCallbacks(JSGlobalObject*);
void waitForThreadsAtExit(JSGlobalObject*); // _thread._shutdown()
void stopWaitingForThreadsAtExit(JSGlobalObject*); // The threads there are count as daemons from now on.
// There are threads to wait for (not daemons), or callbacks to run.
bool hasThreadsOrAtexitCallbacks(JSGlobalObject*);
// The runtime is going away.
void detachSignals(Runtime&);
// For faulthandler (PyFaultHandler.cpp). From now on, when the signal has arrived, dumpTracebackForSignal() is called at the next
// safe point; what would have happened without this happens after that if `chains`, and not at all otherwise. Returns an errno.
int watchSignal(JSGlobalObject*, int number, bool chains);
void unwatchSignal(int number);
// Gets the thread that handles signals out of a system call, with a signal that is kept for this. From any thread; the first function
// is to be called beforehand. The signal does nothing if it comes just before the system call: to be called until the thread has
// taken note. False: nothing is sent, because that thread is not the main one of this VM, or there is no signal to be had.
void prepareToWakeMainThread(JSGlobalObject*);
bool wakeMainThreadFromSystemCall(VM&);
// Asks for dumpTracebackForSignal(globalObject, 0), from any thread, after prepareToWakeMainThread(). False: there is nobody to do it.
bool requestTracebackDump();
// To be called until it is done: see wakeMainThreadFromSystemCall().
bool repeatTracebackDumpRequest();
// It is not to be done after all.
void withdrawTracebackDumpRequest();
// There will be no more of them: faulthandler.cancel_dump_traceback_later().
void cancelTracebackDumpRequests();
// If it is this interpreter that asked for them: it ends.
void cancelTracebackDumpRequests(Runtime&);
void dumpTracebackForSignal(JSGlobalObject*, int number);

// What _thread.get_ident() returns.
uint64_t currentThreadIdent();

enum class TimeRounding : uint8_t { Floor, Ceiling, AwayFromZero };
// A number of seconds, an int or a float, as nanoseconds. False: an exception. (_PyTime_FromSecondsObject)
bool nanosecondsFromSeconds(JSGlobalObject*, JSValue seconds, TimeRounding, int64_t& result);

// ---- Functions, frames ----

// The frame of the Python function that is running, `depth` calls up. Null: there is none.
PyFrame* currentFrame(JSGlobalObject*, unsigned depth = 0);
// The frame object of the call, if anything has asked for it.
PyFrame* frameObjectIfAny(CallFrame*);
// The calls in progress of functions of either language, from the latest. Not those that a stack trace of JS leaves out: of functions
// written in C++, and of the functions of JS that the engine and the host are made of.
struct CallOfEitherLanguage {
    CallFrame* python { nullptr }; // Null: it is of JS,
    CodeBlock* codeBlock { nullptr }; // of this code,
    BytecodeIndex index; // which has got here.
    CallFrame* machineFrame { nullptr }; // The call may have been made part of its caller.
};
void forEachCallOfEitherLanguage(VM&, const ScopedLambda<IterationStatus(const CallOfEitherLanguage&)>&);
// Where that is in the file that the programmer wrote.
struct PositionInJS {
    String filename;
    String function; // As co_name would have it: "<module>" for the top level.
    int line { 0 };
};
PositionInJS positionInJS(VM&, CodeBlock*, BytecodeIndex);
// What stands in for the globals of the code of a file of JS: f_globals of its frames, and where its __warningregistry__ is.
PyDict* namespaceOfFileOfJS(JSGlobalObject*, const String& filename);
// The code as it is run: that of the body, for a generator function. Null: an exception.
CodeBlock* codeBlockOf(JSGlobalObject*, PyCode*);
// The line that co_lines() gives for an offset such as f_lasti. -1: none, or an exception.
int lineOfShownOffset(JSGlobalObject*, PyCode*, int offset);
// Null: the code has not run yet.
CodeBlock* codeBlockIfAny(PyCode*);
// In PyMonitoring.cpp.
PyModule* makeMonitoringModule(JSGlobalObject*);
JSValue sysSetTrace(JSGlobalObject*, JSValue function);
JSValue sysGetTrace(JSGlobalObject*);
JSValue sysSetProfile(JSGlobalObject*, JSValue function);
JSValue sysGetProfile(JSGlobalObject*);
JSValue sysSetTraceAllThreads(JSGlobalObject*, JSValue function);
JSValue sysSetProfileAllThreads(JSGlobalObject*, JSValue function);
// See addTracebackEntry() in PyHooks.h. In PyMonitoring.cpp.
JSValue exceptionIsRaised(JSGlobalObject*, JSValue exception, CallFrame*, BytecodeIndex, bool isRaisedAgain);
// The same for a thread that is not running. Null: it is in no Python code.
PyFrame* topFrameOf(JSGlobalObject*, JSLock::ParkedThread&);
// The code and the line of each Python frame, from the innermost outwards, for as long as the function returns true. For the
// handler of a fatal signal: nothing is allocated, no lock taken.
void forEachPythonFrameWithoutAllocating(CallFrame* topCallFrame, EntryFrame*, const ScopedLambda<bool(const CodeInfo&, int line)>&);
// The globals and locals of the Python function that called the native function that is running.
PyDict* currentGlobals(JSGlobalObject*);
JSValue currentLocals(JSGlobalObject*);
// That function. Null: there is none.
// Not an instance of a class derived from dict or frozendict, which may have a __getitem__ or a __missing__ of its own.
inline bool isExactDict(Runtime& rt, PyDict* dict)
{
    PyType* type = dict->type();
    return type == rt.dictType() || type == rt.frozendictType();
}

PyFunction* currentFunction(JSGlobalObject*);
// cr_origin, for the coroutine function that is being called: (file name, line, name of the function) of the frame that calls it
// and of those below, `depth` of them at most.
JSValue coroutineOrigin(JSGlobalObject*, unsigned depth);
// Its builtins, or those of the interpreter if there is none.
JSValue currentBuiltins(JSGlobalObject*);
// currentBuiltins()[name]. If there is no such key, an AttributeError.
JSValue currentBuiltin(JSGlobalObject*, JSString* name);
// The variables of the frame as they are now: the namespace of the body of a module or a class, for a function a new dict.
JSValue frameLocals(JSGlobalObject*, PyFrame*);

// The name among the candidates that is closest to the one given. Null: none is close.
String calculateSuggestion(const Vector<String>& candidates, const String& name);
// TypeError: f() got an unexpected keyword argument 'x'. Did you mean 'y'? (The last if one of the names it takes is close.)
JSValue raiseUnexpectedKeyword(JSGlobalObject*, StringView function, const String& keyword, std::span<const ASCIILiteral> names);
inline JSValue raiseUnexpectedKeyword(JSGlobalObject* globalObject, StringView function, const String& keyword, std::initializer_list<ASCIILiteral> names) { return raiseUnexpectedKeyword(globalObject, function, keyword, std::span<const ASCIILiteral> { names.begin(), names.size() }); }
// The exception and its traceback as text, without running Lib/traceback.py: see printException().
String formatException(JSGlobalObject*, JSValue exception);

// ---- Exception groups (PyExceptionGroup.cpp) ----

// For `except* type`: the part of the exception that matches, as a group, or None; `rest` is what is left, or None.
JSValue matchExceptionGroup(JSGlobalObject*, JSValue exception, JSValue type, JSValue& rest);
// At the end of a try with `except*` clauses: what to raise, or None. `raised` is a list of what the clauses raised or left.
JSValue prepareReraiseStar(JSGlobalObject*, JSValue original, JSValue raised);

// ---- Type parameters (PyTyping.cpp) ----

// def f[T: bound](): ... The evaluators are functions of one argument, the format; either may be empty.
JSValue makeTypeVar(JSGlobalObject*, JSValue name, JSValue evaluateBound, JSValue evaluateConstraints);
JSValue makeParamSpec(JSGlobalObject*, JSValue name);
JSValue makeTypeVarTuple(JSGlobalObject*, JSValue name);
// Returns the type parameter.
JSValue setTypeParamDefault(JSGlobalObject*, JSValue typeParameter, JSValue evaluateDefault);
// Generic[*parameters], the base a class with type parameters is given.
JSValue subscriptGeneric(JSGlobalObject*, JSValue parameters);
// type name[*parameters] = value
JSValue makeTypeAlias(JSGlobalObject*, JSValue name, JSValue parametersOrNone, JSValue evaluateValue);
void initializeTypingModule(JSGlobalObject*, PyModule*); // _typing

// ---- Template strings (PyTemplate.cpp) ----

// `conversion` is None or a str of one character; an empty format spec is "".
JSValue buildInterpolation(JSGlobalObject*, JSValue value, JSValue expression, JSValue conversion, JSValue formatSpec);
// From a tuple of str and a tuple of interpolations.
JSValue buildTemplate(JSGlobalObject*, JSValue strings, JSValue interpolations);

// ---- Codecs (PyCodecs.cpp) ----

void initializeCodecsModule(JSGlobalObject*, PyModule*); // _codecs

// ---- Streams (PyIO.h) ----

void initializeIOModule(JSGlobalObject*, PyModule*); // _io

// ---- Regular expressions, the Unicode database (PySre.cpp, PyUnicodeData.cpp) ----

void initializeSreModule(JSGlobalObject*, PyModule*); // _sre
void initializeUnicodeDataModule(JSGlobalObject*, PyModule*); // unicodedata

// ---- Numbers, time, constants of the platform (PyMath.cpp, PyCMath.cpp, PyRandom.cpp, PyTime.cpp, PyErrno.cpp) ----

void initializeMathModule(JSGlobalObject*, PyModule*); // math
void initializeMathIntegerModule(JSGlobalObject*, PyModule*); // _math_integer, which is math.integer
void initializeStatisticsModule(JSGlobalObject*, PyModule*); // _statistics
void initializeCMathModule(JSGlobalObject*, PyModule*); // cmath
void initializeRandomModule(JSGlobalObject*, PyModule*); // _random
void initializeTimeModule(JSGlobalObject*, PyModule*); // time
void initializeErrnoModule(JSGlobalObject*, PyModule*); // errno
void initializeStatModule(JSGlobalObject*, PyModule*); // _stat

// ---- Containers and binary data (PyCollections.cpp, PyHeapq.cpp, PyArray.cpp, PyStruct.cpp, PyBinascii.cpp) ----

void initializeCollectionsModule(JSGlobalObject*, PyModule*); // _collections
// Which item of a tuple the value is a descriptor for, if it is what the fields of a named tuple are (collections._tuplegetter). Negative: none.
int64_t indexOfTupleGetter(JSGlobalObject*, JSValue);
void initializeHeapqModule(JSGlobalObject*, PyModule*); // _heapq
void initializeBisectModule(JSGlobalObject*, PyModule*); // _bisect
void initializeArrayModule(JSGlobalObject*, PyModule*); // array
void initializeStructModule(JSGlobalObject*, PyModule*); // _struct
void initializeBinasciiModule(JSGlobalObject*, PyModule*); // binascii
void initializeAbcModule(JSGlobalObject*, PyModule*); // _abc
void initializeCsvModule(JSGlobalObject*, PyModule*); // _csv
void initializeJsonModule(JSGlobalObject*, PyModule*); // _json
void initializeExpatModule(JSGlobalObject*, PyModule*); // pyexpat
void initializePickleModule(JSGlobalObject*, PyModule*); // _pickle (PickleBuffer is in PyMemoryView.cpp)

// ---- The operating system, except on Windows (PyPosix*.cpp, PyPwd.cpp, PyFcntl.cpp, PyLocale.cpp) ----

void initializePosixModule(JSGlobalObject*, PyModule*); // posix
void initializeResourceModule(JSGlobalObject*, PyModule*); // resource
void initializePwdModule(JSGlobalObject*, PyModule*); // pwd
void initializeGrpModule(JSGlobalObject*, PyModule*); // grp
void initializeFcntlModule(JSGlobalObject*, PyModule*); // fcntl
void initializeLocaleModule(JSGlobalObject*, PyModule*); // _locale
void initializeMmapModule(JSGlobalObject*, PyModule*); // mmap
void initializeMultiprocessingModule(JSGlobalObject*, PyModule*); // _multiprocessing
void initializePosixShmemModule(JSGlobalObject*, PyModule*); // _posixshmem (PyMmap.cpp)
void initializePosixSubprocessModule(JSGlobalObject*, PyModule*); // _posixsubprocess
void initializeRemoteDebuggingModule(JSGlobalObject*, PyModule*); // _remote_debugging
void initializeSelectModule(JSGlobalObject*, PyModule*); // select
void initializeSocketModule(JSGlobalObject*, PyModule*); // _socket
void initializeSyslogModule(JSGlobalObject*, PyModule*); // syslog
void initializeTermiosModule(JSGlobalObject*, PyModule*); // termios

// ---- Modules ----

// module.__getattribute__. With `suppress`, a missing attribute is an empty value and no exception.
JSValue moduleGetAttr(JSGlobalObject*, PyModule*, JSString* name, bool suppress = false);
JSValue moduleGetAttribute(JSGlobalObject*, JSValue self, JSValue name);
// import name: what __import__ does.
JSValue importName(JSGlobalObject*, JSString* name, JSValue globals, JSValue locals, JSValue fromList, int level);
// Calls what is not Python code, with the arguments as JS takes them (see PyHooks.h).
JSValue callForeignFunction(JSGlobalObject*, JSValue function, const CallArgs&, JSValue thisValue = { });
// new function(...). `newTarget`: new.target, if not the function.
JSValue constructForeignFunction(JSGlobalObject*, JSValue function, const CallArgs&, JSValue newTarget = { });
// `async for`: aiter(iterable), and what is to be awaited when the loop is left before the end, by `break`, `return` or the exception,
// or None. It is None if nobody `waits`.
JSValue getAIterOfLoop(JSGlobalObject*, JSValue iterable);
JSValue leaveAsyncFor(JSGlobalObject*, JSValue iterator, JSValue exception, bool waits);
// A function written in JS that is found in a class is a method of the instances, as one written in Python is. The instance is its
// `this`; an arrow function, which has none, is to JS what a lambda is to Python and takes it first. Not a method: a class, a bound
// function, a function written in C++ (as in Python).
// prototype[name], read for the object, which inherits from it: a function is bound to the object. Empty: there is none, or an exception.
JSValue attributeFromPrototypeOfJS(JSGlobalObject*, JSObject* prototype, JSValue object, JSString* name);
// object.name in Python, as far as it is a property of the object. Empty: it has none, or an exception.
JSValue attributeFromPropertyOfJS(JSGlobalObject*, JSObject*, JSString* name);
// _js.JSTypeError. It is made when it is first asked for; JSError, if it cannot be made.
PyType* jsTypeErrorType(JSGlobalObject*);
// The same for OSError or a built-in class derived from it: _js.JSFileNotFoundError.
PyType* jsErrorTypeFor(JSGlobalObject*, PyType* base);
// Whether to call the function of JS is to construct it.
bool isClassOfJS(JSCell*);
// What `except` takes besides classes of exceptions: a class of JS.
bool isClassOfJSToCatch(JSGlobalObject*, JSValue);
// Whether the exception stands for something that JS threw and that is an instance of the class, going by the prototypes. It runs no code.
bool standsForInstanceOfJS(JSGlobalObject*, JSValue exception, JSValue classOfJS);
// errno.ENOENT for "ENOENT", and the other way round (null: no name).
std::optional<int> errnoForName(StringView);
ASCIILiteral nameOfErrno(int);
// exception.name, for JS. Empty: there is none, or an exception.
JSValue attributeOfExceptionForJS(JSGlobalObject*, PyException*, JSString* name);
// `with object:` is `using`. The method, bound to the object. Empty: it is no object of JS, or an exception: it cannot be disposed of.
JSValue contextManagerMethodOfJS(JSGlobalObject*, JSValue object, SpecialMethodForWith);
// [(name, value), ...] of its own enumerable properties. Null: an exception.
JSArray* itemsOfObjectOfJS(JSGlobalObject*, JSObject*);
// Once the import system works.
void registerInteropTypes(JSGlobalObject*);
enum class ForeignMethodKind : uint8_t { NotAMethod, TakesThis, TakesFirstArgument };
ForeignMethodKind foreignMethodKind(JSCell*);
// What to call a bound method of such a function is.
JSValue callForeignMethod(JSGlobalObject*, JSValue function, JSValue self, const CallArgs&);

JSValue importModule(JSGlobalObject*, JSString* absoluteName);
JSValue importModule(JSGlobalObject*, ASCIILiteral absoluteName);
JSValue importFrom(JSGlobalObject*, JSValue module, JSString* name);
void importStar(JSGlobalObject*, JSValue module, JSValue locals);
// spec._initializing, which may raise.
bool specIsInitializing(JSGlobalObject*, JSValue spec);
// For a module written in C++ that has no __spec__ and __loader__ yet: has importlib make them. False: an exception.
bool ensureNativeModuleSpec(JSGlobalObject*, PyModule*);
// See TypeFlag::Internal.
ALWAYS_INLINE bool isInternalObject(JSCell* cell) { return cell->type() == PyObjectType && uncheckedDowncast<PyObject>(cell)->type()->has(TypeFlag::Internal); }
// Some objects are made of several cells here, which refer to one another. Those are no cycles: in CPython there is one object,
// or the part does not hold on to the whole. This is the whole that the cell, a part, refers back to. Null: there is none.
JSCell* wholeThatPartRefersTo(JSCell*);
// For the collector, when all that is reachable is marked, about the candidates: the objects with finalizers that are not marked.
// See PyFinalizationOrder.cpp.
struct FinalizationQuestions {
    std::span<JSCell* const> askedAbout; // See isCyclicGarbageToFinalize.
    bool looksAtAllThatDied { false }; // Only in a full collection.
    bool wantsGarbage { false };
    bool wantsToKnowWhatReferenceCountsWouldFree { false };
    bool wantsPieces { false }; // See oldestOfPiece.
};
struct FinalizationOrder {
    // Which of the candidates to finalize now, and which when those are gone. Each in the order in which to take them.
    Vector<unsigned> now;
    Vector<unsigned> later;
    // So many of the first are what reference counts would have freed. The others are in cycles, or hang off them.
    unsigned freedByReferenceCounts { 0 };
    // Among those are objects together with what they alone refer to.
    bool takesAllLevels { false };
    static constexpr unsigned none = std::numeric_limits<unsigned>::max();
    // For each candidate: the candidate whose going would free it, by reference counts. None: nothing dead holds it, or a cycle does.
    Vector<unsigned> letGoBy;
    // If asked for. For each candidate that reference counts would not have freed: the oldest candidate among all that has to do
    // with it, by whatever references, in either direction. (A number that is not a candidate's: no candidate has.) The same for the cells
    // asked about that are such garbage.
    Vector<unsigned> oldestOfPiece;
    Vector<unsigned> oldestOfPieceOfCellAskedAbout;
    // For each of the cells asked about: it is one of those others, or one of them reaches it.
    Vector<bool> isCyclicGarbageToFinalize;
    // False: it was not asked to, or that was too much.
    bool hasLookedAtAllThatDied { false };
    size_t cellsLookedAt { 0 }; // Then: how many cells there were, dead or alive.
    // Then, if it was asked for: the cells that reference counts would not have freed and that no candidate reaches.
    Vector<JSCell*> garbage;
};
FinalizationOrder finalizationOrder(JSC::Heap&, SlotVisitor&, std::span<JSCell* const> candidates, const FinalizationQuestions&);

// function(generator, value) for what is in the variables of the generators, coroutines and async generators that are running, in
// whatever thread: while one runs they are in the frame of its body, not in the generator.
void forEachVariableOfRunningGenerators(VM&, const ScopedLambda<void(PyGenerator*, JSValue)>&);
// What the frames of the threads that do not hold the GIL refer to: what they call, its arguments, and in a frame of Python the
// variables and the temporaries in use. Some of the values are empty.
void forEachValueInFramesOfParkedThreads(VM&, const ScopedLambda<void(JSValue)>&);
// When the interpreter exits: the threads that are still there never run again, and what they kept in instances of threading.local
// goes, as it does when a thread ends.
void forgetThreadLocalsOfOtherThreads(JSGlobalObject*);
// The cells that those given lead to, themselves included, by way of cells of which `isEntered` says so. No cell is allocated.
UncheckedKeyHashSet<JSCell*> cellsReachedFrom(VM&, std::span<JSCell* const>, const ScopedLambda<bool(JSCell*)>& isEntered);

// Which of the locals of a frame of the LLInt or the Baseline JIT hold values that the code may still read. `isCalling`: there is a
// frame that this one called. False: there is no telling.
bool liveLocalsOfFrame(CallFrame*, CodeBlock*, bool isCalling, FastBitVector& live);
// Where the frames end that the instructions of a frame of Python make below its registers (PyBytecode.h).
Register* endOfFrameMadeByInstruction(CallFrame*, CodeBlock*);
std::span<const BlockPiece> blockPiecesOf(CodeBlock*);
// The op_py_catch of a handler is at the end of the code, out of the way, and jumps to where the compiler began the handler: that is
// the place which is in a loop or in a `try`, or not.
unsigned whereHandlerBegins(CodeBlock*, unsigned offsetOfCatch);
// For a built-in function that goes through what it is given, may stop before the end, and keeps nothing: any(g()), next(g()). If the
// generator was made for this call and only the temporary register refers to it, it is closed when the function is done or
// has raised, as when its reference count falls to zero. The function is NativeFlag::ClosesGeneratorArgument, and has one of these
// before it runs any code of the program or returns. In PyTemporaries.cpp.
class GeneratorArgumentScope {
    WTF_MAKE_NONCOPYABLE(GeneratorArgumentScope);
public:
    ALWAYS_INLINE GeneratorArgumentScope(JSGlobalObject* globalObject, JSValue argument)
        : m_runtime(runtime(globalObject))
    {
        if (m_runtime.hasNewGenerator()) [[unlikely]]
            m_generator = m_runtime.claimGeneratorArgument(argument, m_whereToLeaveIt);
    }
    ALWAYS_INLINE ~GeneratorArgumentScope()
    {
        if (m_generator) [[unlikely]]
            release();
    }

private:
    void release();

    Runtime& m_runtime;
    PyGenerator* m_generator { nullptr };
    PyGenerator** m_whereToLeaveIt { nullptr };
};

// A function of the host's event loop module (setEventLoopModule). Empty without an exception: the host has none.
JSValue eventLoopFunction(JSGlobalObject*, ASCIILiteral name);
// The same of the host's module for the import system. Empty, without an exception: there is no such module or function.
JSValue importSystemFunction(JSGlobalObject*, ASCIILiteral name);
// There are calls to make at the next safe point. Any thread, and signal handlers.
void noteCallsOwed(VM&);

// Makes importlib the import system, if there is a standard library to take it from, and imports `site`. What goes wrong
// is printed.
void startImportSystem(JSGlobalObject*);

// ---- Lazy imports (PyImport.cpp) ----

PyRecord* asLazyImportSlow(JSGlobalObject*, JSCell*);
// The placeholder `lazy import` leaves in a namespace, if the value is one.
ALWAYS_INLINE PyRecord* asLazyImport(JSGlobalObject* globalObject, JSValue value)
{
    if (!value || !value.isCell() || value.asCell()->type() != PyObjectType) [[likely]]
        return nullptr;
    return asLazyImportSlow(globalObject, value.asCell());
}
bool lazyImportIsBeingResolved(PyRecord*);
PyType* typeOfLazyImports(JSGlobalObject*); // types.LazyImportType
// What __lazy_import__ does: a placeholder, or the module if the filter wants it imported now.
JSValue lazyImportName(JSGlobalObject*, JSValue name, JSValue builtins, JSValue globals, JSValue locals, JSValue fromList, int level);
// Whether __lazy_modules__ in the globals makes `import name` lazy.
bool isInLazyModules(JSGlobalObject*, JSString* name, PyDict* globals, int level);
// Carries out the import, and puts the result in the place of the placeholder, which was found under the name in the mapping.
JSValue reifyLazyImport(JSGlobalObject*, PyRecord*, JSString* name, JSValue mapping);
// The module has no such attribute. If a lazy import named such a submodule, it is imported now. Empty, with or without an
// exception: there is none. `lookAgain`: the dict of the module may have it all the same.
JSValue loadLazySubmodule(JSGlobalObject*, PyModule*, JSString* name, bool suppress, bool& lookAgain);
// The module of that name has been imported.
void didImportLazyModule(JSGlobalObject*, JSString* name);
void initializeBuiltinsModule(JSGlobalObject*, PyModule*);
void initializeSysModule(JSGlobalObject*, PyModule*);
void addSysExtras(JSGlobalObject*, PyModule* sys); // PySysExtras.cpp
void initializeTypesModule(JSGlobalObject*, PyModule*); // _types
void initializeSysconfigDataModule(JSGlobalObject*, PyModule*); // _sysconfigdata__linux_
void initializeFunctoolsModule(JSGlobalObject*, PyModule*); // _functools
void initializeItertoolsModule(JSGlobalObject*, PyModule*); // itertools
void initializeOperatorModule(JSGlobalObject*, PyModule*); // _operator

// ---- Compiling and running ----

// The flags of compile(), besides those of CodeFlag that stand for __future__ features.
enum CompileFlag : int {
    CompileSourceIsUTF8 = 0x100,
    CompileDontImplyDedent = 0x200,
    CompileOnlyAST = 0x400,
    CompileIgnoreCookie = 0x800,
    CompileTypeComments = 0x1000,
    CompileAllowTopLevelAwait = 0x2000,
    CompileAllowIncompleteInput = 0x4000,
    CompileOptimizedAST = 0x8000 | CompileOnlyAST,
    // Not one of compile()'s: the bytes were read from the file by the interpreter itself, which a message may then name.
    CompileSourceIsFile = 0x40000000,
    // Nor this: tracebacks show the lines of the source although its name is not that of a file (-c, the interactive interpreter).
    CompileSourceIsShown = 0x20000000,
};
// compile(source, filename, mode): a code object. `mode` is "exec", "eval" or "single". A negative `optimize`: as the interpreter.
// `module`, here and below: the name of the module the code is for, by which the filters of warnings select. Empty or None: go by the file name.
JSValue compileSource(JSGlobalObject*, std::span<const uint8_t> source, const String& filename, ASCIILiteral mode, int flags, int optimize, JSValue module = { });
// The text to compile, as bytes; if it is a str, the flags then say to ignore a coding cookie. False: an exception.
// `what`: "string, bytes or code".
bool sourceBytes(JSGlobalObject*, JSValue source, ASCIILiteral function, ASCIILiteral what, Vector<uint8_t>& bytes, int& flags);
// os.fsdecode(), for a file name that is only shown.
String filenameArgument(JSGlobalObject*, JSValue);
// Its two halves. `mode` may be "func_type" too; a negative feature version is the latest. False: an exception.
bool parseSource(JSGlobalObject*, ParseTree&, ParserHost&, std::span<const uint8_t> source, const String& filename, ASCIILiteral mode, int flags, int featureVersion = -1);
JSValue compileTree(JSGlobalObject*, ParseTree&, ParserHost&, const String& filename, int flags, int optimize);
// What the parser and the compiler ask of the runtime, for source said to come from that file.
std::unique_ptr<ParserHost> makeParserHost(JSGlobalObject*, const String& filename, JSValue module = { });
// raise SyntaxError(...), or whatever the failure calls for.
JSValue raiseSyntaxErrorInfo(JSGlobalObject*, const SyntaxErrorInfo&, const String& filename);
// compile(source, filename, mode, PyCF_ONLY_AST): the tree as objects of the module ast (PyAstToObject.cpp).
JSValue parseToAST(JSGlobalObject*, std::span<const uint8_t> source, const String& filename, ASCIILiteral mode, int flags, int optimize, int featureVersion, JSValue module = { });
// compile(tree, filename, mode): a code object or, with PyCF_ONLY_AST, another tree (PyAstFromObject.cpp).
JSValue compileAST(JSGlobalObject*, JSValue tree, const String& filename, ASCIILiteral mode, int flags, int optimize, JSValue module = { });
// isinstance(value, ast.AST). May throw.
bool isAST(JSGlobalObject*, JSValue);
void initializeAstModule(JSGlobalObject*, PyModule*); // _ast
void initializeTokenizeModule(JSGlobalObject*, PyModule*); // _tokenize
void initializeSymtableModule(JSGlobalObject*, PyModule*); // _symtable
void initializeOpcodeModule(JSGlobalObject*, PyModule*); // _opcode
// exec(compile(source, filename, "exec", flags), vars(__main__)). What is raised propagates. Code that awaits at the top level
// (CompileAllowTopLevelAwait) runs on the event loop of the host, if that has one and `unfinished` is not null: that is then
// the JS promise of its end.
void runSourceInMain(JSGlobalObject*, std::span<const uint8_t> source, const String& filename, int flags, JSValue* unfinished);
void runCodeInMain(JSGlobalObject*, JSValue code, JSValue* unfinished);
// linecache._register_code(code, source, filename)
void registerSourceWithLinecache(JSGlobalObject*, JSValue code, std::span<const uint8_t> source, const String& filename);
// The same for a file that is the program: __main__ gets its __file__ and __loader__. (PyCommandLine.cpp)
void runFileInMain(JSGlobalObject*, const String& path, std::span<const uint8_t> source, JSValue* unfinished);
// A line from the terminal, without its end, as the module `readline` reads one: its function _read_line(prompt), which edits the line
// and keeps a history. Importing that module is what turns this on, so it is looked for among the modules there are. Empty without an
// exception: it has not been imported, or is somebody else's, or cannot do it on this terminal. (PyBuiltins.cpp)
JSValue readLineWithEditor(JSGlobalObject*, JSString* prompt);
// sys._baserepl(). The exit status, if the program asked to exit. (PyCommandLine.cpp)
std::optional<int> runBasicInteractiveLoop(JSGlobalObject*);
// As os.path.abspath(): no symbolic link is followed.
String absolutePath(const String&);
// As os.path.realpath(). Null: there is no such file.
String realPath(const String&);
// sys.version, its first word, and sys.platform.
String versionText();
String shortVersionText();
ASCIILiteral platformText();
// sys.flags, anew from the configuration.
void updateSysFlags(JSGlobalObject*);
// exec(code, globals, locals)
// Code that runs with these globals finds its builtins in globals["__builtins__"]: if there is no such item, those of the code that is running
// are put there. False: an exception.
bool ensureBuiltins(JSGlobalObject*, PyDict* globals);
JSValue runCode(JSGlobalObject*, JSValue code, PyDict* globals, JSValue locals, PyTuple* closure = nullptr);

// ---- Warnings ----

// warnings.warn(message, category). False: the warning was turned into an exception.
// ---- Audit hooks ----

// sys.audit(event, *arguments), for what the runtime itself is about to do. An empty value stands for None. False: a hook has
// raised an exception, and it is not to be done.
// For tracemalloc, while it is on. (PyProfilers.cpp)
void noteWhereObjectWasMade(JSGlobalObject*, JSCell*);

bool auditWithHooks(JSGlobalObject*, ASCIILiteral event, std::span<const JSValue> arguments);
// For where the arguments cost something to make.
ALWAYS_INLINE bool isAudited(JSGlobalObject* globalObject) { return !!runtime(globalObject).slot(Runtime::Slot::AuditHooks); }
template<typename... Arguments>
ALWAYS_INLINE bool audit(JSGlobalObject* globalObject, ASCIILiteral event, Arguments... arguments)
{
    if (!isAudited(globalObject)) [[likely]]
        return true;
    std::array<JSValue, sizeof...(Arguments)> values { JSValue(arguments)... };
    return auditWithHooks(globalObject, event, values);
}

bool warn(JSGlobalObject*, PyType* category, const String& message, int stackLevel = 1);
// The same about an object: the `source` of a ResourceWarning, which tracemalloc may know where it was made.
bool warnAbout(JSGlobalObject*, PyType* category, const String& message, int stackLevel, JSValue source);
bool warnExplicit(JSGlobalObject*, PyType* category, const String& message, const String& filename, int lineno, JSValue module = { });

// exception.add_note(note), for the exception that is propagating.
void addNoteToPendingException(JSGlobalObject*, const String& note);

// ---- Exceptions nobody can catch ----

// Hands the exception that is propagating to sys.unraisablehook; it propagates no further. A null message is none.
void reportUnraisable(JSGlobalObject*, const String& message, JSValue object);

void initializeGCModule(JSGlobalObject*, PyModule*); // gc
void initializeJSModule(JSGlobalObject*, PyModule*); // _js
void initializeGlobalObjectModule(JSGlobalObject*, PyModule*); // js
void executeGlobalObjectModule(JSGlobalObject*, PyModule*);
void initializeThreadModule(JSGlobalObject*, PyModule*); // _thread
void initializeQueueModule(JSGlobalObject*, PyModule*); // _queue
void initializeSignalModule(JSGlobalObject*, PyModule*); // _signal
void initializeFaultHandlerModule(JSGlobalObject*, PyModule*); // faulthandler
void initializeLsprofModule(JSGlobalObject*, PyModule*); // _lsprof
void initializeTracemallocModule(JSGlobalObject*, PyModule*); // _tracemalloc
void initializeSysconfigModule(JSGlobalObject*, PyModule*); // _sysconfig
void initializeAtexitModule(JSGlobalObject*, PyModule*); // atexit
void initializeContextVarsModule(JSGlobalObject*, PyModule*); // _contextvars
void initializeImpModule(JSGlobalObject*, PyModule*); // _imp
void initializeMarshalModule(JSGlobalObject*, PyModule*); // marshal
void initializeWarningsModule(JSGlobalObject*, PyModule*); // _warnings
void initializeWeakrefModule(JSGlobalObject*, PyModule*); // _weakref

// ---- Output ----

// sys.stdout.write(text) and the like. `file` may be None, which writes nothing.
void writeToFile(JSGlobalObject*, JSValue file, JSString* text);
void writeToStderr(JSGlobalObject*, const String&);

} // namespace Python
} // namespace JSC
