/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNarrowReferenceValidation.h"

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTHeaps.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTStubRegisters.h"
#include "AOTVector.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlock.h"
#include "B3Const32Value.h"
#include "B3Const64Value.h"
#include "B3EliminateDeadCode.h"
#include "B3InsertionSetInlines.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <algorithm>
#include <span>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/IndexMap.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT {

namespace {

// A word is a value whose low 32 bits are a narrow reference, or differ from one by a constant.
class NarrowReferenceValidation {
public:
    NarrowReferenceValidation(B3::Procedure& proc, const HeapsOfCollection& heaps, const NarrowReferenceOrigins& origins, NarrowReferenceFindings& findings)
        : m_proc(proc)
        , m_heaps(heaps)
        , m_origins(origins)
        , m_findings(findings)
    {
    }

    void run()
    {
        if (mayHaveWords())
            analyze();
    }

    // Gives every word that run() would report a pointer: an opaque copy of the sum that decodes it, right behind its definition.
    uint32_t insertGuards()
    {
        if (!mayHaveWords())
            return 0;
        m_removedDeadCode = B3::eliminateDeadCodeImpl(m_proc);
        m_insertsGuards = true;
        analyze();
        if (m_unprotectedWords.isEmpty())
            return 0;
        createPointers();
        usePointers();
        return m_pointerBeside.size();
    }

    bool removedDeadCode() const { return m_removedDeadCode; }

private:
    enum class Reports : bool { No, Yes };
    using PointerAndWord = std::pair<B3::Value*, B3::Value*>;

    struct Facts {
        static constexpr unsigned none = std::numeric_limits<unsigned>::max();

        unsigned wordNumber { none };
        unsigned pointerNumber { none };
        B3::Value* wordOfSum { nullptr };
        B3::BasicBlock* owner { nullptr };
        B3::Value* lastUse { nullptr };
    };

    enum class EventKind : uint8_t {
        WordDef,
        PointerDef,
        WordUse,
        PointerUse,
        WordUpsilon,
        PointerUpsilon,
        Safepoint,
    };

    struct Event {
        EventKind kind;
        unsigned number { Facts::none };
        unsigned incoming { Facts::none };
        B3::Value* value { nullptr };
    };

    struct BlockState {
        unsigned eventsBegin { 0 };
        unsigned eventsEnd { 0 };
        unsigned positionInPostOrder { 0 };
        unsigned cycle { 0 };
        // A round over a cycle reads the head of a block in front as the same round left it, and any other as the round before did.
        bool isReadInNextRound { false };
        B3::BasicBlock* lastReaderInRound { nullptr };
        unsigned numberOfPredecessorsReported { 0 };
        B3::BasicBlock* lastPredecessorReported { nullptr };
    };

    struct SuccessorPositions {
        unsigned size() const { return block->numSuccessors(); }
        unsigned operator[](unsigned index) const { return states[block->successorBlock(index)].positionInPostOrder; }

        B3::BasicBlock* block;
        const IndexMap<B3::BasicBlock*, BlockState>& states;
    };

    void analyze()
    {
        findWords();
        if (m_words.isEmpty())
            return;
        if (m_hasSafepoint)
            checkSafepoints();
        if (!m_insertsGuards)
            countStoresIntoFrame();
    }

    void checkSafepoints()
    {
        findPointers();

        Vector<B3::BasicBlock*> postOrder = m_proc.blocksInPostOrder();
        m_wordsAtHead.resize(m_proc.size());
        m_pointersAtHead.resize(m_proc.size());
        m_blockStates.resize(m_proc.size());
        for (unsigned position = 0; position < postOrder.size(); ++position) {
            m_blockStates[postOrder[position]].positionInPostOrder = position;
            collectEvents(postOrder[position]);
        }

        forEachComponent(postOrder, [&](std::span<B3::BasicBlock* const> blocks, bool isCycle) {
            if (isCycle)
                solveCycle(blocks);
            for (B3::BasicBlock* block : blocks)
                walk(block, Reports::Yes);
        });
        m_findings.numberOfReferencesAcrossSafepoints += m_wordsAcrossSafepoints.bitCount();
    }

    // Each strongly connected component comes behind those that it leads to, with its blocks in post order.
    template<typename Functor>
    void forEachComponent(const Vector<B3::BasicBlock*>& postOrder, const Functor& functor)
    {
        if (!hasBackEdge(postOrder)) {
            for (B3::BasicBlock* const& block : postOrder)
                functor(std::span<B3::BasicBlock* const> { &block, 1 }, false);
            return;
        }
        Typed::StronglyConnectedComponents components = Typed::computeStronglyConnectedComponents(postOrder.size(), [&](unsigned position) {
            return SuccessorPositions { postOrder[position], m_blockStates };
        });
        Vector<B3::BasicBlock*> blocks;
        for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
            uint32_t begin = components.firstMemberOf[component];
            std::span<uint32_t> positions = components.members.mutableSpan().subspan(begin, components.firstMemberOf[component + 1] - begin);
            AOT::sort(positions);
            blocks.shrink(0);
            for (uint32_t position : positions)
                blocks.append(postOrder[position]);
            functor(blocks.span(), components.isInCycle.get(positions[0]));
        }
    }

