/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSGlobalObject.h"
#include "PyNames.h"
#include "PyObject.h"
#include "VMInlines.h"
#include "WeakGCMap.h"
#include <wtf/Deque.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class ArrayBuffer;
class CodeBlockSet;
class ConservativeRoots;
class JITStubRoutineSet;
class JSWeakMap;

namespace Python {

// Where a frame of Python code keeps its frame object once somebody has asked for it: the first register after those that
// every frame of JS has. Until then it holds undefined, which is what op_enter leaves there. op_py_ret looks at it.
VirtualRegister frameObjectRegister();
class Monitoring;
class TypeLookupCache;
JSString* internString(VM&, ASCIILiteral, unsigned hash);

// The classes of cells whose Structure takes nothing but the global object to make.
#define FOR_EACH_PY_CELL_CLASS(v) \
    v(PyObject) \
    v(PyInstance) \
    v(PyRecord) \
    v(PyTuple) \
    v(PyDict) \
    v(PyDictStorage) \
    v(PySet) \
    v(PySetStorage) \
    v(PyBytes) \
    v(PyByteArray) \
    v(PyByteStorage) \
    v(PyInt) \
    v(PyFloat) \
    v(PyComplex) \
    v(PyStr) \
    v(PyRange) \
    v(PySlice) \
    v(PyCell) \
    v(PyMethod) \
    v(PyGenerator) \
    v(PyAsyncGenAwaitable) \
    v(PyModule) \
    v(PyException) \
    v(PyCode) \
    v(PyFrame) \
    v(PyTraceback) \
    v(PyProperty) \
    v(PyWrapperObject) \
    v(PySuper) \
    v(PyGetSetDescriptor) \
    v(PyMemberDescriptor) \
    v(PyIterator) \
    v(PyDictView) \
    v(PyMultiIterator) \
    v(PyManagedBuffer) \
    v(PyMemoryView)

// The built-in types, exceptions aside. `v(list)` declares Runtime::listType().
#define FOR_EACH_PY_TYPE(v) \
    v(object) \
    v(type) \
    v(none) \
    v(notImplemented) \
    v(ellipsis) \
    v(int) \
    v(bool) \
    v(float) \
    v(complex) \
    v(str) \
    v(bytes) \
    v(bytearray) \
    v(list) \
    v(tuple) \
    v(dict) \
    v(frozendict) \
    v(set) \
    v(frozenset) \
    v(range) \
    v(slice) \
    v(function) \
    v(nativeFunction) \
    v(methodDescriptor) \
    v(wrapperDescriptor) \
    v(methodWrapper) \
    v(classMethodDescriptor) \
    v(getSetDescriptor) \
    v(memberDescriptor) \
    v(method) \
    v(cell) \
    v(code) \
    v(frame) \
    v(frameLocalsProxy) \
    v(traceback) \
    v(module) \
    v(generator) \
    v(coroutine) \
    v(asyncGenerator) \
    v(coroutineWrapper) \
    v(asyncGeneratorASend) \
    v(asyncGeneratorAThrow) \
    v(asyncGeneratorWrappedValue) \
    v(property) \
    v(classmethod) \
    v(staticmethod) \
    v(super) \
    v(mappingProxy) \
    v(listIterator) \
    v(listReverseIterator) \
    v(tupleIterator) \
    v(strIterator) \
    v(bytesIterator) \
    v(bytearrayIterator) \
    v(rangeIterator) \
    v(longRangeIterator) \
    v(dictKeyIterator) \
    v(dictValueIterator) \
    v(dictItemIterator) \
    v(dictReverseKeyIterator) \
    v(dictReverseValueIterator) \
    v(dictReverseItemIterator) \
    v(dictKeys) \
    v(dictValues) \
    v(dictItems) \
    v(setIterator) \
    v(sequenceIterator) \
    v(callableIterator) \
    v(enumerate) \
    v(zip) \
    v(map) \
    v(filter) \
    v(reversed) \
    v(genericAlias) \
    v(genericAliasIterator) \
    v(union) \
    v(sentinel) \
    v(simpleNamespace) \
    v(typeVar) \
    v(paramSpec) \
    v(paramSpecArgs) \
    v(paramSpecKwargs) \
    v(typeVarTuple) \
    v(typeAliasType) \
    v(generic) \
    v(noDefault) \
    v(constEvaluator) \
    v(templateString) \
    v(templateStringIterator) \
    v(interpolation) \
    v(managedBuffer) \
    v(memoryview) \
    v(memoryviewIterator) \
    v(weakRef) \
    v(weakProxy) \
    v(weakCallableProxy) \
    v(jsObject) \
    v(jsPlainObject) \
    v(jsFunction) \
    v(jsPromise) \
    v(jsMap)

// name, base
#define FOR_EACH_PY_EXCEPTION(v) \
    v(BaseException, object) \
    v(BaseExceptionGroup, BaseException) \
    v(Exception, BaseException) \
    v(GeneratorExit, BaseException) \
    v(KeyboardInterrupt, BaseException) \
    v(SystemExit, BaseException) \
    v(ArithmeticError, Exception) \
    v(FloatingPointError, ArithmeticError) \
    v(OverflowError, ArithmeticError) \
    v(ZeroDivisionError, ArithmeticError) \
    v(AssertionError, Exception) \
    v(AttributeError, Exception) \
    v(BufferError, Exception) \
    v(EOFError, Exception) \
    v(ExceptionGroup, BaseExceptionGroup) \
    v(ImportError, Exception) \
    v(ImportCycleError, ImportError) \
    v(ModuleNotFoundError, ImportError) \
    v(LookupError, Exception) \
    v(IndexError, LookupError) \
    v(KeyError, LookupError) \
    v(MemoryError, Exception) \
    v(NameError, Exception) \
    v(UnboundLocalError, NameError) \
    v(OSError, Exception) \
    v(BlockingIOError, OSError) \
    v(ChildProcessError, OSError) \
    v(ConnectionError, OSError) \
    v(BrokenPipeError, ConnectionError) \
    v(ConnectionAbortedError, ConnectionError) \
    v(ConnectionRefusedError, ConnectionError) \
    v(ConnectionResetError, ConnectionError) \
    v(FileExistsError, OSError) \
    v(FileNotFoundError, OSError) \
    v(InterruptedError, OSError) \
    v(IsADirectoryError, OSError) \
    v(NotADirectoryError, OSError) \
    v(PermissionError, OSError) \
    v(ProcessLookupError, OSError) \
    v(TimeoutError, OSError) \
    v(ReferenceError, Exception) \
    v(RuntimeError, Exception) \
    v(NotImplementedError, RuntimeError) \
    v(PythonFinalizationError, RuntimeError) \
    v(RecursionError, RuntimeError) \
    v(StopAsyncIteration, Exception) \
    v(StopIteration, Exception) \
    v(SyntaxError, Exception) \
    v(IndentationError, SyntaxError) \
    v(TabError, IndentationError) \
    v(IncompleteInputError, SyntaxError) \
    v(SystemError, Exception) \
    v(TypeError, Exception) \
    v(ValueError, Exception) \
    v(UnicodeError, ValueError) \
    v(UnicodeDecodeError, UnicodeError) \
    v(UnicodeEncodeError, UnicodeError) \
    v(UnicodeTranslateError, UnicodeError) \
    v(Warning, Exception) \
    v(BytesWarning, Warning) \
    v(DeprecationWarning, Warning) \
    v(EncodingWarning, Warning) \
    v(FutureWarning, Warning) \
    v(ImportWarning, Warning) \
    v(PendingDeprecationWarning, Warning) \
    v(ResourceWarning, Warning) \
    v(RuntimeWarning, Warning) \
    v(SyntaxWarning, Warning) \
    v(UnicodeWarning, Warning) \
    v(UserWarning, Warning) \
    v(JSError, Exception)

#define PY_FORWARD_DECLARE(Class) class Class;
FOR_EACH_PY_CELL_CLASS(PY_FORWARD_DECLARE)
#undef PY_FORWARD_DECLARE
class PyFunction;
class PyNativeFunction;
class PyType;
class PyWeakRef;
class Threads;
class ThreadState;
struct StrIndexCache;
struct StructSeqDef;
struct TypeDef;

enum class CellClass : uint8_t {
#define PY_DEFINE_ENUMERATOR(Class) Class,
    FOR_EACH_PY_CELL_CLASS(PY_DEFINE_ENUMERATOR)
#undef PY_DEFINE_ENUMERATOR
    Count,
};

enum class TypeID : uint8_t {
#define PY_DEFINE_ENUMERATOR(id) id##Type,
    FOR_EACH_PY_TYPE(PY_DEFINE_ENUMERATOR)
#undef PY_DEFINE_ENUMERATOR
#define PY_DEFINE_ENUMERATOR(id, base) id,
    FOR_EACH_PY_EXCEPTION(PY_DEFINE_ENUMERATOR)
#undef PY_DEFINE_ENUMERATOR
    Count,
};

// The objects of JS that Python objects are shown to JS with, one of each for a global object. See PyJSView.h.
enum class JSViewObject : uint8_t {
    ObjectPrototype, // Of every PyObject.
    FunctionPrototype, // Of functions and types.
    ExceptionPrototype,
    TypeErrorPrototype, // Of the instances of TypeError: see PyException::getPrototype().
    IteratorPrototype, // Of what [Symbol.iterator]() returns.
    AsyncIteratorPrototype,
    Namespace, // importModuleFromJS("__python__")
    // Functions, which take the object as `this`.
    Iterator,
    AsyncIterator,
    Then,
    Next,
    Return,
    ToJSON,
    // What follows the promise of an item of an async iterator.
    AsyncItem,
    AsyncEnd,
    Identity,
    Count,
};

// A lock among the threads of Python for the use of the runtime, held across points where the GIL may change hands. The thread
// that holds it may take it again. It must stay where it is in memory. All of this is for whoever holds the GIL.
class ThreadLock {
    WTF_MAKE_NONCOPYABLE(ThreadLock);
public:
    ThreadLock() = default;

