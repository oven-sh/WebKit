/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTFieldGetters.h"
#include "AOTID.h"
#include "AOTImageForward.h"
#include "AOTVector.h"

namespace JSC { namespace AOT {

class BuildContext;
class LinkedFunctions;

namespace Typed {
class Plan;
class Program;
}

struct FieldReturnedByGetter {
    FunctionID getter;
    Typed::PropertyKeyID field;
};

struct CaptureReturnedByFunction {
    FunctionID function;
    Typed::ReturnedCapture returned;
};

struct ObjectOfKeyedLookup {
    FunctionID function;
    Typed::CapturedVariable object;
};

struct BodyFactsForTables {
    BitVector functionsThatOnlyReturn;
    Vector<FieldReturnedByGetter> fieldsReturnedByGetters;
    Vector<CaptureReturnedByFunction> capturesReturnedByFunctions;
    Vector<ObjectOfKeyedLookup> objectsOfKeyedLookups;
    BitVector functionsWithFrames;
};

BodyFactsForTables collectBodyFactsForTables(const Typed::Program&, const Typed::Plan&, const LinkedFunctions&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
