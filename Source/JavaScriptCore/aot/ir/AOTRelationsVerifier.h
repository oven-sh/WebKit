/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <wtf/Forward.h>

namespace JSC { namespace AOT {

class BuildContext;
struct InternalError;

namespace Typed {

class Program;
struct Function;

// What the relations of a function leave out counts as something that does not happen, so a row that the front end forgets is a false proof.
// This holds each record against the body it was made for, as read. It makes no row, and it looks at no record that the program verifier refuses.
enum class RelationsRule : uint32_t {
    IsReturned = 10,
    IsThrown,
    IsYielded,
    IsStoredInVariable,
    IsStoredInEnvironment,
    IsStoredInUnknownObject,
    IsHandedToUnknownCode,
    IsCalled,
    IsConstructed,
    HasMembersRead,
    HasMembersWritten,
    HasMembersDeletedOrDefined,
    HasKeysEnumerated,
    IsIterated,
    IsCompared,
    IsConvertedToPrimitive,
    IsIncomplete,
    IsHandedToCode,
    IsStoredIntoOperand,
    IsStoredIntoResult,
    ReadIsSubject,
    ContentsAreSubject,
    StoreHasRow,
    IsOperandOfUser,
    KindFitsOperation,
    ResultIsListed,
    StoreIsOnEveryPath,
    StoreIsBeforeTargetLeaves,
};

Vector<InternalError> findErrorsInRelations(FunctionID, const Function&);
bool verifyRelations(const Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