    bool isHeld() const { return m_count; }
    bool isHeldByThisThread(JSGlobalObject*) const;
    // Waits for as long as it takes. False: an exception, which the handler of a signal raised, or because nobody can release it.
    bool acquire(JSGlobalObject*);
    void release(JSGlobalObject*);
    // The thread ends. If it holds the lock, nobody else could let go of it.
    void holderEnds(Threads&, const ThreadState&);

private:
    uint64_t m_owner { 0 }; // ThreadState::serial
    unsigned m_count { 0 };
};

// All that Python needs of a global object besides what programs make. It belongs to the global object, which visits it.
class Runtime {
    WTF_MAKE_TZONE_ALLOCATED(Runtime);
    WTF_MAKE_NONCOPYABLE(Runtime);
public:
    // Makes it the first time.
    JS_EXPORT_PRIVATE static Runtime& ensure(JSGlobalObject*);
    static Runtime& from(JSGlobalObject* globalObject)
    {
        ASSERT(globalObject->pythonRuntime());
        return *globalObject->pythonRuntime();
    }

    explicit Runtime(JSGlobalObject*);
    ~Runtime();
    // For the collector, with the world stopped, when nothing more will be marked and the global object is not. The heap has forgotten the runtime by the time the program goes on: nobody
    // asks it anything any more, for all the cells it knows are dead, and they are swept in any order, some of them long before the global object, whose destructor destroys this. What the
    // world outside knows of the runtime (the signals, faulthandler, the C API) ends here as well.
    void globalObjectDidDie();

    template<typename Visitor> void visit(Visitor&);

    VM& vm() const { return m_vm; }
    JSGlobalObject* globalObject() const { return m_globalObject; }

    template<typename Class> Structure* structure() const;
    Structure* keywordNamesStructure() const { return m_keywordNamesStructure.get(); }
    Structure* typeStructure() const { return m_typeStructure.get(); }
    Structure* nativeFunctionStructure() const { return m_nativeFunctionStructure.get(); }
    Structure* functionStructure() const { return m_functionStructure.get(); }
    Structure* listSubclassStructure() const { return m_listSubclassStructure.get(); }
    Structure* weakRefStructure() const { return m_weakRefStructure.get(); }
    // Of what kwargs() makes in JS: a PyRecord that is told from all others by it.

    JSObject* jsViewObject(JSViewObject which) const { return m_jsViewObjects[static_cast<unsigned>(which)].get(); }
    void setJSViewObject(JSViewObject which, JSObject* object) { m_jsViewObjects[static_cast<unsigned>(which)].set(m_vm, m_globalObject, object); }

    PyType* type(TypeID id) const { return m_types[static_cast<unsigned>(id)].get(); }
#define PY_DEFINE_ACCESSOR(id) PyType* id##Type() const { return type(TypeID::id##Type); }
    FOR_EACH_PY_TYPE(PY_DEFINE_ACCESSOR)
#undef PY_DEFINE_ACCESSOR
#define PY_DEFINE_ACCESSOR(id, base) PyType* id() const { return type(TypeID::id); }
    FOR_EACH_PY_EXCEPTION(PY_DEFINE_ACCESSOR)
#undef PY_DEFINE_ACCESSOR

    // The type a module written in C++ defines so, made the first time it is asked for. The definition, which must live as
    // long as the process, is what tells it from others. A null base is object.
    PyType* typeFor(const TypeDef&, PyType* base = nullptr);
    PyType* typeIfMadeFor(const TypeDef& def) const { return typeIfMadeForDefinitionAt(&def); } // Null: it has not been asked for yet.
    // The same table for struct sequences, which are made in another way: structSeqTypeFor(). The definition lives as long as the process too.
    PyType* typeIfMadeFor(const StructSeqDef& def) const { return typeIfMadeForDefinitionAt(&def); }
    void keepTypeMadeFor(const StructSeqDef& def, PyType* type) { keepTypeMadeForDefinitionAt(&def, type); }

    JSString* name(NameID id) const { return m_names[static_cast<unsigned>(id)].get(); }
    JSString* name(Special special) const { return m_names[static_cast<unsigned>(special)].get(); }
    // The name of an entry of a static table (a NativeDef, a NativeGetSet), interned.
    template<typename Entry>
    JSString* nameIn(const Entry& entry) const { return entry.knownOfName.id != NameID::Count ? name(entry.knownOfName.id) : internString(m_vm, entry.name, entry.knownOfName.hash); }
    struct Names {
#define PY_DEFINE_ACCESSOR(id, text) JSString* id() const { return runtime.name(NameID::id); }
        FOR_EACH_PY_NAME(PY_DEFINE_ACCESSOR)
#undef PY_DEFINE_ACCESSOR
        const Runtime& runtime;
    };
    const Names names { *this };

    // The type of any value a Python program can hold.
    PyType* typeOf(JSValue value) const
    {
        if (value.isCell()) {
            JSCell* cell = value.asCell();
            JSType type = cell->type();
            if (isPyObjectType(type)) [[likely]]
                return uncheckedDowncast<PyObject>(cell)->type();
            if (type == StringType)
                return strType();
            if (type == ArrayType)
                return listType();
        } else if (value.isInt32())
            return intType();
        return typeOfSlow(value);
    }
    PyType* typeOfSlow(JSValue) const;

    JSValue notImplemented() const { return m_notImplemented.get(); }
    JSValue ellipsis() const { return m_ellipsis.get(); }
    PyTuple* emptyTuple() const { return m_emptyTuple.get(); }
    PyBytes* emptyBytes() const { return m_emptyBytes.get(); }

    PyDict* modules() const { return m_modules.get(); } // sys.modules
    PyModule* builtinsModule() const { return m_builtinsModule.get(); }
    PyDict* builtins() const { return m_builtins.get(); }
    PyModule* sysModule() const { return m_sysModule.get(); }
    PyDict* sysDict() const { return m_sysDict.get(); }

    // builtins.__import__ as it is at first: an import statement that finds it there does not go through a call.
    JSValue defaultImportFunction() const { return m_defaultImportFunction.get(); }
    void setDefaultImportFunction(JSValue value) { set(m_defaultImportFunction, value); }
    // The contextvars.Context the thread is in; empty until something needs one.
    JSValue currentContext() const { return slot(Slot::CurrentContext); }
    void setCurrentContext(JSValue value)
    {
        setSlot(Slot::CurrentContext, value);
        didChangeContextVariables();
    }
    // A number that changes whenever a context variable may have another value than before: the thread has entered or left a context,
    // a variable has been set, another thread has taken over. What was read from a variable holds for as long as the number stays.
    uint64_t contextVariablesVersion() const { return m_contextVariablesVersion; }
    void didChangeContextVariables() { ++m_contextVariablesVersion; }

    // State of the native modules that programs must not see (it is not in a module's dict) and that outlives the modules.
    enum class Slot : unsigned {
        CurrentContext,
        NativeModules, // dict: name -> dict (nativeModuleNamespace())
        ImportlibBootstrap, // The module importlib._bootstrap, once it has been loaded. (What sys.modules has under its name is the program's to change.)
        FindAndLoad, // importlib._bootstrap._find_and_load, once importlib is the import system
        Monitoring, // See Monitoring::state().
        TraceFunction, // sys.settrace() and sys.setprofile(), of the thread that is running. Empty: none.
        ProfileFunction,
        AtexitCallbacks, // list
        AuditHooks, // list: sys.addaudithook(). Empty: there are none.
        AllocationTracebacks, // A JSWeakMap: see PyProfilers.cpp.
        Warnings, // See warningsState() in PyWarnings.cpp.
        SignalHandlers, // list
        RaisedBySignalHandler, // While one of the calls that are owed was running. See runPendingCalls().
        ThrownException, // What generator.throw() is throwing,
        ThrowArguments, // and the tuple of arguments it was made from. Null: it is raised where the generator waits, whomever it waits for.
        RecentBufferOwners, // See getBytesLike().
        InstanceDictDescriptor, // The __dict__ and the __weakref__ in the dict of every class that adds them.
        InstanceWeakrefDescriptor,
        MemberKeys, // dict: see PyMemberDescriptor::create().
        LazyImportsMode, // sys.set_lazy_imports(), sys.set_lazy_imports_filter(), sys.setdlopenflags()
        LazyImportsFilter,
        LazyModules, // sys.lazy_modules, a set
        LazyPendingSubmodules, // dict: see registerLazySubmodules() in PyImport.cpp
        DlopenFlags,
        Sre, // See sreModuleState().
        Abc, // See abcState() in PyAbc.cpp,
        Json, // jsonState() in PyJson.cpp,
        Pickle, // pickleState() in PyPickle.cpp,
        Expat, // and expatState() in PyExpat.cpp.
        IntactPycFile, // See readPycFile() in PyCodeCache.cpp.
        GCGarbage, // gc.garbage and gc.callbacks, lists. The program has them too.
        GCCallbacks,
        JSErrorClasses, // dict: see jsErrorTypeFor() in PyException.cpp.
        ExceptionClassesOfJSPrototypes, // A JSWeakMap: see exceptionClassForPrototypeOfJS() there.
        BoundMethodsOfJS, // A JSWeakMap: see bindIfFunction() in PyInterop.cpp.
        TypesOfJSClasses, // A JSWeakMap: see typeForClassOfJS() in PyJSClass.cpp.
        ArgumentsOfJSConstructors, // A JSWeakMap: see classOfJSNew() there.
        NamespacesOfFilesOfJS, // dict: see namespaceOfFileOfJS() in PyFrame.cpp.
        BuiltinAll, // The functions, whatever the names are bound to.
        BuiltinAny,
        Decimal, // See Decimal::State in PyDecimal.h.
        Count
    };
    JSValue slot(Slot which) const { return m_slots[static_cast<unsigned>(which)].get(); }
    void setSlot(Slot which, JSValue value) { set(m_slots[static_cast<unsigned>(which)], value); }

