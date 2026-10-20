/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "AOTSchemaEnums.h"
#include <array>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

using Typed::PropertyKeyID;

struct StringUses {
    BitVector isNamed;
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
