/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTSavedFrameMap.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "CallData.h"
#include "GPRInfo.h"
#include "JSObject.h"
#include "MarkedBlockCage.h"
#include "MarkedSpace.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/Atomics.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class JSPromise;

namespace AOT {

struct Instance;

// What a suspension stub pushes. It writes preservedRegister only where SavedFrame::preservedRegister is a register.
struct SuspensionWords {
    uint64_t siteWord;
    uint64_t preservedRegister;
};

enum class SuspensionKind : uint8_t { Await, Yield };

struct SuspendedMachineFrame {
    // Of the words from bottom to frame, those that a SavedFrame keeps.
    inline size_t numberOfWords() const;
    inline const uint64_t* firstSavedWord() const;

    uint64_t* frame;
    const uint64_t* bottom;
    void* resumeAddress;
    const SuspensionWords* words;
};

class SavedFrame final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetCallData;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    template<typename Visitor> ALWAYS_INLINE static void visitFields(SavedFrame*, Visitor&);
    static CallData getCallData(JSCell*);

#if CPU(X86_64)
    static constexpr GPRReg preservedRegister = X86Registers::ebx;
#else
    static constexpr GPRReg preservedRegister = InvalidGPRReg;
#endif
    static constexpr uint32_t locationInRegister = 0;
    static uint32_t locationInFrame(intptr_t offsetFromFramePointer)
    {
        RELEASE_ASSERT(offsetFromFramePointer < 0 && !(offsetFromFramePointer % static_cast<intptr_t>(sizeof(uint64_t))));
        return static_cast<uint32_t>(-offsetFromFramePointer / static_cast<intptr_t>(sizeof(uint64_t)));
    }
    static uint32_t narrowLocationInFrame(intptr_t offsetFromFramePointer)
    {
        RELEASE_ASSERT(offsetFromFramePointer < 0 && !(offsetFromFramePointer % static_cast<intptr_t>(sizeof(uint32_t))));
        return static_cast<uint32_t>(-offsetFromFramePointer / static_cast<intptr_t>(sizeof(uint32_t)));
    }
    static constexpr uint32_t locationHolding(uint32_t narrowLocation) { return (narrowLocation + 1) / 2; }
    static constexpr bool isInUpperHalf(uint32_t narrowLocation) { return narrowLocation % 2; }

    // The site word, from bit 0: the offset of the live slot map, the saved registers or pairs, the number of words skipped at the bottom, 32 bits of promise location.
    // A promise near the frame pointer leaves the upper half zero, which makes the word a short immediate.
    static constexpr unsigned liveSlotMapOffsetBits = 18;
    static constexpr uint32_t functionSavesPreservedRegister = 1u << liveSlotMapOffsetBits;
    static constexpr uint32_t functionSavesR15 = 2u << liveSlotMapOffsetBits;
    static constexpr unsigned savedRegisterBits = 2;
    static constexpr unsigned skippedAtBottomShift = liveSlotMapOffsetBits + savedRegisterBits;
    static constexpr unsigned skippedAtBottomBits = 7;
    static constexpr unsigned promiseLocationShift = skippedAtBottomShift + skippedAtBottomBits;
    static_assert(promiseLocationShift + 32 <= 64);
    static constexpr size_t maxNumberOfWords = UINT16_MAX;
    // deadWordsAtBottom counts the outgoing arguments of the function and the stack slots that are live across no suspension.
    static std::optional<uint64_t> siteWord(uint32_t liveSlotMapOffset, uint32_t promiseLocation, uint32_t savedRegistersOrPairs, uint32_t deadWordsAtBottom = 0, size_t numberOfWordsInFrame = 0)
    {
        if (liveSlotMapOffset >> liveSlotMapOffsetBits || savedRegistersOrPairs >> savedRegisterBits || numberOfWordsInFrame > maxNumberOfWords)
            return std::nullopt;
        uint64_t skippedAtBottom = std::min(deadWordsAtBottom, (1u << skippedAtBottomBits) - 1) & ~1u;
        return liveSlotMapOffset | savedRegistersOrPairs << liveSlotMapOffsetBits | static_cast<uint64_t>(promiseLocation) << promiseLocationShift | skippedAtBottom << skippedAtBottomShift;
    }
    static constexpr uint32_t liveSlotMapOffsetOf(uint64_t siteWord) { return static_cast<uint32_t>(siteWord) & ((1u << liveSlotMapOffsetBits) - 1); }
    static constexpr uint32_t savedRegistersOrPairsOf(uint64_t siteWord) { return static_cast<uint32_t>(siteWord >> liveSlotMapOffsetBits) & ((1u << savedRegisterBits) - 1); }
    static constexpr uint32_t promiseLocationOf(uint64_t siteWord) { return static_cast<uint32_t>(siteWord >> promiseLocationShift); }
    // The callee-save area holds registers of whoever resumed the function, and every restore writes it anew. The number of words kept has to stay even.
    static constexpr uint32_t skippedAtTopOf(uint64_t siteWord)
    {
        uint32_t savedRegistersOrPairs = savedRegistersOrPairsOf(siteWord);
#if CPU(X86_64)
        return savedRegistersOrPairs == (functionSavesPreservedRegister | functionSavesR15) >> liveSlotMapOffsetBits ? 2 : 0;
#else
        return 2 * savedRegistersOrPairs;
#endif
    }
    static constexpr uint32_t skippedAtBottomOf(uint64_t siteWord) { return static_cast<uint32_t>(siteWord >> skippedAtBottomShift) & ((1u << skippedAtBottomBits) - 1); }

