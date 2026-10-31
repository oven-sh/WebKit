/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

inline FunctionID writtenFunctionOf(const Program& program, FunctionID id)
{
    while (FunctionID source = program.at(id).derivation.source)
        id = source;
    return id;
}

// Flow lost sight of the closures of such a function, so it cannot tell whether a statement renames one. Only a call by value records which closure runs.
inline bool recordsCalleeOnlyOnceRenamed(const Program& program, FunctionID id)
{
    const Function& written = program.at(writtenFunctionOf(program, id));
    return written.has(FunctionCapabilities::RecordsCalleeOnceRenamed) && !written.has(FunctionCapabilities::RecordsCallee);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
