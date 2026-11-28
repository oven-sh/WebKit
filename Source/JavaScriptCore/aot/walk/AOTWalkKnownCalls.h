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
#include "AOTWalkLinks.h"

namespace JSC { namespace AOT { namespace Typed {

void addEffectsOfEnteringByValue(Effects&);

Reference buildCallAsWrittenThatGives(Walker&, Draft&&, TypeID given);

Effects effectsOfLibraryFunctionBehindRow(const Walker&, const BuiltinOverload&);
std::optional<Effects> effectsOfSearchForWhatIsNoString(const TypeArena&, InlineBuiltin, TypeID searchedOrNotPassed);
bool buildToStringOfPrimitiveByRows(Walker&, const BodyValue& call);
bool callLibraryFunctionBehindRows(Walker&, const BodyValue& call);
inline constexpr OpcodeSet byteLiteralOpcodes { Opcode::Call };
AOT_LINK_OF_WALK(buildByteLiteral, byteLiteralOpcodes)
bool isPrivateNameOfBuiltinMember(const Program&, PropertyKeyID);
ALWAYS_INLINE bool isPrivateAliasOfBuiltinMember(const Program& program, PropertyKeyID key)
{
    return key && !liesInThread(key) && program.at(key).kind == PropertyKeyKind::EnginePrivateName && isPrivateNameOfBuiltinMember(program, key);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
