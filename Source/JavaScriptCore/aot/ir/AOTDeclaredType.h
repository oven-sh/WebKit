/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "AOTSchemaIDs.h"
#include "AOTTypeKinds.h"
#include <optional>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

namespace Wire {
struct DeclaredType;
}

namespace Typed {

class IRPrinter;
class RecordChecker;
template<typename> struct PayloadWords;

// Any flag of a position but the one that says of which kind its type is.
struct PositionMark {
    consteval PositionMark(PositionFlags flag)
        : flag(flag)
    {
        if (flag == PositionFlags::IsExpected)
            onlyEstablishAndExpectChangeTheKindOfPosition();
    }

    static void onlyEstablishAndExpectChangeTheKindOfPosition() { }

    PositionFlags flag;
};

// The type of a position: a parameter, a result, a field, a variable, a capture, the target of a conversion.
// Established: every store into the position is held to the type. Expected: the position takes whatever arrives.
struct DeclaredType {
    constexpr DeclaredType() = default;
    explicit DeclaredType(ProvenInEveryVersion proven, TypeSiteID site = { })
        : site(site)
        , type(proven)
    {
    }
    explicit DeclaredType(ExpectedType expected, TypeSiteID site = { })
        : site(site)
        , type(expected.predicted())
        , flags(PositionFlags::IsExpected)
    {
    }

    bool isExpected() const { return flags.contains(PositionFlags::IsExpected); }
    std::optional<ProvenInEveryVersion> established() const { return isExpected() ? std::nullopt : std::optional(Proven::byPosition(type)); }
    ExpectedType expected() const { return ExpectedType(type); }
    // For printers, tables and comparisons of declarations. It says nothing of a value.
    TypeID written() const { return type; }
    OptionSet<PositionFlags> writtenFlags() const { return flags; }

    void establish(ProvenInEveryVersion proven)
    {
        type = proven;
        flags.remove(PositionFlags::IsExpected);
    }
    void expect(ExpectedType expected)
    {
        type = expected.predicted();
        flags.add(PositionFlags::IsExpected);
    }

    // The kind stays: what is proven of an established position is expected of an expected one.
    void changeType(ProvenInEveryVersion proven) { type = proven; }

    bool has(PositionMark mark) const { return flags.contains(mark.flag); }
    void add(PositionMark mark) { flags.add(mark.flag); }
    void remove(PositionMark mark) { flags.remove(mark.flag); }
    DeclaredType with(PositionMark mark) const
    {
        DeclaredType marked = *this;
        marked.add(mark);
        return marked;
    }

    friend bool operator==(const DeclaredType&, const DeclaredType&) = default;

    TypeSiteID site;

private:
    friend struct PayloadWords<DeclaredType>;
    friend void read(RecordChecker&, const Wire::DeclaredType&, DeclaredType&);
    friend void printFields(IRPrinter&, const DeclaredType&);
    template<typename Subject, typename Visitor> friend void walkDeclaredType(Subject&, Visitor&);

    TypeID type;
    OptionSet<PositionFlags> flags;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
