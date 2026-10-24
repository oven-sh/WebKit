/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallGraph;
class Program;

enum class StackCheck : uint8_t { Required, CoveredByCallers, DeferredToCallee };

ASCIILiteral nameOf(StackCheck);

class StackChecks {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(StackChecks);
public:
    StackChecks() = default;
    explicit StackChecks(Vector<StackCheck>&& byFunction)
        : m_byFunction(WTF::move(byFunction))
    {
    }

    StackCheck of(FunctionID id) const { return id && id.index() < m_byFunction.size() ? m_byFunction[id.index()] : StackCheck::Required; }

    void dump(PrintStream&, const Program&) const;

private:
    Vector<StackCheck> m_byFunction;
};

StackChecks elideStackChecks(const Program&, const CallGraph&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
