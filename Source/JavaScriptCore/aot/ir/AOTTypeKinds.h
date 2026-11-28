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

struct Proven {
    static ProvenInEveryVersion byConstruction(TypeID type) { return ProvenInEveryVersion(type); }
    static ProvenInEveryVersion byCompiler(TypeID type) { return ProvenInEveryVersion(type); }
    static ProvenInEveryVersion byPosition(TypeID type) { return ProvenInEveryVersion(type); }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
