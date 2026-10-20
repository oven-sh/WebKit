/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <compare>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;

enum class ConstantCollectionKind : uint8_t { Set, Map };

struct ConstantCollection {
    DataID data;
    ConstantCollectionKind kind { ConstantCollectionKind::Set };

    friend bool operator==(const ConstantCollection&, const ConstantCollection&) = default;
    friend auto operator<=>(const ConstantCollection&, const ConstantCollection&) = default;
};

class ConstantCollections {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ConstantCollections);
public:
    ConstantCollections() = default;

    std::span<const ConstantCollection> collections() const LIFETIME_BOUND { return m_collections.span(); }

    void dump(PrintStream&, const Program&) const;

private:
    friend ConstantCollections embedConstantCollections(ProgramEditor&, const FlowResults&, BuildContext&);

    Vector<ConstantCollection> m_collections;
};

ConstantCollections embedConstantCollections(ProgramEditor&, const FlowResults&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
