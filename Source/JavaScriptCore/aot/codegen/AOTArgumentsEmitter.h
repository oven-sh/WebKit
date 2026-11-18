/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ArgumentsEmitter final : public OperationEmitter {
public:
    explicit ArgumentsEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateGetArgument(const ValueContext&);
    void generateGetRestLength(const ValueContext&);
    void generateCreateRest(const ValueContext&);

    std::optional<uint32_t> findForwardedRest(ValueID rest) const;
    Word32 restLength(uint32_t firstArgument);
    Pointer restList(uint32_t firstArgument);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
