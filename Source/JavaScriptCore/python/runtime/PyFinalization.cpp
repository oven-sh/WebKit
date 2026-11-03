/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyRuntime.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBuffer.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyExec.h"
#include "PyGenerator.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyStr.h"
#include "PyStructSeq.h"
#include "PyThreads.h"
#include "PyWeakRef.h"
#include "TopExceptionScope.h"
#include <wtf/SetForScope.h>

namespace JSC {


namespace Python {

// Finalizers, the callbacks of weak references, and the module gc. See the comment in PyRuntime.h.

void Runtime::registerFinalizable(JSCell* cell)
{
    {
        Locker locker { m_finalizationLock };
        m_finalizable.add(cell);
    }
    if (allocationTracebackLimit) [[unlikely]]
        noteWhereObjectWasMade(m_globalObject, cell);
}

void Runtime::unregisterFinalizable(JSCell* cell)
{
    Locker locker { m_finalizationLock };
    m_finalizable.remove(cell);
}

bool Runtime::keepUnreachableObjectsToFinalize(SlotVisitor& visitor)
{
    Locker locker { m_finalizationLock };
    // All of them are found before any is marked.
    Vector<JSCell*> candidates;
    for (JSCell* cell : m_finalizable) {
        if (!visitor.isMarked(cell))
            candidates.append(cell);
    }
    if (candidates.isEmpty()) {
        if (m_markingObserver)
            m_markingObserver->didFinishMarking();
        return false;
    }

    // An object is finalized before what it refers to. What another candidate reaches waits: it is kept alive by that one
    // now, and comes up again in the collection that finds that one gone, unless its finalizer found it a home.
    // That takes a collection per level. A structure that is deeper than a few levels (a linked list of objects with
    // finalizers) is finalized in one round, from the top down, as CPython's collector does with what hangs off a cycle.
    constexpr unsigned deepestToTakeLevelByLevel = 3;
    Vector<unsigned> ranks = finalizationRanks(visitor, candidates.span());
    unsigned deepest = 0;
    for (unsigned rank : ranks)
        deepest = std::max(deepest, rank);
    Vector<unsigned> order(candidates.size(), [](size_t i) {
        return static_cast<unsigned>(i);
    });
    if (deepest > deepestToTakeLevelByLevel) {
        std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
            return ranks[a] < ranks[b];
        });
    }
    for (unsigned i : order) {
        if (ranks[i] && deepest <= deepestToTakeLevelByLevel)
            continue;
        m_pendingFinalization.append(candidates[i]);
        m_finalizable.remove(candidates[i]);
        visitor.appendUnbarriered(candidates[i]);
    }
    didQueueCalls();
    return true;
}

void Runtime::queueWeakRefCallback(PyWeakRef* weakRef)
{
    Locker locker { m_finalizationLock };
    m_pendingWeakRefCallbacks.append(weakRef);
    m_pendingWeakRefCallbacksAreInOrder = false;
    didQueueCalls();
}

template<typename Visitor>
void Runtime::visitPendingCalls(Visitor& visitor)
{
    Locker locker { m_finalizationLock };
    for (JSCell* cell : m_pendingFinalization)
        visitor.appendUnbarriered(cell);
    for (PyWeakRef* weakRef : m_pendingWeakRefCallbacks)
        visitor.appendUnbarriered(weakRef);
}

template void Runtime::visitPendingCalls(AbstractSlotVisitor&);
template void Runtime::visitPendingCalls(SlotVisitor&);

// ---- Exceptions nobody can catch ----

static constexpr ASCIILiteral unraisableFields[] = { "exc_type"_s, "exc_value"_s, "exc_traceback"_s, "err_msg"_s, "object"_s };
static constexpr StructSeqDef unraisableDef { "sys.UnraisableHookArgs"_s, unraisableFields, 5 };

