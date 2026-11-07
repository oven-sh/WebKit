/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTWalkKnown.h"
#include "AOTWalkKnownCalls.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet callsAsWrittenTypedForKnownSummaries { Opcode::Call, Opcode::CallList, Opcode::Construct, Opcode::ConstructList, Opcode::SuperConstruct, Opcode::SuperConstructList };

static TypeID typeHeardByRuleForKnownSummaries(Walker& walker, const Draft& call, TypeID found)
{
    if (!found)
        return { };
    TypeArena& types = walker.types();
    TypeID byRow = walker.resultTypeOf(call);
    if (!byRow)
        return { };
    TypeID applied = types.intersectionOf(found, byRow);
    if (!applied || isNeverType(applied) || applied == byRow || !types.isSubset(applied, byRow))
        return { };
    walker.setResultOfCallBeingTyped(applied);
    bool isHeard = walker.resultTypeOf(call) == applied;
    walker.setResultOfCallBeingTyped({ });
    return isHeard ? applied : TypeID();
}

static void fileTypeHandedToRuleForKnownSummaries(Walker& walker, Reference record, TypeID applied)
{
    walker.code().addFactUse(FactUse {
        .record = record.id(),
        .kind = FactKind::OperandSummaries,
        .subjectKind = FactSubject::Kind::Operation,
        .type = applied,
    });
}

Reference buildCallAsWrittenThatGives(Walker& walker, Draft&& draft, TypeID given)
{
    RELEASE_ASSERT(callsAsWrittenTypedForKnownSummaries.contains(draft.opcode));
    TypeID proven = typeHeardByRuleForKnownSummaries(walker, draft, given);
    Opcode opcode = draft.opcode;
    walker.setResultOfCallBeingTyped(proven);
    Reference built = walker.build(WTF::move(draft));
    walker.setResultOfCallBeingTyped({ });
    if (proven && built && built.isValue() && walker.code().at(built.id()).opcode == opcode)
        fileTypeHandedToRuleForKnownSummaries(walker, built, proven);
    return built;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
