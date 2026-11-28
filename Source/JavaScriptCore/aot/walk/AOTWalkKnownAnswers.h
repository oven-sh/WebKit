/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTEffects.h"
#include "AOTSharedArray.h"
#include "AOTThreadBlockCache.h"
#include <bit>
#include <span>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class WalkState;

class SetOfAnswersForGood {
public:
    bool isEmpty() const { return !m_size; }
    uint32_t size() const { return m_size; }
    bool contains(uint32_t number) const { return wordAt(number / bitsOfWord) >> number % bitsOfWord & 1; }
    ALWAYS_INLINE bool add(uint32_t number)
    {
        uint32_t word = number / bitsOfWord;
        if (!m_size || word < m_firstWord || word - m_firstWord >= m_words.size())
            return addWhereWordIsMissing(number);
        uint64_t bit = 1ull << number % bitsOfWord;
        if (m_words[word - m_firstWord] & bit)
            return false;
        m_words.mutableAt(word - m_firstWord) |= bit;
        ++m_size;
        return true;
    }
    ALWAYS_INLINE void addInPlaceOfNeighbor(uint32_t number)
    {
        uint32_t word = number / bitsOfWord;
        if (!m_size || word < m_firstWord || word - m_firstWord >= m_words.size()) {
            addWhereWordIsMissing(number);
            return;
        }
        uint64_t held = m_words[word - m_firstWord];
        uint64_t bit = 1ull << number % bitsOfWord;
        if (held & bit)
            return;
        uint64_t bitOfNeighbor = 1ull << (number ^ 1) % bitsOfWord;
        m_words.mutableAt(word - m_firstWord) = (held | bit) & ~bitOfNeighbor;
        if (!(held & bitOfNeighbor))
            ++m_size;
    }
    bool remove(uint32_t number)
    {
        if (!contains(number))
            return false;
        m_words.mutableAt(number / bitsOfWord - m_firstWord) &= ~(1ull << number % bitsOfWord);
        --m_size;
        return true;
    }
    ALWAYS_INLINE void keepMembersOf(const SetOfAnswersForGood& other)
    {
        if (!m_size)
            return;
        if (!other.m_size)
            return clear();
        if (m_words.size() > wordsGoneThroughAsTheyLie)
            return keepMembersOfSetWithMembers(other);
        for (uint32_t place = 0; place < m_words.size(); ++place) {
            uint64_t held = m_words[place];
            uint64_t kept = held & other.wordAt(m_firstWord + place);
            if (kept == held)
                continue;
            m_words.mutableAt(place) = kept;
            m_size -= std::popcount(held ^ kept);
        }
    }
    void clear()
    {
        if (!m_words.isEmpty())
            m_words.clear();
        m_size = 0;
    }
    void swap(SetOfAnswersForGood& other)
    {
        m_words.swap(other.m_words);
        std::swap(m_firstWord, other.m_firstWord);
        std::swap(m_size, other.m_size);
    }

private:
    static constexpr uint32_t bitsOfWord = 64;
    static constexpr uint32_t wordsGoneThroughAsTheyLie = 2;

    bool addWhereWordIsMissing(uint32_t number);
    uint64_t wordAt(uint32_t word) const { return word >= m_firstWord && word - m_firstWord < m_words.size() ? m_words[word - m_firstWord] : 0; }
    void makeRoomFrom(uint32_t word);
    void keepMembersOfSetWithMembers(const SetOfAnswersForGood&);

    SharedArray<uint64_t, 2> m_words;
    uint32_t m_firstWord { 0 };
    uint32_t m_size { 0 };
};

class ListOfAnswersUntilEnded {
public:
    bool isEmpty() const { return !m_numberHeld; }
    uint32_t size() const { return m_numberHeld; }
    uint32_t numberOfPlaces() const { return m_places.size(); }
    bool contains(uint32_t number) const
    {
        uint32_t place = firstPlaceNotBelow(number);
        return place < m_places.size() && m_places[place] == number;
    }
    bool add(uint32_t number);
    bool appendIfBehindAll(uint32_t number)
    {
        uint32_t size = m_places.size();
        if (size && (m_places[size - 1] & ~hasEnded) >= number)
            return false;
        m_places.append(number);
        ++m_numberHeld;
        return true;
    }
    bool remove(uint32_t number);
    void keepMembersOf(const ListOfAnswersUntilEnded& other)
    {
        if (!m_numberHeld)
            return;
        if (!other.m_numberHeld)
            return clear();
        keepMembersOfListWithMembers(other);
    }
    void keepMembersOfEither(const ListOfAnswersUntilEnded& other, const ListOfAnswersUntilEnded& beside, Vector<uint32_t, 8>& gone);
    void clear()
    {
        if (!m_places.isEmpty())
            m_places.clear();
        m_numberHeld = 0;
    }
    void swap(ListOfAnswersUntilEnded& other)
    {
        m_places.swap(other.m_places);
        std::swap(m_numberHeld, other.m_numberHeld);
    }

