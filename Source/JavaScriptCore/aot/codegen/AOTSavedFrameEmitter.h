/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTStubsCoroutines.h"

namespace JSC { namespace AOT { namespace Typed {

class SavedFrameEmitter final : public OperationEmitter {
public:
    explicit SavedFrameEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateNewGenerator(const ValueContext&);
    void generateNewAsyncGenerator(const ValueContext&);
    void generateTakeGeneratorPrototype(const ValueContext&);
    void generateStepAsyncGenerator(const ValueContext&);
    void generateAwaitMayContinue(const ValueContext&);
    void generateAwaitBySavingFrame(const ValueContext&);
    void generateAwaitCallBySavingFrame(const ValueContext&);
    void generateYieldBySavingFrame(const ValueContext&);
    void generateAsyncGeneratorYield(const ValueContext&);
    void generateReturnAsyncResult(const ValueContext&);
    void generatePromiseAllOfElements(const ValueContext&);

    void generateStepGenerator(const ValueContext&);
    void generateIsUnmodifiedGenerator(const ValueContext&);

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

private:
    CoroutineKind kindOfFunction() const;
    CellPointer allocateGenerator(std::optional<CellPointer> closure, LBasicBlock slowPath);
    LValue cellOrZero(ValueID driver);
    LValue driverAsPassed(ValueID driver);
    TypedPointer stateOf(CellPointer generator);
    Pointer framePointerAtThisPoint();
    enum class RunsCodeOfAwaited : bool { InFrameOfFunction, BehindFunction };
    StubCallResult suspend(const ValueContext&, StubKey, std::initializer_list<LValue> operandsInFrontOfSiteWord, ValueID driver, RunsCodeOfAwaited = RunsCodeOfAwaited::InFrameOfFunction);
    void suspendGenerator(const ValueContext&, GeneratorSuspension);
    void defineValueAndWord(const ValueContext&, StubCallResult);
    void branchUnlessUnmodifiedGenerator(Operand, const Type&, LBasicBlock otherwise);

    struct HeapWordsInFrame {
        ValueID made;
        Machine::StackSlot* slot { nullptr };
    };
    void findHeapWordsAliveAtSuspensions();
    bool areNamedOnlyByReadsAndStoresOfWords(ValueID made) const;
    Machine::StackSlot* slotOfHeapWordsInFrame(ValueID frame);
    TypedPointer wordInFrame(Machine::StackSlot*, uint32_t index);

    bool isKeptInHeapFrame(ValueID id) const { return m_wordsOfKeptValues[id.index()]; }
    bool isTracedInHeapFrame(ValueID id) const { return m_wordsOfKeptValues[id.index()] <= m_numberOfTracedWords; }
    bool findBlocksBehindFirstYield();
    bool chooseValuesKeptInHeapFrame(Vector<ValueID, 16>& kept);
    Representation formKeptInHeapFrame(ValueID) const;
    bool choosesWordFor(Representation, bool isRead, ValueID, Vector<ValueID, 16>& kept);
    bool isNamedInThisStretch(uint32_t stretchWhereNamed) const { return stretchWhereNamed == m_stretch || (m_stretchInFront && stretchWhereNamed == m_stretchInFront); }
    LValue readHeapFrameOutOfSlot();
    LValue heapFrame();
    TypedPointer wordOfHeapFrame(LValue frame, ValueID kept);
    void storeIntoHeapFrame(LValue frame, ValueID kept);
    LValue readOutOfHeapFrame(LValue frame, ValueID kept);
    LValue addressOfEntrypoint(unsigned index);
    void makeHeapFrame(const ValueContext& firstYield, CellPointer generator, LValue resumeAddress);
    void yieldOutOfHeapFrame(const ValueContext&);

    std::span<uint16_t> m_wordsOfKeptValues;
    std::span<uint32_t> m_stretchesWhereNamed;
    std::span<LValue> m_keptValuesAsDefined;
    BitVector m_blocksBehindFirstYield;
    BitVector m_valuesStoredInHeapFrame;
    Machine::StackSlot* m_slotOfHeapFrame { nullptr };
    ValueID m_firstYield;
    BlockID m_blockOfFirstYield;
    uint32_t m_numberOfTracedWords { 0 };
    uint32_t m_numberOfWordsInHeapFrame { 0 };
    std::span<uint32_t> m_lastStretchesOfBlocks;
    LValue m_heapFrame { nullptr };
    uint32_t m_stretchOfHeapFrame { 0 };
    uint32_t m_numberOfStretches { 1 };
    uint32_t m_stretch { 1 };
    uint32_t m_stretchInFront { 0 };
    uint16_t m_numberOfEntrypoints { 1 };
    bool m_hasPassedFirstYield { false };

    Vector<HeapWordsInFrame, 2> m_heapWordsInFrame;
    Vector<ValueID, 2> m_heapWordsAliveAtSuspensions;
    bool m_hasLookedForHeapWordsAliveAtSuspensions { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
