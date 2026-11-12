/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"
#include "AOTWalkKnownAnswers.h"

namespace JSC { namespace AOT { namespace Typed {

bool callOnlyMakesGenerator(Walker&, FunctionID callee);
Effects effectsOfDirectCall(Walker&, FunctionID callee, std::span<const Reference> entryArguments);
bool effectsOfDirectCallHangOnArguments(Walker&, FunctionID callee);
bool unitDoesMoreThanBodyForKnownEffects(const Plan&, FunctionID);
uint64_t layoutsWrittenBy(Walker&, const Draft&);
uint64_t bitsOfLayoutsOf(Walker&, TypeID);

enum class KnownNote : uint8_t {
    Outcome,
    Relation,
    LengthOfList,
    PositionFoundInsideList,
    FlatString,
    ValueKeptByPlace,
    HoistedArrayGuard,
    NumberReadOutOfList,
    HoistedFieldOfCopy,
    PassedTestOfBeingAsMade,
    AnswerOfQuestionStillStands,
    ViewOverMemoryThatNoOtherThreadGrows,
    OrderedPieceBounds,
    MemberOfBuiltinIsAsBuilt,
    HolderOfBuiltinHasNoAddedNames,
    RoomOfViewIsAsInFrontOfLoop,
    VectorOfView,
    MirroredLengthOfView,
    MirroredVectorOfView,
    MirroredLengthOfBuffer,
    MirroredVectorOfBuffer,
    NoExtraPropertyUnderKey,
    IsOfNoneOfLayouts,
    StorageOfListMadeInSight,
    PlaceOfKeyInMap,
    KeyIsAbsentFromMap,
    VariableIsAsInFrontOfLoop,
    LoopBoundElementUnchanged,
    NeverLiesBelow,
    NeverAbove,
    LengthOfViewIsFoundAbove,
    ElementReadOffFreshList,
    ElementReadUnderNumber,
    StructureOfClassIsSeen,
    ArrayNameOfInstance,
    SubclassOfArrayName,
    ElementsOfSubclassAreSeen,
    ValueOfFieldOfHeir,
    LoopAppendsToLiteral,
    StaticFieldInVariable,
    StaticFieldValue,
    LengthEqualsInitialCounter,
    FinalLiteralLength,
    CapacityForRemainingIterations,
    PushAppendsToLiteral,
    CapacityCoversLoop,
    LoopCapacityGuard,
    ClassesThatInstanceOfDenied,
    InstanceOfPassedClasses,
    AnswerOfReadOfSlotThatMayBeEmpty,
    PieceIsStoredByBodyThatCutIt,
    AssignedMemberClosure,
    PrototypeOfConstruction,
    ListedOwnKeys,
    FieldArrayInPreheader,
    HoistedUnknownField,
    CarriedResult,
    CarriedElement,
    CarriedField,
    WordOfNumber,
    WideSum,
    LowBitsOfWideSum,
    PositionBelowLength,
    LastIndexOfRegExp,
    NumberOfText,
    ElementOfPresenceTest,
};
inline constexpr AbstractHeapSet elementGuardHeaps {
    AbstractHeap::ArrayStorage, AbstractHeap::Elements, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::ShapeWords
};
void noteKnown(Walker&, KnownNote, Reference subject, Reference other, uint32_t word, const LocationMasks& restsOn, uint64_t noted, bool endsAtSuspension = false);
std::optional<uint64_t> findKnown(const Walker&, KnownNote, Reference subject, Reference other = { }, uint32_t word = 0);
std::optional<uint64_t> findKnown(const Walker&, const WalkState&, KnownNote, Reference subject, Reference other = { }, uint32_t word = 0);
void endKnown(Walker&, KnownNote, Reference subject, Reference other = { }, uint32_t word = 0);
constexpr bool isLookupInTable(InlineBuiltin builtin) { return builtin == InlineBuiltin::MapGet || builtin == InlineBuiltin::MapHas || builtin == InlineBuiltin::SetHas; }

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
void recordEdgeOutcome(Walker&, BlockID from, uint32_t targetIndex);
std::optional<bool> outcomeKnownOf(Walker&, Reference condition);

void endWhatOperationWritesOrRuns(Walker&, const Draft&, const Effects&);
inline constexpr OpcodeSet storesThatNameOnePlace { Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::GetDeferredClosureSlot, Opcode::GetDeferredClosureSlotOrUndefined, Opcode::PutModuleVariable, Opcode::PutEnvironmentField };
std::optional<MemoryLocation> locationThatStoreNames(const Program&, const Draft&, const Effects&);
ALWAYS_INLINE bool storeMayLeaveWhatLiesBeside(const WalkState& state, const Draft& draft)
{
    return storesThatNameOnePlace.contains(draft.opcode) && state.holdsPlacesThatEffectsReach();
}
ALWAYS_INLINE void endWhatOperationWrites(Walker& walker, const Draft& draft, const Effects& effects)
{
    if (!effects.writes.isEmpty() || effects.mayRunOtherCode() || draft.opcode == Opcode::ReleaseEnvironment)
        endWhatOperationWritesOrRuns(walker, draft, effects);
}
bool writerEndsNoGuard(const Walker&, const Draft&);
bool allocatesNewCell(const Operation&);
std::optional<Vector<uint32_t, 2>> objectsMadeHereThatAloneAreWritten(Walker&, const Draft&, const Effects&);
std::optional<Vector<uint32_t, 2>> receiverMadeHereThatAloneIsWritten(Walker&, const TerminatorDraft&, const Effects&);
bool writesReceiverArrayAlone(const Walker&, const Draft& call, const Effects&);
bool writesReceiverArrayAlone(const TerminatorDraft&, const Effects&);
void endWhatIsWrittenInFreshArray(Walker&, Reference array, const Effects&);
bool mayBeSameObject(Walker&, Reference, Reference);
bool isOfLayoutAlone(Walker&, Reference, LayoutID);


inline constexpr OpcodeSet opcodesThatMayTakeArgumentsLast { Opcode::Call, Opcode::Construct, Opcode::CallDirect };
inline constexpr OpcodeSet opcodesThatMakeArguments { Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments };
bool restListMayFallAwayInCopy(const Walker&, FunctionID callee);

class ArgumentsMadeWhereTheyLeave {
    WTF_MAKE_NONCOPYABLE(ArgumentsMadeWhereTheyLeave);
public:
    ArgumentsMadeWhereTheyLeave(Walker&, const BodyValue& call);
    ~ArgumentsMadeWhereTheyLeave();

private:
    void makeRestListOfCopyWhereItLeaves(const BodyValue& call);

private:
    Walker& m_walker;
    ValueID m_name;
    Reference m_boundBefore;
};

std::optional<uint32_t> onlyTargetDecidedOnWay(Walker&, const BodyTerminator&);
bool isKnownToLieBelow(Walker&, Reference value, Reference bound);
bool isKnownToLieInside(Walker&, Reference position, Reference length);
void noteStorageOfListMade(Walker&, Reference list);
void noteLengthThatListIsBornWith(Walker&, Reference list);
struct WrittenListOfVariable {
    explicit operator bool() const { return !!data; }

