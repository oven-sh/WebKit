/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <wtf/Assertions.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct Function;

class ProvisionalFunctions {
public:
    static constexpr uint32_t firstIndex = 1u << 31;

    static constexpr bool contains(FunctionID id) { return id && id.index() >= firstIndex; }

    static constexpr uint32_t ordinalOf(FunctionID id)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(contains(id));
        return id.index() - firstIndex;
    }

    static constexpr FunctionID idOf(uint32_t ordinal)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(ordinal < FunctionID::hashTableDeletedIndex - firstIndex);
        return FunctionID(firstIndex + ordinal);
    }

    virtual const Function& at(FunctionID) const = 0;

protected:
    ProvisionalFunctions() = default;
    ~ProvisionalFunctions() = default;
};

class FunctionRemap {
public:
    FunctionRemap() = default;

    explicit FunctionRemap(Vector<FunctionID>&& finalIDs)
        : m_finalIDs(WTF::move(finalIDs))
    {
    }

    bool isIdentity() const { return m_finalIDs.isEmpty(); }

    FunctionID operator()(FunctionID id) const
    {
        if (!ProvisionalFunctions::contains(id))
            return id;
        FunctionID finalID = m_finalIDs[ProvisionalFunctions::ordinalOf(id)];
        RELEASE_ASSERT(finalID);
        return finalID;
    }

    void apply(FunctionID& id) const { id = (*this)(id); }

private:
    Vector<FunctionID> m_finalIDs;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