    // A key under which an object that has no field for it keeps its __dict__.
    PrivateName dictKey() const { return m_dictKey; }
    // Of an object of JS that stands for keyword arguments: see isKeywordMarker().
    PrivateName keywordsKey() const { return m_keywordsKey; }
    // Keys under which a type written in C++ keeps what its instances have no field for. See hiddenField().
    static constexpr unsigned numberOfHiddenKeys = 4;
    PrivateName hiddenKey(unsigned index) const { return m_hiddenKeys[index]; }
    // See PyInstance.
    PrivateName instanceFieldKey(unsigned index) const { return m_instanceKeys[index]; }
    PrivateName instanceHasDictKey() const { return m_instanceKeys[2]; }

    // ---- The exception being handled: sys.exception() ----

    // Each generator that is running has a slot of its own, on top of that of what resumed it.
    struct HandledException {
        WriteBarrier<Unknown> value; // Empty: none.
    };
    // For whoever may hand it on: see below.
    JSValue handledExceptionInTopSlot() const;
    JSValue handledException() const; // In the topmost slot that has one. Empty: none.
    bool isHandlingException() const;
    JSValue setHandledExceptionInTopSlot(JSValue); // Returns what was there.
    // A handler of the frame is left, otherwise than by an exception that goes on: what was being handled before is so again.
    void handlerIsLeft(CallFrame*, JSValue previous);
    // For the unwinder: the frame is left because the thread is to end, with all the handlers that it is in.
    void handlersAreLeftByTermination(CallFrame*);

    // ---- Entries of tracebacks that are only noted ----
    //
    // Most exceptions are caught by a handler that never looks at them: `except KeyError: pass`. For such a one the entry of the
    // frame that catches is not made: a traceback object, the frame object it refers to, and, because there is a frame object, all
    // that a call has to do for it when it returns (PyFrame::callIsEnding()), which makes the frame object of the caller, and so on up
    // the stack. What the entry would be made of is noted here, and the entry is made if anybody could come to look. (So an
    // ErrorInstance keeps the raw stack, and makes `stack` of it when that is first asked for.)
    //
    // An exception is unseen (PyException::isUnseen()) from where the runtime makes it in order to raise it, without running code of
    // the program, until something of the program's could get hold of it: PyException::willBeSeen(). That is called by what gives out the
    // exception being handled or the pending one, by a handler that names it or hands it on (`as`, `with`, `except*`) or that can be
    // suspended, when it is raised otherwise than by the compiler's own instructions, when tools are told of it, when it comes up to a frame of JS or
    // to a function of C++, before the unwinder runs code of the program (to close what a loop that is left was going through), and when the program is
    // handed objects that it had no reference to. So while there is a note, only the slot, the
    // registers of the handler and the VM refer to the exception, and when the handler is left the note can be dropped: nobody
    // will ever know.
    //
    // A note is gone before its frame is. It is taken out when the entry is made, when the handler is left, and when the unwinder
    // leaves the frame. A handler that can be suspended has none.
    //
    // The notes of the running thread are visited (visitNotedEntries()). Those of a thread that waits for its turn go with its saved state, and
    // are not. That is sound as long as its stack is scanned: while there is a note, the registers of the handler hold the exception and
    // the frame holds the code block. Saved states that are visited visit the notes too.
    struct NotedEntry {
        PyException* exception;
        CallFrame* callFrame;
        CodeBlock* codeBlock;
        unsigned offset; // Of the instruction that raised it, or called what did.
    };
    // The innermost frame last. (No inline capacity: a saved state is, as a rule, on a stack that is scanned, and what is unused of a buffer there is whatever
    // deeper calls have left, held for as long as the thread waits.)
    using NotedEntries = Vector<NotedEntry>;
    // For the unwinder: the frame is about to catch the exception, which is unseen.
    void noteEntry(PyException*, CallFrame*, CodeBlock*, unsigned offset);
    void makeNotedEntry(PyException*);
    void makeNotedEntries(); // Of all threads.
    // For the unwinder: `thrown` leaves the frame. Empty: the thread is to end.
    ALWAYS_INLINE void frameIsLeftByException(CallFrame* callFrame, JSValue thrown)
    {
        if (!m_notedEntries.isEmpty()) [[unlikely]]
            takeOutNotedEntriesOf(callFrame, thrown);
        if (!thrown) [[unlikely]]
            handlersAreLeftByTermination(callFrame);
    }
    template<typename Visitor> static void visitNotedEntries(Visitor&, const NotedEntries&);
#if ASSERT_ENABLED
    void assertNoNotedEntryOf(CallFrame*) const;
#endif
    // What a `catch` of JS has caught is being handled while the code of the clause runs, and what it calls. Nothing in that code
    // says where it is left. So the catch is only noted here, with where the clause begins and ends (PythonMark::CatchOfJS), and whoever
    // next asks for a slot first looks at the stack: a catch whose frame is gone, or has gone on to other code, is over. What
    // is left goes into the slot that was on top at the time, as an `except` would have put it there, and comes out when it is over.
    // An async function or async generator of JS has another frame each time it is resumed. A catch of its whose frame is gone is put by
    // for it, and is looked for when there is a frame of it on the stack: it goes on if the frame is in the clause. In between
    // nothing of it is being handled, as with a coroutine of Python's. For a generator of JS a catch is over at the first `yield`.
    void didCatchInJS(CallFrame*, CodeBlock*, BytecodeIndex, JSValue thrown);
    void forgetCatchesInJSThatAreOver();
    PyGenerator* runningGenerator() const { return m_runningGenerator; }
    void setRunningGenerator(PyGenerator* generator) { m_runningGenerator = generator; }
    static constexpr ptrdiff_t offsetOfRunningGenerator() { return OBJECT_OFFSETOF(Runtime, m_runningGenerator); }
    static constexpr ptrdiff_t offsetOfNotImplemented() { return OBJECT_OFFSETOF(Runtime, m_notImplemented); }

    // ---- Threads (PyThreads.h) ----

    Threads& threads() const { return *m_threads; }
    // A call of a Python function by the C++ of this runtime, which wants what the function returns as it is. Any other C++
    // that calls one is the host's, and is given what JS is given: see resultForCaller(). They are told apart by the frame
    // through which the VM was entered, which the function notes when it starts.
    struct RuntimeCall {
        RuntimeCall* previous;
        EntryFrame* entryFrame { nullptr };
    };
    RuntimeCall* runtimeCall { nullptr };

    // For what the collector scans as it scans a stack, for as long as a thread is without the GIL (ThreadLocalState): the compiler says so if a field is added that brings padding.
#if COMPILER(CLANG)
#define PY_BEGIN_WITHOUT_PADDING _Pragma("clang diagnostic push") _Pragma("clang diagnostic error \"-Wpadded\"")
#define PY_END_WITHOUT_PADDING _Pragma("clang diagnostic pop")
#else
#define PY_BEGIN_WITHOUT_PADDING
#define PY_END_WITHOUT_PADDING
#endif

    PY_BEGIN_WITHOUT_PADDING
    // See "Generators" in PyTemporaries.cpp.
    struct NewGenerator {
        PyGenerator* generator { nullptr };
        JSCell* iterator { nullptr }; // What has taken it since: enumerate(g()). It is that which the use will be given.
        CallFrame* caller { nullptr }; // Only compared with: the frame may be gone.
        unsigned use { 0 }; // The offset of the instruction.
        unsigned otherUse { 0 }; // m.f(g()) is compiled twice, for an f that is a method and one that is not.
    };
    using NewGenerators = std::array<NewGenerator, 4>;

    // What of all this belongs to the thread that is running. It goes with the thread when another takes its place. It refers
    // to cells, and is visited by nobody: it must be on the stack of the thread, or where the collector looks as it looks at a stack (SavedStates).
    // As a rule it lies on the stack of a thread that has given up the GIL, for as long as that lasts, where the frames of the thread's last calls lay, and the collector takes every word there
    // for a reference. So there is no room in it that is not written: no vector with room of its own, no padding (of a word with one byte written the rest still points into the old cell's block).
    struct ThreadLocalState {
        JSValue currentContext;
        JSValue handledException;
        PyGenerator* runningGenerator { nullptr };
        JSValue asyncGenFirstIter;
        JSValue asyncGenFinalizer;
        JSValue sendResult;
        JSValue exceptionGroupRest;
        JSValue thrownException;
        JSValue throwArguments;
        JSValue threadLocals;
        JSValue traceFunction;
        JSValue profileFunction;
        int tracingDepth { 0 };
        int tracedEvent { -1 };
        CallFrame* tracedCallFrame { nullptr };
        int tracedLine { -1 };
        int tracedOffset { -1 };
        Vector<JSCell*> reprStack;
        NotedEntries notedEntries;
        CallFrame* reraisingFrame { nullptr };
        RuntimeCall* runtimeCall { nullptr };
        int frameDepth { 0 };
        unsigned buffersThatKeepGIL { 0 };
        void* stackWhereUnaskedCallsBegan { nullptr };
        uintptr_t isRunningPendingCalls { false };
        uintptr_t isCallingCollectionCallbacks { false };
        NewGenerators newGenerators;
        unsigned ownershipEpoch { 0 }; // When they were put here.
        uint32_t thread { 0 };
    };
    PY_END_WITHOUT_PADDING
    static_assert(sizeof(decltype(ThreadLocalState::reprStack)) == 2 * sizeof(void*) && sizeof(NotedEntries) == 2 * sizeof(void*), "A vector with room of its own has room that nobody writes while it is empty.");
    // Leaves the runtime as a thread that has just started finds it.
    void takeThreadLocalState(ThreadLocalState&);
    void putThreadLocalState(ThreadLocalState&);
    // The thread that runs: 0 for the main one, for any other a number that no other thread of the process ever has (ThreadState::number()).
    // While no thread holds the GIL: noThread.
    static constexpr uint32_t noThread = std::numeric_limits<uint32_t>::max();
    uint32_t currentThread() const { return m_currentThread; }
    void setCurrentThread(uint32_t thread) { m_currentThread = thread; }
    // A JSWeakMap: for each _thread._local object this thread has used, the dict that holds the thread's attributes of it.
    JSValue threadLocals() const { return m_threadLocals.get(); }
    void setThreadLocals(JSValue value) { set(m_threadLocals, value); }
    // Each interpreter has its own, as in CPython. The program can take them and not give them back: they go with the runtime.
    ThreadLock importLock; // _imp.acquire_lock()
    ThreadLock warningsLock; // _warnings._acquire_lock()

