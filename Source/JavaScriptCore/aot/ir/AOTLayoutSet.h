/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaIDs.h"
#include "AOTTypeKinds.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/Assertions.h>

namespace JSC { namespace AOT { namespace Typed {

class IRPrinter;
class TextParser;

// The layouts of the objects that a type names, that a test asks for, or that arrive at an access.
// Complete: each object has one of these. Open: flow saw these arrive beside what it did not see.
struct LayoutSet {
    LayoutSet() = default;

    // Whoever makes a complete set says that no other layout arrives.
    static LayoutSet complete(Vector<LayoutID>&& members) { return { WTF::move(members), true }; }
    static LayoutSet open(Vector<LayoutID>&& members) { return { WTF::move(members), false }; }
    // What the set says, of the layouts that take the place of its members: for whoever copies or renumbers layouts.
    LayoutSet withMembers(Vector<LayoutID>&& others) const { return { WTF::move(others), isComplete }; }

    // Each object of the set has one of these layouts. Nothing where the set is open.
    std::optional<ProvenValue<std::span<const LayoutID>>> all() const LIFETIME_BOUND { return isComplete ? std::optional(Proven::byClosedSet(members.span())) : std::nullopt; }
    // The same, of a set that the verifier holds complete: the set of a type, of a test, of an access by slot, of a family.
    ProvenValue<std::span<const LayoutID>> completeMembers() const LIFETIME_BOUND
    {
        RELEASE_ASSERT(isComplete);
        return Proven::byClosedSet(members.span());
    }
    // What has been seen to arrive. Anything else may arrive too.
    Expected<std::span<const LayoutID>> seen() const LIFETIME_BOUND { return members.span(); }

    bool listsEveryLayout() const { return isComplete; }

    // For printers, tables and the algebra of sets. It says nothing of an object.
    const Vector<LayoutID>& written() const LIFETIME_BOUND { return members; }

    friend bool operator==(const LayoutSet&, const LayoutSet&) = default;

private:
    friend void canonicalize(LayoutSet&);
    friend void parseFields(TextParser&, LayoutSet&);
    friend void printFields(IRPrinter&, const LayoutSet&);
    template<typename Subject, typename Visitor> friend void walkLayoutSet(Subject&, Visitor&);

    LayoutSet(Vector<LayoutID>&& members, bool isComplete)
        : members(WTF::move(members))
        , isComplete(isComplete)
    {
    }

    Vector<LayoutID> members;
    bool isComplete { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