    void forEach(NOESCAPE const Invocable<void(uint32_t number)> auto& functor) const
    {
        for (uint32_t place = 0; place < m_places.size(); ++place) {
            if (uint32_t held = m_places[place]; !(held & hasEnded))
                functor(held);
        }
    }
    void forEachNotSharedWith(const ListOfAnswersUntilEnded& other, NOESCAPE const Invocable<void(uint32_t number)> auto& functor) const
    {
        m_places.forEachStretchNotSharedWith(other.m_places, [&](uint32_t begin, uint32_t end) {
            for (uint32_t place = begin; place < end; ++place) {
                if (uint32_t held = m_places[place]; !(held & hasEnded))
                    functor(held);
            }
        });
    }
    void removeAllMatching(NOESCAPE const Invocable<bool(uint32_t number)> auto& ends)
    {
        for (uint32_t place = 0; place < m_places.size(); ++place) {
            uint32_t held = m_places[place];
            if (held & hasEnded || !ends(held))
                continue;
            m_places.mutableAt(place) = held | hasEnded;
            --m_numberHeld;
        }
        dropEndedPlacesIfMany();
    }

private:
    static constexpr uint32_t hasEnded = 1u << 31;
    static constexpr uint32_t endedPlacesAlwaysBorne = 16;

    uint32_t firstPlaceNotBelow(uint32_t number) const;
    void dropEndedPlacesIfMany();
    void keepMembersOfListWithMembers(const ListOfAnswersUntilEnded&);

    SharedArray<uint32_t, 4> m_places;
    uint32_t m_numberHeld { 0 };
};

struct KnownAnswer {
    enum class Family : uint8_t { Note, Operation, Conversion };

    uint64_t noted { 0 };
    Reference holder;
    uint32_t key { 0 };
    uint32_t older { 0 };
    uint32_t olderOfHolder { 0 };
    uint32_t restsOn { 0 };
    Family family { Family::Note };
    uint8_t note { 0 };
    bool endsAtSuspension { false };
};

class AnswersOfWalk {
    WTF_MAKE_NONCOPYABLE(AnswersOfWalk);
public:
    struct Key {
        uint32_t hash { 0 };
        uint32_t firstWord { 0 };
        uint32_t numberOfWords { 0 };
        uint32_t newest { 0 };
        uint32_t numberOfAnswers { 0 };
        uint32_t amongKeysWithManyAnswers { 0 };
        bool hasAnswersThatRestOnNothing { false };
    };
    static constexpr uint32_t mostAnswersGoneThroughUnderKey = 8;
    using WordsOfKey = Vector<uint32_t, 16>;

    AnswersOfWalk() = default;

    void clear();
    void releaseRoom();
    void takeRoomOf(AnswersOfWalk& former)
    {
        takeRoom(m_answers, former.m_answers);
        takeRoom(m_masks, former.m_masks);
        takeRoom(m_keys, former.m_keys);
        takeRoom(m_wordsOfKeys, former.m_wordsOfKeys);
        takeRoom(m_placesOfKeys, former.m_placesOfKeys);
        takeRoom(m_newestOfFirstAnswers, former.m_newestOfFirstAnswers);
        takeRoom(m_answersOfHolders, former.m_answersOfHolders);
        takeRoom(m_conversions, former.m_conversions);
    }

    uint32_t size() const { return m_answers.size(); }
    const KnownAnswer& at(uint32_t number) const LIFETIME_BOUND { return m_answers[number]; }
    const Key& keyAt(uint32_t key) const LIFETIME_BOUND { return m_keys[key]; }
    std::span<const uint32_t> wordsOf(const Key& key) const LIFETIME_BOUND { return m_wordsOfKeys.span().subspan(key.firstWord, key.numberOfWords); }
    const LocationMasks& whatAnswerRestsOn(const KnownAnswer& answer) const LIFETIME_BOUND { return m_masks[answer.restsOn - 1]; }

