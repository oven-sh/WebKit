/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyRuntime.h"

#include "JSCInlines.h"
#include "MarkedSpaceInlines.h"
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
#include "SubspaceInlines.h"
#include "TopExceptionScope.h"
#include <wtf/SetForScope.h>

namespace JSC {


namespace Python {

// Finalizers, the callbacks of weak references, and the module gc. See the comment in PyRuntime.h.

static FinalizerState* finalizerStateOf(JSCell* cell)
{
    if (cell->type() != PyObjectType)
        return nullptr;
    const ClassInfo* info = cell->classInfo();
    if (info == PyRecord::info())
        return &uncheckedDowncast<PyRecord>(cell)->finalizerState();
    if (info == PyManagedBuffer::info())
        return &uncheckedDowncast<PyManagedBuffer>(cell)->finalizerState();
    return nullptr;
}

// These say for themselves whether they are in the list, or are put there once by whoever makes them.
static bool isListedOnceByItself(JSCell* cell)
{
    return cell->type() == PyInstanceType || cell->type() == PyGeneratorType || cell->classInfo() == PyAsyncGenAwaitable::info();
}

void Runtime::registerFinalizable(JSCell* cell)
{
    if (FinalizerState* state = finalizerStateOf(cell)) {
        state->isWanted = true;
        if (std::exchange(state->isListed, true))
            return;
    } else if (!isListedOnceByItself(cell) && !m_otherFinalizable.add(cell).isNewEntry)
        return;
    m_youngFinalizable.append(cell);
    if (allocationTracebackLimit) [[unlikely]]
        didRegisterWhileTracingAllocations(cell);
}

void Runtime::didRegisterWhileTracingAllocations(JSCell* cell)
{
    noteWhereObjectWasMade(m_globalObject, cell);
}

void Runtime::unregisterFinalizable(PyRecord* record)
{
    record->finalizerState().isWanted = false;
}

void Runtime::unregisterFinalizable(PyManagedBuffer* buffer)
{
    buffer->finalizerState().isWanted = false;
}

void Runtime::registerFinalizableLate(const MarkedArgumentBuffer& objects)
{
    UncheckedKeyHashSet<JSCell*> known;
    forEachFinalizable([&](JSCell* cell) {
        known.add(cell);
    });
    {
        Locker locker { m_finalizationLock };
        // A finalizer is called once.
        for (JSCell* cell : m_finalized)
            known.add(cell);
        for (JSCell* cell : m_justFinalized)
            known.add(cell);
        for (JSCell* cell : m_pendingFinalization)
            known.add(cell);
    }
    for (unsigned i = 0; i < objects.size(); ++i) {
        if (JSCell* cell = objects.at(i).asCell(); known.add(cell).isNewEntry)
            registerFinalizable(cell);
    }
}

// Whether it is in the list for nothing: whatever it had to finalize has been seen to.
static bool isNoLongerWanted(JSCell* cell)
{
    if (cell->type() == PyInstanceType)
        return false;
    if (cell->type() == PyGeneratorType)
        return uncheckedDowncast<PyGenerator>(cell)->finalization() == PyGenerator::Finalization::None;
    if (FinalizerState* state = finalizerStateOf(cell))
        return !state->isWanted;
    if (auto* awaitable = PyAsyncGenAwaitable::tryFrom(cell))
        return awaitable->state() != PyAsyncGenAwaitable::State::Initial;
    return false;
}

// Dead or alive.
void Runtime::didTakeOffList(JSCell* cell)
{
    if (cell->type() == PyInstanceType)
        return;
    if (cell->type() == PyGeneratorType)
        uncheckedDowncast<PyGenerator>(cell)->setFinalization(PyGenerator::Finalization::None);
    else if (FinalizerState* state = finalizerStateOf(cell))
        *state = { };
    else if (!isListedOnceByItself(cell))
        m_otherFinalizable.remove(cell);
}

// Those that still have something to finalize, as far as they know, in the order in which they were registered.
void Runtime::forEachFinalizable(const ScopedLambda<void(JSCell*)>& function)
{
    for (auto* list : { &m_oldFinalizable, &m_youngFinalizable }) {
        for (JSCell* cell : *list) {
            if (!isNoLongerWanted(cell))
                function(cell);
        }
    }
}

// A generator that has not begun has nothing to end. (A coroutine that has not begun is complained about.)
static bool hasNothingToFinalize(JSCell* cell)
{
    if (cell->type() != PyGeneratorType)
        return false;
    auto* generator = uncheckedDowncast<PyGenerator>(cell);
    return !generator->isStarted() && generator->kind() != PyGenerator::Kind::Coroutine && !generator->finalizer();
}

// Its finalizer is code of the program, or may be. What the finalizers of the built-in types do (close a file) looks the same in any
// order, as long as an object comes before what it refers to.
static bool hasFinalizerOfProgram(Runtime& rt, JSCell* cell)
{
    return cell->type() == PyGeneratorType || rt.typeOf(cell)->isHeapType();
}

bool Runtime::keepUnreachableObjectsToFinalize(SlotVisitor& visitor)
{
    Locker locker { m_finalizationLock };
    // All of them are found before any is marked.
    Vector<JSCell*> candidates;
    Vector<JSCell**> places;
    unsigned finalizersOfProgram = 0;
    auto look = [&](Vector<JSCell*>& list) {
        for (JSCell*& cell : list) {
            if (!cell)
                continue;
            if (isNoLongerWanted(cell)) {
                didTakeOffList(cell);
                cell = nullptr;
                continue;
            }
            // What has nothing to finalize is left to die, unless one of the others reaches it: see didMarkAll().
            if (visitor.isMarked(cell) || hasNothingToFinalize(cell))
                continue;
            candidates.append(cell);
            places.append(&cell);
            finalizersOfProgram += hasFinalizerOfProgram(*this, cell);
        }
    };
    if (m_vm.heap.collectionScope() == CollectionScope::Full)
        look(m_oldFinalizable);
    look(m_youngFinalizable);
    auto finish = [&]() WTF_REQUIRES_LOCK(m_finalizationLock) {
        if (m_markingObserver && m_markingObserver->didFinishMarking(visitor))
            return true;
        didMarkAll(visitor);
        return false;
    };
    // A collection that was under way when the program asked for one is finished first.
    bool wasAskedFor = m_scopeAskedFor && (*m_scopeAskedFor == CollectionScope::Eden || m_vm.heap.collectionScope() == CollectionScope::Full);
    bool wantsGarbage = wasAskedFor && m_markingObserver && m_markingObserver->wantsGarbage();
    // That takes time for every dead cell, which at exit is every cell there is.
    bool looksAtAllThatDied = wasAskedFor && (wantsGarbage || finalizersOfProgram > 1);
    if (candidates.isEmpty() && !wantsGarbage)
        return finish();

    // An object is finalized before what it refers to.
    Vector<PyWeakRef*> weakRefs;
    if (!candidates.isEmpty() && !m_leavesCyclicGarbageForLater)
        weakRefs = weakRefsWithCallbacksToUnmarkedObjects(visitor);
    Vector<JSCell*> referents = weakRefs.map([](PyWeakRef* weakRef) {
        return weakRef->referent();
    });
    FinalizationOrder order = finalizationOrder(m_vm.heap, visitor, candidates.span(), { referents.span(), looksAtAllThatDied, wantsGarbage, m_leavesCyclicGarbageForLater });

    // All that the candidates reach is about to be marked, so the weak references to it stay as they are until the finalizers have
    // run and another collection finds it dead: first __del__, then the weak references, as with reference counts. But what is
    // in a cycle, or hangs off one, is not freed by reference counts, and CPython's collector does it the other way round for
    // the weak references that have callbacks: it clears them, calls the callbacks of those that are not garbage
    // themselves, and then calls the finalizers. (So such a reference stays dead if a finalizer brings its referent back.)
    for (unsigned i = 0; i < weakRefs.size(); ++i) {
        if (!order.isCyclicGarbageToFinalize[i])
            continue;
        PyWeakRef* weakRef = weakRefs[i];
        weakRef->setReferent(nullptr);
        if (visitor.isMarked(weakRef)) {
            m_pendingWeakRefCallbacks.append(weakRef);
            m_pendingWeakRefCallbacksAreInOrder = false;
        } else
            weakRef->clearCallback();
    }

    for (unsigned n = 0; n < order.now.size(); ++n) {
        JSCell* cell = candidates[order.now[n]];
        visitor.appendUnbarriered(cell);
        if (m_leavesCyclicGarbageForLater && n >= order.freedByReferenceCounts)
            continue;
        m_pendingFinalization.append(cell);
        *places[order.now[n]] = nullptr;
        didTakeOffList(cell);
    }
    // The others are kept alive by those. They come up again, in this order, in the collection that finds those gone.
    for (unsigned i : order.later) {
        *places[i] = nullptr;
        m_finalizableToComeLast.append(candidates[i]);
    }
    if (!candidates.isEmpty())
        didQueueCalls();

    Vector<JSCell*> toKeep;
    if (wantsGarbage)
        m_markingObserver->didFindGarbage(order.garbage.span(), order.cellsLookedAt, toKeep);
    for (JSCell* cell : toKeep)
        visitor.appendUnbarriered(cell);
    if (candidates.isEmpty() && toKeep.isEmpty())
        return finish();
    return true;
}

// Dead or alive.
Vector<PyWeakRef*> Runtime::weakRefsWithCallbacksToUnmarkedObjects(SlotVisitor& visitor)
{
    Vector<PyWeakRef*> result;
    auto* space = m_vm.heap.pyWeakRefSpace<SubspaceAccess::Concurrently>();
    if (!space)
        return result;
    auto look = [&](HeapCell* cell) {
        auto* weakRef = static_cast<PyWeakRef*>(cell);
        if (!weakRef->structureID() || weakRef->isInternal() || !weakRef->callback())
            return;
        JSCell* referent = weakRef->referent();
        if (referent && !visitor.isMarked(referent) && weakRef->structure()->realm() == m_globalObject)
            result.append(weakRef);
    };
    space->forEachMarkedBlock([&](MarkedBlock::Handle* handle) {
        handle->forEachLiveCell([&](size_t, HeapCell* cell, HeapCell::Kind) {
            look(cell);
            return IterationStatus::Continue;
        });
    });
    // Those that earlier collections found dead are gone: they are swept when a collection ends.
    space->forEachPreciseAllocation([&](PreciseAllocation* allocation) {
        look(allocation->cell());
    });
    return result;
}

// The collection will mark no more.
void Runtime::didMarkAll(SlotVisitor& visitor)
{
    bool isFull = m_vm.heap.collectionScope() == CollectionScope::Full;
    auto stays = [&](JSCell* cell) {
        if (!cell)
            return false;
        if (visitor.isMarked(cell))
            return true;
        // What had nothing to finalize was not kept. Anything else would be freed now and never be finalized.
        ASSERT(hasNothingToFinalize(cell));
        didTakeOffList(cell);
        return false;
    };
    if (isFull) {
        m_oldFinalizable.removeAllMatching([&](JSCell* cell) {
            return !stays(cell);
        });
    }
    for (JSCell* cell : m_youngFinalizable) {
        if (stays(cell))
            m_oldFinalizable.append(cell);
    }
    m_youngFinalizable.shrink(0);
    m_oldFinalizable.appendVector(m_finalizableToComeLast);
    m_finalizableToComeLast.shrink(0);
    if (isFull && m_oldFinalizable.size() < m_oldFinalizable.capacity() / 4)
        m_oldFinalizable.shrinkToFit();

#if ASSERT_ENABLED
    if (isFull) {
        UncheckedKeyHashSet<JSCell*> seen;
        for (JSCell* cell : m_oldFinalizable) {
            if (!visitor.isMarked(cell) || !seen.add(cell).isNewEntry) [[unlikely]] {
                dataLogLn("A registered object of type ", typeOf(cell)->nameInMessages(), visitor.isMarked(cell) ? " is in the list twice" : " is about to be freed");
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
    }
    // A generator that is running is referred to by the frame of its body, in whatever thread.
    auto assertAreMarked = [&](PyGenerator* running) {
        for (PyGenerator* generator = running; generator; generator = generator->resumedBy())
            ASSERT(visitor.isMarked(generator) && generator->isRunning());
    };
    assertAreMarked(m_runningGenerator);
    threads().forEachThread([&](ThreadState& thread) {
        if (!thread.saved)
            return;
        for (auto& entry : thread.saved->entries) {
            if (entry.runtime == this)
                assertAreMarked(entry.state.runningGenerator);
        }
    });
#endif
    // What a collection has seen alive is old.
    if (isFull) {
        m_finalized.removeIf([&](JSCell* cell) {
            return !visitor.isMarked(cell);
        });
        for (JSCell* cell : m_justFinalized) {
            if (visitor.isMarked(cell))
                m_finalized.add(cell);
        }
        m_justFinalized.clear();
    } else {
        m_justFinalized.removeAllMatching([&](JSCell* cell) {
            return !visitor.isMarked(cell);
        }, m_justFinalizedAndOld);
    }
    m_justFinalizedAndOld = m_justFinalized.size();
    forgetDeadTemporaries(visitor);

    double duration = m_collectionBegan ? (MonotonicTime::now() - m_collectionBegan).seconds() : 0;
    m_collectionBegan = { };
    // gc.collect() does its own accounting. See gcDisable().
    if (gc.isCollecting || !gc.isEnabled)
        return;
    int generation = m_vm.heap.collectionScope() == CollectionScope::Full ? 2 : 0;
    ++gc.stats[generation].collections;
    gc.stats[generation].duration += duration;
    if (hasCollectionCallbacks()) {
        m_collectionsToTellOf.append({ generation, duration });
        didQueueCalls();
    }
}

void Runtime::WeakCellTable::removeUnmarked(JSC::Heap& heap)
{
    for (unsigned i = 0; i < m_cells.size(); ++i) {
        if (m_cells[i] && !heap.isMarked(m_cells[i])) {
            m_cells[i] = nullptr;
            m_free.append(i);
        }
    }
    if (m_free.size() == m_cells.size()) {
        m_cells.clear();
        m_free.clear();
    }
}

void Runtime::reconcileWeakReferencesAtGCEnd()
{
    scheduledTasks.removeUnmarked(m_vm.heap);
}

bool Runtime::hasBeenFinalized(JSCell* cell)
{
    Locker locker { m_finalizationLock };
    return m_finalized.contains(cell) || m_justFinalized.contains(cell);
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
static constexpr StructSeqDef unraisableDef { "UnraisableHookArgs"_s, unraisableFields, 5, true, true };

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
    if (hook && failed(scope, audit(globalObject, "sys.unraisablehook"_s, hook, arguments))) {
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
    EXCEPTION_ASSERT(!scope.exception() || !function);
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

void finalizeGeneratorNow(JSGlobalObject* globalObject, PyGenerator* generator)
{
    // Once. If it does not let itself be closed, the collector is not to try again.
    generator->setFinalization(PyGenerator::Finalization::None);
    finalizeGenerator(globalObject, generator);
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
    if (scope.exception() && !getVM(globalObject).hasPendingTerminationException())
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

static bool makeCallsOwedToProgram(VM&);

void makeCallsOwed(VM& vm)
{
    vm.traps().clearTrap(VMTraps::NeedPythonCalls);
    if (vm.heap.pythonRuntimes().isEmpty())
        return;
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Another thread has waited long enough for its turn.
    Threads& threads = vm.heap.pythonRuntimes().first()->threads();
    if (threads.dropIsRequested()) [[unlikely]]
        threads.yield();
    if (threads.throwIfStopping()) [[unlikely]]
        return;
    // What other threads had to leave to the main one.
    if (vm.heap.hasPostponedEmbedderFinalizers() && vm.heap.mayRunEmbedderFinalizers()) [[unlikely]]
        vm.heap.runPostponedEmbedderFinalizers();
    // The frames of C++ from here down to a finalizer or a handler stay while that runs, and it may ask for a collection.
    wipeStackBelow(vm.heap.pythonRuntimes().first()->globalObject(), stackBytesWipedForCallsOwed);
    bool hasFinalized = makeCallsOwedToProgram(vm);
    scope.release();
    // What has been finalized is to be found dead by the next collection. The frames of its finalizer are gone, but not what they
    // held, and this may be the loop of the host, where the same few frames of C++ come to lie there again and again.
    if (hasFinalized && !vm.heap.pythonRuntimes().isEmpty())
        wipeStackBelow(vm.heap.pythonRuntimes().first()->globalObject());
}

// Whether it has called anything but handlers of signals.
static NEVER_INLINE bool makeCallsOwedToProgram(VM& vm)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool hasFinalized = false;
    // What is called may start Python in another global object, and a collection may find one gone.
    Vector<Runtime*, 1> runtimes = vm.heap.pythonRuntimes();
    for (Runtime* runtime : runtimes) {
        if (!vm.heap.pythonRuntimes().contains(runtime))
            continue;
        // What the handler of a signal raises is thrown. The trap is set again then: the rest is for the next safe point.
        if (failed(scope, checkSignals(runtime->globalObject())))
            return hasFinalized;
        if (runtime->hasPendingCalls()) {
            hasFinalized = true;
            runtime->runPendingCalls(true);
            RETURN_IF_EXCEPTION(scope, hasFinalized);
        }
    }
    return hasFinalized;
}

void Runtime::runPendingCalls(bool mayPutOff)
{
    // What is queued while these run is run by the loop below, not within one of the calls.
    if (m_isRunningPendingCalls)
        return;

    // With reference counts a finalizer runs where its object is dropped. Here it runs wherever the program happens to be when the
    // collector has come, which may be at the recursion limit, where every call fails, and its effect would be lost. So what is
    // called here has this many frames to itself, whatever the depth. (The depth is lowered and the limit left alone:
    // sys.getrecursionlimit() says what the program set.) The stack has a limit of its own. Near that, this waits for better times.
    constexpr int framesForPendingCalls = 200;
#if ASAN_ENABLED || ASSERT_ENABLED
    constexpr size_t stackForPendingCalls = 2 * MB;
#else
    constexpr size_t stackForPendingCalls = 256 * KB;
#endif
    if (mayPutOff && !isSafeToRecurseWithMargin(m_vm, std::min(stackForPendingCalls, Thread::currentSingleton().stack().size() / 4))) [[unlikely]] {
        m_vm.traps().fireTrap(VMTraps::NeedPythonCalls);
        return;
    }
    SetForScope loweredDepth(frameDepth, std::min(frameDepth, std::max(recursionLimit - framesForPendingCalls, 0)));

    SetForScope isRunning(m_isRunningPendingCalls, true);
    JSGlobalObject* globalObject = m_globalObject;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
    SetForScope savedFrame(reraisingFrame, nullptr);
    // For finalizers that make new garbage with such finalizers for ever.
    unsigned collectionsLeft = 8;
    auto& collection = m_collectionBeingToldOf;

    while (true) {
        // A long queue does not hold up Ctrl-C, and a handler is called with the frame of the program, not with that of a finalizer
        // that happens to come next.
        if (!isFinalized && failed(scope, checkSignals(globalObject))) [[unlikely]] {
            m_vm.traps().fireTrap(VMTraps::NeedPythonCalls);
            return;
        }
        // Once out of the queue, only the stack holds it.
        JSCell* object = nullptr;
        PyWeakRef* weakRef = nullptr;
        bool isDone = false;
        bool collectionStarts = false;
        bool collectionIsOver = false;
        {
            Locker locker { m_finalizationLock };
            if (!collection && !m_collectionsToTellOf.isEmpty()) {
                collection = m_collectionsToTellOf.takeFirst();
                collectionStarts = true;
            } else if (!m_pendingWeakRefCallbacks.isEmpty()) {
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
            } else if (!m_pendingFinalization.isEmpty()) {
                object = m_pendingFinalization.takeFirst();
                m_justFinalized.append(object);
            } else if (collection)
                collectionIsOver = true;
            else {
                m_hasPendingCalls = false;
                isDone = true;
            }
        }
        if (collectionStarts || collectionIsOver) {
            tellOfCollection(collection->generation, collection->duration, collectionIsOver);
            if (collectionIsOver)
                collection = std::nullopt;
        } else if (isDone) {
            if (!std::exchange(m_shouldCollectAfterPendingCalls, false) || !collectionsLeft--)
                return;
            m_vm.clearLastException();
            m_vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
            continue;
        } else if (weakRef) {
            JSValue callback = weakRef->callback();
            weakRef->clearCallback();
            if (callback) {
                call(globalObject, callback, weakRef);
                if (scope.exception() && !m_vm.hasPendingTerminationException())
                    reportUnraisable(globalObject, makeString("Exception ignored while calling weakref callback "_s, reprForMessage(globalObject, callback)), { });
            }
        } else
            finalizeObject(globalObject, object);
        ensureStillAliveHere(object);
        ensureStillAliveHere(weakRef);
        // The host is ending what runs here, and that cannot be reported and forgotten. The rest is for another time.
        if (m_vm.hasPendingTerminationException()) [[unlikely]] {
            Locker locker { m_finalizationLock };
            didQueueCalls();
            return;
        }
    }
}

// The threads that are still there when the interpreter exits never run again. So their frames never let go of what they refer to,
// and in CPython none of that, nor what it leads to, is ever finalized: the locals of the thread, and by way of the functions that it
// is in their globals, with all that hangs on those. A finalizer would find things as no code was meant to see them: the thread
// may be half way through close(). But CPython empties the modules that are still referred to when sys.modules is gone, and in the end
// sys and builtins. What is only held by one of those goes. So does what belongs to the thread and not to its frames: the values
// of its threading.local(), the exception it is handling.
UncheckedKeyHashSet<JSCell*> Runtime::whatOtherThreadsHold()
{
    Vector<JSCell*> roots;
    forEachValueInFramesOfParkedThreads(m_vm, [&](JSValue value) {
        if (value && value.isCell())
            roots.append(value.asCell());
    });
    if (roots.isEmpty())
        return { };

    // Every cell leads to its structure, that to the global object, and that to everything.
    auto isOfTheProgram = [](JSCell* cell) {
        switch (cell->type()) {
        case StructureType:
        case GlobalObjectType:
        case GlobalLexicalEnvironmentType:
        case GlobalProxyType:
        case NativeExecutableType:
        case ProgramExecutableType:
        case ModuleProgramExecutableType:
        case EvalExecutableType:
        case FunctionExecutableType:
        case UnlinkedFunctionExecutableType:
        case UnlinkedProgramCodeBlockType:
        case UnlinkedModuleProgramCodeBlockType:
        case UnlinkedEvalCodeBlockType:
        case UnlinkedFunctionCodeBlockType:
        case CodeBlockType:
        case StringType:
        case HeapBigIntType:
        case SymbolType:
            return false;
        default:
            return true;
        }
    };
    DeferGC deferGC(m_vm);
    UncheckedKeyHashSet<JSCell*> emptied { sysDict(), builtins() };
    for (JSCell* cell : cellsReachedFrom(m_vm, roots.span(), isOfTheProgram)) {
        if (auto* module = PyModule::tryFrom(cell)) {
            emptied.add(module);
            if (PyDict* dict = module->dict())
                emptied.add(dict);
        }
    }
    return cellsReachedFrom(m_vm, roots.span(), [&](JSCell* cell) {
        return isOfTheProgram(cell) && !emptied.contains(cell);
    });
}

void Runtime::finalizeAllAtExit()
{
    auto isDone = [&] {
        bool hasSomethingToFinalize = hasPendingFinalizers();
        forEachFinalizable([&](JSCell* cell) {
            hasSomethingToFinalize |= !hasNothingToFinalize(cell);
        });
        return !hasSomethingToFinalize;
    };
    // (The stack is wiped before every call on the way to a collection, because what was called before, the last collection for
    // one, has left what it dealt with where the frames of the next come to lie. See wipeStackBelow().)
    auto collect = [&] {
        for (unsigned round = 0; round < 64 && !isDone(); ++round) {
            wipeStackBelow(m_globalObject);
            collectAsAskedFor();
            if (!hasPendingCalls())
                break;
            bool mayFindMore = hasPendingFinalizers();
            runPendingCalls();
            if (!mayFindMore || m_vm.hasPendingTerminationException())
                break;
        }
    };
    // A program that leaves nothing to finalize exits without further ado.
    // Otherwise, first what is garbage already, as it would have been all along with reference counts.
    wipeStackBelow(m_globalObject);
    collect();
    if (isDone() || m_vm.hasPendingTerminationException())
        return;
    // Then the modules go, as they do when CPython exits, and with them what only they hold: the globals of the program.
    // The collector finds that, and puts it in order. (CPython also sets the globals to None, one after the other, to get at
    // what cycles keep alive. Here finalizers find them as they were.)
    for (auto name : { "path"_s, "argv"_s, "ps1"_s, "ps2"_s, "last_exc"_s, "last_type"_s, "last_value"_s, "last_traceback"_s, "__interactivehook__"_s, "path_hooks"_s, "path_importer_cache"_s, "meta_path"_s })
        sysDict()->set(m_vm, internString(m_vm, name), jsUndefined()); // None, not gone: importlib then says that the interpreter is shutting down.
    builtins()->remove(m_vm, internString(m_vm, "_"_s));
    modules()->clear(m_vm);
    wipeStackBelow(m_globalObject);
    collect();
    if (m_vm.hasPendingTerminationException()) [[unlikely]]
        return;
    // Then what something else holds on to: JS, a frame of this thread that is still on the stack. In the order it was made.
    {
        forgetThreadLocalsOfOtherThreads(m_globalObject);
        auto heldByOtherThreads = whatOtherThreadsHold();
        Vector<JSCell*> rest;
        forEachFinalizable([&](JSCell* cell) {
            // A generator someone still holds is not closed, nor is a coroutine complained about.
            if (cell->type() != PyGeneratorType && !heldByOtherThreads.contains(cell))
                rest.append(cell);
        });
        for (auto* list : { &m_oldFinalizable, &m_youngFinalizable }) {
            for (JSCell* cell : *list)
                didTakeOffList(cell);
            list->clear();
        }
        Locker locker { m_finalizationLock };
        for (JSCell* cell : rest)
            m_pendingFinalization.append(cell);
        didQueueCalls();
    }
    m_vm.writeBarrier(m_globalObject);
    runPendingCalls();
}

// ---- Collections that are asked for ----

// A program that asks for a collection expects what it has dropped to be gone afterwards. The collector takes every word of the
// stack that looks like a reference for one, and the frames of C++ between the program's call and the collector lie where those
// of its earlier calls were: what they do not write would still refer to what those dealt with. So that is wiped first, before
// the first of those frames (NativeFlag::OnWipedStack).
// What is wiped lies below the stack pointer of whoever asks for it. The frame of that function is not wiped, and later frames come
// to lie there too: so it is to be such that it has no slot that it does not write, in a build that is not optimized either, where
// every variable and many a temporary has a slot, written or not, and a local of the last function of Python that returned may lie
// just there. See callNativeFunctionOnWipedStack(). What wipes is not written in C++, so that it has no frame itself.
#if ENABLE(C_LOOP)
NEVER_INLINE void wipeStackBelow(JSGlobalObject* globalObject, size_t size)
{
    char* top = static_cast<char*>(currentStackPointer());
    char* limit = static_cast<char*>(getVM(globalObject).softStackLimit());
    if (top <= limit || static_cast<size_t>(top - limit) < size + 16 * KB)
        return;
    volatile char* wiped = static_cast<volatile char*>(alloca(size));
    for (size_t i = 0; i < size; i += sizeof(uintptr_t))
        *reinterpret_cast<volatile uintptr_t*>(wiped + i) = 0;
}
#endif

bool Runtime::hasPendingFinalizers()
{
    Locker locker { m_finalizationLock };
    return !m_pendingFinalization.isEmpty() || !m_pendingWeakRefCallbacks.isEmpty();
}

void Runtime::collectAsAskedFor(CollectionScope collectionScope, bool leavesCyclicGarbageForLater)
{
    // The VM remembers the last exception thrown, for the debugger, long after it has been handled, and with it the
    // traceback and all that the frames in it refer to.
    m_vm.clearLastException();
    forgetCatchesInJSThatAreOver();
    SetForScope scopeAskedFor(m_scopeAskedFor, collectionScope);
    SetForScope leavesCyclicGarbage(m_leavesCyclicGarbageForLater, leavesCyclicGarbageForLater);
    // The one is asked for to get rid of things, the other to be quick: what it frees is swept when the memory is wanted.
    if (collectionScope == CollectionScope::Full)
        m_vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
    else
        m_vm.heap.collectSync(CollectionScope::Eden);
}

void Runtime::finalizeWhatReferenceCountsWouldHaveFreed()
{
    for (unsigned round = 0; round < 64; ++round) {
        bool hasSomethingToDo = hasPendingFinalizers();
        forEachFinalizable([&](JSCell* cell) {
            hasSomethingToDo |= hasFinalizerOfProgram(*this, cell) && !hasNothingToFinalize(cell);
        });
        if (!hasSomethingToDo)
            return;
        wipeStackBelow(m_globalObject);
        collectAsAskedFor(CollectionScope::Full, true);
        if (!hasPendingCalls())
            return;
        bool mayFindMore = hasPendingFinalizers();
        runPendingCalls();
        if (!mayFindMore || m_vm.hasPendingTerminationException())
            return;
    }
}

void collectAndFinalize(JSGlobalObject* globalObject, Runtime::MarkingObserver* observer, CollectionScope collectionScope, bool leavesCyclicGarbageForLater)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    ASSERT(!rt.isRunningPendingCalls());
    writeOutPendingOutput(globalObject); // Until then they are not to be collected.
    // An object with a finalizer takes two rounds: one finds it, the next frees it, which may clear weak references in turn
    // and finds what only it referred to. The limit is for finalizers that make new garbage with finalizers for ever.
    for (unsigned round = 0; round < 64; ++round) {
        wipeStackBelow(globalObject);
        if (observer) {
            observer->willCollect();
            rt.setMarkingObserver(observer);
        }
        rt.collectAsAskedFor(collectionScope, leavesCyclicGarbageForLater);
        if (observer) {
            rt.setMarkingObserver(nullptr);
            observer->didCollect();
        }
        if (!rt.hasPendingCalls())
            break;
        bool mayFindMore = rt.hasPendingFinalizers();
        // What the handler of a signal that came meanwhile raises is for whoever waits for this. The calls that are left are made
        // at the next safe point then.
        rt.runPendingCalls();
        if (!mayFindMore || collectionScope == CollectionScope::Eden || vm.exceptionForInspection())
            break;
    }
}

} // namespace Python
} // namespace JSC
