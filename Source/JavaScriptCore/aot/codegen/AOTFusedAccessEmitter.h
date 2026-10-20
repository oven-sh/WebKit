/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

std::optional<CheckedReadKind> checkedReadKindOf(const Program&, const Type&);

class FusedAccessEmitter final : public OperationEmitter {
public:
    explicit FusedAccessEmitter(CodeGenerator&);

    void generatePropGetChecked(const ValueContext&);
    void generatePropGetChain(const ValueContext&);
    void generatePropGetPair(const ValueContext&);
    void generatePropGetAndTest(const ValueContext&);
    void generateCallMethod(const ValueContext&);
    void generateTryPutByIds(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