    // Leaves what is live at the heads that a round reads from the round before. Once those stay, one more round finds every other head.
    void solveCycle(std::span<B3::BasicBlock* const> blocks)
    {
        unsigned cycle = ++m_numberOfCycles;
        for (B3::BasicBlock* block : blocks)
            m_blockStates[block].cycle = cycle;
        for (B3::BasicBlock* block : blocks) {
            for (B3::BasicBlock* successor : block->successorBlocks()) {
                BlockState& state = m_blockStates[successor];
                if (state.cycle != cycle)
                    continue;
                if (state.positionInPostOrder < m_blockStates[block].positionInPostOrder)
                    state.lastReaderInRound = block;
                else
                    state.isReadInNextRound = true;
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (B3::BasicBlock* block : blocks)
                changed |= walk(block, Reports::No);
        }
    }

    // What is live at the head of a block is read by its predecessors alone.
    bool isLastReader(B3::BasicBlock* block, B3::BasicBlock* successor, Reports reports)
    {
        BlockState& state = m_blockStates[successor];
        if (reports == Reports::No)
            return state.lastReaderInRound == block && !state.isReadInNextRound;
        return std::exchange(state.lastPredecessorReported, block) != block && ++state.numberOfPredecessorsReported == successor->numPredecessors();
    }

    const Facts& factsOf(B3::Value* value) const
    {
        static constexpr Facts factsOfAddedValue { };
        return value->index() < m_facts.size() ? m_facts[value->index()] : factsOfAddedValue;
    }

    bool isWord(B3::Value* value) const { return factsOf(value).wordNumber != Facts::none; }
    bool isPointer(B3::Value* value) const { return factsOf(value).pointerNumber != Facts::none; }

    bool hasBackEdge(const Vector<B3::BasicBlock*>& postOrder) const
    {
        for (unsigned position = 0; position < postOrder.size(); ++position) {
            for (B3::BasicBlock* successor : postOrder[position]->successorBlocks()) {
                if (m_blockStates[successor].positionInPostOrder >= position)
                    return true;
            }
        }
        return false;
    }

    static bool differs(const BitVector& now, const BitVector& before)
    {
        return before.isEmpty() ? !now.isEmpty() : now != before;
    }

    static bool isCellCageBase(B3::Value* value)
    {
        auto* argument = value->as<B3::ArgumentRegValue>();
        return argument && argument->argumentReg() == Reg(cellCageBaseGPR);
    }

    static bool isBase(B3::Value* value)
    {
        return isCellCageBase(value) || (value->opcode() == B3::Add && isCellCageBase(value->child(0)) && value->child(1)->hasInt());
    }

    static bool isShift(B3::Value* value, B3::Opcode opcode)
    {
        return value->opcode() == opcode && value->child(1)->isInt32(cell32Shift);
    }

    static bool keepsLow32Bits(B3::Value* value)
    {
        switch (value->opcode()) {
        case B3::Identity:
        case B3::Opaque:
        case B3::ZExt32:
        case B3::SExt32:
        case B3::Trunc:
            return true;
        case B3::BitAnd:
            return value->child(1)->hasInt() && (value->child(1)->asInt() & 0xffffffffll) == 0xffffffffll;
        default:
            return false;
        }
    }

    static B3::Value* withoutIdentity(B3::Value* value)
    {
        while (value->opcode() == B3::Identity)
            value = value->child(0);
        return value;
    }

    static B3::Value* pointerSource(B3::Value* value)
    {
        while (value->opcode() == B3::Identity || value->opcode() == B3::Opaque)
            value = value->child(0);
        return value;
    }

    static B3::Value* sourceOfLow32Bits(B3::Value* value)
    {
        while (keepsLow32Bits(value))
            value = value->child(0);
        return value;
    }

    static bool isEncodingOf(B3::Value* word, B3::Value* pointer)
    {
        word = sourceOfLow32Bits(word);
        return isShift(word, B3::ZShr) && pointerSource(word->child(0)) == sourceOfLow32Bits(pointer);
    }

    static B3::Value* wordOf(B3::Value* value, bool* isDisplaced = nullptr)
    {
        for (;;) {
            if (keepsLow32Bits(value) || isShift(value, B3::Shl)) {
                value = value->child(0);
                continue;
            }
            switch (value->opcode()) {
            case B3::Add:
            case B3::Sub:
                if (value->type() != B3::Int32 || !value->child(1)->hasInt32())
                    return value;
                if (isDisplaced && value->child(1)->asInt32())
                    *isDisplaced = true;
                value = value->child(0);
                continue;
            case B3::Select: {
                B3::Value* first = value->child(1);
                B3::Value* second = value->child(2);
                if (first->isConstant() || isEncodingOf(second, first))
                    value = second;
                else if (second->isConstant() || isEncodingOf(first, second))
                    value = first;
                else
                    return value;
                continue;
            }
            default:
                return value;
            }
        }
    }

    static B3::Value* wordDecodedBy(B3::Value* sum, bool& isDisplaced)
    {
        if (sum->opcode() != B3::Add)
            return nullptr;
        for (unsigned i = 0; i < 2; ++i) {
            if (isShift(sum->child(i), B3::Shl) && isBase(sum->child(!i)))
                return wordOf(sum->child(i)->child(0), &isDisplaced);
        }
        return nullptr;
    }

    static bool isAddressOf(B3::Value* user, unsigned child)
    {
        return user->as<B3::MemoryValue>() && child + 1 == user->numChildren();
    }

    static bool isLateUse(B3::ValueRep::Kind constraint)
    {
        return constraint == B3::ValueRep::LateColdAny || constraint == B3::ValueRep::LateRegister || constraint == B3::ValueRep::SomeLateRegister;
    }

    static bool isPlainCopyOfLow32Bits(B3::Value* value)
    {
        switch (value->opcode()) {
        case B3::Identity:
        case B3::Opaque:
        case B3::ZExt32:
        case B3::Trunc:
            return true;
        default:
            return false;
        }
    }

    // The offset from the cell, if the sum is the address of the cell that the word names plus a constant.
    static std::optional<int64_t> offsetOfPlainSum(B3::Value* sum, B3::Value* word)
    {
        for (unsigned i = 0; i < 2; ++i) {
            if (!isShift(sum->child(i), B3::Shl) || !isBase(sum->child(!i)))
                continue;
            B3::Value* index = sum->child(i)->child(0);
            while (index != word && isPlainCopyOfLow32Bits(index))
                index = index->child(0);
            if (index != word)
                return std::nullopt;
            B3::Value* base = sum->child(!i);
            return isCellCageBase(base) ? 0 : base->child(1)->asInt();
        }
        return std::nullopt;
    }

    void createPointer(B3::InsertionSet& insertionSet, unsigned index, B3::Value* cageBase, B3::Value* word)
    {
        B3::Origin origin = word->origin();
        B3::Value* low32Bits = word->type() == B3::Int32 ? word : insertionSet.insert<B3::Value>(index, B3::Trunc, origin, word);
        B3::Value* extended = insertionSet.insert<B3::Value>(index, B3::ZExt32, origin, low32Bits);
        B3::Value* shift = insertionSet.insert<B3::Const32Value>(index, origin, cell32Shift);
        B3::Value* shifted = insertionSet.insert<B3::Value>(index, B3::Shl, origin, extended, shift);
        B3::Value* sum = insertionSet.insert<B3::Value>(index, B3::Add, origin, cageBase, shifted);
        m_pointerBeside.add(word, insertionSet.insert<B3::Value>(index, B3::Opaque, origin, sum));
    }

    void createPointers()
    {
        B3::InsertionSet insertionSet(m_proc);
        // In front of everything, because a word may be an argument register.
        B3::Value* cageBase = insertionSet.insert<B3::ArgumentRegValue>(0, B3::Origin(), cellCageBaseGPR);
        insertionSet.execute(m_proc.at(0));
        Vector<B3::BasicBlock*> blocksEndedByWords;
        uint32_t numberOfGuardsThatMayBeDropped = 0;
        auto isDropped = [&](B3::Value* word) {
            if (!m_origins.ordinalOfDroppedGuard)
                return false;
            Typed::ValueID origin = Typed::valueOfOrigin(word->origin());
            return origin && m_origins.originsOfDroppedGuard.get(origin.index()) && numberOfGuardsThatMayBeDropped++ == *m_origins.ordinalOfDroppedGuard;
        };
        for (B3::BasicBlock* block : m_proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                B3::Value* word = block->at(index);
                if (!isWord(word) || !m_unprotectedWords.get(factsOf(word).wordNumber) || m_displacedWords.contains(word) || isDropped(word))
                    continue;
                if (isShift(word, B3::ZShr)) {
                    m_pointerBeside.add(word, word->child(0));
                    continue;
                }
                if (word->effects().terminal) {
                    blocksEndedByWords.append(block);
                    continue;
                }
                createPointer(insertionSet, index + 1, cageBase, word);
            }
            insertionSet.execute(block);
        }
        for (B3::BasicBlock* block : blocksEndedByWords) {
            createPointer(insertionSet, 0, cageBase, block->last());
            insertionSet.execute(block->successorBlock(0));
        }
    }

