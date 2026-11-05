/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"
#include "AOTWalkKnownAnswers.h"

namespace JSC { namespace AOT { namespace Typed {

Effects effectsOfDirectCall(Walker&, FunctionID callee, std::span<const Reference> entryArguments);
uint64_t layoutsWrittenBy(Walker&, const Draft&);
uint64_t bitsOfLayoutsOf(Walker&, TypeID);

enum class KnownNote : uint8_t {
    Outcome,
    Relation,
    LengthOfList,
    FlatString,
    ValueKeptByPlace,
    ListTestedInFrontOfLoop,
    NumberReadOutOfList,
};
void noteKnown(Walker&, KnownNote, Reference subject, Reference other, uint32_t word, const LocationMasks& restsOn, uint64_t noted, bool endsAtSuspension = false);
std::optional<uint64_t> findKnown(Walker&, KnownNote, Reference subject, Reference other = { }, uint32_t word = 0);
void endKnown(Walker&, KnownNote, Reference subject, Reference other = { }, uint32_t word = 0);
extern const OpcodeSet opcodesThatFindEqualOperationBuiltTakes;
extern const OpcodeSet opcodesThatNoteOperationBuiltTakes;
Reference findEqualOperationBuiltOfItsOpcodes(Walker&, const Draft&, const Effects&);
ALWAYS_INLINE Reference findEqualOperationBuilt(Walker& walker, const Draft& draft, const Effects& effects) { return opcodesThatFindEqualOperationBuiltTakes.contains(draft.opcode) ? findEqualOperationBuiltOfItsOpcodes(walker, draft, effects) : Reference(); }
void noteOperationBuiltOfItsOpcodes(Walker&, const Draft&, const Effects&, Reference built);
ALWAYS_INLINE void noteOperationBuilt(Walker& walker, const Draft& draft, const Effects& effects, Reference built)
{
    if (opcodesThatNoteOperationBuiltTakes.contains(draft.opcode))
        noteOperationBuiltOfItsOpcodes(walker, draft, effects, built);
}
Reference findConversionMade(Walker&, Reference value, Representation wanted);
void noteConversionMade(Walker&, Reference value, Representation wanted, Reference converted);
void noteWhatBranchDecidesOnEdge(Walker&, BlockID from, uint32_t targetIndex);
std::optional<bool> outcomeKnownOf(Walker&, Reference condition);

void endWhatOperationWritesOrRuns(Walker&, const Draft&, const Effects&);
ALWAYS_INLINE void endWhatOperationWrites(Walker& walker, const Draft& draft, const Effects& effects)
{
    if (!effects.writes.isEmpty() || effects.mayRunOtherCode() || draft.opcode == Opcode::ReleaseEnvironment)
        endWhatOperationWritesOrRuns(walker, draft, effects);
}
bool mayBeSameObject(Walker&, Reference, Reference);


inline constexpr OpcodeSet opcodesThatMayTakeArgumentsLast { Opcode::Call, Opcode::Construct, Opcode::CallDirect };
inline constexpr OpcodeSet opcodesThatMakeArguments { Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments };
class ArgumentsMadeWhereTheyLeave {
    WTF_MAKE_NONCOPYABLE(ArgumentsMadeWhereTheyLeave);
public:
    ArgumentsMadeWhereTheyLeave(Walker&, const BodyValue& call);
    ~ArgumentsMadeWhereTheyLeave();

private:
    Walker& m_walker;
    ValueID m_name;
    Reference m_boundBefore;
};

std::optional<uint32_t> onlyTargetDecidedOnWay(Walker&, const BodyTerminator&);
bool isKnownToLieBelow(Walker&, Reference value, Reference bound);
bool isKnownToLieInside(Walker&, Reference position, Reference length);

bool isKnownToLieInsideList(Walker&, Reference list, Reference position);
std::optional<Effects> effectsOfAppendToListAsMade(Walker&, const Draft& append);

PropertyKeyID nameThatKeyIs(Walker&, Reference key);
PropertyKeyID nameWrittenAsKey(const Walker&, ValueID key);
bool defineAccessorUnderNameThatKeyIs(Walker&, const BodyValue& definition);
} } }

#endif // ENABLE(AOT)
