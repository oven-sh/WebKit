/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTOperationEmitter.h"
#include "AOTStubsCoroutines.h"

namespace JSC { namespace AOT { namespace Typed {

struct LeafStepOfGenerator;

class SavedFrameEmitter final : public OperationEmitter {
    friend class LeafStepPlanner;

public:
    explicit SavedFrameEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateNewGenerator(const ValueContext&);
    void generateNewGeneratorOfLibrary(const ValueContext&);
    void generateNewAsyncGenerator(const ValueContext&);
    void generateTakeGeneratorPrototype(const ValueContext&);
    void generateIsCallDelegated(const ValueContext&);
    void generateGiveDelegatedResult(const ValueContext&);
    void generateCallDelegate(const ValueContext&);
    void generateStepAsyncGenerator(const ValueContext&);
    void generateAwaitMayContinue(const ValueContext&);
    void generateAwaitBySavingFrame(const ValueContext&);
    void generateAwaitCallBySavingFrame(const ValueContext&);
    void generateYieldBySavingFrame(const ValueContext&);
    void generateAsyncGeneratorYield(const ValueContext&);
    void generateReturnAsyncResult(const ValueContext&);
    void generatePromiseAllOfElements(const ValueContext&);
    void generateStartReactionChain(const ValueContext&);

    void generateStepGenerator(const ValueContext&);
    void generateIsUnmodifiedGenerator(const ValueContext&);
    void branchUnlessGeneratorsPlain(CellPointer function, LBasicBlock otherwise);
    void branchUnlessUnmodifiedAsyncGenerator(CellPointer generator, LBasicBlock otherwise);

    void generateNewHeapWords(const ValueContext&);
    void generateGetHeapFrameWord(const ValueContext&);
    void generatePutHeapFrameWord(const ValueContext&);

    bool layOutHeapFrame();
    bool standsBehindFirstYield(BlockID block) const { return m_blocksBehindFirstYield.get(block.index()) || (block == m_blockOfFirstYield && m_hasPassedFirstYield); }
    void keepInHeapFrame(const ValueContext& defined);
    void beginStretchOfHeapFrame(BlockID, LValue frameHandedIn = nullptr);
    ValueID readBackOutOfHeapFrame(BlockID, ValueID yieldInFront);
    void passHandlerArgumentsInLandingPad(BlockID site, const Edge& handler);
    LValue phiOfKeptParameter(ValueID parameter) const { return m_keptValuesAsDefined[parameter.index()]; }
    bool keepsWordsInMachineFrame() const { return m_keepsWordsInMachineFrame; }
    bool hasWordInMachineFrame(ValueID id) const { return m_keepsWordsInMachineFrame && m_wordsOfKeptValues[id.index()]; }
    void clearWordsInMachineFrame();
    bool isReadAsNamedInAnotherStretch(ValueID id) const { return hasWordInMachineFrame(id) && !m_passesHandlerArguments && !isNamedInThisStretch(m_stretchesWhereNamed[id.index()]); }
    const Machine::StackSlot* slotOfKeptWords() const { return m_slotOfKeptWords; }
    bool leavesMachineFrameAtSuspensions() const { return m_slotOfHeapFrame; }
    bool clearsWordsOnWayBackAlong(const Edge& edge) { return !findWordsToClearOnWayBackAlong(edge).isEmpty(); }
    void clearWordsOnWayBackAlong(const Edge&);
    bool liesInLoopInsideStretch(BlockID block) const { return !m_frontsOfLoopsInsideStretches.empty() && m_frontsOfLoopsInsideStretches[block.index()]; }
    bool goesOnInStretchAroundLoop(BlockID);
    uint32_t numberOfTracedWords() const { return m_numberOfTracedPlaces; }

private:
    CoroutineKind kindOfFunction() const;
    Word32 loadStructureIDOfGenerators(CellPointer closure, LBasicBlock otherwise);
    CellPointer allocateGenerator(std::optional<CellPointer> closure, GeneratorFunctionOrigin, LBasicBlock slowPath);
    GeneratorFunctionOrigin originOfGeneratorFunction(FunctionID) const;
    LValue cellOrZero(ValueID driver);
    LValue driverAsPassed(ValueID driver);
    TypedPointer stateOf(CellPointer generator);
    Pointer framePointerAtThisPoint();
    enum class RunsCodeOfAwaited : bool { InFrameOfFunction, BehindFunction };
    bool awaitsOnlyResultOfCall(const ValueContext& await) const;
    StubCallResult suspend(const ValueContext&, StubKey, std::initializer_list<LValue> operandsInFrontOfSiteWord, ValueID driver, RunsCodeOfAwaited = RunsCodeOfAwaited::InFrameOfFunction);
    void suspendGenerator(const ValueContext&, GeneratorSuspension);
    void defineValueAndWord(const ValueContext&, StubCallResult);
    void branchUnlessUnmodifiedGenerator(Operand, const Type&, LBasicBlock otherwise);