    void usePointers()
    {
        B3::InsertionSet insertionSet(m_proc);
        for (B3::BasicBlock* block : m_proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                B3::Value* value = block->at(index);
                if (!factsOf(value).owner)
                    continue;
                auto keepAlive = [&](B3::Value* word) {
                    B3::Value* pointer = m_pointerBeside.get(word);
                    if (!pointer)
                        return;
                    auto* patchpoint = insertionSet.insert<B3::PatchpointValue>(value->effects().terminal ? index : index + 1, B3::Void, value->origin());
                    patchpoint->effects = B3::Effects::none();
                    patchpoint->effects.writesLocalState = true;
                    patchpoint->effects.reads = B3::HeapRange::top();
                    patchpoint->append(pointer, B3::ValueRep::ColdAny);
                    patchpoint->setRunsWithoutFrame(true);
                    patchpoint->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });
                };
                auto becomesPointer = [&](B3::Value* sum, B3::Value* word) {
                    return m_pointerBeside.contains(word) && offsetOfPlainSum(sum, word);
                };

                if (auto* upsilon = value->as<B3::UpsilonValue>()) {
                    if (isWord(upsilon->phi()))
                        keepAlive(wordOf(upsilon->child(0)));
                    continue;
                }
                if (value->opcode() == B3::Phi)
                    continue;
                if (B3::Value* word = factsOf(value).wordOfSum) {
                    if (!becomesPointer(value, word)) {
                        if (isPointer(value))
                            keepAlive(word);
                        continue;
                    }
                    B3::Value* pointer = m_pointerBeside.get(word);
                    if (int64_t offset = *offsetOfPlainSum(value, word))
                        pointer = insertionSet.insert<B3::Value>(index, B3::Add, value->origin(), pointer, insertionSet.insert<B3::Const64Value>(index, value->origin(), offset));
                    m_replacements.append({ value, pointer });
                    continue;
                }
                if (wordOf(value) != value)
                    continue;
                for (unsigned i = 0; i < value->numChildren(); ++i) {
                    B3::Value* child = withoutIdentity(value->child(i));
                    if (B3::Value* word = factsOf(child).wordOfSum; word && isAddressOf(value, i)) {
                        if (!becomesPointer(child, word))
                            keepAlive(word);
                        continue;
                    }
                    if (isPointer(pointerSource(child)))
                        continue;
                    if (!usesOnlyBits(value, i))
                        keepAlive(wordOf(child));
                }
            }
            insertionSet.execute(block);
        }
        for (auto [sum, pointer] : m_replacements)
            sum->replaceWithIdentity(pointer);
    }

    static bool isFrameAddress(B3::Value* address)
    {
        while (address->opcode() == B3::Identity || (address->opcode() == B3::Add && address->child(1)->hasInt()))
            address = address->child(0);
        return address->opcode() == B3::SlotBase;
    }

    void countStoresIntoFrame()
    {
        for (B3::BasicBlock* block : m_proc) {
            for (B3::Value* value : *block) {
                if (value->opcode() == B3::Store && value->child(0)->type() == B3::Int32 && isFrameAddress(value->lastChild()) && isWord(wordOf(value->child(0))))
                    ++m_findings.numberOfReferencesStoredInFrame;
            }
        }
    }

    bool isPermanent(B3::Value* word) const
    {
        auto* memory = word->as<B3::MemoryValue>();
        if (!memory || memory->opcode() != B3::Load || !memory->range())
            return false;
        return std::ranges::any_of(m_heaps.permanentCells, [&](B3::HeapRange cells) {
            return cells.begin() <= memory->range().begin() && memory->range().end() <= cells.end();
        });
    }

    bool definesReference(B3::Value* value) const
    {
        bool isLoadOrParameter = (value->opcode() == B3::Load || value->opcode() == B3::Phi) && value->type() == B3::Int32;
        if (!isLoadOrParameter && !isShift(value, B3::ZShr))
            return false;
        Typed::ValueID origin = Typed::valueOfOrigin(value->origin());
        if (!origin)
            return false;
        return m_origins.references.get(origin.index()) || (m_origins.laterReferences && m_origins.laterReferences->get(origin.index()));
    }

    bool add(unsigned Facts::* number, Vector<B3::Value*>& list, B3::Value* value)
    {
        if (value->isConstant() || m_facts[value->index()].*number != Facts::none)
            return false;
        m_facts[value->index()].*number = list.size();
        list.append(value);
        return true;
    }

    bool addWord(B3::Value* word)
    {
        return !isPermanent(word) && add(&Facts::wordNumber, m_words, word);
    }

    bool addPointer(B3::Value* pointer)
    {
        return add(&Facts::pointerNumber, m_pointers, pointer);
    }

    bool mayHaveWords() const
    {
        for (B3::BasicBlock* block : m_proc) {
            for (B3::Value* value : *block) {
                if (definesReference(value) || (value->opcode() == B3::Add && (isBase(value->child(0)) || isBase(value->child(1)))))
                    return true;
            }
        }
        return false;
    }

    void findWords()
    {
        m_facts.resize(m_proc.values().size());
        for (B3::BasicBlock* block : m_proc) {
            for (B3::Value* value : *block) {
                m_facts[value->index()].owner = block;
                if (!m_hasSafepoint)
                    m_hasSafepoint = isSafepoint(value);
                if (auto* upsilon = value->as<B3::UpsilonValue>())
                    m_upsilons.add(upsilon->phi(), Vector<B3::UpsilonValue*, 2> { }).iterator->value.append(upsilon);
                if (definesReference(value))
                    addWord(value);
                bool isDisplaced = false;
                B3::Value* word = wordDecodedBy(value, isDisplaced);
                if (!word)
                    continue;
                m_facts[value->index()].wordOfSum = word;
                addWord(word);
                if (isDisplaced) {
                    m_displacedWords.add(word);
                    m_displacedSums.add(value);
                }
            }
        }
        for (bool changed = !m_words.isEmpty(); changed;) {
            changed = false;
            for (auto& entry : m_upsilons) {
                if (!entry.key->type().isInt() || !isWord(entry.key))
                    continue;
                for (B3::UpsilonValue* upsilon : entry.value) {
                    B3::Value* incoming = wordOf(upsilon->child(0));
                    changed |= incoming->type() == B3::Int32 && addWord(incoming);
                }
            }
        }
    }

    void findPointers()
    {
        Vector<B3::Value*> selects;
        for (B3::BasicBlock* block : m_proc) {
            for (B3::Value* value : *block) {
                for (unsigned i = 0; i < value->numChildren(); ++i) {
                    B3::Value* child = withoutIdentity(value->child(i));
                    if (factsOf(child).wordOfSum && !isAddressOf(value, i))
                        addPointer(child);
                }
                if (value->type() == B3::Int64 && value->opcode() == B3::Select && !isWord(value) && wordOf(value) == value)
                    selects.append(value);
            }
        }
        for (unsigned i = 0; i < m_words.size(); ++i) {
            if (isShift(m_words[i], B3::ZShr))
                addPointer(pointerSource(m_words[i]->child(0)));
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (B3::Value* value : selects) {
                if (isPointer(pointerSource(value->child(1))) || isPointer(pointerSource(value->child(2))))
                    changed |= addPointer(value);
            }
            for (auto& entry : m_upsilons) {
                if (entry.key->type() != B3::Int64 || isWord(entry.key))
                    continue;
                for (B3::UpsilonValue* upsilon : entry.value) {
                    if (isPointer(pointerSource(upsilon->child(0))))
                        changed |= addPointer(entry.key);
                }
            }
        }

        m_pointersOfWord.resize(m_words.size());
        for (unsigned number = 0; number < m_pointers.size(); ++number) {
            B3::Value* pointer = m_pointers[number];
            if (pointer->opcode() == B3::Select || pointer->opcode() == B3::Phi)
                m_joinedPointers.set(number);
            B3::Value* word = factsOf(pointer).wordOfSum;
            if (!word)
                continue;
            if (isWord(word) && protects(pointer, word))
                m_pointersOfWord[factsOf(word).wordNumber].append(number);
        }
        for (unsigned number = 0; number < m_words.size(); ++number) {
            B3::Value* source = pointerSource(isShift(m_words[number], B3::ZShr) ? m_words[number]->child(0) : m_words[number]);
            if (isPointer(source) && protects(source, m_words[number]))
                m_pointersOfWord[number].append(factsOf(source).pointerNumber);
        }
    }

    bool protects(B3::Value* pointer, B3::Value* word)
    {
        bool isOutermost = !m_depth;
        if (isOutermost)
            m_assumed.shrink(0);
        ++m_depth;
        bool result = protectsAssumingCycles(pointerSource(pointer), word);
        --m_depth;
        if (isOutermost && !result) {
            for (auto& key : m_assumed) {
                if (auto found = m_protects.find(key); found != m_protects.end() && found->value)
                    m_protects.remove(found);
            }
        }
        return result;
    }

    bool protectsAssumingCycles(B3::Value* pointer, B3::Value* word)
    {
        if (word->isConstant() || (pointer == pointerSource(word) && pointer->type() == B3::Int64))
            return true;
        if (factsOf(pointer).wordOfSum == word)
            return m_displacedWords.contains(word) == m_displacedSums.contains(pointer);
        if (isShift(word, B3::ZShr) && pointerSource(word->child(0)) == pointer)
            return true;
        if (pointer->opcode() != B3::Select && pointer->opcode() != B3::Phi)
            return false;

        PointerAndWord key { pointer, word };
        auto added = m_protects.add(key, true);
        if (!added.isNewEntry)
            return added.iterator->value;
        m_assumed.append(key);
        bool result = pointer->opcode() == B3::Select ? selectProtects(pointer, word) : phiProtects(pointer, word);
        m_protects.set(key, result);
        return result;
    }

    bool selectProtects(B3::Value* pointer, B3::Value* word)
    {
        for (unsigned i = 1; i <= 2; ++i) {
            B3::Value* other = pointer->child(3 - i);
            if ((other->isConstant() || wordOf(other) == word) && protectsAssumingCycles(pointerSource(pointer->child(i)), word))
                return true;
        }
        if (word->opcode() != B3::Select || word->child(0) != pointer->child(0))
            return false;
        return protectsAssumingCycles(pointerSource(pointer->child(1)), wordOf(word->child(1))) && protectsAssumingCycles(pointerSource(pointer->child(2)), wordOf(word->child(2)));
    }

    bool phiProtects(B3::Value* pointer, B3::Value* word)
    {
        auto pointers = m_upsilons.find(pointer);
        if (pointers == m_upsilons.end())
            return false;
        bool wordIsJoinedThere = word->opcode() == B3::Phi && factsOf(word).owner == factsOf(pointer).owner;
        for (B3::UpsilonValue* incoming : pointers->value) {
            B3::Value* incomingWord = word;
            if (wordIsJoinedThere) {
                incomingWord = nullptr;
                if (auto words = m_upsilons.find(word); words != m_upsilons.end()) {
                    for (B3::UpsilonValue* candidate : words->value) {
                        if (factsOf(candidate).owner == factsOf(incoming).owner)
                            incomingWord = wordOf(candidate->child(0));
                    }
                }
            }
            if (!incomingWord || !protectsAssumingCycles(pointerSource(incoming->child(0)), incomingWord))
                return false;
        }
        return true;
    }

    bool isSafepoint(B3::Value* value) const
    {
        if (value->opcode() != B3::Patchpoint && value->opcode() != B3::CCall)
            return false;
        B3::Effects effects = value->effects();
        return effects.writes.overlaps(m_heaps.collectorState) && effects.writes.overlaps(m_heaps.weakTableStorage) && effects.reads.overlaps(m_heaps.frameSlots);
    }

    static bool savesFrame(B3::Value* safepoint)
    {
        auto* stackmap = safepoint->as<B3::StackmapValue>();
        return stackmap && stackmap->isSuspension();
    }

    static bool usesOnlyBits(B3::Value* user, unsigned child)
    {
        switch (user->opcode()) {
        case B3::Equal:
        case B3::NotEqual:
        case B3::Above:
        case B3::Below:
        case B3::AboveEqual:
        case B3::BelowEqual:
        case B3::LessThan:
        case B3::GreaterThan:
        case B3::LessEqual:
        case B3::GreaterEqual:
            return user->child(!child)->isConstant();
        case B3::Branch:
        case B3::Select:
        case B3::Return:
            return true;
        case B3::Check:
            return !child;
        case B3::Store:
            return child || user->child(0)->type() != B3::Int32;
        default:
            return false;
        }
    }

    void check(B3::Value* safepoint, const BitVector& words, const BitVector& pointers)
    {
        auto* patchpoint = safepoint->as<B3::PatchpointValue>();
        bool isSuspension = savesFrame(safepoint);
        m_wordsAcrossSafepoints.merge(words);
        m_wordsToCheck = words;
        m_wordsToCheck.exclude(m_unprotectedWords);
        for (size_t number : m_wordsToCheck) {
            B3::Value* word = m_words[number];
            bool isProtected = false;
            if (!isSuspension) {
                for (unsigned pointer : m_pointersOfWord[number])
                    isProtected |= pointers.get(pointer);
                for (size_t pointer : pointers) {
                    if (isProtected)
                        break;
                    isProtected = m_joinedPointers.get(pointer) && protects(m_pointers[pointer], word);
                }
            }
            for (unsigned i = 0; patchpoint && !isProtected && i < patchpoint->numChildren(); ++i) {
                if (!isLateUse(patchpoint->constrainedChild(i).rep().kind()))
                    continue;
                // The map of a saved frame lists a narrow reference as what it is.
                bool isListed = isSuspension && !m_displacedWords.contains(word) && wordOf(patchpoint->child(i)) == word;
                isProtected = isListed || protects(patchpoint->child(i), word);
            }
            if (isProtected || !m_reported.add({ word, safepoint }).isNewEntry)
                continue;
            if (m_insertsGuards) {
                m_unprotectedWords.set(number);
                continue;
            }
            auto kind = m_displacedWords.contains(word) ? NarrowReferenceViolationKind::IsDisplaced : NarrowReferenceViolationKind::HasNoPointer;
            String detail = toString(": ", deepDump(m_proc, word), " is used by ", deepDump(m_proc, factsOf(word).lastUse), " behind ", pointerDump(safepoint));
            m_findings.violations.append(NarrowReferenceViolation { kind, word->origin(), safepoint->origin(), WTF::move(detail) });
        }
    }

    void collectEvents(B3::BasicBlock* block)
    {
        BlockState& state = m_blockStates[block];
        state.eventsBegin = m_events.size();
        auto useWord = [&](B3::Value* word, B3::Value* user) {
            if (isWord(word))
                m_events.append(Event { EventKind::WordUse, factsOf(word).wordNumber, Facts::none, user });
        };
        auto usePointer = [&](B3::Value* pointer) {
            if (!isPointer(pointer))
                return false;
            m_events.append(Event { EventKind::PointerUse, factsOf(pointer).pointerNumber });
            return true;
        };
        for (unsigned index = block->size(); index--;) {
            B3::Value* value = block->at(index);
            if (auto* upsilon = value->as<B3::UpsilonValue>()) {
                const Facts& phi = factsOf(upsilon->phi());
                if (phi.wordNumber != Facts::none)
                    m_events.append(Event { EventKind::WordUpsilon, phi.wordNumber, factsOf(wordOf(upsilon->child(0))).wordNumber, value });
                else if (phi.pointerNumber != Facts::none)
                    m_events.append(Event { EventKind::PointerUpsilon, phi.pointerNumber, factsOf(pointerSource(upsilon->child(0))).pointerNumber, value });
                continue;
            }
            if (value->opcode() == B3::Phi)
                continue;
            const Facts& facts = factsOf(value);
            if (facts.wordNumber != Facts::none)
                m_events.append(Event { EventKind::WordDef, facts.wordNumber });
            if (facts.pointerNumber != Facts::none)
                m_events.append(Event { EventKind::PointerDef, facts.pointerNumber });
            if (isSafepoint(value))
                m_events.append(Event { EventKind::Safepoint, Facts::none, Facts::none, value });
            if (facts.wordOfSum) {
                if (facts.pointerNumber != Facts::none)
                    useWord(facts.wordOfSum, value);
                continue;
            }
            if (wordOf(value) != value)
                continue;
            for (unsigned i = 0; i < value->numChildren(); ++i) {
                B3::Value* child = withoutIdentity(value->child(i));
                if (B3::Value* word = factsOf(child).wordOfSum; word && isAddressOf(value, i)) {
                    useWord(word, value);
                    continue;
                }
                if (usePointer(pointerSource(child)))
                    continue;
                if (!usesOnlyBits(value, i))
                    useWord(wordOf(child), value);
            }
        }
        state.eventsEnd = m_events.size();
    }

    bool walk(B3::BasicBlock* block, Reports reports)
    {
        BitVector words;
        BitVector pointers;
        bool isFirst = true;
        for (B3::BasicBlock* successor : block->successorBlocks()) {
            bool isLastRead = isLastReader(block, successor, reports);
            if (isLastRead && isFirst) {
                words = std::exchange(m_wordsAtHead[successor], { });
                pointers = std::exchange(m_pointersAtHead[successor], { });
            } else {
                words.merge(m_wordsAtHead[successor]);
                pointers.merge(m_pointersAtHead[successor]);
                if (isLastRead) {
                    m_wordsAtHead[successor] = { };
                    m_pointersAtHead[successor] = { };
                }
            }
            isFirst = false;
        }
        auto useWord = [&](unsigned number, B3::Value* user) {
            words.set(number);
            m_facts[m_words[number]->index()].lastUse = user;
        };
        BlockState& state = m_blockStates[block];
        for (const Event& event : m_events.span().subspan(state.eventsBegin, state.eventsEnd - state.eventsBegin)) {
            switch (event.kind) {
            case EventKind::WordDef:
                words.clear(event.number);
                break;
            case EventKind::PointerDef:
                pointers.clear(event.number);
                break;
            case EventKind::WordUse:
                useWord(event.number, event.value);
                break;
            case EventKind::PointerUse:
                pointers.set(event.number);
                break;
            case EventKind::WordUpsilon:
                if (event.incoming != Facts::none && words.get(event.number)) {
                    B3::Value* phiLastUse = factsOf(m_words[event.number]).lastUse;
                    useWord(event.incoming, phiLastUse ? phiLastUse : event.value);
                }
                words.clear(event.number);
                break;
            case EventKind::PointerUpsilon:
                if (event.incoming != Facts::none && pointers.get(event.number))
                    pointers.set(event.incoming);
                pointers.clear(event.number);
                break;
            case EventKind::Safepoint:
                if (reports == Reports::Yes)
                    check(event.value, words, pointers);
                break;
            }
        }

        bool changed = reports == Reports::No && state.isReadInNextRound && (differs(words, m_wordsAtHead[block]) || differs(pointers, m_pointersAtHead[block]));
        m_wordsAtHead[block] = WTF::move(words);
        m_pointersAtHead[block] = WTF::move(pointers);
        return changed;
    }

    B3::Procedure& m_proc;
    const HeapsOfCollection& m_heaps;
    const NarrowReferenceOrigins& m_origins;
    NarrowReferenceFindings& m_findings;
    Vector<Facts> m_facts;
    Vector<B3::Value*> m_words;
    Vector<B3::Value*> m_pointers;
    UncheckedKeyHashSet<B3::Value*> m_displacedWords;
    UncheckedKeyHashSet<B3::Value*> m_displacedSums;
    UncheckedKeyHashMap<B3::Value*, Vector<B3::UpsilonValue*, 2>> m_upsilons;
    UncheckedKeyHashMap<PointerAndWord, bool> m_protects;
    Vector<PointerAndWord, 8> m_assumed;
    unsigned m_depth { 0 };
    IndexMap<B3::BasicBlock*, BitVector> m_wordsAtHead;
    IndexMap<B3::BasicBlock*, BitVector> m_pointersAtHead;
    IndexMap<B3::BasicBlock*, BlockState> m_blockStates;
    Vector<Event> m_events;
    unsigned m_numberOfCycles { 0 };
    bool m_hasSafepoint { false };
    BitVector m_wordsAcrossSafepoints;
    Vector<Vector<unsigned, 2>> m_pointersOfWord;
    BitVector m_joinedPointers;
    UncheckedKeyHashSet<PointerAndWord> m_reported;
    bool m_insertsGuards { false };
    bool m_removedDeadCode { false };
    BitVector m_unprotectedWords;
    BitVector m_wordsToCheck;
    UncheckedKeyHashMap<B3::Value*, B3::Value*> m_pointerBeside;
    Vector<std::pair<B3::Value*, B3::Value*>> m_replacements;
};

} // anonymous namespace