void reportUnraisable(JSGlobalObject* globalObject, const String& message, JSValue object)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue exception = takePendingException(globalObject);
    if (!exception)
        return;

    JSString* typeKey = internString(vm, "__unraisable_hook_args_type__"_s);
    JSValue type = rt.sysDict()->get(typeKey);
    if (!type) {
        type = makeStructSeqType(globalObject, unraisableDef);
        rt.sysDict()->set(vm, typeKey, type);
    }
    JSValue traceback = PyException::from(exception)->traceback();
    JSValue arguments = makeStructSeq(globalObject, PyType::from(type), {
        rt.typeOf(exception), exception, traceback ? traceback : jsUndefined(),
        message.isNull() ? jsUndefined() : JSValue(makeStr(vm, message)), object ? object : jsUndefined() });

    JSValue hook = rt.sysDict()->get(internString(vm, "unraisablehook"_s));
    if (hook && !audit(globalObject, "sys.unraisablehook"_s, hook, arguments)) {
        JSValue second = takePendingException(globalObject);
        writeToStderr(globalObject, "Exception ignored in audit hook:\n"_s);
        printException(globalObject, second);
        hook = { };
    }
    if (hook && !isNone(hook)) {
        call(globalObject, hook, arguments);
        if (!scope.exception())
            return;
        JSValue second = takePendingException(globalObject);
        writeToStderr(globalObject, "Exception ignored in sys.unraisablehook:\n"_s);
        printException(globalObject, second);
    }
    writeToStderr(globalObject, makeString(message.isNull() ? "Exception ignored"_str : message, ":\n"_s));
    printException(globalObject, exception);
}

// ---- Running what is queued ----

// Never raises: what goes wrong is reported.
void warnOfUnawaitedCoroutine(JSGlobalObject* globalObject, PyGenerator* coroutine)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto report = [&] {
        if (scope.exception())
            reportUnraisable(globalObject, makeString("Exception ignored while finalizing coroutine "_s, reprForMessage(globalObject, coroutine)), { });
    };
    // warnings.py words it, as it can say where the coroutine was made. If that fails for any reason but that the filters make
    // the warning an exception, the warning is given here.
    bool hasWarned = false;
    JSValue module = importModule(globalObject, "warnings"_s);
    if (scope.exception() && clearPendingExceptionIfMatches(globalObject, rt.ImportError()))
        module = { };
    JSValue function = module ? getAttrIfPresent(globalObject, module, internString(vm, "_warn_unawaited_coroutine"_s)) : JSValue();
    if (function) {
        call(globalObject, function, coroutine);
        hasWarned = !scope.exception() || pendingExceptionMatches(globalObject, rt.RuntimeWarning());
    }
    report();
    if (hasWarned)
        return;
    warnAbout(globalObject, rt.RuntimeWarning(), makeString("coroutine "_s, reprForMessage(globalObject, coroutine->qualname()), " was never awaited"_s), 1, coroutine);
    report();
}

static void finalizeAwaitable(JSGlobalObject* globalObject, PyAsyncGenAwaitable* awaitable)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(rt.vm());
    if (awaitable->state() != PyAsyncGenAwaitable::State::Initial)
        return;
    using Kind = PyAsyncGenAwaitable::Kind;
    ASCIILiteral method = awaitable->kind() == Kind::Send ? "asend"_s : awaitable->kind() == Kind::Throw ? "athrow"_s : "aclose"_s;
    PyGenerator* generator = awaitable->generator();
    warnAbout(globalObject, rt.RuntimeWarning(), makeString("coroutine method '"_s, method, "' of "_s, reprForMessage(globalObject, generator->qualname()), " was never awaited"_s), 1, generator);
    if (scope.exception())
        reportUnraisable(globalObject, makeString("Exception ignored while finalizing async generator "_s, reprForMessage(globalObject, generator)), { });
}

static void finalizeGenerator(JSGlobalObject* globalObject, PyGenerator* generator)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (generator->isCompleted() || generator->isRunning())
        return;

    if (generator->kind() == PyGenerator::Kind::AsyncGenerator) {
        // The event loop asked to be the one to close it.
        if (JSValue finalizer = generator->finalizer(); finalizer && !generator->isClosed) {
            call(globalObject, finalizer, generator);
            if (scope.exception())
                reportUnraisable(globalObject, makeString("Exception ignored while finalizing generator "_s, reprForMessage(globalObject, generator)), { });
            return;
        }
    }
    if (generator->kind() == PyGenerator::Kind::Coroutine && !generator->isStarted()) {
        warnOfUnawaitedCoroutine(globalObject, generator);
        generator->complete();
        return;
    }
    generatorClose(globalObject, generator);
    if (scope.exception())
        reportUnraisable(globalObject, makeString("Exception ignored while closing generator "_s, reprForMessage(globalObject, generator)), { });
}

static void finalizeObject(JSGlobalObject* globalObject, JSCell* cell)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (auto* generator = PyGenerator::tryFrom(cell))
        return finalizeGenerator(globalObject, generator);
    if (auto* awaitable = PyAsyncGenAwaitable::tryFrom(cell))
        return finalizeAwaitable(globalObject, awaitable);
    JSValue method = rt.typeOf(cell)->lookup(Special::Del);
    if (!method)
        return;
    callSpecial(globalObject, method, cell);
    if (scope.exception())
        reportUnraisable(globalObject, makeString("Exception ignored while calling deallocator "_s, reprForMessage(globalObject, method)), { });
}