    // It decodes as a cell and is mapped nowhere, so the first use traps.
    static constexpr uint64_t poisonedWord = 0x0000dead00000000ULL;

    static SavedFrame* create(VM&, Instance&, CoroutineKind, size_t numberOfWords, uint32_t skippedAtTop, uint32_t skippedAtBottom);
    static SavedFrame* create(VM&, Instance&, CoroutineKind, const SuspendedMachineFrame&);
    static SavedFrame* saveAsyncFunction(VM&, Instance&, const SuspendedMachineFrame&);
    // For a function that is still on the stack and was never suspended. The frame is saved into it later, from the same site.
    static SavedFrame* createForAsyncFunction(VM&, Instance&, const SuspendedMachineFrame&);
    static SavedFrame* forFrame(const Instance&, const uint64_t* frame);
    void save(VM&, const SuspendedMachineFrame&, SuspensionKind);
    void willBeRestoredToStack()
    {
        willWrite();
        m_liveSlotMapOffsetPlusOne = 0;
        didWrite();
    }

    CoroutineKind kind() const { return m_kind; }
    bool isOnStack() const { return !m_liveSlotMapOffsetPlusOne; }
    void setIsEnteredByJobLoop(bool isEnteredByJobLoop) { m_flags = isEnteredByJobLoop ? m_flags | isEnteredByJobLoopFlag : m_flags & ~isEnteredByJobLoopFlag; }
    SuspensionKind suspensionKind() const { return m_flags & isSuspendedAtYieldFlag ? SuspensionKind::Yield : SuspensionKind::Await; }
    // What the site of a StepAsyncGenerator reads beside the value it is resumed with. Every suspension resets it to Result.
    void setStepStatus(AsyncGeneratorStepStatus status) { m_flags = (m_flags & ~stepStatusMask) | static_cast<uint8_t>(status) << stepStatusShift; }
    AsyncGeneratorStepStatus stepStatus() const { return static_cast<AsyncGeneratorStepStatus>((m_flags & stepStatusMask) >> stepStatusShift); }
    const void* resumeAddress() const { return m_resumeAddress; }
    std::optional<uint64_t> wordAt(uint32_t location) const;

    // The index of the element is added to CombinatorElement: m_asyncContext is not free for it, because the call that this frame waits for writes it.
    enum class Continuation : uint8_t { None, Promise, Waiter, CombinatorElement };
    static constexpr uint32_t maxCombinatorElements = 32;

    Continuation continuation() const { return std::min(m_continuation, Continuation::CombinatorElement); }
    JSPromise* promise() const;
    SavedFrame* waiter() const;
    // The promise that Promise.all returned, if the function is one of its elements.
    JSPromise* promiseOfCombinator() const;
    JSCell* promiseOrWaiter() const { return m_promiseOrWaiter; }
    void setWaiter(VM&, SavedFrame* waiter, JSValue asyncContext);
    void queueResumeJob(JSGlobalObject*, VM&, JSValue resolution, bool isRejected, JSValue asyncContext);
    void setCombinatorElement(VM&, JSCell* globalContext, uint32_t index);
    void fulfill(JSGlobalObject* globalObject, VM& vm, JSValue value) { settle(globalObject, vm, value, false); }
    void reject(JSGlobalObject* globalObject, VM& vm, JSValue error) { settle(globalObject, vm, error, true); }
    JSPromise* ensurePromise(JSGlobalObject*, VM&);

    void poisonDeadUnlistedWords(VM&);

