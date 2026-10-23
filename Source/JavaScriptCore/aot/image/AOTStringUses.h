/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include <array>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

using Typed::PropertyKeyID;

struct StringUses {
    BitVector isNamed;
    // Named by something else than the operation that makes the cell of a string constant and those that have the text for a message: an operation that reads the text, a key, a regular expression.
    BitVector isNamedBesideCreation;
    BitVector isConstant;
    BitVector isCreatedRepeatedly;
    BitVector isConstantInTables;
    BitVector isAtom;
    Vector<Vector<uint32_t>> namedByFunction;
    Vector<uint32_t> functionsInSourceOrder;
};

struct KeyUse {
    uint32_t uses { 0 };
    uint32_t tableLookupUses { 0 };
    FunctionID firstTableLookupFunction;
    std::array<uint32_t, numberOfEnumerators<Typed::Access>> usesByAccess { };
};

struct KeyUses {
    Vector<KeyUse> byKey;
    BitVector keysNamedByTables;
    BitVector stringsNamedAsKeysByTables;
};

struct ProgramUses {
    StringUses strings;
    KeyUses keys;
};

ProgramUses collectProgramUses(const Typed::PipelineOutput&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
