/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Known summaries: the type of what a call gives that stays as it is written, where the rows of the built-ins that it calls or Link's list of its callees say more than the row of the operation. The rule of the call is handed the type (PositionTypes::resultOfCallBeingTyped()), and the record keeps it among its facts.
// At a suspension: nothing is kept here. At a throw: a call that throws gives nothing, so the type speaks of what is returned alone.
// Withheld: without DirectCalls no list of callees is read, without BuiltinCalls no row is asked. The row's own type stands.

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

// The type stays within what the row gives by itself, so within widestTypeOfOperation(). Null: it says no more than the row, or the positions in force do not hand it on, as in a closed arm.
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

// The verifier of types hands its rule what stands here: PositionsOfTypeVerifier::resultOfCallBeingTyped().
static void fileTypeHandedToRuleForKnownSummaries(Walker& walker, Reference record, TypeID applied)
{
    walker.code().addFactUse(FactUse {
        .record = record.id(),
        .kind = FactKind::OperandSummaries,
        .subjectKind = FactSubject::Kind::Operation,
        .type = applied,
    });
}

static TypeID typeOfResultOfCallAsWrittenForKnownSummaries(Walker& walker, const BodyValue& call, const TypesOfInputs& typeOf)
{
    if (walker.has(Mechanism::BuiltinCalls)) {
        if (TypeID byRows = resultOfCallOfBuiltinsByRows(walker, call, typeOf))
            return byRows;
    }
    if (!walker.has(Mechanism::DirectCalls))
        return { };
    const LinkedCallOfSeveral* listed = walker.facts().linked().known.findCallOfSeveral(walker.currentFunction(), call.id());
    return listed ? listed->result : TypeID();
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

Reference buildAsWrittenWithWhatIsKnown(Walker& walker, const BodyValue& value, Draft&& draft)
{
    if (draft.opcode != value.opcode())
        return walker.build(WTF::move(draft));

    auto typeOfInput = [&](ValueID input) {
        Reference built = input ? walker.referenceOf(input) : Reference();
        return built ? walker.typeOf(built) : TypeID();
    };
    TypesOfInputs typeOf(typeOfInput);
    std::optional<Effects> effects = effectsOfOperationAsWritten(walker, walker.currentFunction(), value, typeOf);
    TypeID proven;
    if (callsAsWrittenTypedForKnownSummaries.contains(draft.opcode))
        proven = typeHeardByRuleForKnownSummaries(walker, draft, typeOfResultOfCallAsWrittenForKnownSummaries(walker, value, typeOf));

    Opcode opcode = draft.opcode;
    walker.setResultOfCallBeingTyped(proven);
    Reference built = walker.build(WTF::move(draft), effects ? &*effects : nullptr);
    walker.setResultOfCallBeingTyped({ });
    if (proven && built && built.isValue() && walker.code().at(built.id()).opcode == opcode)
        fileTypeHandedToRuleForKnownSummaries(walker, built, proven);
    return built;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
