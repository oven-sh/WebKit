/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApiInstance.h"

#include "CApiType.h"
#include "JSCInlines.h"
#include "SimpleMarkingConstraint.h"
#include "SlotVisitorInlines.h"

#if PYTHON_HAS_CAPI

// What the collector is told about what C holds. See "The C API" in ../README.md.

namespace JSC {
namespace Python {
namespace CApi {

// What C holds is reachable, as what a global variable of the program holds is. The collector learns of it as it learns of an embedder's other
// roots: by a marking constraint, which it evaluates, with the world stopped, again and again until nothing new comes of it.
//
// A reference that C owns is held in one of two places. Either in an instance whose tp_traverse names it: then it is an edge of the graph like any
// other, and counts for as much as the instance does. Or anywhere else: a static variable, the state of a module, C's stack or heap, an instance
// whose tp_traverse does not tell. Then it is a root. Which is which is found as CPython's collector finds it: all the instances are asked, and a cell
// of which C owns more references than they account for is held from elsewhere.
//
// An evaluation begins from nothing if the world has run since the last: C stores into its instances without telling anybody. (By the collector's own count of its phases,
// which the conservative scan goes by too. Heap::mutatorExecutionVersion() misses a thread that comes back from a wait while the collector works beside the program.) If it has
// not, what was counted holds, and only the instances that have been marked since are asked, for what they hold to be marked: the list is gone over for them. The marker
// leaves no word: it runs on several threads, and in a collection that goes on beside the program its word would be thrown away each time.
//
// What an instance holds is marked here and now, and if that is an instance it is asked in its turn: a list of a million instances, each of which holds the next, takes
// one evaluation. Where an object of Python lies between one instance and the next, the marker has to go that bit of the way, and each such bit takes another
// evaluation (as each link of a chain of WeakMaps does in JSC).
template<typename Visitor>
static void markWhatCHolds(Visitor& visitor)
{
    Interpreter* current = s_interpreter.load(std::memory_order_relaxed);
    if (!current)
        return;
    SetRootMarkReasonScope rootScope(visitor, RootMarkReason::StrongReferences);

    // The young: nobody is asked. A cycle through C is found by a full collection. (An old instance in which C has put a young object is not looked at
    // by such a collection, and need not be: the reference is one that C owns.)
    // Nor when nothing is let go of any more: C has a reference that is not in the books, so what the instances account for says nothing.
    if (current->vm.heap.collectionScope() == CollectionScope::Eden || !s_interpreterThatLetsGo.load(std::memory_order_relaxed)) {
        current->counts.forEach([&](JSCell* cell, CountTable::Counts&) {
            visitor.appendUnbarriered(cell);
        });
        return;
    }

    // (JSC's verifier has marks of its own, and is told what an instance holds when it visits the instance.)
    constexpr bool isMarker = std::is_same_v<Visitor, SlotVisitor>;
    Vector<Body*> toAsk;
    auto mark = [&](JSCell* cell) {
        if (visitor.isMarked(cell))
            return;
        visitor.appendUnbarriered(cell);
        if constexpr (isMarker) {
            if (Body* body = Body::tryFromCellOfHandle(cell))
                toAsk.append(body);
        }
    };
    auto forEachBody = [&](auto function) {
        for (auto* list : { &current->oldBodies, &current->youngBodies }) {
            for (Body* body : *list)
                function(body);
        }
    };

    uint64_t version = current->vm.heap.phaseVersion();
    if (!isMarker || current->versionWhenCounted != version) {
        if (isMarker)
            current->versionWhenCounted = version;
        current->counts.forEach([&](JSCell*, CountTable::Counts& counts) {
            counts.accountedFor = 0;
        });
        forEachBody([&](Body* body) {
            // This may be out of date, if helpers of the marker are still at work. Marks only come, and marking is not over before an evaluation has found nothing to do.
            bool isMarked = visitor.isMarked(body);
            if (isMarker)
                body->wasAsked = isMarked;
            traverse(body, [&](JSCell* target, CountTable::Counts& counts) {
                ++counts.accountedFor;
                if (isMarked)
                    mark(target);
            });
        });
        // (More than C owns: a tp_traverse that names what is not its own. It is not believed.)
        current->counts.forEach([&](JSCell* cell, CountTable::Counts& counts) {
            if (counts.owned != counts.accountedFor)
                mark(cell);
        });
    } else {
        forEachBody([&](Body* body) {
            if (!body->wasAsked && visitor.isMarked(body))
                toAsk.append(body);
        });
    }

    while (!toAsk.isEmpty()) {
        Body* body = toAsk.takeLast();
        if (body->wasAsked)
            continue;
        body->wasAsked = true;
        traverse(body, [&](JSCell* target, CountTable::Counts&) {
            mark(target);
        });
    }
}

void addMarkingConstraint(VM& vm)
{
    vm.heap.addMarkingConstraint(makeUnique<SimpleMarkingConstraint>("Pyc"_s, "Python C API"_s,
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([](auto& visitor) {
            markWhatCHolds(visitor);
        })),
        ConstraintVolatility::GreyedByMarking, ConstraintConcurrency::Sequential, ConstraintParallelism::Sequential));
}

// All that is reachable is marked, and so is all that the runtime keeps for finalizers.
bool markWhatIsLeft(SlotVisitor& visitor)
{
    Interpreter& current = interpreter();
    // THE REALM OF THE INTERPRETER HAS DIED, with all that is in it. To call a tp_dealloc takes the realm, and the realm leads to whatever its modules hold: the instances that are left could be ended only as
    // at exit, after the modules have been let go of, and a realm that is dropped does not get that. Nothing is kept: if nobody else marks the global object (then this is called again), the interpreter ends in this
    // collection, before anything is swept (runtimeIsEnding()), and with it the table and the lists. What C really owns is a root, so there is none of that. (While something is let go of, its
    // Structure holds the realm.)
    if (!visitor.isMarked(current.globalObject)) {
        ASSERT(current.letGo.isEmpty() && current.takenBack.isEmpty());
        return false;
    }
    if (takeBackWhatTheLivingHold(visitor) || letGoOfWhatIsDead(visitor))
        return true;
    // A key that is still not marked is held only by instances that are dead and that nobody keeps, so the references will never be given back. That does not happen while every instance that has
    // something to give back is kept. But no key is to be the address of a freed cell, whatever happens: it stays.
    bool markedSomething = false;
    current.counts.forEach([&](JSCell* cell, CountTable::Counts&) {
        if (visitor.isMarked(cell))
            return;
        visitor.appendUnbarriered(cell);
        markedSomething = true;
    });
    return markedSomething;
}

bool didFinishMarking(JSC::Heap& heap)
{
    Interpreter& current = interpreter();
#if ASSERT_ENABLED
    current.counts.forEach([&](JSCell* cell, CountTable::Counts&) {
        ASSERT(heap.isMarked(cell));
    });
#endif
    auto isDead = [&](Body* body) {
        return !heap.isMarked(body);
    };
    if (heap.collectionScope() == CollectionScope::Full)
        current.oldBodies.removeAllMatching(isDead);
    current.youngBodies.removeAllMatching(isDead);
    current.oldBodies.appendVector(std::exchange(current.youngBodies, { }));

    current.versionWhenCounted = std::nullopt;
    return didSeeWhichFacesDied(heap);
}

// ---- The module gc ----

size_t numberOfInstancesSlow()
{
    Interpreter& current = interpreter();
    return current.oldBodies.size() + current.youngBodies.size();
}

bool hasInstancesToDestroySlow()
{
    Interpreter& current = interpreter();
    for (auto* list : { &current.oldBodies, &current.youngBodies }) {
        for (Body* body : *list) {
            if (body->life() < Body::Life::BeingDestroyed && madeByExtension(body->type())->hasSomethingToDestroy())
                return true;
        }
    }
    return false;
}

bool isTracked(Body* body)
{
    // Those of a class that derives from the type of an extension are the collector's business in CPython whatever the type says.
    return body->isTracked || !MadeByExtension::of(body->type());
}

bool isTracked(JSCell* cell)
{
    Face* face = Face::tryFromCell(cell);
    return !face || isTracked(face->body());
}

JSCell* faceOfBodyIfAnySlow(JSCell* cell)
{
    Body* body = Body::tryFromCell(cell);
    return body ? body->face() : nullptr;
}

void forEachInstanceThatRefersTo(std::span<const JSValue> values, const ScopedLambda<void(JSCell*)>& function)
{
    Interpreter& current = interpreter();
    // Nothing here lets a collection go on: tp_traverse calls only what may be called with the world stopped.
    for (auto* list : { &current.oldBodies, &current.youngBodies }) {
        for (Body* body : *list) {
            if (!body->face() || !body->isTracked)
                continue;
            bool refers = false;
            traverse(body, [&](JSCell* target, CountTable::Counts&) {
                Body* held = Body::tryFromCellOfHandle(target);
                refers |= std::ranges::find(values, JSValue(held ? held->face() : target)) != values.end();
            });
            if (refers)
                function(body->face());
        }
    }
}

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