// The collector's thread may be the one that calls this.
void Runtime::didQueueCalls()
{
    m_hasPendingCalls = true;
    noteCallsOwed(m_vm);
}

static std::atomic<void (*)(VM&)> s_callsOwedFunction;

void setCallsOwedFunction(void (*function)(VM&))
{
    s_callsOwedFunction.store(function);
}

void noteCallsOwed(VM& vm)
{
    vm.traps().fireTrap(VMTraps::NeedPythonCalls);
    if (auto* function = s_callsOwedFunction.load())
        function(vm);
}

void makeCallsOwed(VM& vm)
{
    vm.traps().clearTrap(VMTraps::NeedPythonCalls);
    if (vm.heap.pythonRuntimes().isEmpty())
        return;
    // Another thread has waited long enough for its turn.
    if (Threads& threads = vm.heap.pythonRuntimes().first()->threads(); threads.dropIsRequested()) [[unlikely]]
        threads.yield();
    // What other threads had to leave to the main one.
    if (vm.heap.hasPostponedEmbedderFinalizers() && vm.heap.mayRunEmbedderFinalizers()) [[unlikely]]
        vm.heap.runPostponedEmbedderFinalizers();
    // What is called may start Python in another global object, and a collection may find one gone.
    Vector<Runtime*, 1> runtimes = vm.heap.pythonRuntimes();
    for (Runtime* runtime : runtimes) {
        if (!vm.heap.pythonRuntimes().contains(runtime))
            continue;
        // What the handler of a signal raises is thrown. The trap is set again then: the rest is for the next safe point.
        if (!checkSignals(runtime->globalObject()))
            return;
        if (runtime->hasPendingCalls())
            runtime->runPendingCalls();
    }
}

void Runtime::runPendingCalls()
{
    // What is queued while these run is run by the loop below, not within one of the calls.
    if (m_isRunningPendingCalls)
        return;
    SetForScope isRunning(m_isRunningPendingCalls, true);
    JSGlobalObject* globalObject = m_globalObject;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
    SetForScope savedFrame(reraisingFrame, nullptr);
    // For finalizers that make new garbage with such finalizers for ever.
    unsigned collectionsLeft = 8;

    while (true) {
        // Once out of the queue, only the stack holds it.
        JSCell* object = nullptr;
        PyWeakRef* weakRef = nullptr;
        bool isDone = false;
        {
            Locker locker { m_finalizationLock };
            if (!m_pendingWeakRefCallbacks.isEmpty()) {
                // The references to an object are told of its death latest first. The collector finds them in no order.
                if (!std::exchange(m_pendingWeakRefCallbacksAreInOrder, true)) {
                    Vector<PyWeakRef*> all(m_pendingWeakRefCallbacks.size(), [&](size_t) {
                        return m_pendingWeakRefCallbacks.takeFirst();
                    });
                    std::ranges::sort(all, [](PyWeakRef* a, PyWeakRef* b) {
                        return a->serial() > b->serial();
                    });
                    for (PyWeakRef* each : all)
                        m_pendingWeakRefCallbacks.append(each);
                }
                weakRef = m_pendingWeakRefCallbacks.takeFirst();
            } else if (!m_pendingFinalization.isEmpty())
                object = m_pendingFinalization.takeFirst();
            else {
                m_hasPendingCalls = false;
                isDone = true;
            }
        }
        if (isDone) {
            if (!std::exchange(m_shouldCollectAfterPendingCalls, false) || !collectionsLeft--)
                return;
            m_vm.clearLastException();
            m_vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
            continue;
        }
        if (weakRef) {
            JSValue callback = weakRef->callback();
            weakRef->clearCallback();
            if (callback) {
                call(globalObject, callback, weakRef);
                if (scope.exception())
                    reportUnraisable(globalObject, makeString("Exception ignored while calling weakref callback "_s, reprForMessage(globalObject, callback)), { });
            }
        } else
            finalizeObject(globalObject, object);
        ensureStillAliveHere(object);
        ensureStillAliveHere(weakRef);
    }
}