    struct UsesOfHeapWords {
        uint32_t numberOfAccesses { 0 };
        bool isUsedOtherwise { false };
    };
    void findHeapWordsAliveAtSuspensions();
    void findUsesOfHeapWords();
    bool areNamedOnlyByReadsAndStoresOfWords(ValueID made);
    Machine::StackSlot* slotOfHeapWordsInFrame(ValueID frame);
    TypedPointer wordInFrame(Machine::StackSlot*, uint32_t index);

    bool isKeptInHeapFrame(ValueID id) const { return m_wordsOfKeptValues[id.index()]; }
    bool isTracedInHeapFrame(ValueID id) const { return m_wordsOfKeptValues[id.index()] <= m_numberOfTracedWords; }
    bool findBlocksBehindFirstYield();
    bool chooseValuesKeptInHeapFrame(Vector<ValueID, 16>& kept);
    Representation formKeptInHeapFrame(ValueID) const;
    bool choosesWordFor(Representation, bool isRead, ValueID, Vector<ValueID, 16>& kept);
    bool isNamedInThisStretch(uint32_t stretchWhereNamed) const { return stretchWhereNamed == m_stretch || (m_stretchInFront && stretchWhereNamed == m_stretchInFront); }
    bool isNamedAbove(uint32_t stretchWhereNamed);
    bool heapFrameIsNamed() { return m_stretchOfHeapFrame == m_stretch || (m_heapFrameHoldsBelowItsStretch && isNamedAbove(m_stretchOfHeapFrame)); }
    uint32_t stretchThatGoesOnInto(BlockID);
    LValue readHeapFrameOutOfSlot();
    LValue heapFrame();
    TypedPointer wordOfHeapFrame(LValue frame, ValueID kept);
    void storeIntoHeapFrame(LValue frame, ValueID kept);
    LValue readOutOfHeapFrame(LValue frame, ValueID kept);
    LValue addressOfEntrypoint(unsigned index);
    LValue addressOfLeafStep(const LeafStepOfGenerator*);
    void findLeafSteps();
    const LeafStepOfGenerator* leafStepOf(ValueID yield) const;
    void storeLeafStep(ValueID yield);
    void makeHeapFrame(const ValueContext& firstYield, CellPointer generator, LValue resumeAddress);
    void yieldOutOfHeapFrame(const ValueContext&);
    bool mayBeSetInHeapFrame(ValueID traced) const { return m_wordsThatMayBeSet.get(m_wordsOfKeptValues[traced.index()] - 1); }
    void followWordsThatMayBeSetInto(BlockID);
    void clearWordsOfWhatIsDeadAcross(const ValueContext& yield);
    void clearWordOfHeapFrame(size_t word);
    Vector<uint32_t, 8> findWordsToClearOnWayBackAlong(const Edge&);
    void layBlocksOfEachLoopTogether();
    void findLoopsInsideStretches();
    void giveWordsWhoseLivesAreApartOnePlace(uint32_t numberOfWords);
    uint32_t placeOfWord(size_t word) const { return m_placesOfWords.empty() ? static_cast<uint32_t>(word) : m_placesOfWords[word]; }
    void findWordsNeededAtStartsOfBlocks();
    template<typename Functor> void forEachArgumentReadAlong(const Edge&, const Functor&) const;
    template<typename Read, typename Defined> ValueID forEachValueReadInStretch(BlockID, ValueID yieldInFront, const Read&, const Defined&) const;

