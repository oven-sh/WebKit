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
    // Code of the program may run while the value is written.
    bool writingRunsCode(ValueID) const;
    // The builder may list objects and arrays of the value while they are written. An exception leaves them in the list.
    bool mayListHolders(ValueID) const;
    // An object in the value may hold an object of its own layout, so the value may hold itself. Compiled code goes round until the text is too long.
    bool mayHoldItself(ValueID) const;
    void commit() { }

    IRBuilder& builder;
    const EmissionInputs& inputs;
    ValueID stringBuilder;
    // The function that a stack trace lists below the code of the program that runs while the value is written.
    BuiltinID calledBuiltin { };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