    static constexpr uint8_t isEnteredByJobLoopFlag = 1;
    static constexpr uint8_t isSuspendedAtYieldFlag = 2;
    static constexpr unsigned stepStatusShift = 2;
    static constexpr uint8_t stepStatusMask = 3 << stepStatusShift;
    AOT_CLASS_THRESHOLD(uint32_t, minWordsForStringMove, 32);
    static constexpr size_t maxWordsAllocatedInStub = (MarkedSpace::largeCutoff - 64) / sizeof(uint64_t);
    static constexpr ptrdiff_t offsetOfNumberOfWords() { return OBJECT_OFFSETOF(SavedFrame, m_numberOfWords); }
    static constexpr ptrdiff_t offsetOfNumberOfWordsSkippedAtTop() { return OBJECT_OFFSETOF(SavedFrame, m_numberOfWordsSkippedAtTop); }
    static constexpr ptrdiff_t offsetOfNumberOfWordsSkippedAtBottom() { return OBJECT_OFFSETOF(SavedFrame, m_numberOfWordsSkippedAtBottom); }
    static constexpr ptrdiff_t offsetOfVersion() { return OBJECT_OFFSETOF(SavedFrame, m_version); }
    static constexpr ptrdiff_t offsetOfLiveSlotMapOffsetPlusOne() { return OBJECT_OFFSETOF(SavedFrame, m_liveSlotMapOffsetPlusOne); }
    static constexpr ptrdiff_t offsetOfKind() { return OBJECT_OFFSETOF(SavedFrame, m_kind); }
    static constexpr ptrdiff_t offsetOfNumberOfSavedRegisterPairs() { return OBJECT_OFFSETOF(SavedFrame, m_numberOfSavedRegisterPairs); }
    static constexpr ptrdiff_t offsetOfFlags() { return OBJECT_OFFSETOF(SavedFrame, m_flags); }
    static constexpr ptrdiff_t offsetOfContinuation() { return OBJECT_OFFSETOF(SavedFrame, m_continuation); }
    static constexpr ptrdiff_t offsetOfResumeAddress() { return OBJECT_OFFSETOF(SavedFrame, m_resumeAddress); }
    static constexpr ptrdiff_t offsetOfPromiseOrWaiter() { return OBJECT_OFFSETOF(SavedFrame, m_promiseOrWaiter); }
    static constexpr ptrdiff_t offsetOfAsyncContext() { return OBJECT_OFFSETOF(SavedFrame, m_asyncContext); }
    static constexpr ptrdiff_t offsetOfPreservedRegister() { return OBJECT_OFFSETOF(SavedFrame, m_preservedRegister); }
    static constexpr ptrdiff_t offsetOfWords() { return sizeof(SavedFrame); }
    static constexpr size_t allocationSize(size_t numberOfWords) { return sizeof(SavedFrame) + numberOfWords * sizeof(uint64_t); }

private:
    SavedFrame(VM& vm, Structure* structure, CoroutineKind kind, uint16_t numberOfWords, uint8_t skippedAtTop, uint8_t skippedAtBottom)
        : Base(vm, structure)
        , m_numberOfWords(numberOfWords)
        , m_numberOfWordsSkippedAtTop(skippedAtTop)
        , m_numberOfWordsSkippedAtBottom(skippedAtBottom)
        , m_kind(kind)
    {
    }

    uint64_t* words() { return reinterpret_cast<uint64_t*>(this + 1); }
    const uint64_t* words() const { return reinterpret_cast<const uint64_t*>(this + 1); }
    void willWrite()
    {
        ++m_version;
        WTF::storeStoreFence();
    }
    void didWrite()
    {
        WTF::storeStoreFence();
        ++m_version;
    }
    std::optional<SavedFrameMap> liveSlotMap(uint32_t offsetPlusOne) const;
    std::optional<uint32_t> narrowReferenceAt(uint32_t narrowLocation) const;
    void settle(JSGlobalObject*, VM&, JSValue, bool isRejected);
    uint32_t combinatorIndex() const { return static_cast<uint32_t>(m_continuation) - static_cast<uint32_t>(Continuation::CombinatorElement); }

