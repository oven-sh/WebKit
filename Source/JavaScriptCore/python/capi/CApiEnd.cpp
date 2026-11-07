/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApi.h"

#include "CApiType.h"
#include "JSCInlines.h"
#include "MarkedVector.h"
#include "PyAbstract.h"
#include "PyInternals.h"

#if PYTHON_HAS_CAPI

// HOW AN INSTANCE ENDS. It is two cells, a face and a body: CApiInstance.h.
//
// TO DESTROY IS NOT TO FINALIZE. A finalizer (tp_finalize, __del__) that is called for an object which another finalizer has just brought back gives a wrong answer. A tp_dealloc is the
// end of what C keeps in the object: NOBODY MAY HAVE IT THEN, OR GET IT LATER.
//
//  F  THE FINALIZER goes through the runtime's queue like any __del__, once: the face is registered, found dead, kept with all that it leads to (the body, what C holds there), and handed
//     to the finalizer. If that brings it back, it is back.
//  L  LETTING GO. A collection has marked all that is reachable, and all that it keeps for whatever reason (finalizers to run, what gc.DEBUG_SAVEALL shows), to the end, by the marker's own
//     ways. A body that is still not marked is dead, and so is its face. The body is marked now, for its end. The face is not: the marker does not go from such a body to its face. What the body
//     holds is marked, as for any body that is marked: C will give it back in tp_dealloc. Its class is not, if that has died with it: see "The class of a body that is let go of".
//     What reference counts would not have freed (it is in a cycle, or hangs off one) CPython's collector would have found, and the first thing it does is done here, with the world stopped: the
//     weak references of Python's to it die.
//     One that another dead instance holds in its data, and whose face would be missed, is not let go of yet: see "Who is still held keeps its face".
//  B  TAKING BACK. The marker has come to rest again. A face that is marked now is led to by what a body holds (body -> list -> face); or L has marked it; or, if the collection goes on beside the
//     program, somebody has taken it by a weak reference. Nobody can tell which. Its body is alive again, AND SO IS EVERY BODY THAT IT HOLDS, with its face, and so on: an instance that lives keeps
//     what it holds whole. That marks faces, so the marker goes on, and comes here again.
//  E  WHEN THAT COLLECTION ENDS the face of a body that is still let go of is not marked: it is dead as any cell is dead, and is about to be freed. Whoever knew its address without marking
//     it has been told, or is told as its block is swept, in whatever way JSC and Bun tell him of any cell. The body has no face from now on.
//     What has been taken back stays as it was. If it is in a cycle or hangs off one, it is asked once to let go of what it holds (tp_clear), and a later collection finds the face dead. If
//     reference counts would have freed it (parent -> list -> child), nothing is called: CPython never clears such an object, whose tp_dealloc may need what it holds; it
//     waits until its holder has gone.
//     (From L until tp_clear is called the program may run, and may take a face by a weak reference of JS's. It then has an object that is cleared in its hands: valid, and empty.
//     But if somebody is SEEN to take one, C by a pointer that it does not own, nothing is cleared of all that waits for it: it may be reachable from that one. It comes up again.)
//  D  A BODY WITHOUT A FACE IS DESTROYED WHEN C OWNS NO REFERENCE TO IT. Python and JS get at a body by its face only, and there is none; a new one is made in one place (faceOfSlow()), and
//     then there is one. C gets at it by a reference that it owns, and there is none, or by a pointer that it does not own, which in CPython would point to freed memory by now.
//     No object of Python's refers to a body. So among bodies without faces there is nothing but C's references, and the counts are exact: tp_dealloc gives back what it held, and a body whose
//     count falls to 0 by that comes next, parents before children, as in CPython. What is left with a count is in a cycle of bodies or hangs off one: tp_clear, and then the counts again.
//
// Nothing is revoked: the two questions are asked when the turn has come. A body that C has taken a reference to meanwhile, or has handed to Python, simply lives on.