    // ---- sys.monitoring, sys.settrace(), sys.setprofile(): see PyMonitoring.h ----

    Monitoring& monitoring() { return *m_monitoring; }
    TypeLookupCache& typeLookupCache() { return *m_typeLookupCache; }
    // Not 0: a tool, a trace function or a profile function is running. What it does is told to nobody.
    int tracingDepth { 0 };
    // What it is being told of (Event), and where that happens. -1: nothing.
    int tracedEvent { -1 };
    CallFrame* tracedCallFrame { nullptr };
    // Not -1: the line that call is on as far as anybody can tell (f_lineno), which is not always that of the instruction it is at, and
    // where that line is in the code (f_lasti).
    int tracedLine { -1 };
    int tracedOffset { -1 };

    // sys.set_asyncgen_hooks(). Empty: none.
    JSValue asyncGenFirstIter() const { return m_asyncGenFirstIter.get(); }
    void setAsyncGenFirstIter(JSValue value) { set(m_asyncGenFirstIter, value); }
    JSValue asyncGenFinalizer() const { return m_asyncGenFinalizer.get(); }
    void setAsyncGenFinalizer(JSValue value) { set(m_asyncGenFinalizer, value); }

    // ---- Finalizers and the callbacks of weak references ----
    //
    // The collector runs no Python code. An object with a finalizer (__del__; a generator suspended where there is something
    // to clean up) is registered when it is made. When a collection finds one that nothing reaches, it keeps it, with all that
    // it reaches, and queues it; the finalizer is called, once, at the next safe point: between two instructions of Python
    // code, where any code may run. If nothing has taken hold of the object by then, the next collection frees it.
    //
    // Every call that is owed is owed by one thread, and only that thread makes it: the best guess at the thread that would have dropped
    // the last reference. That is the one that made the object, or the weak reference. See README, "Which thread runs a finalizer".

    //
    // Registering is writing the object down. Most such objects are done, or dead, long before the next collection, so nothing
    // else is done until then, and nothing is ever looked up: whether an object still has something to finalize is asked of
    // the object when a collection goes through the list. An object is written down when it is made, by whatever calls the
    // __new__ of a built-in type for a class with a __del__ (didMakeObject()). It makes no difference how often: the object
    // knows. One with a FinalizerState may be written down again when it has been finalized, any other is so once in its life.
    void registerFinalizable(JSCell*);
    // The same for a generator, of which there are many, when it is made.
    ALWAYS_INLINE void registerNewGenerator(JSCell* generator)
    {
        m_youngFinalizable.append(Finalizable::of(generator, m_currentThread, m_era));
        if (allocationTracebackLimit) [[unlikely]]
            didRegisterWhileTracingAllocations(generator);
    }
    // For an object with a FinalizerState: for the time being it has nothing to finalize.
    void unregisterFinalizable(PyRecord*);
    void unregisterFinalizable(PyManagedBuffer*);
    // For objects that were made long ago and whose class has a __del__ only now. Those that have been finalized are left alone.
    void registerFinalizableLate(const MarkedArgumentBuffer&);
    // For the collector, with the world stopped, once all that is reachable is marked. True: it marked something. There may be several runtimes in a heap, and what one of them keeps may lead to what is
    // another's. So each step is taken by all of them, again and again until none has marked anything, before any takes the next (Heap::keepUnreachablePythonObjectsToFinalize()).
    enum class StepOfKeeping : uint8_t { QueuesThatStandStill, OtherQueues, WhatHasDied, WhatObserversKeep };
    bool keepUnreachableObjectsToFinalize(SlotVisitor&, StepOfKeeping);
    // When none has anything left to keep: the collection will mark no more. Once for each collection.
    void didMarkAll(SlotVisitor&);
    // For a weak reference whose referent has just died.
    void queueWeakRefCallback(PyWeakRef*);
    // Since the last collection, of whatever kind. With the GIL.
    bool hasRegisteredFinalizableSinceCollection() const { return !m_youngFinalizable.isEmpty(); }

    // This thread owes calls.
    ALWAYS_INLINE bool hasPendingCalls() const { return m_hasPendingCalls; }
    // Those that this thread owes, or those that any thread owes and nobody has begun: whoever waits for a collection (gc.collect()) makes
    // all the calls that come of it, and all that are owed already, before it goes on. Or, of the other threads', those for what is older than the era that is served (the exit's first step).
    enum class CallsOf : uint8_t { ThisThread, AllThreads, OfEarlierEras };
    // Some of them are finalizers or callbacks of weak references: the next collection may find more than the last.
    bool hasPendingFinalizers(CallsOf = CallsOf::AllThreads);
    bool owesCalls(CallsOf);
    // At a safe point. What the calls raise is reported and goes no further. But signals are looked at between the calls, and what
    // a handler raises is thrown: what is left in the queue then is left for the next safe point. So is what a handler raises in the
    // middle of a call, which it ends, unless the program asked for the calls (gc.collect()): it did not make the call, it may be
    // asleep, and its Ctrl-C is not to end up as "Exception ignored in __del__".
    // `mayPutOff`: nobody asked for it or waits for it. Then it is put off if the stack is nearly used up.
    void runPendingCalls(bool mayPutOff = false, CallsOf = CallsOf::ThisThread);
    // It has made the calls it owed, and will run no more code. What it comes to owe from now on is the main thread's.
    void threadDidEnd(uint32_t thread);
    // One of those calls is running, in this thread. What it queues waits for it to return.
    bool isRunningPendingCalls() const { return m_isRunningPendingCalls; }
    // What is thrown, as the VM has it: for what JS threw, the value and not the exception that a runtime makes of it.
    void didRaiseInSignalHandler(JSValue thrown);
    // For reportUnraisable(): true if it is not to be reported, because runPendingCalls() is going to throw it.
    bool keepIfRaisedBySignalHandler(JSValue thrown);
    // Nobody asked for those calls, and the frame is of the code that happened to run when their time came. It has not called them,
    // and what they do is not laid at its door (PyWarnings.cpp).
    bool wasInterruptedByPendingCalls(CallFrame* callFrame) const { return m_stackWhereUnaskedCallsBegan && static_cast<void*>(callFrame) > m_stackWhereUnaskedCallsBegan; }
    // gc.collect() in one of those calls, unless that is one of gc.callbacks (see below): there is a collection when all that are queued have been made.
    void collectAfterPendingCalls() { m_shouldCollectAfterPendingCalls = true; }
    // This thread is telling gc.callbacks of a collection of the engine's own, and is in one of them. (A callback may let go of the GIL: this goes with the thread, like isRunningPendingCalls().)
    bool isCallingCollectionCallbacks() const { return m_isCallingCollectionCallbacks; }