    uint32_t findKey(std::span<const uint32_t> words) const;
    struct PlaceOfKey {
        uint32_t hash { 0 };
        uint32_t found { 0 };
    };
    PlaceOfKey placeOfKey(std::span<const uint32_t> words) const
    {
        uint32_t hash = hashOf(words);
        return { hash, findKey(words, hash) };
    }
    uint32_t numberOf(std::span<const uint32_t> wordsOfKey, PlaceOfKey, KnownAnswer, const LocationMasks& restsOn, bool mayBeNumbered);
    uint32_t newestOfFirstAnswersUnder(const Key& key) const { return m_newestOfFirstAnswers[key.amongKeysWithManyAnswers - 1]; }
    void setOlder(uint32_t number, uint32_t older) { m_answers[number].older = older; }

    struct ConversionMade {
        Reference converted;
        uint32_t older { 0 };
        Representation wanted { Representation::Unchosen };
        uint8_t numberInFront { 0 };
    };
    uint32_t outcomesOf(Reference condition) const { return condition.id().index() < m_answersOfHolders.size() ? m_answersOfHolders[condition.id().index()].outcomes : 0; }
    uint32_t numberOutcomesOf(Reference condition)
    {
        AnswersOfHolder& row = rowOf(condition.id().index());
        if (!row.outcomes) {
            m_numberOfOutcomes += 2;
            row.outcomes = m_numberOfOutcomes - 1;
        }
        return row.outcomes;
    }
    uint32_t newestConversionOf(Reference value) const { return value.id().index() < m_answersOfHolders.size() ? m_answersOfHolders[value.id().index()].newestConversion : 0; }
    const ConversionMade& conversionAt(uint32_t number) const LIFETIME_BOUND { return m_conversions[number]; }
    uint32_t numberConversion(Reference value, Representation wanted, Reference converted);
    bool isBehindFirstConversions(const ConversionMade& made) const { return made.numberInFront == mostAnswersGoneThroughUnderKey; }
    uint32_t amongKeysWithManyAnswers(Reference value) const { return m_recordsWithManyConversions.get(value.id().index()); }
    uint32_t newestOfFirstConversions(uint32_t amongKeysWithManyAnswers) const { return m_newestOfFirstAnswers[amongKeysWithManyAnswers - 1]; }
    void setOlderConversion(uint32_t number, uint32_t older) { m_conversions[number].older = older; }
    bool conversionsEndAtSuspension() const { return m_conversionsEndAtSuspension; }
    void setConversionsEndAtSuspension(bool end) { m_conversionsEndAtSuspension = end; }

    uint32_t newestOfHolder(Reference holder) const { return holder.isRecord() && holder.id().index() < m_answersOfHolders.size() ? m_answersOfHolders[holder.id().index()].newest : 0; }
    uint32_t numberOfAnswersOfHolder(Reference holder) const { return holder.isRecord() && holder.id().index() < m_answersOfHolders.size() ? m_answersOfHolders[holder.id().index()].number : 0; }

    WordsOfKey& keyAskedLast() LIFETIME_BOUND { return m_keyAskedLast; }
    PlaceOfKey& placeOfKeyAskedLast() LIFETIME_BOUND { return m_placeOfKeyAskedLast; }
    const void* whoAskedLast() const { return m_whoAskedLast; }
    void setWhoAskedLast(const void* asker) { m_whoAskedLast = asker; }

private:
    struct AnswersOfHolder {
        uint32_t newest { 0 };
        uint32_t number { 0 };
        uint32_t outcomes { 0 };
        uint32_t newestConversion { 0 };
    };

    static uint32_t hashOf(std::span<const uint32_t> words);
    uint32_t findKey(std::span<const uint32_t> words, uint32_t hash) const;
    uint32_t addKey(std::span<const uint32_t> words, uint32_t hash);
    void growPlacesOfKeys();
    void noteThatKeyHasManyAnswers(uint32_t key);
    uint32_t& placeOfNoteUnderKeyWithManyAnswers(uint32_t key, const KnownAnswer&) LIFETIME_BOUND;
    void makeRoomForNoteUnderKeyWithManyAnswers();
    static constexpr size_t rowsMadeAhead = 32;
    AnswersOfHolder& rowOf(uint32_t record)
    {
        if (m_answersOfHolders.size() <= record)
            m_answersOfHolders.grow(record + 1 + rowsMadeAhead);
        return m_answersOfHolders[record];
    }