namespace JSC {
namespace Python {
namespace CApi {

CAPI_FILE(End)

static bool hasFinalizer(PyType* type, MadeByExtension* extension)
{
    return extension->slots[tp_finalize] || type->lookup(Special::Del);
}

void didMake(JSGlobalObject* globalObject, Face* face, MadeByExtension* extension)
{
    if (hasFinalizer(face->type(), extension))
        runtime(globalObject).registerFinalizable(face);
}

FinalizerState* finalizerStateOfSlow(JSCell* cell)
{
    Face* face = Face::tryFromCell(cell);
    return face ? &face->body()->finalizerState() : nullptr;
}

// ---- L ----

static void watch(Interpreter& current, Body* body, bool isWatched)
{
    if (auto* counts = current.counts.find(body))
        counts->isWatched = isWatched;
}

// While something waits, for the end of its collection or for its tp_clear, C may come by it: see didSeeSomebodyTake(). It may be one to which C owns no reference yet.
static void didChangeWhatWaits(Interpreter& current)
{
    bool somethingWaits = current.numberToBeCleared || !current.letGo.isEmpty() || !current.takenBack.isEmpty();
    current.counts.watchesNewKeys = somethingWaits;
    if (!somethingWaits)
        current.clearingIsOff = false;
}

// THE CLASS OF A BODY THAT IS LET GO OF. C needs the type of the instance for its end: tp_dealloc asks it for tp_free and gives back the reference that the instance owns to it. But a class leads
// to much (class -> method -> globals of the module -> whatever is there), and if it has died with the instance that may well be the face: class Sub(Node) with a method, and x = Sub() beside it. Nothing
// would let go of that: a tp_clear does not, and tp_dealloc is not called while the face lives. (CPython's collector clears the other objects of the garbage: the dict, the function, the class.)
// So FROM L ON SUCH A BODY IS AN INSTANCE OF THE NEAREST BASE OF ITS CLASS THAT THIS COLLECTION HAS FOUND ALIVE, no further down than the type that the extension has made: the data and the slots are the
// same. That one is marked already, so the body marks nothing by its type, and the class dies like anything else, with all that only it leads to. The reference moves with it, as it does when __class__
// is assigned to. What the class added to the instance is in the face, and ends with it; what C's tp_dealloc gets is what is left. (In CPython the tp_dealloc of a class made in Python does that, and then
// calls the base's. Py_TYPE(self) is still the class there.)
// If the extension's own type has died too, the body is of that type, which is kept by that: there is no doing without it.
// A body that is taken back has a face that is marked, and the face names the class: it is of that class again.
static PyType* classForItsEnd(SlotVisitor& visitor, PyType* type)
{
    for (PyType* candidate = type; candidate; candidate = candidate->base()) {
        if (visitor.isMarked(candidate) || MadeByExtension::of(candidate))
            return candidate;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// WHO IS STILL HELD KEEPS ITS FACE. A dead instance to which C owns a reference is held by another dead instance, in its data. (Any other owner would have kept it alive.) That one's tp_dealloc
// is still to run, owns the reference, and may look at what it holds or hand it on: to a method, to the program. So what it holds is whole until it has been given back: its class, its attributes,
// its weak references. It is let go of by the collection that finds it dead with no reference of C's left.
// Not so where the holder is cyclic garbage itself. There CPython's collector lets the weak references die first and then clears one object after the other, the __dict__ of an instance of a class among them: what a tp_clear
// or a tp_dealloc finds in another piece of the garbage hangs on the order, and nobody can count on it.
// And a face that is nothing but the class, and a class that stays, is not missed: whoever asks for one gets a new one. So instances of C's own types, which have neither attributes nor weak references,
// go with their holders however long the chain.
static bool facesHaveNothingOfTheirOwn(PyType* type)
{
    if (type->has(TypeFlag::HasDict) || type->has(TypeFlag::HasWeakref))
        return false;
    for (; !MadeByExtension::of(type); type = type->base()) {
        if (type->slots())
            return false;
    }
    return true;
}

// `vm`: null for the collector, with the world stopped.
void moveToClass(Interpreter& current, Body* body, PyType* type, VM* vm)
{
    PyType* old = body->type();
    if (old == type) [[likely]]
        return;
    ASSERT(madeByExtension(old) == madeByExtension(type));
    if (vm)
        body->setType(*vm, type);
    else
        body->setTypeInCollection(type);
    if (ownsReferenceToType(madeByExtension(type))) {
        retain(current, toC(type));
        release(current, toC(old));
    }
}

bool letGoOfWhatIsDead(SlotVisitor& visitor)
{
    Interpreter& current = interpreter();
    Vector<Body*> dead;
    bool isFull = current.vm.heap.collectionScope() == CollectionScope::Full;
    // A thread that waits has asked for this collection, to find what reference counts would have freed by now. CPython's collector does not run in such a thread: what only that
    // would have found is kept as it is, face and all, and comes up again.
    bool leavesCyclicGarbageForLater = current.runtime.leavesCyclicGarbageForLater();
    auto look = [&](Vector<Body*>& list) {
        for (Body* body : list) {
            if (!visitor.isMarked(body) && body->isAliveOrCleared() && madeByExtension(body->type())->hasSomethingToDestroy())
                dead.append(body);
        }
    };
    if (isFull)
        look(current.oldBodies);
    look(current.youngBodies);
    if (dead.isEmpty())
        return false;

    // Which of them reference counts would have freed. (To that graph an instance is one object: see Face::visitChildren(). It is made by another visitor than the marker, and may
    // differ from what the marker finds. Whether tp_clear is called hangs on it, and nothing else.)
    Vector<JSCell*> objects = dead.map([](Body* body) -> JSCell* {
        return body->face() ? static_cast<JSCell*>(body->face()) : body;
    });
    // (All of it, at whatever depth: a cycle that hangs below an instance which reference counts would have freed is a cycle.)
    FinalizationQuestions questions;
    questions.wantsAllThatReferenceCountsWouldFree = true;
    questions.wantsWhatWaitsForThat = true;
    FinalizationOrder order = finalizationOrder(current.vm.heap, visitor, objects.span(), questions);
    UncheckedKeyHashSet<JSCell*> freedByReferenceCounts;
    freedByReferenceCounts.addAll(order.allFreedByReferenceCounts);
    for (unsigned i = 0; i < dead.size(); ++i)
        dead[i]->wasCyclicGarbage = !freedByReferenceCounts.contains(objects[i]);
    // A cycle to which something leads that reference counts would have freed (parent -> child, and child.me = child). In CPython the parent goes at once, and finds the child whole; the collector comes
    // later. So such garbage is for later too: it is kept as it is, and comes up again when its holder has gone.
    BitVector waitsForItsHolder(dead.size());
    for (unsigned i : order.later)
        waitsForItsHolder.quickSet(i);

    Vector<JSCell*> facesOfCyclicGarbage;
    for (unsigned i = 0; i < dead.size(); ++i) {
        Body* body = dead[i];
        if (!body->wasCyclicGarbage || (!leavesCyclicGarbageForLater && !waitsForItsHolder.quickGet(i))) {
            body->setLife(Body::Life::LetGo);
            watch(current, body, true);
            current.letGo.append(body);
            PyType* type = classForItsEnd(visitor, body->type());
            // (The face is marked: it is taken back when the marker has come to rest, with all that it holds.)
            if (!body->wasCyclicGarbage && body->face() && current.counts.find(body) && (type != body->type() || !facesHaveNothingOfTheirOwn(type)))
                visitor.appendUnbarriered(body->face());
            else
                moveToClass(current, body, type, nullptr);
            if (body->wasCyclicGarbage && body->face() && body->face()->type()->has(TypeFlag::HasWeakref))
                facesOfCyclicGarbage.append(body->face());
        }
        visitor.appendUnbarriered(body);
    }
    didChangeWhatWaits(current);
    if (!facesOfCyclicGarbage.isEmpty())
        current.runtime.letWeakRefsDie(visitor, facesOfCyclicGarbage.span());
    return true;
}

// ---- B ----

bool takeBackWhatTheLivingHold(SlotVisitor& visitor)
{
    Interpreter& current = interpreter();
    Vector<Body*> toFollow;
    for (Body* body : current.letGo) {
        // (The first: see didSeeSomebodyTake().)
        if (body->life() != Body::Life::LetGo || (body->face() && visitor.isMarked(body->face())))
            toFollow.append(body);
    }
    if (toFollow.isEmpty())
        return false;
    while (!toFollow.isEmpty()) {
        Body* body = toFollow.takeLast();
        if (body->life() == Body::Life::LetGo) {
            body->setAliveOrCleared();
            watch(current, body, body->isCleared);
            // The marker has been at the body, and has passed by the face.
            if (Face* face = body->face()) {
                moveToClass(current, body, face->type(), nullptr);
                visitor.appendUnbarriered(face);
            }
        }
        traverse(body, [&](JSCell* target, CountTable::Counts&) {
            if (Body* held = Body::tryFromCellOfHandle(target); held && held->life() == Body::Life::LetGo)
                toFollow.append(held);
        });
    }
    current.letGo.removeAllMatching([&](Body* body) {
        if (body->life() == Body::Life::LetGo)
            return false;
        current.takenBack.append(body);
        return true;
    });
    return true;
}

static void waitsForItsClearNoLonger(Interpreter& current, Body* body)
{
    ASSERT(body->life() == Body::Life::ToBeCleared);
    body->setLife(Body::Life::Alive);
    watch(current, body, false);
    --current.numberToBeCleared;
}

// Somebody has got hold of an instance that is not simply alive: C has taken a reference to it or handed it to the program, by a pointer that it does not own; or whoever has the face, by a
// weak reference that the runtime knows nothing of, has handed it to C.
void didSeeSomebodyTake(Body* body)
{
    Interpreter& current = interpreter();
    // It may get into another cycle.
    body->isCleared = false;
    switch (body->life()) {
    case Body::Life::Cleared:
        body->setLife(Body::Life::Alive);
        watch(current, body, false);
        return;
    case Body::Life::LetGo:
        // The collection that let go of it goes on beside the program. The marker has been at the body and has passed by the face: it is sent there again. (That is what
        // WeakRef.prototype.deref() does.) What the body holds follows when the marker has come to rest.
        body->setLife(Body::Life::Alive);
        watch(current, body, false);
        if (Face* face = body->face())
            moveToClass(current, body, face->type(), &current.vm);
        current.vm.writeBarrier(body);
        break;
    case Body::Life::ToBeCleared:
        waitsForItsClearNoLonger(current, body);
        break;
    default:
        return;
    }
    current.clearingIsOff = true;
    didChangeWhatWaits(current);
}

// ---- E ----

// True: it has queued something.
bool didSeeWhichFacesDied(JSC::Heap& heap)
{
    Interpreter& current = interpreter();
    Vector<Body*> toDestroy;
    Vector<Body*> toClear;
    auto clearIfThatIsDue = [&](Body* body) {
        if (current.clearingIsOff || !body->wasCyclicGarbage || body->isCleared || !madeByExtension(body->type())->slots[tp_clear])
            return;
        body->setLife(Body::Life::ToBeCleared);
        watch(current, body, true);
        ++current.numberToBeCleared;
        toClear.append(body);
    };
    for (Body* body : std::exchange(current.takenBack, { })) {
        ASSERT(heap.isMarked(body) && (!body->face() || (heap.isMarked(body->face()) && body->type() == body->face()->type())));
        // (If the program has run since, C may have made an end of it.)
        if (body->isAliveOrCleared())
            clearIfThatIsDue(body);
    }
    for (Body* body : std::exchange(current.letGo, { })) {
        // THE FACE IS DEAD. All that follows stands on this.
        RELEASE_ASSERT(body->life() == Body::Life::LetGo && heap.isMarked(body) && (!body->face() || !heap.isMarked(body->face())));
        body->setAliveOrCleared();
        watch(current, body, body->isCleared);
        body->clearFace();
        if (!current.counts.find(body))
            toDestroy.append(body);
        else
            clearIfThatIsDue(body);
    }
    didChangeWhatWaits(current);
    // What can go by counts goes first, and takes with it what it alone held.
    for (auto* list : { &toDestroy, &toClear }) {
        for (Body* body : *list)
            current.runtime.queueToFinalize(body, body->thread, Runtime::TellsThread::WhenCollectionEnds);
    }
    return !toDestroy.isEmpty() || !toClear.isEmpty();
}

// ---- D ----

// What C does at the end of an object is done in the middle of whatever the program is doing, which may be C with an exception set.
class IndicatorSetAside {
    WTF_MAKE_NONCOPYABLE(IndicatorSetAside);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    IndicatorSetAside()
        : m_saved(std::exchange(errorIndicator(), nullptr))
    {
    }
    ~IndicatorSetAside() { setErrorIndicator(interpreter(), m_saved); }

private:
    Handle m_saved;
};

// Nobody is there to catch it.
static void reportWhatCRaised(JSGlobalObject* globalObject, PyType* type, ASCIILiteral slot)
{
    if (!errorIndicator()) [[likely]]
        return;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject));
    throwWhatCRaised(globalObject);
    EXCEPTION_ASSERT_UNUSED(scope, scope.exception());
    reportUnraisable(globalObject, makeString("Exception ignored in "_s, slot, " of '"_s, type->name(), '\''), { });
}

// (By the key, not by the number: when nothing is let go of any more, what C has given back is held as well. See CountTable::remove().)
static ALWAYS_INLINE bool nobodyHasIt(Interpreter& current, Body* body)
{
    return (body->isAliveOrCleared() || body->life() == Body::Life::ToBeCleared) && !body->face() && !current.counts.find(body);
}

// Those that this thread is about to destroy: a tp_dealloc that gives back the last reference to another does not call the other's, however long the chain. A collection that comes in
// between may queue them as well, for the threads that made them, and whoever comes second finds nothing to do. He has to find the cell, though: what has been destroyed is kept by nobody else. So
// the list is one that the collector marks.
using ListOfBodies = MarkedVector<Body*>;
static constinit thread_local ListOfBodies* t_toDestroyNext = nullptr;

static void destroyItAndWhatGoesWithIt(JSGlobalObject* globalObject, Body* first)
{
    Interpreter& current = interpreter();
    ListOfBodies next;
    SetForScope toDestroyNext(t_toDestroyNext, &next);
    next.append(first);
    IndicatorSetAside indicator;
    // (In the order in which they were let go of.)
    for (size_t i = 0; i < next.size(); ++i) {
        // The host is ending what runs here: nothing of C's is begun. What is left is found again by the next collection.
        // (In whichever form: code of Python's that a tp_dealloc has called has been cut short, and whoever called it for C has put the exception away.)
        if (current.vm.hasPendingTermination()) [[unlikely]]
            break;
        Body* body = next[i];
        if (!nobodyHasIt(current, body))
            continue;
        // (Another's tp_clear has let go of it before its own was called.)
        if (body->life() == Body::Life::ToBeCleared) {
            waitsForItsClearNoLonger(current, body);
            didChangeWhatWaits(current);
        }
        PyType* type = body->type();
        // From here on tp_traverse is not called: what it still holds counts as held from elsewhere.
        body->setLife(Body::Life::BeingDestroyed);
        if (auto dealloc = madeByExtension(type)->slot<DestructorFunction>(tp_dealloc))
            dealloc(toC(body));
        // Before anything of the program's runs.
        body->setLife(Body::Life::Destroyed);
        ensureStillAliveHere(body);
        reportWhatCRaised(globalObject, type, "tp_dealloc"_s);
    }
}

// C has taken a reference to a body which a collection has let go of. Or a first reference to something, while a collection that has let go of bodies is under way.
void didTakeWatchedReference(JSCell* cell)
{
    if (Body* body = Body::tryFromCellOfHandle(cell))
        didSeeSomebodyTake(body);
}

void didGiveBackLastReferenceToBodyWithoutFace(Body* body)
{
    Interpreter& current = interpreter();
    if (!nobodyHasIt(current, body) || !madeByExtension(body->type())->hasSomethingToDestroy())
        return;
    if (t_toDestroyNext)
        t_toDestroyNext->append(body);
    else
        current.runtime.queueToFinalize(body, body->thread, Runtime::TellsThread::Now);
}

// AN INSTANCE IS HELD WHILE C IS IN IT. C may let other threads run, and one of them may serve the same queue, or collect. It clears what holds this one, that gives back the last reference, and this one's
// tp_dealloc would begin, and return, under a call that goes on; or the collection finds it dead once more, and the call is made a second time beside the first. With a reference of the caller's the
// instance is what it is: held from outside the instances, so a root.
// (In the books and nowhere else: nobody is seen to take anything, and nothing is begun when it is given back. Whoever has made the call looks at what is left.)
class HeldForCall {
    WTF_MAKE_NONCOPYABLE(HeldForCall);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    HeldForCall(Interpreter& current, Body* body)
        : m_current(current)
        , m_body(body)
    {
        m_current.counts.add(m_body);
    }
    ~HeldForCall() { m_current.counts.remove(m_body, s_interpreterThatLetsGo.load(std::memory_order_relaxed)); }

private:
    Interpreter& m_current;
    Body* m_body;
};

static void serve(JSGlobalObject* globalObject, Body* body)
{
    Interpreter& current = interpreter();
    if (body->life() == Body::Life::ToBeCleared) {
        waitsForItsClearNoLonger(current, body);
        bool isOff = current.clearingIsOff;
        didChangeWhatWaits(current);
        if (!isOff) {
            PyType* type = body->type();
            IndicatorSetAside indicator;
            // What it lets go of, and nobody else has, goes when this has returned.
            ListOfBodies next;
            {
                HeldForCall held(current, body);
                {
                    SetForScope toDestroyNext(t_toDestroyNext, &next);
                    madeByExtension(type)->slot<ClearFunction>(tp_clear)(toC(body));
                }
                // What has been cleared is still a valid object. It is not asked again, unless somebody is seen to take hold of it: then it may get into another cycle.
                if (body->life() == Body::Life::Alive) {
                    body->isCleared = true;
                    body->setLife(Body::Life::Cleared);
                    watch(current, body, true);
                }
            }
            reportWhatCRaised(globalObject, type, "tp_clear"_s);
            for (size_t i = 0; i < next.size(); ++i)
                destroyItAndWhatGoesWithIt(globalObject, next[i]);
        }
    }
    if (nobodyHasIt(current, body))
        destroyItAndWhatGoesWithIt(globalObject, body);
}

void TurnInQueue::begin(JSGlobalObject* globalObject, JSCell* cell)
{
    if (!isSetUpFor(runtime(globalObject)))
        return;
    // The turn may come at a safe point of code that C has called back, and that C may run for another interpreter.
    CRunsFor running(runtime(globalObject));
    if (Body* body = Body::tryFromCell(cell)) {
        m_isOver = true;
        serve(globalObject, body);
        return;
    }
    Face* face = Face::tryFromCell(cell);
    if (!face)
        return;
    Body* body = face->body();
    MadeByExtension* extension = madeByExtension(face->type());
    if (body->life() >= Body::Life::BeingDestroyed || body->isFinalized || !hasFinalizer(face->type(), extension)) {
        m_isOver = true;
        return;
    }
    body->isFinalized = true;
    // That of a class which derives from the type comes in place of the type's.
    auto finalize = extension->slot<DestructorFunction>(tp_finalize);
    if (!finalize || face->type()->lookup(Special::Del))
        return;
    m_isOver = true;
    IndicatorSetAside indicator;
    HeldForCall held(interpreter(), body);
    finalize(toC(body));
    reportWhatCRaised(globalObject, face->type(), "tp_finalize"_s);
}

PY_CAPI int PyObject_GC_IsTracked(Handle object)
{
    CAPI_QUIET_AND_ONLY_READS(0);
    if (Body* body = bodyOrNull(object))
        return isTracked(body);
    // (It asks for the type.)
    return typeOf(object) && isTrackedByCollector(interpreter().runtime, toLookAt(object));
}

PY_CAPI int PyObject_GC_IsFinalized(Handle object)
{
    CAPI_QUIET(0);
    if (Body* body = bodyOrNull(object))
        return body->isFinalized;
    JSCell* cell = countedCell(object);
    return cell && interpreter().runtime.hasBeenFinalized(cell);
}

// For a tp_dealloc that wants the finalizer called first. -1: the object has been brought back, and is not to be destroyed.
PY_CAPI int PyObject_CallFinalizerFromDealloc(Handle)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    // It has been called: a face with a finalizer to run does not die.
    return 0;
}

// For a tp_dealloc. They refer to the face, which has died.
PY_CAPI void PyObject_ClearWeakRefs(Handle)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
}

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
