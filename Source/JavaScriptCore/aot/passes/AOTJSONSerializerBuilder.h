/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONEmission.h"

namespace JSC { namespace AOT { namespace Typed {

struct JSONSerializerBuilder {
    // False: appendRoot() would write nothing and leave for whenThereIsNoText.
    bool hasPlanFor(ValueID) const;
    // All that it writes is in the string builder when it returns.
    void appendRoot(ValueID, BlockID whenThereIsNoText);
    void commit() { }

    IRBuilder& builder;
    const EmissionInputs& inputs;
    ValueID stringBuilder;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