    Vector<KnownAnswer> m_answers;
    Vector<LocationMasks> m_masks;
    Vector<Key> m_keys;
    Vector<uint32_t> m_wordsOfKeys;
    Vector<uint32_t> m_placesOfKeys;
    Vector<uint32_t> m_newestOfFirstAnswers;
    Vector<uint32_t> m_placesOfNotes;
    uint32_t m_numberOfPlacesOfNotesTaken { 0 };
    Vector<AnswersOfHolder> m_answersOfHolders;
    Vector<ConversionMade> m_conversions;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_recordsWithManyConversions;
    uint32_t m_numberOfOutcomes { 0 };
    bool m_conversionsEndAtSuspension { false };
    WordsOfKey m_keyAskedLast;
    PlaceOfKey m_placeOfKeyAskedLast;
    const void* m_whoAskedLast { nullptr };
};

class KnownAnswers {
public:
    using AsksWhetherAnswerEnds = ScopedLambda<bool(const KnownAnswer&, bool isWrittenOutsideObjects)>;

    KnownAnswers() = default;
    KnownAnswers(const KnownAnswers& other) { *this = other; }
    ALWAYS_INLINE ~KnownAnswers() { clear(); }

    KnownAnswers& operator=(const KnownAnswers& other)
    {
        if (this == &other)
            return *this;
        if (other.m_table)
            m_table = other.m_table;
        if (other.isEmpty())
            m_held = nullptr;
        else if (m_held)
            *m_held = *other.m_held;
        else
            m_held = makeUniqueWithoutFastMallocCheck<Held>(*other.m_held);
        return *this;
    }
    void swap(KnownAnswers& other)
    {
        if (!m_table)
            m_table = other.m_table;
        if (!other.m_table)
            other.m_table = m_table;
        m_held.swap(other.m_held);
    }

    void setTable(AnswersOfWalk& table) { m_table = &table; }
    AnswersOfWalk* table() const { return m_table; }

    ALWAYS_INLINE bool isEmpty() const { return !m_held || (m_held->forGood.isEmpty() && m_held->outcomes.isEmpty() && m_held->conversions.isEmpty() && m_held->untilSuspension.isEmpty() && m_held->untilEnded.isEmpty() && m_held->onFreshHolders.isEmpty()); }
    uint32_t size() const { return m_held ? m_held->forGood.size() + m_held->outcomes.size() + m_held->conversions.size() + m_held->untilSuspension.size() + m_held->untilEnded.size() + m_held->onFreshHolders.size() : 0; }
    ALWAYS_INLINE void clear()
    {
        if (m_held)
            dropWhatIsHeld();
    }
    void meetWith(const KnownAnswers& other)
    {
        if (isEmpty())
            return;
        if (other.isEmpty())
            return clear();
        m_held->forGood.keepMembersOf(other.m_held->forGood);
        m_held->outcomes.keepMembersOf(other.m_held->outcomes);
        m_held->conversions.keepMembersOf(other.m_held->conversions);
        m_held->untilSuspension.keepMembersOf(other.m_held->untilSuspension);
        if (m_held->onFreshHolders.isEmpty() && other.m_held->onFreshHolders.isEmpty())
            m_held->untilEnded.keepMembersOf(other.m_held->untilEnded);
        else
            meetWhatRestsOnHeapsWith(*other.m_held);
        if (m_held->untilEnded.isEmpty())
            forgetWhatRestsOnHeaps();
    }

    const KnownAnswer* find(std::span<const uint32_t> wordsOfKey) const LIFETIME_BOUND;
    const KnownAnswer* find(AnswersOfWalk::PlaceOfKey) const LIFETIME_BOUND;
    void note(std::span<const uint32_t> wordsOfKey, const KnownAnswer& answer, const LocationMasks& restsOn, bool mayBeNumbered, bool holderIsFresh)
    {
        if (m_table)
            note(wordsOfKey, m_table->placeOfKey(wordsOfKey), answer, restsOn, mayBeNumbered, holderIsFresh);
    }
    void note(std::span<const uint32_t> wordsOfKey, AnswersOfWalk::PlaceOfKey, const KnownAnswer&, const LocationMasks& restsOn, bool mayBeNumbered, bool holderIsFresh);
    void end(std::span<const uint32_t> wordsOfKey);

    void forEachNoteAbout(Reference holder, uint8_t kind, NOESCAPE const Invocable<void(uint64_t noted)> auto& functor) const
    {
        if (!m_held)
            return;
        m_held->untilEnded.forEach([&](uint32_t number) {
            const KnownAnswer& answer = m_table->at(number);
            if (answer.family == KnownAnswer::Family::Note && answer.note == kind && answer.holder == holder)
                functor(answer.noted);
        });
    }