    // See writeOutPendingOutput().
    ALWAYS_INLINE bool mayHavePendingOutput() const { return m_mayHavePendingOutput; }
    void setMayHavePendingOutput(bool value) { m_mayHavePendingOutput = value; }
    void didQueueCallsFor(uint32_t thread, bool mayUncoverMore = true);
    // When the interpreter exits: whatever is still registered is finalized, reachable or not, but for what the threads hold that
    // are still there.
    void finalizeAllAtExit();
    // See the function of that name in PyBuffer.h.
    void releaseUnreachableBuffers(bool thorough);
    class MarkingObserver {
    public:
        virtual ~MarkingObserver() = default;
        // With the world stopped, when all that is reachable is marked, and nothing has been kept for the sake of a finalizer yet. Once in a collection. Nobody has m_finalizationLock: it may ask
        // the runtime what takes that (forEachUnmarkedObjectWithFinalizer()).
        virtual void didMarkAllThatIsReachable(SlotVisitor&) { }
        // With the world stopped, when a collection has marked all that it will: no runtime of the heap has anything left to keep.
        // True: it has marked something itself, so there is more to mark. It is told again when that is done. The collector HAS m_finalizationLock of this runtime: nothing that takes it may be asked.
        virtual bool didFinishMarking(SlotVisitor&) { return false; }
        // In a collection that was asked for, with the world stopped, before any object is kept for its finalizer: whether it
        // wants to be told what has died that reference counts would not have freed, and is told. It adds what is to be kept.
        virtual bool wantsGarbage() { return false; }
        virtual void didFindGarbage(std::span<JSCell* const>, size_t /* cellsLookedAt */, Vector<JSCell*>& /* toKeep */) { }
        // For collectAndFinalize(): before and after each of its collections, in the thread that asked for them.
        virtual void willCollect() { }
        virtual void didCollect() { }
    };
    void setMarkingObserver(MarkingObserver* observer) { m_markingObserver = observer; }
    // For the heap.
    void didMarkAllThatIsReachable(SlotVisitor& visitor)
    {
        if (m_markingObserver) [[unlikely]]
            m_markingObserver->didMarkAllThatIsReachable(visitor);
    }
    // A collection for which the program waits. It takes the time to look at all that has died: see PyFinalizationOrder.cpp.
    // `leavesCyclicGarbageForLater`: only what reference counts would have freed is finalized. What has a finalizer and is in a
    // cycle, or hangs off one, is kept, and comes up again in the next collection.
    // CallsOf::AllThreads: this thread will make the calls that come of it. Otherwise each is left to the thread that owes it.
    void collectAsAskedFor(CollectionScope = CollectionScope::Full, bool leavesCyclicGarbageForLater = false, CallsOf = CallsOf::AllThreads);
    // The thread that has one of these waits for a collection. Every collection that ends meanwhile is one that it waits for, whoever began it and of whichever kind it is: one that
    // was under way or had been asked for is finished first, and finds what the program has just dropped.
    class WaitForCollection {
        WTF_MAKE_NONCOPYABLE(WaitForCollection);
    public:
        WaitForCollection(Runtime& runtime, bool leavesCyclicGarbageForLater, CallsOf callsOf)
            : m_isWaitedFor(runtime.m_collectionIsWaitedFor, true)
            , m_leavesCyclicGarbageForLater(runtime.m_leavesCyclicGarbageForLater, leavesCyclicGarbageForLater)
            , m_callsThatAskerMakes(runtime.m_callsThatAskerMakes, callsOf)
        {
        }
    private:
        SetForScope<bool> m_isWaitedFor;
        SetForScope<bool> m_leavesCyclicGarbageForLater;
        SetForScope<CallsOf> m_callsThatAskerMakes;
    };
    // While there is one, this thread makes calls that other threads owe (runPendingCalls() with the same CallsOf). CallsOf::OfEarlierEras begins an era: see m_era.
    //
    // WHAT ITS OWN COLLECTIONS FIND IS ITS OWN TO MAKE; WHEN NOTHING CAN MOVE ANY MORE IT GIVES BACK WHAT IT HAS NOT BEGUN, AND TELLS THE MAKERS.
    // - What it takes: what it serves of what the collections find that it waits for (WaitForCollection, with the same CallsOf), which do not wake the makers for that. Nothing changes hands: a call that is
    //   taken lies in its maker's queue (CallsOwed::taken), and the maker leaves it alone, woken or running, told or not. The program has asked THIS thread, and another may be in no state to run a
    //   finalizer: on top of a wait in which it holds a lock that the finalizer wants.
    // - What other collections have handed out, before it came or since, is its maker's as ever, who has been told; one that serves all threads makes it if it comes to it first, and goes to it first
    //   (takeNextCall()). (Taken as well, a call that its maker was on the way to would wait for a caller that spins, in a finalizer, for what that call does.)
    // - The exit's (CallsOf::OfEarlierEras) serves only what is of an earlier era. It takes what is owed, too: when it comes, and again before each further collection of its own (willCollectAgain()). What
    //   another collection hands out meanwhile it may begin where it stands at the head of its maker's queue (mayBeginWhatIsHandedOutTo()).
    // - A finalizer that it runs may wait for what one of those calls does. Nobody can see that. But if that is so, and nobody else brings about what it waits for, there comes a standstill: every thread
    //   of Python's, this one among them, waits for another thread, without a deadline (Threads::allOthersWaitForGood(), asked by the last that goes to sleep: Threads::waitUntil()). Then nothing but a
    //   call that is owed can end any of those waits. So what is taken and not begun goes into the other half of its maker's queue, and the makers are told (giveBackWhatIsTaken()): from then on it is
    //   what another collection has handed out. In any other wait it is left to whoever can move to end it: a thread that the finalizer has started, a child, time. (This thread asks again every time it goes
    //   back to sleep, which in a finalizer it does every 50 ms or so: IdleWait::collectIfDue(). That a thread which ends asks too is for promptness.)
    // - If it leaves early (the handler of a signal has raised), what it serves of what is still taken becomes its own, in its own queue, and it makes it at its next safe point.
    class ServesOtherThreads {
        WTF_MAKE_NONCOPYABLE(ServesOtherThreads);
    public:
        ServesOtherThreads(Runtime&, CallsOf);
        ~ServesOtherThreads();
        void willCollectAgain();
    private:
        friend class Runtime;
        void takeWhatIsOwed() WTF_REQUIRES_LOCK(m_runtime.m_finalizationLock);
        bool keepWhatIsTaken() WTF_REQUIRES_LOCK(m_runtime.m_finalizationLock);
        Runtime& m_runtime;
        CallsOf m_callsOf;
        ThreadState* m_thread { nullptr };
    };
    // For Threads::waitUntil(), at a standstill. Whether this thread, which is about to go to sleep, has got something to do by that.
    bool giveBackWhatIsTaken();
    // For when the interpreter exits, before the program is told (atexit): what is unreachable by then and reference
    // counts would have freed would have been finalized long ago.
    void finalizeWhatReferenceCountsWouldHaveFreed();
    // Its finalizer has been called, and yet it is still there.
    bool hasBeenFinalized(JSCell*);
    // For MarkingObserver::didMarkAllThatIsReachable(): function(cell, isInQueue) for the objects that an earlier collection has found dead and whose finalizer has not been called yet, and for
    // those that this collection is about to find dead.
    void forEachUnmarkedObjectWithFinalizer(SlotVisitor&, const ScopedLambda<void(JSCell*, bool)>&);
    // All that is in a queue, whatever else holds it.
    void forEachObjectToBeFinalized(const ScopedLambda<void(JSCell*)>&);

    // ---- Cells that can be found again and are not kept alive by that ----

    // The collector does not visit what is in such a table. When a collection has marked all that it will, it takes out what it
    // has not marked. So the table never refers to a cell that has been freed, whatever else is or is not done about the cell.
    class WeakCellTable {
        WTF_MAKE_NONCOPYABLE(WeakCellTable);
    public:
        WeakCellTable() = default;

        // Where it is, until it is removed or dies. Nothing moves.
        unsigned add(JSCell* cell)
        {
            if (m_free.isEmpty()) {
                m_cells.append(cell);
                return m_cells.size() - 1;
            }
            unsigned index = m_free.takeLast();
            ASSERT(!m_cells[index]);
            m_cells[index] = cell;
            return index;
        }
        void remove(unsigned index, JSCell* cell)
        {
            ASSERT_UNUSED(cell, m_cells[index] == cell);
            m_cells[index] = nullptr;
            m_free.append(index);
        }
        unsigned size() const { return m_cells.size(); }
        JSCell* at(unsigned index) const { return m_cells[index]; } // Null: nothing is there.
        void removeUnmarked(JSC::Heap&);

    private:
        Vector<JSCell*> m_cells;
        Vector<unsigned> m_free;
    };
    // For the collector, with the world stopped, when marking is over and no cell has been freed yet.
    void reconcileWeakReferencesAtGCEnd();
    WeakCellTable scheduledTasks; // See registerTask() in PyAsyncio.cpp.
    // See PyMemoryView::pinMemoryOfJS(). The view is not visited. An entry owns a pin of the memory, and a reference to it: when the
    // list is gone through, the collector has let go of the memory of the arrays that have died.
    class PinningView {
    public:
        PinningView(JSCell* view, ArrayBuffer& pinned); // It takes the pin over.
        PinningView(PinningView&&);
        ~PinningView();
        JSCell* view;

    private:
        RefPtr<ArrayBuffer> m_memory;
    };
    Vector<PinningView> pinningViews;
    size_t pinningViewsThatAreOld { 0 }; // So many, the first, have been through a collection.
    void unpinMemoryOfDeadViews();

    // ---- The module gc (PyGC.cpp) ----

    struct CollectionStats {
        uint64_t collections { 0 };
        uint64_t collected { 0 };
        uint64_t uncollectable { 0 };
        uint64_t candidates { 0 };
        double duration { 0 };
    };
    struct {
        bool isEnabled { true };
        bool isCollecting { false }; // gc.collect() is at work,
        uint32_t collectingThread { noThread }; // in this thread.
        // The runtime collects as a means to an end (to tell what is alive, or who refers to whom) where CPython collects nothing: that is in no statistics, and gc.callbacks do not hear of it.
        bool collectsForItself { false };
        int debugFlags { 0 };
        std::array<int, 3> thresholds { 2000, 10, 10 };
        std::array<CollectionStats, 3> stats;
    } gc;
    bool hasCollectionCallbacks() const;
    // For gc.collect(), around all that it does: see m_threadBetweenStartAndStop. False: some thread is between "start" and "stop", and the call goes untold.
    bool willTellOfCollect();
    void didTellOfCollect();
    // Calls gc.callbacks for a collection of the engine's own, which is over by then.
    void tellOfCollection(int generation, double duration, bool isOver);
    // For the collector, whenever it is about to look at the stacks. The first time in a collection is when that begins, as far as gc.get_stats() goes. See clearDeadTemporaries(),
    // and Threads::gatherConservativeRoots() for what is scanned with them.
    void willScanStacks(ConservativeRoots&, JITStubRoutineSet&, CodeBlockSet&);

    // ---- Objects that only a temporary register refers to (PyTemporaries.cpp) ----

