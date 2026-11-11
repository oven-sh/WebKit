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
struct DeclaredCapture;
struct DeclaredType;
}

namespace Typed {

class IRPrinter;
class RecordChecker;
struct DeclaredCapture;
template<typename> struct PayloadWords;

struct PositionMark {
    consteval PositionMark(PositionFlags flag)
        : flag(flag)
    {
        if (flag == PositionFlags::IsExpected || flag == PositionFlags::IsFromCompiler)
            onlyEstablishAndExpectChangeTheKindOfPosition();
    }

    static void onlyEstablishAndExpectChangeTheKindOfPosition() { }

    PositionFlags flag;
};

struct DeclaredType {
    constexpr DeclaredType() = default;
    explicit DeclaredType(ProvenInEveryVersion proven, TypeSiteID site = { })
        : site(site)
        , type(proven)
        , flags(PositionFlags::IsFromCompiler)
    {
    }
    explicit DeclaredType(ExpectedType expected, TypeSiteID site = { })
        : site(site)
        , type(expected.predicted())
        , flags(PositionFlags::IsExpected)
    {
    }

    bool isExpected() const { return flags.contains(PositionFlags::IsExpected); }
    bool isEstablished() const { return flags.contains(PositionFlags::IsFromCompiler) && !isExpected(); }
    bool isGuarded() const { return !flags.containsAny({ PositionFlags::IsFromCompiler, PositionFlags::IsExpected }); }
    std::optional<ProvenInEveryVersion> established() const { return isEstablished() ? std::optional(Proven::byCompiler(type)) : std::nullopt; }
    ExpectedType expected() const { return ExpectedType(type); }
    TypeID written() const { return type; }
    OptionSet<PositionFlags> writtenFlags() const { return flags; }

    void establish(ProvenInEveryVersion proven)
    {
        type = proven;
        flags.remove(PositionFlags::IsExpected);
        flags.add(PositionFlags::IsFromCompiler);
    }
    void expect(ExpectedType expected)
    {
        type = expected.predicted();
        flags.add(PositionFlags::IsExpected);
    }

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
    friend void read(RecordChecker&, const Wire::DeclaredCapture&, DeclaredCapture&);
    friend void printFields(IRPrinter&, const DeclaredCapture&);
    template<typename Subject, typename Visitor> friend void walkDeclaredCapture(Subject&, Visitor&);

    TypeID type;
    OptionSet<PositionFlags> flags;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
