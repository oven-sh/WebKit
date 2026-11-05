/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkCoroutinePositions.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCallingConventions.h"
#include "AOTFacts.h"
#include "AOTFamilies.h"
#include "AOTLinkTables.h"
#include "AOTObjects.h"
#include "AOTTypeArena.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

TypeID typeHeldOfResultOfCoroutine(const Walker& walker, FunctionID callee)
{
    if (!walker.facts().held().findCoroutine(callee) && !walker.facts().linked().findCoroutine(callee))
        return { };
    TypeArena& types = walker.types();
    return types.forObject(types.objects().coroutineResultOf(callee));
}

TypeID typeOfPromiseExpectedToGive(Walker& walker, TypeID settledWith)
{
    TypeArena& types = walker.types();
    if (!settledWith || !types.at(settledWith).isJavaScriptValue())
        return { };
    return types.builtinClass(BuiltinClasses::Promise, singleElementSpan(settledWith));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