    DataID data;
    ArrayStorage storage { ArrayStorage::Other };
    uint32_t length { 0 };
};
WrittenListOfVariable writtenListThatVariableHolds(Walker&, VariableID);
WrittenListOfVariable writtenListReadBy(Walker&, ValueID input);
enum class AnswerOfSearch : uint8_t { Membership, Place };
Reference buildSearchInWrittenList(Walker&, DataID, Reference sought, AnswerOfSearch, Origin);
WrittenListOfVariable literalArrayInInstance(Walker&, FieldReference);
bool isReadOutOfVariableOfFileThatStays(Walker&, ValueID input);
void noteStorageOfWrittenList(Walker&, Reference list, const WrittenListOfVariable&);
bool nameStorageOfListMadeInSight(Walker&, Draft& read);
std::optional<uint32_t> lengthOfListMadeInSight(Walker&, Reference list);
std::optional<uint32_t> lengthOfListMadeInSight(const Walker&, const WalkState&, Reference list);
Reference elementWrittenInListMadeInSight(Walker&, Reference list, uint32_t position);
Reference elementOfConstantArray(Walker&, DataID, uint32_t position);
Reference buildSearchInListMadeInSight(Walker&, Reference list, Reference sought, AnswerOfSearch, Origin);
Reference elementWrittenInListMadeInSight(Walker&, const WalkState&, Reference list, uint32_t position);
void checkLengthOfListMadeInSight(Walker&, Reference list, uint32_t length, Origin);
struct WhatListWasMadeWith {
    TypeID proven;
    TypeID shown;
};
WhatListWasMadeWith whatListWasMadeWith(Walker&, Reference list);

bool isKnownToLieInsideList(Walker&, Reference list, Reference position);
std::optional<Effects> effectsOfAppendToListAsMade(Walker&, const Draft& append);

PropertyKeyID nameThatKeyIs(Walker&, Reference key);
PropertyKeyID nameWrittenAsKey(const Walker&, ValueID key);
bool defineAccessorUnderNameThatKeyIs(Walker&, const BodyValue& definition);
} } }

#endif // ENABLE(AOT)
