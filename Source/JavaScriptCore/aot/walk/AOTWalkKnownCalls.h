/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// What other files of the walk call beside walk/AOTWalkKnown.h.

// AOTWalkKnownEffects.cpp. What entering a function by its value does in front of the body.
void addEffectsOfEnteringByValue(Effects&);
// For whoever takes effects out of Link's tables: in built code a field of a promoted environment is a variable, and Link goes on speaking of the field.
void addVariablesThatStandForFields(const Plan&, Effects&);

// AOTWalkKnownSummaries.cpp. A call or a construction as it is written, of which the caller has proven what it gives. `given` may be null.
Reference buildCallAsWrittenThatGives(Walker&, Draft&&, TypeID given);

// AOTWalkKnownRows.cpp. Each goes by the table of built-ins and by types alone.
// Of a row that names a function of the library: Link's effects of that function. Unknown code where the program holds no body for it.
Effects effectsOfLibraryFunctionBehindRow(const Walker&, const BuiltinOverload&);
// `xs.indexOf(n)`, `xs.includes(o)`: nothing: the code in line does what the schema declares for it.
std::optional<Effects> effectsOfSearchForWhatIsNoString(const TypeArena&, InlineBuiltin, TypeID searchedOrNotPassed);
// `n.toString()`, `flag.toString()`, `big.toString(10)`: the conversion. True: the call is built and bound.
bool buildToStringOfPrimitiveByRows(Walker&, const BodyValue& call);
// A built-in whose rows all name one function of the library: the direct call, or a copy. True: the call is built and bound.
bool callLibraryFunctionBehindRows(Walker&, const BodyValue& call);
// `$push`, `$Array`: only the host stores under such a key.
bool isPrivateAliasOfBuiltinMember(const Program&, PropertyKeyID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
