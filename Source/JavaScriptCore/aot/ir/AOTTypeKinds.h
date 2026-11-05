/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTExpected.h"
#include "AOTID.h"

namespace JSC { namespace AOT { namespace Typed {

struct DeclaredType;
struct Function;

// What an annotation or a guess says of a value. Only a test makes it true.
class ExpectedType {
public:
    constexpr ExpectedType() = default;
    constexpr explicit ExpectedType(TypeID type)
        : m_type(type)
    {
    }

    constexpr TypeID predicted() const { return m_type; }
    constexpr TypeID payload() const { return m_type; }
    constexpr explicit operator bool() const { return !!m_type; }
    friend constexpr bool operator==(ExpectedType, ExpectedType) = default;

private:
    TypeID m_type;
};

// The type of a position as whoever reads the position may take it. It is never the type of a value in Code: that is computed. Only Proven makes one.
class ProvenType {
public:
    constexpr operator TypeID() const { return m_type; }
    constexpr TypeID payload() const { return m_type; }

private:
    friend class ProvenInEveryVersion;
    constexpr ProvenType(TypeID type)
        : m_type(type)
    {
    }

    TypeID m_type;
};

class ProvenInEveryVersion : public ProvenType {
private:
    friend struct Proven;
    constexpr explicit ProvenInEveryVersion(TypeID type)
        : ProvenType(type)
    {
    }
};

// Every reason for which the type of a position may be written down as true. The front end's word is none.
struct Proven {
    // The operation that takes it has no other form.
    static ProvenInEveryVersion byConstruction(TypeID type) { return ProvenInEveryVersion(type); }
    // The compiler made the position and lays down every store into it: an environment, a capture, a token of a class (PositionFlags::IsFromCompiler).
    static ProvenInEveryVersion byCompiler(TypeID type) { return ProvenInEveryVersion(type); }
    // Every store into the position is held to its type at run time: a slot.
    static ProvenInEveryVersion byPosition(TypeID type) { return ProvenInEveryVersion(type); }
    // Every member has been listed: the layouts that a type names or a test asks for. It holds of a value as far as the type or the test does.
    template<typename T> static ProvenValue<T> byClosedSet(T members) { return ProvenValue<T>(std::move(members)); }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