void findNarrowReferenceViolations(B3::Procedure& proc, const HeapsOfCollection& heaps, const NarrowReferenceOrigins& origins, NarrowReferenceFindings& findings)
{
    NarrowReferenceValidation(proc, heaps, origins, findings).run();
}

static uint32_t insertGuards(B3::Procedure& proc, const HeapsOfCollection& heaps, const NarrowReferenceOrigins& origins, bool& changed)
{
    NarrowReferenceFindings findings;
    NarrowReferenceValidation validation(proc, heaps, origins, findings);
    uint32_t inserted = validation.insertGuards();
    changed = inserted || validation.removedDeadCode();
    return inserted;
}

uint32_t insertNarrowReferenceGuards(B3::Procedure& proc, const HeapsOfCollection& heaps, const NarrowReferenceOrigins& origins)
{
    bool changed = false;
    return insertGuards(proc, heaps, origins, changed);
}

static HeapsOfCollection heapsOfCollectionIn(B3::Procedure& proc)
{
    const AOTHeaps& heaps = AOTHeaps::of(proc);
    return {
        .collectorState = heaps.rangeOf(heaps.collectorState),
        .weakTableStorage = heaps.rangeOf(heaps.weakTableStorage),
        .frameSlots = heaps.rangeOf(heaps.frameSlots),
        .permanentCells = { heaps.rangeOf(heaps.AOTInstance_stringCells), heaps.rangeOf(heaps.AOTInstance_closureCells) },
    };
}

B3::ClientPhase guardNarrowReferences(uint32_t& numberOfGuards, NarrowReferenceOrigins&& origins)
{
    return {
        B3::ClientPhase::Point::AfterSinking,
        "guardNarrowReferences"_s,
        createSharedTask<bool(B3::Procedure&)>([&numberOfGuards, origins = WTF::move(origins)](B3::Procedure& proc) {
            bool changed = false;
            numberOfGuards += insertGuards(proc, heapsOfCollectionIn(proc), origins, changed);
            return changed;
        })
    };
}

B3::ClientPhase validateNarrowReferences(NarrowReferenceFindings& findings, NarrowReferenceOrigins&& origins)
{
    return {
        B3::ClientPhase::Point::BeforeLowerToAir,
        "validateNarrowReferences"_s,
        createSharedTask<bool(B3::Procedure&)>([&findings, origins = WTF::move(origins)](B3::Procedure& proc) {
            findNarrowReferenceViolations(proc, heapsOfCollectionIn(proc), origins, findings);
            return false;
        })
    };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