    // The frame has m_numberOfWordsSkippedAtTop words above the words kept and m_numberOfWordsSkippedAtBottom words below them.
    uint16_t m_numberOfWords;
    uint8_t m_numberOfWordsSkippedAtTop;
    uint8_t m_numberOfWordsSkippedAtBottom;
    uint32_t m_version { 0 };
    uint32_t m_liveSlotMapOffsetPlusOne { 0 };
    CoroutineKind m_kind;
    Continuation m_continuation { Continuation::None };
    uint8_t m_numberOfSavedRegisterPairs { 0 };
    uint8_t m_flags { 0 };
    void* m_resumeAddress { nullptr };
    JSCell* m_promiseOrWaiter { nullptr };
    uint64_t m_preservedRegister { 0 };
    EncodedJSValue m_asyncContext { 0 };
};
static_assert(sizeof(SavedFrame) == 64);
static_assert(SavedFrame::allocationSize(SavedFrame::maxWordsAllocatedInStub) <= MarkedSpace::largeCutoff && SavedFrame::maxWordsAllocatedInStub <= SavedFrame::maxNumberOfWords);
static_assert(SavedFrame::offsetOfNumberOfWords() == sizeof(JSObjectWithButterfly) && SavedFrame::offsetOfVersion() == SavedFrame::offsetOfNumberOfWords() + 4);
static_assert(SavedFrame::offsetOfNumberOfWordsSkippedAtTop() == SavedFrame::offsetOfNumberOfWords() + 2 && SavedFrame::offsetOfNumberOfWordsSkippedAtBottom() == SavedFrame::offsetOfNumberOfWords() + 3);
static_assert(SavedFrame::offsetOfKind() + 1 == SavedFrame::offsetOfContinuation() && SavedFrame::offsetOfContinuation() + 1 == SavedFrame::offsetOfNumberOfSavedRegisterPairs() && SavedFrame::offsetOfNumberOfSavedRegisterPairs() + 1 == SavedFrame::offsetOfFlags());
static_assert(static_cast<uint32_t>(SavedFrame::Continuation::CombinatorElement) + SavedFrame::maxCombinatorElements <= UINT8_MAX + 1);
static_assert(!static_cast<unsigned>(AsyncGeneratorStepStatus::Result) && numberOfEnumerators<AsyncGeneratorStepStatus> <= (SavedFrame::stepStatusMask >> SavedFrame::stepStatusShift) + 1);

inline size_t SuspendedMachineFrame::numberOfWords() const
{
    size_t numberOfSkippedWords = SavedFrame::skippedAtTopOf(words->siteWord) + SavedFrame::skippedAtBottomOf(words->siteWord);
    RELEASE_ASSERT(static_cast<size_t>(frame - bottom) >= numberOfSkippedWords);
    return frame - bottom - numberOfSkippedWords;
}

inline const uint64_t* SuspendedMachineFrame::firstSavedWord() const
{
    return bottom + SavedFrame::skippedAtBottomOf(words->siteWord);
}

ALWAYS_INLINE std::optional<uint64_t> SavedFrame::wordAt(uint32_t location) const
{
    if (location == locationInRegister) {
        if constexpr (preservedRegister == InvalidGPRReg)
            return std::nullopt;
        return m_preservedRegister;
    }
    uint32_t lastLocation = m_numberOfWords + m_numberOfWordsSkippedAtTop;
    if (location <= m_numberOfWordsSkippedAtTop || location > lastLocation)
        return std::nullopt;
    return words()[lastLocation - location];
}

ALWAYS_INLINE std::optional<uint32_t> SavedFrame::narrowReferenceAt(uint32_t narrowLocation) const
{
    std::optional<uint64_t> word = wordAt(locationHolding(narrowLocation));
    if (!word)
        return std::nullopt;
    return static_cast<uint32_t>(isInUpperHalf(narrowLocation) ? *word >> 32 : *word);
}

template<typename Visitor>
ALWAYS_INLINE void SavedFrame::visitFields(SavedFrame* saved, Visitor& visitor)
{
    visitor.appendUnbarriered(saved->m_promiseOrWaiter);
    visitor.appendUnbarriered(JSValue::decode(saved->m_asyncContext));
    uint32_t version = saved->m_version;
    WTF::loadLoadFence();
    Vector<JSCell*, 32> cells;
    Vector<uint32_t, 8> narrowReferences;
    auto collectLiveCells = [&](uint32_t offsetPlusOne) {
        std::optional<SavedFrameMap> map = saved->liveSlotMap(offsetPlusOne);
        if (!map)
            return false;
        bool hasAllValues = map->forEachValue([&](uint32_t location) {
            std::optional<uint64_t> word = saved->wordAt(location);
            if (!word)
                return false;
            JSValue value = JSValue::decode(static_cast<EncodedJSValue>(*word));
            if (value && value.isCell())
                cells.append(value.asCell());
            return true;
        });
        return hasAllValues && map->forEachNarrowReference([&](uint32_t narrowLocation) {
            std::optional<uint32_t> reference = saved->narrowReferenceAt(narrowLocation);
            if (!reference)
                return false;
            narrowReferences.append(*reference);
            return true;
        });
    };
    // A reader that races with save() can see a new resume address with an old offset. The version says afterwards whether it did.
    bool isWellFormed = true;
    if (uint32_t offsetPlusOne = saved->m_liveSlotMapOffsetPlusOne; offsetPlusOne && !(version & 1))
        isWellFormed = collectLiveCells(offsetPlusOne);
    WTF::loadLoadFence();
    if (version & 1 || saved->m_version != version) {
        visitor.didRace(saved, "a SavedFrame is being written");
        return;
    }
    RELEASE_ASSERT(isWellFormed);
    for (JSCell* referencedCell : cells)
        visitor.appendUnbarriered(referencedCell);
    for (uint32_t reference : narrowReferences)
        visitNarrowReference(visitor, saved, reference);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