    // The function that `nativeFrame` is the call of has made `object`, which it is about to return and nothing else refers to.
    void didMakeResult(CallFrame* nativeFrame, JSCell* object);
    // The instruction that the frame, of Python code, is at has.
    void didMakeResultOfInstruction(CallFrame*, JSCell* object);
    ALWAYS_INLINE bool ownsTemporaries() const { return m_ownedTemporaryCount; }
    // If ownsTemporaries(): the call, or the instruction, is over, with `result` or with an exception. Those of its operands that
    // it was the one use of are finalized.
    void didCallWithTemporaries(CallFrame* nativeFrame, JSValue result);
    void didRunInstructionWithTemporaries(CallFrame*, std::span<const JSValue> operands, JSValue result);
    // For the code of a type that keeps none of the arguments it is called with, when it has made the instance or failed to.
    void didCallType(PyType*, JSValue result, bool resultIsFresh);
    // For a method that is NativeFlag::KeepsNothing, when it is done. `function`: the one in its definition, if another such method
    // of the object may have called it without a frame of its own: then the use is not over.
    void didCallMethod(JSValue self, JSValue result, bool resultIsFresh, void* function = nullptr);
    // For a function that has made what it returns, and has shown it to nobody.
    void didCallFunction(JSValue (*function)(JSGlobalObject*, JSValue, JSValue, JSValue, JSValue, JSValue, JSValue, JSValue, JSValue), JSValue result);
    // For the code of a type whose instances may be owned: base[key] has given `result`, or raised.
    void didSubscript(JSValue base, JSValue result);
    // If ownsTemporaries(): for the code of a type that copies what is stored in its items: base[key] = value is done, or has raised.
    void didStoreInItem(JSValue value);
    // Code of the program is about to run that is shown the arguments of a call, or may change the variables of the frames.
    void forgetOwnedTemporaries() { m_ownedTemporaryCount = 0; }
    // The program is being handed objects that it may have had no reference to.
    void temporariesMayHaveEscaped()
    {
        forgetTemporaries();
        ++m_ownershipEpoch;
        makeNotedEntries();
    }
    // ---- See "Generators" in PyTemporaries.cpp ----
    // A generator function is about to return the generator to whoever called it.
    void didMakeGenerator(PyGenerator*, CallFrame* frameOfFunction);
    // There are notes of generators whose use has not come.
    ALWAYS_INLINE bool hasNewGenerator() const { return m_newGenerators[0].generator; }
    // If so. The instruction that the frame is at goes through the value and keeps nothing of it: op_py_get_iter, `in`, a, b = g(). Whether it owns something now.
    bool claimNewGenerators(JSValue, CallFrame*, unsigned offset);
    // (The interpreter does that by itself for a generator, if there is no other note.)
    static constexpr ptrdiff_t offsetOfNewGenerator() { return OBJECT_OFFSETOF(Runtime, m_newGenerators) + OBJECT_OFFSETOF(NewGenerator, generator); }
    static constexpr ptrdiff_t offsetOfCallerOfNewGenerator() { return OBJECT_OFFSETOF(Runtime, m_newGenerators) + OBJECT_OFFSETOF(NewGenerator, caller); }
    static constexpr ptrdiff_t offsetOfUseOfNewGenerator() { return OBJECT_OFFSETOF(Runtime, m_newGenerators) + OBJECT_OFFSETOF(NewGenerator, use); }
    static constexpr ptrdiff_t offsetOfNewGeneratorBefore() { return offsetOfNewGenerator() + sizeof(NewGenerator); }
    static constexpr ptrdiff_t offsetOfOwnershipEpoch() { return OBJECT_OFFSETOF(Runtime, m_ownershipEpoch); }
    // If there are notes. For a built-in function or type that takes them up, when it has just been called and has run nothing: the instruction that called it. Without
    // a frame: none did.
    struct CallOfInstruction {
        CallFrame* frame { nullptr };
        unsigned offset { 0 };
        JSCell** whereToLeaveArgument { nullptr }; // See leaveGeneratorArgumentIn().
    };
    CallOfInstruction callInProgress();
    // See GeneratorArgumentScope. Null: there is nothing in it that is known to be out of the program's reach.
    JSCell* claimGeneratorArgument(JSValue argument, JSCell**& whereToLeaveIt);
    // enumerate(g()), zip(g(), h()), iter(g()): the call has made an iterator that has its arguments to itself (TypeFlag::HasIteratorsToItself), or has returned its
    // argument. The notes that were made for the call are about the use of the iterator from now on, which is the value of a call too. Null: the call
    // has done something else.
    void iteratorWasMade(const CallOfInstruction&, JSCell* iterator);
    // A tool is about to be shown the arguments of a call.
    void forgetNewGenerators() { m_newGenerators.fill({ }); }
    // For what makes the call at the offset on behalf of the frame, and has something to do between the return of the function and the
    // closing of the generator: the function leaves it there. When the call is over, whether the function got that far or not: didCallWithGeneratorArgument().
    void leaveGeneratorArgumentIn(JSCell*& slot, CallFrame*, unsigned offset);
    void didCallWithGeneratorArgument() { m_callOnBehalf = { }; }
    // The loop is left before the iterator is exhausted.
    void releaseIteratorOfLoop(JSValue iterator);
    // A built-in function or `in` is done with it, perhaps by an exception.
    void releaseGeneratorGoneThrough(JSValue);
    // An iterator (TypeFlag::HasIteratorsToItself) lets go of it, perhaps because of an exception.
    void releaseWhatIteratorHad(JSValue);
    // An exception is caught in the frame, or leaves it.
    void releaseIteratorsOfLoopsLeft(CallFrame*, unsigned offset, bool isCaught);
    unsigned ownershipEpoch() const { return m_ownershipEpoch; }

    // ---- What an instruction leaves for the next ----

    JSValue takeSendResult() { return take(m_sendResult); }
    void setSendResult(JSValue value) { set(m_sendResult, value); }
    JSValue takeExceptionGroupRest() { return take(m_exceptionGroupRest); }
    void setExceptionGroupRest(JSValue value) { set(m_exceptionGroupRest, value); }

    // The objects whose repr() is being computed, to catch one that contains itself.
    Vector<JSCell*> reprStack;

    // See StrView.
    std::unique_ptr<StrIndexCache, void (*)(StrIndexCache*)> strIndexCache { nullptr, nullptr };

    // The frame in which an exception is being raised again: it is in the traceback already.
    CallFrame* reraisingFrame { nullptr };
    // The same for a call of a function of JS: a `finally`, or what the compiler makes that works like one, has caught this there.
    // Only ever compared.
    struct {
        CallFrame* callFrame { nullptr };
        CodeBlock* codeBlock { nullptr };
        EncodedJSValue thrown { };
    } caughtToBeThrownAgainInJS;
    // finalizeInterpreter() has run, or is running.
    bool isFinalized { false };
    bool isRunningExitHandlers { false };

    // Whether what runs on the main thread runs in the host's event loop, which asyncio asks (loopOfHost() in PyAsyncio.cpp). It does,
    // but for what a program in Python does itself, from its first statement to its last, or to its first `await` at the top level:
    // there, as in CPython, no loop is running unless the program runs one. What the host's loop calls while such a program waits
    // (PyEventWait.h) is in the loop again. Innermost last: where on the stack that began, and which of the two it is.
    struct EventLoopPosition {
        void* stackPosition;
        bool isInLoop;
    };
    Vector<EventLoopPosition, 4> eventLoopPositions;
    class EventLoopPositionScope {
        WTF_MAKE_NONCOPYABLE(EventLoopPositionScope);
        WTF_FORBID_HEAP_ALLOCATION;
    public:
        EventLoopPositionScope(Runtime& rt, bool isInLoop)
            : m_runtime(rt)
        {
            rt.eventLoopPositions.append({ this, isInLoop });
        }
        ~EventLoopPositionScope() { m_runtime.eventLoopPositions.removeLast(); }

    private:
        Runtime& m_runtime;
    };

    // ---- Settings ----

    int optimizationLevel { 0 }; // -O
    int maxStrDigits { 4300 };
    int recursionLimit { 1000 };
    // tracemalloc.start(): how many frames to note of where an object was made. 0: none, it is off. See PyProfilers.cpp.
    unsigned allocationTracebackLimit { 0 };
    int coroutineOriginTrackingDepth { 0 }; // sys.set_coroutine_origin_tracking_depth()
    // How many frames of Python code there are. op_py_bind_args counts a frame in; op_py_ret, or the unwinder when an exception
    // leaves the frame, counts it out. For the body of a generator: op_py_generator_enter, op_py_generator_suspend and _return.
    int frameDepth { 0 };
    // How many buffers this thread holds of memory that JS could take away if it ran: see PyBuffer.h.
    unsigned buffersThatKeepGIL { 0 };
    // False: there are as many as there may be, and a RecursionError is raised. Otherwise leaveFrame() has to follow.
    bool tryEnterFrame();
    void leaveFrame() { --frameDepth; }
    // The frames that whoever traces the program is being told about while an exception unwinds the stack. The frames that the
    // exception has left are still there below them, and still needed: see clearDeadTemporariesOf().
    Vector<CallFrame*, 2> framesToldOfException;
    // The call is about to end, by a return or an exception. Its frame object, if it has one, takes over what the frame holds.
    // (A generator that yields, or ends without an exception, tells its frame object itself.)
    // `leftBy`: the exception that leaves it, for the unwinder.
    void frameIsEnding(CallFrame*, JSValue leftBy = { }, bool callerIsEntryFrame = true);

    // ---- For the interpreter (LowLevelInterpreter64.asm) ----

    StructureID iteratorStructureID; // Of PyIterator.
    Allocator iteratorAllocator; // Of its cells, as ObjectAllocationProfile has one for generated code. None: Options::forceGCSlowPaths.
    StructureID functionStructureID; // Of PyFunction.
    TaggedNativeFunction hostFunctionOfNativeFunctions; // What PyNativeFunction::is() goes by,
    TaggedNativeFunction hostFunctionOfTypes; // and PyType::is().
    // What beginBinaryOperation() and its like leave to their caller, which reads it at once. `progress`: with IsReflected.
    struct {
        PyFunction* function { nullptr };
        unsigned progress { 0 };
        EncodedJSValue left { };
        EncodedJSValue right { };
    } methodToCall;
    EncodedJSValue selfOfCall { }; // What beginCallWithSelf() leaves to llint_python_call_with_self_trampoline, which takes it at once.
    PyType* lazyImportType { nullptr }; // Of the placeholders of `lazy import`. Null: there has been none. typeFor() keeps it alive.
    static constexpr ptrdiff_t offsetOfType(TypeID id) { return OBJECT_OFFSETOF(Runtime, m_types) + static_cast<unsigned>(id) * sizeof(WriteBarrier<PyType>); }

private:
    void initialize();
    void initializeStructures();
    void initializeNames();
    void initializeTypes();
    void initializeModules();
    template<typename Visitor> void visitPendingCalls(Visitor&);

    JSValue take(WriteBarrier<Unknown>& slot)
    {
        JSValue value = slot.get();
        slot.clear();
        return value;
    }
    void set(WriteBarrier<Unknown>& slot, JSValue value) { slot.set(m_vm, m_globalObject, value); }

    VM& m_vm;
    JSGlobalObject* m_globalObject;

