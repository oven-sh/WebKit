#pragma once

#include "HeapCell.h"
#include <wtf/ScopedLambda.h>

namespace JSC {

class Heap;

// Owns the cells of every subspace whose CellAttributes say CellLifetime::ExplicitlyFreed.
//
// Between collections the client may hand a cell it has freed straight back out: the heap does not
// hear about either event. It only learns which cells are free at the end of marking, and reclaims
// those. Both functions run with the world stopped, on whichever thread conducts the collection.
class ExplicitlyFreedCellClient {
public:
    virtual ~ExplicitlyFreedCellClient() = default;

    // Marking has converged. This is the last moment at which a mark on an explicitly freed cell
    // means "marking reached it"; afterwards every cell that stays is marked.
    virtual void didConvergeMarking(Heap&) { }

    // Calls the functor with every cell that is free. The heap takes them back, so the client must
    // forget them.
    virtual void takeFreedCells(Heap&, const ScopedLambda<void(HeapCell*)>&) = 0;
};

} // namespace JSC
