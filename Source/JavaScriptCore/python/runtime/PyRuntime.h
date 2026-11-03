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
#include <wtf/ListHashSet.h>
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {
namespace Python {

// Where a frame of Python code keeps its frame object once somebody has asked for it: the first register after those that
// every frame of JS has. Until then it holds undefined, which is what op_enter leaves there. op_py_ret looks at it.
VirtualRegister frameObjectRegister();
class Monitoring;

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
    v(jsFunction) \
    v(jsPromise)

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
struct StrIndexCache;
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
    Structure* keywordMarkerStructure() const { return m_keywordMarkerStructure.get(); }

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

    JSString* name(NameID id) const { return m_names[static_cast<unsigned>(id)].get(); }
    JSString* name(Special special) const { return m_names[static_cast<unsigned>(special)].get(); }
    struct Names {
#define PY_DEFINE_ACCESSOR(id, text) JSString* id() const { return runtime.name(NameID::id); }
        FOR_EACH_PY_NAME(PY_DEFINE_ACCESSOR)
#undef PY_DEFINE_ACCESSOR
        const Runtime& runtime;
    };
    const Names names { *this };

    // The type of any value a Python program can hold.
    PyType* typeOf(JSValue) const;

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
    void setCurrentContext(JSValue value) { setSlot(Slot::CurrentContext, value); }

    // State of the native modules that programs must not see (it is not in a module's dict) and that outlives the modules.
    enum class Slot : unsigned {
        CurrentContext,
        NativeModules, // dict: name -> module
        FindAndLoad, // importlib._bootstrap._find_and_load, once importlib is the import system
        Monitoring, // See Monitoring::state().
        TraceFunction, // sys.settrace() and sys.setprofile(), of the thread that is running. Empty: none.
        ProfileFunction,
        AtexitCallbacks, // list
        AuditHooks, // list: sys.addaudithook(). Empty: there are none.
        AllocationTracebacks, // A JSWeakMap: see PyProfilers.cpp.
        Warnings, // See warningsState() in PyWarnings.cpp.
        SignalHandlers, // list
        ThrownException, // What generator.throw() is throwing,
        ThrowArguments, // and the tuple of arguments it was made from.
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
        Count
    };
    JSValue slot(Slot which) const { return m_slots[static_cast<unsigned>(which)].get(); }
    void setSlot(Slot which, JSValue value) { set(m_slots[static_cast<unsigned>(which)], value); }

    // A key under which an object that has no field for it keeps its __dict__.
    PrivateName dictKey() const { return m_dictKey; }
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
    JSValue handledExceptionInTopSlot() const;
    JSValue setHandledExceptionInTopSlot(JSValue); // Returns what was there.
    JSValue handledException() const; // In the topmost slot that has one. Empty: none.
    PyGenerator* runningGenerator() const { return m_runningGenerator.get(); }
    void setRunningGenerator(PyGenerator*);

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

    // What of all this belongs to the thread that is running. It goes with the thread when another takes its place. It refers
    // to cells, and is visited by nobody: it must be on the stack of the thread.
    struct ThreadLocalState {
        JSValue currentContext;
        JSValue handledException;
        PyGenerator* runningGenerator { nullptr };
        JSValue asyncGenFirstIter;
        JSValue asyncGenFinalizer;
        JSValue pendingKeywordNames;
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
        Vector<JSCell*, 8> reprStack;
        CallFrame* reraisingFrame { nullptr };
        RuntimeCall* runtimeCall { nullptr };
        int frameDepth { 0 };
        unsigned buffersThatKeepGIL { 0 };
        bool isRunningPendingCalls { false };
    };
    // Leaves the runtime as a thread that has just started finds it.
    void takeThreadLocalState(ThreadLocalState&);
    void putThreadLocalState(ThreadLocalState&);
    // A JSWeakMap: for each _thread._local object this thread has used, the dict that holds the thread's attributes of it.
    JSValue threadLocals() const { return m_threadLocals.get(); }
    void setThreadLocals(JSValue value) { set(m_threadLocals, value); }

    // ---- sys.monitoring, sys.settrace(), sys.setprofile(): see PyMonitoring.h ----

    Monitoring& monitoring() { return *m_monitoring; }
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

    void registerFinalizable(JSCell*);
    void unregisterFinalizable(JSCell*);
    // For the collector, with the world stopped, once all that is reachable is marked. True: it marked something.
    bool keepUnreachableObjectsToFinalize(SlotVisitor&);
    // For a weak reference whose referent has just died.
    void queueWeakRefCallback(PyWeakRef*);

    ALWAYS_INLINE bool hasPendingCalls() const { return m_hasPendingCalls; }
    // At a safe point. What the calls raise is reported and goes no further.
    void runPendingCalls();
    // One of those calls is running, in this thread. What it queues waits for it to return.
    bool isRunningPendingCalls() const { return m_isRunningPendingCalls; }
    // gc.collect() in one of those calls: there is a collection when all that are queued have been made.
    void collectAfterPendingCalls() { m_shouldCollectAfterPendingCalls = true; }
    void didQueueCalls();
    // When the interpreter exits: whatever is still registered is finalized, reachable or not.
    void finalizeAllAtExit();
    // See the function of that name in PyBuffer.h.
    void releaseUnreachableBuffers(bool thorough);
    // Told, with the world stopped, when a collection has marked all that it will.
    class MarkingObserver {
    public:
        virtual ~MarkingObserver() = default;
        virtual void didFinishMarking() = 0;
    };

    // ---- What an instruction leaves for the next ----

    JSValue takePendingKeywordNames() { return take(m_pendingKeywordNames); }
    void setPendingKeywordNames(JSValue value) { set(m_pendingKeywordNames, value); }
    JSValue takeSendResult() { return take(m_sendResult); }
    void setSendResult(JSValue value) { set(m_sendResult, value); }
    JSValue takeExceptionGroupRest() { return take(m_exceptionGroupRest); }
    void setExceptionGroupRest(JSValue value) { set(m_exceptionGroupRest, value); }

    // The objects whose repr() is being computed, to catch one that contains itself.
    Vector<JSCell*, 8> reprStack;

    // The code object of each piece of code that has been asked for one, for as long as something holds it.
    WeakGCMap<FunctionExecutable*, PyCode> codeObjects;

    // See StrView.
    std::unique_ptr<StrIndexCache, void (*)(StrIndexCache*)> strIndexCache { nullptr, nullptr };

    // The frame in which an exception is being raised again: it is in the traceback already.
    CallFrame* reraisingFrame { nullptr };
    // finalizeInterpreter() has run, or is running.
    bool isFinalized { false };

    // ---- Settings ----

    int optimizationLevel { 0 }; // -O
    int maxStrDigits { 4300 };
    int recursionLimit { 1000 };
    // tracemalloc.start(): how many frames to note of where an object was made. 0: none, it is off. See PyProfilers.cpp.
    unsigned allocationTracebackLimit { 0 };
    int coroutineOriginTrackingDepth { 0 }; // sys.set_coroutine_origin_tracking_depth()
    // How many frames of Python code there are. op_py_bind_args counts a frame in; op_py_ret, or the unwinder when an exception
    // leaves the frame, counts it out. The body of a generator has neither instruction: PyGenerator::resume() counts it.
    int frameDepth { 0 };
    // How many buffers this thread holds of memory that JS could take away if it ran: see PyBuffer.h.
    unsigned buffersThatKeepGIL { 0 };
    // False: there are as many as there may be, and a RecursionError is raised. Otherwise leaveFrame() has to follow.
    bool tryEnterFrame();
    void leaveFrame() { --frameDepth; }
    // The call is about to end, by a return or an exception. Its frame object, if it has one, takes over what the frame holds.
    // (A generator that yields, or ends without an exception, tells its frame object itself.)
    void frameIsEnding(CallFrame*);

    // ---- For the interpreter (LowLevelInterpreter64.asm) ----

    StructureID iteratorStructureID; // Of PyIterator.
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
    WriteBarrier<Structure> m_keywordMarkerStructure;
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
    WriteBarrier<PyGenerator> m_runningGenerator;
    WriteBarrier<Unknown> m_asyncGenFirstIter;
    WriteBarrier<Unknown> m_asyncGenFinalizer;
    WriteBarrier<Unknown> m_threadLocals;
    RefPtr<Threads> m_threads;

    WriteBarrier<Unknown> m_pendingKeywordNames;
    WriteBarrier<Unknown> m_sendResult;
    WriteBarrier<Unknown> m_exceptionGroupRest;

    PrivateName m_dictKey;
    std::array<PrivateName, numberOfHiddenKeys> m_hiddenKeys;
    std::array<PrivateName, 3> m_instanceKeys;
    std::unique_ptr<Monitoring> m_monitoring;

    Lock m_finalizationLock;
    ListHashSet<JSCell*> m_finalizable WTF_GUARDED_BY_LOCK(m_finalizationLock); // Not visited.
    Deque<JSCell*> m_pendingFinalization WTF_GUARDED_BY_LOCK(m_finalizationLock);
    Deque<PyWeakRef*> m_pendingWeakRefCallbacks WTF_GUARDED_BY_LOCK(m_finalizationLock);
    bool m_pendingWeakRefCallbacksAreInOrder WTF_GUARDED_BY_LOCK(m_finalizationLock) { true };
    bool m_hasPendingCalls { false };
    bool m_isRunningPendingCalls { false };
    bool m_shouldCollectAfterPendingCalls { false };
    MarkingObserver* m_markingObserver { nullptr };

    Lock m_moduleTypesLock;
    UncheckedKeyHashMap<const TypeDef*, WriteBarrier<PyType>> m_moduleTypes WTF_GUARDED_BY_LOCK(m_moduleTypesLock);
};

#define PY_DEFINE_STRUCTURE_ACCESSOR(Class) \
    template<> inline Structure* Runtime::structure<Class>() const { return m_structures[static_cast<unsigned>(CellClass::Class)].get(); }
FOR_EACH_PY_CELL_CLASS(PY_DEFINE_STRUCTURE_ACCESSOR)
#undef PY_DEFINE_STRUCTURE_ACCESSOR

inline Runtime& runtime(JSGlobalObject* globalObject) { return Runtime::from(globalObject); }

} // namespace Python
} // namespace JSC