    std::span<uint16_t> m_wordsOfKeptValues;
    std::span<uint32_t> m_stretchesWhereNamed;
    std::span<LValue> m_keptValuesAsDefined;
    BitVector m_blocksBehindFirstYield;
    BitVector m_valuesStoredInHeapFrame;
    BitVector m_wordsThatMayBeSet;
    Vector<BitVector> m_wordsThatMayBeSetAtStartsOfBlocks;
    std::span<uint32_t> m_placesOfBlocks;
    std::span<uint32_t> m_placesBehindLoops;
    std::span<uint32_t> m_tracedWordsDefinedInFrontOfPlaces;
    struct RangeOfWords {
        uint32_t first { 0 };
        uint32_t end { 0 };
        uint32_t size() const { return end - first; }
    };
    std::span<RangeOfWords> m_changesOfNeedsAtTurns;
    std::span<RangeOfWords> m_wordsReadLastAtTurns;
    std::span<uint16_t> m_changesOfNeeds;
    std::span<uint16_t> m_wordsReadLast;
    std::span<uint32_t> m_headsAroundTurns;
    BitVector m_turnsOfHeads;
    BitVector m_wordsNeeded;
    UncheckedKeyHashMap<uint32_t, BitVector> m_wordsNeededAtHeads;
    BitVector m_blocksAboveYields;
    Vector<uint32_t, 8> m_wordsToClearAtStart;
    uint32_t m_numberOfBlocksFollowed { 0 };
    bool m_everyWayBackLeadsToWhatDominates { true };
    Machine::StackSlot* m_slotOfHeapFrame { nullptr };
    ValueID m_firstYield;
    BlockID m_blockOfFirstYield;
    uint32_t m_numberOfTracedWords { 0 };
    uint32_t m_numberOfWordsInHeapFrame { 0 };
    std::span<uint32_t> m_lastStretchesOfBlocks;
    LValue m_heapFrame { nullptr };
    uint32_t m_stretchOfHeapFrame { 0 };
    bool m_standsAtStartOfStretch { false };
    mutable size_t m_placeBehindSuspensionFoundLast { 0 };
    bool m_heapFrameHoldsBelowItsStretch { true };
    uint32_t m_numberOfStretches { 1 };
    uint32_t m_stretch { 1 };
    uint32_t m_stretchInFront { 0 };
    Vector<uint32_t, 64> m_firstStretchesAbove;
    Vector<BlockID, 64> m_blocksOfStretches;
    std::span<uint32_t> m_suspensionsInFrontOfPlaces;
    uint16_t m_numberOfEntrypoints { 1 };
    Vector<std::pair<ValueID, const LeafStepOfGenerator*>, 1> m_leafSteps;
    bool m_hasPassedFirstYield { false };
    bool m_keepsWordsInMachineFrame { false };
    bool m_passesHandlerArguments { false };
    Machine::StackSlot* m_slotOfKeptWords { nullptr };
    std::span<uint16_t> m_placesOfWords;
    std::span<uint16_t> m_numbersOfNeededWordsAtPlaces;
    uint32_t m_numberOfTracedPlaces { 0 };
    std::span<uint32_t> m_frontsOfLoopsInsideStretches;
    std::span<uint32_t> m_headsOfLoopsInsideStretchesBehind;
    std::span<const BlockID> m_orderOfBlocks;

    UncheckedKeyHashMap<uint32_t, Machine::StackSlot*, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_slotsOfHeapWords;
    UncheckedKeyHashMap<uint32_t, UsesOfHeapWords, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_usesOfHeapWords;
    BitVector m_heapWordsAliveAtSuspensions;
    bool m_hasFoundUsesOfHeapWords { false };
    bool m_hasLookedForHeapWordsAliveAtSuspensions { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