void Runtime::finalizeAllAtExit()
{
    auto isDone = [&] {
        Locker locker { m_finalizationLock };
        return m_finalizable.isEmpty() && m_pendingFinalization.isEmpty() && m_pendingWeakRefCallbacks.isEmpty();
    };
    auto collect = [&] {
        for (unsigned round = 0; round < 64 && !isDone(); ++round) {
            m_vm.clearLastException();
            m_vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
            if (!hasPendingCalls())
                break;
            runPendingCalls();
        }
    };
    // A program that leaves nothing to finalize exits without further ado.
    // Otherwise, first what is garbage already, as it would have been all along with reference counts.
    collect();
    if (isDone())
        return;
    // Then the modules go, as they do when CPython exits, and with them what only they hold: the globals of the program.
    // The collector finds that, and puts it in order. (CPython also sets the globals to None, one after the other, to get at
    // what cycles keep alive. Here finalizers find them as they were.)
    for (auto name : { "path"_s, "argv"_s, "ps1"_s, "ps2"_s, "last_exc"_s, "last_type"_s, "last_value"_s, "last_traceback"_s, "__interactivehook__"_s, "path_hooks"_s, "path_importer_cache"_s, "meta_path"_s })
        sysDict()->remove(m_vm, internString(m_vm, name));
    builtins()->remove(m_vm, internString(m_vm, "_"_s));
    modules()->clear(m_vm);
    collect();
    // Then what something else holds on to: JS, a frame that is still on the stack. In the order it was made.
    {
        Locker locker { m_finalizationLock };
        for (JSCell* cell : m_finalizable) {
            // A generator someone still holds is not closed, nor is a coroutine complained about.
            if (cell->type() != PyGeneratorType)
                m_pendingFinalization.append(cell);
        }
        m_finalizable.clear();
        didQueueCalls();
    }
    runPendingCalls();
}

// ---- The module gc ----

static JSValue gcCollect(JSGlobalObject* globalObject, JSValue generation)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (generation) {
        int number = toCInt(globalObject, generation);
        RETURN_IF_EXCEPTION(scope, { });
        if (number < 0 || number > 2)
            return raiseValueError(globalObject, scope, "invalid generation"_s);
    }
    // From a finalizer, or the callback of a weak reference. Nothing that a collection found now could be finalized before this
    // call returns, and a hundred finalizers that do this would make for a hundred collections, or, if it is a call of this
    // function that runs them, as many times the rounds below. (In CPython a collection within a collection does nothing.)
    if (rt.isRunningPendingCalls()) {
        rt.collectAfterPendingCalls();
        return jsNumber(0);
    }
    collectAndFinalize(globalObject);
    // FIXME: How many unreachable objects were found is not known.
    return jsNumber(0);
}

// A program that asks for a collection expects what it has dropped to be gone afterwards. The collector takes every word of the
// stack that looks like a reference for one, and the frames of C++ between here and there will lie where those of earlier
// calls were: what they do not write would still refer to what those dealt with. So that is wiped first.
static NEVER_INLINE void wipeStackBelow(VM& vm)
{
    constexpr size_t size = 128 * KB;
    char* top = static_cast<char*>(currentStackPointer());
    char* limit = static_cast<char*>(vm.softStackLimit());
    if (top <= limit || static_cast<size_t>(top - limit) < size + 16 * KB)
        return;
    volatile char* wiped = static_cast<volatile char*>(alloca(size));
    for (size_t i = 0; i < size; i += sizeof(uintptr_t))
        *reinterpret_cast<volatile uintptr_t*>(wiped + i) = 0;
}

void collectAndFinalize(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    ASSERT(!rt.isRunningPendingCalls());
    // An object with a finalizer takes two rounds: one finds it, the next frees it, which may clear weak references in turn
    // and finds what only it referred to. The limit is for finalizers that make new garbage with finalizers for ever.
    for (unsigned round = 0; round < 64; ++round) {
        // The VM remembers the last exception thrown, for the debugger, long after it has been handled, and with it the
        // traceback and all that the frames in it refer to.
        vm.clearLastException();
        wipeStackBelow(vm);
        vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
        if (!rt.hasPendingCalls())
            break;
        rt.runPendingCalls();
    }
}

static JSString* gcStateKey(VM& vm, ASCIILiteral name) { return internString(vm, name); }

static JSValue gcEnable(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.sysDict()->set(rt.vm(), gcStateKey(rt.vm(), "__gc_disabled__"_s), jsBoolean(false));
    return jsUndefined();
}

// The collector cannot be turned off: it is the only thing that frees memory. What was asked for is remembered.
static JSValue gcDisable(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.sysDict()->set(rt.vm(), gcStateKey(rt.vm(), "__gc_disabled__"_s), jsBoolean(true));
    return jsUndefined();
}

