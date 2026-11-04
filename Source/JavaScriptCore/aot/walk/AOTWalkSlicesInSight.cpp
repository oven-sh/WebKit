/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `var args = Array.prototype.slice.call(arguments, 1)`, `[a, b, c].slice(1)`: the state holds every element of what is sliced, and the bounds are written integers.
// The slice is a list of those elements, made where the call stands. What was sliced is read by nothing, and where nothing else needs it the sweep drops it.
// At a suspension: nothing.
// At a throw: nothing throws.
// Withheld, with EscapeAnalysis or Folding: the call is built by its row.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// A relative index held against the length. Nothing: it is known only when the program runs.
static std::optional<uint32_t> boundOfSliceInSight(const Walker& walker, Reference bound, uint32_t length, uint32_t whereLeftOut)
{
    if (!bound)
        return whereLeftOut;
    if (!bound.isConstant())
        return std::nullopt;
    const Operation& written = walker.code().at(bound.id());
    if (written.opcode == Opcode::Undefined)
        return whereLeftOut;
    if (written.opcode != Opcode::Integer)
        return std::nullopt;
    int64_t relative = written.integer();
    if (relative < 0)
        return static_cast<uint32_t>(std::max<int64_t>(static_cast<int64_t>(length) + relative, 0));
    return static_cast<uint32_t>(std::min<int64_t>(relative, length));
}

Reference buildSliceOfElementsInSight(Walker& walker, const SliceOfElementsInSight& asked)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !walker.has(Mechanism::Folding) || !asked.sliced || walker.isUnreachable())
        return { };
    // Of what is no list no species is asked, and an `arguments` that is as it was made answers by itself for each index below its length.
    std::optional<Vector<Reference, 8>> elements = elementsOfFreshArguments(walker, asked.sliced);
    if (!elements && asked.isListByItsRow && isFreshListAsMade(walker, asked.sliced))
        elements = elementsOfFreshList(walker, asked.sliced);
    if (!elements)
        return { };

    uint32_t length = static_cast<uint32_t>(elements->size());
    std::optional<uint32_t> start = boundOfSliceInSight(walker, asked.bounds.empty() ? Reference() : asked.bounds[0], length, 0);
    std::optional<uint32_t> end = boundOfSliceInSight(walker, asked.bounds.size() < 2 ? Reference() : asked.bounds[1], length, length);
    if (!start || !end)
        return { };

    Draft literal(Opcode::NewArray, asked.origin);
    walker.chooseConservativeFields(literal);
    literal.setContents(TypeTable::anything);
    literal.setElementsOperands(*start < *end ? elements->span().subspan(*start, *end - *start) : std::span<const Reference>());
    literal.expected = asked.expected;
    literal.resultIsReturnedAtOnce = asked.resultIsReturnedAtOnce;
    literal.inputValueGiven = asked.call.id();
    Reference list = walker.build(WTF::move(literal));
    noteFreshList(walker, list);
    if (list)
        walker.noteSliceMadeOfElementsInSight();
    return list;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