    std::array<WriteBarrier<Structure>, static_cast<unsigned>(CellClass::Count)> m_structures;
    WriteBarrier<Structure> m_keywordNamesStructure;
    WriteBarrier<Structure> m_typeStructure;
    WriteBarrier<Structure> m_nativeFunctionStructure;
    WriteBarrier<Structure> m_functionStructure;
    WriteBarrier<Structure> m_listSubclassStructure;
    WriteBarrier<Structure> m_weakRefStructure;
    std::array<WriteBarrier<JSObject>, static_cast<unsigned>(JSViewObject::Count)> m_jsViewObjects;
    std::array<WriteBarrier<PyType>, static_cast<unsigned>(TypeID::Count)> m_types;
    std::array<WriteBarrier<JSString>, numberOfNames> m_names;

    WriteBarrier<Unknown> m_notImplemented;
    WriteBarrier<Unknown> m_ellipsis;
    WriteBarrier<PyTuple> m_emptyTuple;
    WriteBarrier<PyBytes> m_emptyBytes;

    WriteBarrier<PyDict> m_modules;
    WriteBarrier<PyModule> m_builtinsModule;
    WriteBarrier<PyDict> m_builtins;
    WriteBarrier<PyModule> m_sysModule;
    WriteBarrier<PyDict> m_sysDict;
    WriteBarrier<Unknown> m_defaultImportFunction;
    std::array<WriteBarrier<Unknown>, static_cast<unsigned>(Slot::Count)> m_slots;

    WriteBarrier<Unknown> m_handledException;
    PyGenerator* m_runningGenerator { nullptr }; // The frame of its body refers to it.
    NotedEntries m_notedEntries;
    Lock m_notedEntriesLock; // For the collector.
    void takeOutNotedEntriesOf(CallFrame*, JSValue thrown);
    uint64_t m_contextVariablesVersion { 0 };
    struct CatchInJS {
        enum State : uint8_t { Noted, BeingPutInSlot, InSlot };
        CallFrame* callFrame; // Only ever compared.
        UnlinkedCodeBlock* code;
        unsigned catchOffset; // Of the op_catch.
        unsigned start;
        unsigned end;
        JSValue thrown; // InSlot: the exception that was made of it.
        PyGenerator* slotOwner; // Null: the thread's own slot.
        JSObject* generator; // What keeps the state of the async function whose body the code is, if that can be suspended in the clause. Otherwise null.
        unsigned yieldPoints; // Where the code can be suspended in the clause: PythonMark::yieldPoints().
        JSValue previous { }; // InSlot: what the slot held.
        State state { Noted };
    };
    // Outermost first. Only the main thread runs code of JS. The lock is for the collector.
    Vector<CatchInJS, 4> m_catchesInJS;
    Lock m_catchesInJSLock;
    // `generator` -> the catches that were not over when it was suspended, outermost first: of each `start`, `end`, `yieldPoints` and
    // `thrown`. Null: there are none.
    WriteBarrier<JSWeakMap> m_catchesOfSuspendedGeneratorsOfJS;
    ALWAYS_INLINE void lookAtCatchesInJS() const
    {
        if (!m_catchesInJS.isEmpty() || m_catchesOfSuspendedGeneratorsOfJS) [[unlikely]]
            const_cast<Runtime*>(this)->putCatchesInJSInSlots();
    }
    void putCatchesInJSInSlots();
    void dropCatchesInJSThatAreOver(CallFrame* catching, unsigned clauseStart);
    void takeUpCatchesOfResumedGeneratorsOfJS(CallFrame* catching, unsigned clauseStart);
    JSValue handledExceptionInSlotOf(PyGenerator*) const;
    void setHandledExceptionInSlotOf(PyGenerator*, JSValue);
    WriteBarrier<Unknown> m_asyncGenFirstIter;
    WriteBarrier<Unknown> m_asyncGenFinalizer;
    WriteBarrier<Unknown> m_threadLocals;
    RefPtr<Threads> m_threads;

    WriteBarrier<Unknown> m_sendResult;
    WriteBarrier<Unknown> m_exceptionGroupRest;

    PrivateName m_dictKey;
    PrivateName m_keywordsKey;
    std::array<PrivateName, numberOfHiddenKeys> m_hiddenKeys;
    std::array<PrivateName, 3> m_instanceKeys;
    std::unique_ptr<Monitoring> m_monitoring;
    std::unique_ptr<TypeLookupCache> m_typeLookupCache;

    Lock m_finalizationLock;
    // The objects that are registered, in the order in which they were: first those that a collection has seen, then the others. Not
    // visited: a collection takes out what it finds dead. What it has seen and left in is old, so a collection of the young
    // only looks at the others. The thread that runs Python code uses these, and the collector while that thread is stopped, which
    // may be several times in one collection: see keepUnreachableObjectsToFinalize().
    //
    // The addresses are kept complemented. A list is copied as it grows and shrinks, in vector registers, which keep what they held until
    // the thread's next large copy; whenever a collection suspends the thread, the kernel saves them on its stack, and other frames
    // come to lie there. What the collector reads there is then no address: a thread that looked and went on waiting would
    // otherwise keep alive what the next to look is meant to find. (Other conservative collectors do the same with their weak tables:
    // GC_HIDE_POINTER in Boehm's, for finalization and disappearing links, HIDE_POINTER in Mono's, for weak links.)
    struct Finalizable {
        static Finalizable of(JSCell* cell, uint32_t thread, uint32_t era) { return { ~std::bit_cast<uintptr_t>(cell), thread, era }; }
        JSCell* cell() const { return std::bit_cast<JSCell*>(~complement); }
        void clear() { complement = ~static_cast<uintptr_t>(0); }

        uintptr_t complement;
        uint32_t thread; // That registered it.
        uint32_t era; // When: see m_era.
    };
    // The main thread, as it ends, finalizes what has been dropped by then, whoever made it, and what comes of that, while daemon threads go on running. What they make from then on
    // is theirs to finalize, as at any other time, or a program whose daemons keep making garbage would not exit. So that step begins an era, and takes what is older (ServesOtherThreads).
    uint32_t m_era { 0 };
    uint32_t m_eraThatIsServed { 0 }; // 0: none.
    uint64_t m_firstWeakRefOfEraThatIsServed { 0 };
    Vector<Finalizable> m_oldFinalizable;
    Vector<Finalizable> m_youngFinalizable;
    void forEachFinalizable(const ScopedLambda<void(JSCell*)>&);
    void didTakeOffList(JSCell*, bool isToBeFinalized = false);
    void didRegisterWhileTracingAllocations(JSCell*);