    void noteOutcome(Reference condition, bool isTrue)
    {
        if (!m_table)
            return;
        uint32_t ifTrue = m_table->numberOutcomesOf(condition);
        ASSERT(ifTrue & 1);
        if (!m_held)
            m_held = makeUniqueWithoutFastMallocCheck<Held>();
        m_held->outcomes.addInPlaceOfNeighbor(ifTrue - !isTrue);
    }
    std::optional<bool> outcomeOf(Reference condition) const;
    void noteConversion(Reference value, Representation wanted, Reference converted, bool endsAtSuspension);
    Reference findConversion(Reference value, Representation wanted) const;

    ALWAYS_INLINE void endWhatIsWritten(const Effects& effects, const WalkState& state)
    {
        if (!m_held)
            return;
        if (effects.maySuspend)
            endWhatEndsAtSuspension();
        if (!m_held->untilEnded.isEmpty())
            endWhatIsWrittenOutsideFreshObjects(effects, state);
    }
    void endWhatIsWritten(const Effects&, NOESCAPE const AsksWhetherAnswerEnds& ends);
    void endWhatIsWrittenIn(Reference holder, const Effects&);
    void forEachHolderOfAnswerAboutFreshHolderLackingIn(const KnownAnswers& arriving, NOESCAPE const Invocable<void(Reference holder)> auto& functor) const
    {
        if (!m_held || m_held->onFreshHolders.isEmpty())
            return;
        if (!arriving.m_held) {
            m_held->onFreshHolders.forEach([&](uint32_t number) {
                functor(m_table->at(number).holder);
            });
            return;
        }
        m_held->onFreshHolders.forEachNotSharedWith(arriving.m_held->onFreshHolders, [&](uint32_t number) {
            if (!arriving.m_held->onFreshHolders.contains(number))
                functor(m_table->at(number).holder);
        });
    }
    void holderHasLeft(Reference holder);
    void holderIsFreshAgain(Reference holder);
    void endWhatIsNotFresh(const WalkState& state)
    {
        if (!m_held)
            return;
        endWhatEndsAtSuspension();
        if (!m_held->untilEnded.isEmpty())
            endWhatIsWrittenOutsideFreshObjects(Effects(), state);
    }

private:
    struct Held {
        Held() { }
        static void* operator new(size_t) { return ThreadBlockCache<Held, KindOfBlocks::HeldByKnownAnswers>::allocate(); }
        static void operator delete(void* held) { ThreadBlockCache<Held, KindOfBlocks::HeldByKnownAnswers>::free(held); }

        SetOfAnswersForGood forGood;
        SetOfAnswersForGood untilSuspension;
        SetOfAnswersForGood outcomes;
        SetOfAnswersForGood conversions;
        ListOfAnswersUntilEnded untilEnded;
        ListOfAnswersUntilEnded onFreshHolders;
        LocationMasks restsOn;
        SharedArray<uint32_t, 2> newestUnderKeysWithManyAnswers;
    };

    void dropWhatIsHeld();
    bool holds(uint32_t number) const
    {
        const KnownAnswer& answer = m_table->at(number);
        if (answer.restsOn)
            return m_held->untilEnded.contains(number) || m_held->onFreshHolders.contains(number);
        return (answer.endsAtSuspension ? m_held->untilSuspension : m_held->forGood).contains(number);
    }
    void drop(uint32_t number)
    {
        const KnownAnswer& answer = m_table->at(number);
        if (answer.restsOn)
            m_held->untilEnded.remove(number) || m_held->onFreshHolders.remove(number);
        else
            (answer.endsAtSuspension ? m_held->untilSuspension : m_held->forGood).remove(number);
    }
    void endWhatEndsAtSuspension()
    {
        m_held->untilSuspension.clear();
        if (m_table->conversionsEndAtSuspension())
            m_held->conversions.clear();
    }
    uint32_t findHeldUnder(uint32_t key) const;
    uint32_t newestConversionNotedOf(Reference value) const
    {
        uint32_t among = m_table->amongKeysWithManyAnswers(value);
        uint32_t newest = among <= m_held->newestUnderKeysWithManyAnswers.size() ? m_held->newestUnderKeysWithManyAnswers[among - 1] : 0;
        return newest ? newest : m_table->newestOfFirstConversions(among);
    }
    void endWhatIsWrittenOutsideFreshObjects(const Effects&, const WalkState&);
    void meetWhatRestsOnHeapsWith(const Held& other);
    void forgetWhatRestsOnHeaps()
    {
        m_held->untilEnded.clear();
        m_held->restsOn = { };
    }

    AnswersOfWalk* m_table { nullptr };
    std::unique_ptr<Held> m_held;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