static JSValue gcIsEnabled(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    JSValue disabled = rt.sysDict()->get(gcStateKey(rt.vm(), "__gc_disabled__"_s));
    return jsBoolean(!disabled || !disabled.asBoolean());
}

static JSValue gcGetThreshold(JSGlobalObject* globalObject) { return PyTuple::create(globalObject, { jsNumber(2000), jsNumber(10), jsNumber(0) }); }
static JSValue gcSetThreshold(JSGlobalObject*, const CallArgs&) { return jsUndefined(); }
static JSValue gcGetCount(JSGlobalObject* globalObject) { return PyTuple::create(globalObject, { jsNumber(0), jsNumber(0), jsNumber(0) }); }
static JSValue gcGetDebug(JSGlobalObject*) { return jsNumber(0); }
static JSValue gcSetDebug(JSGlobalObject*, JSValue) { return jsUndefined(); }
static JSValue gcGetObjects(JSGlobalObject* globalObject, JSValue generation)
{
    if (!audit(globalObject, "gc.get_objects"_s, generation && !isNone(generation) ? generation : jsNumber(-1)))
        return { };
    return newList(globalObject);
}
template<bool referrers>
static JSValue gcGetRelated(JSGlobalObject* globalObject, const CallArgs& args)
{
    if (isAudited(globalObject) && !audit(globalObject, referrers ? "gc.get_referrers"_s : "gc.get_referents"_s, PyTuple::create(globalObject, args.positional())))
        return { };
    return newList(globalObject);
}
static JSValue gcNothing(JSGlobalObject*) { return jsUndefined(); }
static JSValue gcZero(JSGlobalObject*) { return jsNumber(0); }
static JSValue gcIsTracked(JSGlobalObject*, JSValue object) { return jsBoolean(object.isCell() && !object.isString() && !object.isHeapBigInt()); }
static JSValue gcIsFinalized(JSGlobalObject*, JSValue) { return jsBoolean(false); }

static JSValue gcGetStats(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    MarkedArgumentBuffer stats;
    for (unsigned i = 0; i < 3; ++i) {
        PyDict* dict = PyDict::create(globalObject);
        for (auto name : { "collections"_s, "collected"_s, "uncollectable"_s })
            dict->set(vm, internString(vm, name), jsNumber(0));
        stats.append(dict);
    }
    return newList(globalObject, spanOf(stats));
}

static constexpr NativeDef gcFunctions[] = {
    PY_FUNCTION("collect", gcCollect, "(generation=2)"),
    PY_FUNCTION("enable", gcEnable, "()"),
    PY_FUNCTION("disable", gcDisable, "()"),
    PY_FUNCTION("isenabled", gcIsEnabled, "()"),
    PY_FUNCTION("get_threshold", gcGetThreshold, "()"),
    PY_FUNCTION("set_threshold", gcSetThreshold, "(*args)"),
    PY_FUNCTION("get_count", gcGetCount, "()"),
    PY_FUNCTION("get_debug", gcGetDebug, "()"),
    PY_FUNCTION("set_debug", gcSetDebug, "(flags, /)"),
    PY_FUNCTION("get_stats", gcGetStats, "()"),
    // FIXME: The heap can be walked; who refers to whom takes a heap snapshot.
    PY_FUNCTION("get_objects", gcGetObjects, "(generation=None)"),
    PY_FUNCTION("get_referrers", gcGetRelated<true>, "(*objs)"),
    PY_FUNCTION("get_referents", gcGetRelated<false>, "(*objs)"),
    PY_FUNCTION("is_tracked", gcIsTracked, "(obj, /)"),
    PY_FUNCTION("is_finalized", gcIsFinalized, "(obj, /)"),
    PY_FUNCTION("freeze", gcNothing, "()"),
    PY_FUNCTION("unfreeze", gcNothing, "()"),
    PY_FUNCTION("get_freeze_count", gcZero, "()"),
};


void initializeGCModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, gcFunctions);
    module->add(globalObject, "garbage"_s, newList(globalObject));
    module->add(globalObject, "callbacks"_s, newList(globalObject));
    module->add(globalObject, "DEBUG_STATS"_s, jsNumber(1));
    module->add(globalObject, "DEBUG_COLLECTABLE"_s, jsNumber(2));
    module->add(globalObject, "DEBUG_UNCOLLECTABLE"_s, jsNumber(4));
    module->add(globalObject, "DEBUG_SAVEALL"_s, jsNumber(32));
    module->add(globalObject, "DEBUG_LEAK"_s, jsNumber(38));
}

} // namespace Python
} // namespace JSC