    // Nobody visits these. A collection forgets those of objects that it has found dead.
    struct OwnedTemporary {
        JSCell* object;
        CallFrame* frame;
        unsigned firstUse; // Offsets of instructions.
        unsigned lastUse;
    };
    void forgetTemporaries()
    {
        m_ownedTemporaryCount = 0;
        forgetNewGenerators();
    }
    void forgetDeadTemporaries(SlotVisitor&);
    void ownTemporary(CallFrame*, unsigned offset, JSCell*);
    template<typename IsOperand> void releaseTemporariesUsedAt(CallFrame*, unsigned offset, JSValue result, const IsOperand&);
    static constexpr unsigned maxOwnedTemporaries = 4;
    std::array<OwnedTemporary, maxOwnedTemporaries> m_ownedTemporaries;
    unsigned m_ownedTemporaryCount { 0 };
    unsigned m_ownershipEpoch { 1 };
    // The last made first, and none behind an empty one.
    NewGenerators m_newGenerators;
    void forget(NewGenerator&);
    JSCell* claimGeneratorArgumentAmongNotes(JSValue argument, JSCell**& whereToLeaveIt);
    bool isToBeClosed(PyGenerator*);
    void releaseGeneratorLeftHalfWay(JSValue);
    template<typename Functor> void forgetNewGeneratorsFor(const CallOfInstruction&, const Functor&);
    CallOfInstruction m_callOnBehalf;
    // What the marks say of the value of a call, for didMakeGenerator(), which asks about the same few calls over and over. A code
    // block is only compared with, and a collection forgets them all: until then there is no other at its address.
    // The question cannot wait for the use. In `f(g())` the generator is only out of the program's reach if f is known to keep nothing when
    // g() returns: an f written in Python keeps it and runs no code of ours, the note of it would stay, and the next time round the
    // same g() may return the same generator to an f that closes what it is given.
    struct UseOfCall {
        enum Kind : uint8_t { Other, ForHeader, Argument };
        CodeBlock* codeBlock { nullptr };
        unsigned call { 0 }; // Offsets of instructions.
        unsigned use { 0 };
        unsigned otherUse { 0 };
        int callee { 0 }; // Argument: the register.
        Kind kind { Other };
    };
    std::array<UseOfCall, 16> m_usesOfCalls;
    void findUseOfCall(UseOfCall&, CodeBlock*, unsigned call);
    // Of the value of the call at the offset, which the frame is at. Null: it is no use that owns what it is given.
    const UseOfCall* owningUseOfCall(CallFrame*, unsigned call);
    struct CollectionNotice {
        int generation;
        double duration;
    };
    // A call that is owed: the finalizer of an object, or the callback of a weak reference.
    struct CallOwed {
        JSCell* cell { nullptr };
        uint32_t sequence { 0 }; // They are numbered as they are queued, whichever thread's queue it is. It wraps.
        uint32_t era : 30 { 0 }; // Of an object: see m_era.
        uint32_t isWeakRef : 1 { 0 };
        // What has to happen in one order is a group: whoever begins its first call takes the others with it. Groups are of finalizers.
        uint32_t continuesGroup : 1 { 0 };
        // The order in which each half of a queue is kept, group by group, and by which lists are merged.
        bool comesBefore(const CallOwed&) const;
    };
    // What a thread owes. The cells are kept by every collection: see keepUnreachableObjectsToFinalize().
    // A call lies in the queue of the thread that made the object (of the main thread, if that one has ended) until somebody begins it, be it another thread that makes it in the end. Or a call would
    // lie behind one that waits, in the queue of a stranger, while the thread that could make it sits idle.
    struct CallsOwed {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CallsOwed);
        uint32_t thread { 0 };
        // Its place among the threads for one that makes the calls of all of them (takeNextCall()): given when something is first put into `calls`, or the thread first looks. 0: the main thread's, which comes
        // first; or none yet.
        uint32_t rank { 0 };
        // The queue, in two halves. What somebody who serves other threads has taken, which the thread leaves alone (ServesOtherThreads; there is something in it only while somebody serves), and the rest.
        Deque<CallOwed> taken;
        Deque<CallOwed> calls;
        // The rest of the groups that this thread has begun, from whichever queue, the last begun in front (one from the same place as that in front is merged with it). Nobody else takes from here.
        Deque<CallOwed> inHand;
        // Those groups, the last begun last.
        struct GroupInHand {
            size_t restOfHand; // So much of what is in hand lies behind it.
            // Where it stands: at `calls` of another thread, from which one that makes the calls of all threads has taken it (of the heir, if that thread has ended). noThread: in what is one queue to
            // this thread. See takeNextCall().
            uint32_t threadFromWhoseCalls;
        };
        Vector<GroupInHand, 2> groupsInHand;
        // One of what is in hand has been taken out of the middle, with so many behind it.
        void didRemoveFromHand(size_t behindIt)
        {
            size_t endOfGroup = inHand.size();
            for (size_t i = groupsInHand.size(); i--;) {
                auto& group = groupsInHand[i];
                if (group.restOfHand > behindIt)
                    --group.restOfHand;
                if (group.restOfHand == endOfGroup)
                    groupsInHand.removeAt(i);
                else
                    endOfGroup = group.restOfHand;
            }
        }
        // What a collection has noted for this thread until it queues it (queueCallbacks()): those for cyclic garbage while it marks, the others when it ends.
        Vector<PyWeakRef*> callbacksOfThisCollection;
        // A full collection has found some of these, and none has been taken since: the thread is in foreign code, or does not get its turn.
        bool hasStoodSinceFullCollection { false };
        bool hasFinalizers() const { return hasCallsThatNobodyHasBegun() || !inHand.isEmpty(); }
        bool hasCallsThatNobodyHasBegun() const { return !taken.isEmpty() || !calls.isEmpty(); }
        template<typename Function>
        void forEachCall(const Function& function) const
        {
            for (auto* queue : { &inHand, &taken, &calls }) {
                for (auto& call : *queue)
                    function(call);
            }
        }
    };
    // The first is the main thread's. A thread has one from the first time it owes something, or looks, until it ends.
    Vector<std::unique_ptr<CallsOwed>, 1> m_callsOwed WTF_GUARDED_BY_LOCK(m_finalizationLock);
    CallsOwed& callsOwedBy(uint32_t thread) WTF_REQUIRES_LOCK(m_finalizationLock);
    void didTakeFrom(CallsOwed&) WTF_REQUIRES_LOCK(m_finalizationLock);
    void rankIfItHasNone(CallsOwed&) WTF_REQUIRES_LOCK(m_finalizationLock);
    uint32_t m_lastRank WTF_GUARDED_BY_LOCK(m_finalizationLock) { 0 };
    uint32_t m_sequenceOfNextCall WTF_GUARDED_BY_LOCK(m_finalizationLock) { 0 };
    // The half of the queue for what a collection has found: whoever waits for the collection takes what it serves. What continues a group goes where the group began.
    Deque<CallOwed>& halfFor(CallsOwed&, JSCell*, uint32_t era, bool isWeakRef) WTF_REQUIRES_LOCK(m_finalizationLock);
    bool isOfEarlierEra(JSCell*, uint32_t era, bool isWeakRef) const; // Than m_eraThatIsServed.
    void queueCall(Deque<CallOwed>&, JSCell*, uint32_t era, bool isWeakRef = false, bool continuesGroup = false) WTF_REQUIRES_LOCK(m_finalizationLock);
    void noteCallback(CallsOwed&, PyWeakRef*) WTF_REQUIRES_LOCK(m_finalizationLock);
    void queueCallbacks(CallsOwed&, Vector<PyWeakRef*>&) WTF_REQUIRES_LOCK(m_finalizationLock); // It empties the list.
    // The next call for this thread to make: of a group that it has begun, or the first of a group, or a single call, that it may begin; the rest of the group is then in its hand.
    bool takeNextCall(CallsOwed& own, CallsOf, CallOwed&) WTF_REQUIRES_LOCK(m_finalizationLock);
    bool mayBeginCallsOf(const CallsOwed&, CallsOf) const WTF_REQUIRES_LOCK(m_finalizationLock);
    bool mayBeginWhatIsTakenOf(const CallsOwed&, CallsOf) const WTF_REQUIRES_LOCK(m_finalizationLock);
    bool mayBeginWhatIsHandedOutTo(const CallsOwed&, CallsOf) const WTF_REQUIRES_LOCK(m_finalizationLock);
    // By this thread, which finds it at the head of what is taken in its own queue: another thread serves it.
    bool isLeftToOneThatServes(const CallOwed&) const WTF_REQUIRES_LOCK(m_finalizationLock);
    // Its thread will not go on with the groups: they are groups like any other again.
    static void putBackWhatIsInHand(CallsOwed&);
    // A collection has left something in the queue, or found something there: its thread is told, unless all of it is taken.
    void didQueueCallsIn(CallsOwed&) WTF_REQUIRES_LOCK(m_finalizationLock);
    void tellThoseThatOwe() WTF_REQUIRES_LOCK(m_finalizationLock);
    static void mergeCalls(Deque<CallOwed>& from, Deque<CallOwed>& into);
    static void assertIsInOrder(const Deque<CallOwed>&);
    bool takenBecomesItsMakers() WTF_REQUIRES_LOCK(m_finalizationLock);
    void assertNothingIsTakenUnlessSomebodyServes() WTF_REQUIRES_LOCK(m_finalizationLock);
    Vector<ServesOtherThreads*, 2> m_thoseThatServeOthers WTF_GUARDED_BY_LOCK(m_finalizationLock);
    // The thread if it can still make calls, else the main one.
    uint32_t threadToMakeCallsOf(uint32_t thread);
    uint32_t m_currentThread { 0 };
    // Those whose finalizer has been called: those that a full collection has found alive since, and the others. Not visited. They were
    // kept by the collection that queued them, so they are old, and a full collection takes out what it finds dead.
    UncheckedKeyHashSet<JSCell*> m_finalized WTF_GUARDED_BY_LOCK(m_finalizationLock);
    Vector<JSCell*> m_justFinalized WTF_GUARDED_BY_LOCK(m_finalizationLock);
    size_t m_justFinalizedAndOld WTF_GUARDED_BY_LOCK(m_finalizationLock) { 0 }; // So many of the first.
    // gc.callbacks hear "start" and "stop" in turn, as in CPython, where nothing is collected between the two. So one thread at a time is between them: it has told the start of a collection and
    // will tell its end, and until then nobody tells anything. What is collected meanwhile goes untold; what was collected before and has not been told yet waits.
    // Of the engine's own collections, which are over by then: whoever comes first tells, and tells of the end when it has made the calls that it owes.
    Deque<CollectionNotice> m_collectionsToTellOf WTF_GUARDED_BY_LOCK(m_finalizationLock);
    uint32_t m_threadBetweenStartAndStop WTF_GUARDED_BY_LOCK(m_finalizationLock) { noThread };
    std::optional<CollectionNotice> m_collectionBeingToldOf WTF_GUARDED_BY_LOCK(m_finalizationLock); // None: that thread is in gc.collect().
    MonotonicTime m_collectionBegan;
    Vector<PyWeakRef*> weakRefsWithCallbacksToUnmarkedObjects(SlotVisitor&);
    UncheckedKeyHashSet<JSCell*> whatOtherThreadsHold();
    bool m_hasEnded { false };
    bool m_isPuttingFinalizersInOrder { false }; // In keepUnreachableObjectsToFinalize(), with m_finalizationLock.
    void end();
    bool m_hasPendingCalls { false };
    bool m_isRunningPendingCalls { false };
    // These two from where finalizeAllAtExit() lets go of the modules to where it is left.
    bool m_hasLetGoOfModules { false };
    // Not visited: a collection takes out what it finds dead, when it has put the finalizers in order.
    Vector<PyDict*> m_namespacesOfModulesLetGoOf;
    uint64_t m_mostLooksAtInnerReferences { 16'000'000 }; // See DeadGraph::candidatesWithHoldersFirst().
    void noteNamespacesOfModules();
    void* m_stackWhereUnaskedCallsBegan { nullptr };
    uint32_t m_threadWhereSignalHandlerRaised { noThread }; // In one of the calls it owed, which has not returned yet.
    bool m_hasKeptWhatSignalHandlerRaised { false };
    bool m_shouldCollectAfterPendingCalls { false };
    bool m_isCallingCollectionCallbacks { false };
    bool m_mayHavePendingOutput { false };
    MarkingObserver* m_markingObserver { nullptr };
    bool m_collectionIsWaitedFor { false }; // See WaitForCollection, for these three.
    bool m_leavesCyclicGarbageForLater { false };
    CallsOf m_callsThatAskerMakes { CallsOf::ThisThread };

    PyType* typeIfMadeForDefinitionAt(const void*) const WTF_IGNORES_THREAD_SAFETY_ANALYSIS;
    void keepTypeMadeForDefinitionAt(const void*, PyType*);
    Lock m_moduleTypesLock;
    UncheckedKeyHashMap<const void*, WriteBarrier<PyType>> m_moduleTypes WTF_GUARDED_BY_LOCK(m_moduleTypesLock);
};

#define PY_DEFINE_STRUCTURE_ACCESSOR(Class) \
    template<> inline Structure* Runtime::structure<Class>() const { return m_structures[static_cast<unsigned>(CellClass::Class)].get(); }
FOR_EACH_PY_CELL_CLASS(PY_DEFINE_STRUCTURE_ACCESSOR)
#undef PY_DEFINE_STRUCTURE_ACCESSOR

inline Runtime& runtime(JSGlobalObject* globalObject) { return Runtime::from(globalObject); }

} // namespace Python
} // namespace JSC
