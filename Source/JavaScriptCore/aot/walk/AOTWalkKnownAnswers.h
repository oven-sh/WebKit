/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTEffects.h"
#include "AOTSharedArray.h"
#include <bit>
#include <span>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class WalkState;

// The numbers of answers that no write ends, so nothing ever goes through them. A copy shares everything with the original, and a meet looks at the parts that the two do not share: SharedSetOfNumbers of AOTWalkState.h is the model.
// The numbers of a walk only grow, so a set that was emptied begins anew at the first number that it is given: what it costs does not follow how many numbers the walk has handed out before.
class SetOfAnswersForGood {
public:
    bool isEmpty() const { return !m_size; }
    uint32_t size() const { return m_size; }
    bool contains(uint32_t number) const { return wordAt(number / bitsOfWord) >> number % bitsOfWord & 1; }
    // False: it was a member.
    bool add(uint32_t number)
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
    // False: it was none.
    bool remove(uint32_t number);
    // Every meet asks each set of a state. Most are empty, and most of the others have a word or two, which are gone through as they lie.
    void keepMembersOf(const SetOfAnswersForGood& other)
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
    // m_words[0] holds the numbers from 64 times this on.
    uint32_t m_firstWord { 0 };
    uint32_t m_size { 0 };
};

// The numbers of the answers that rest on a heap, ascending. Whatever ends some of them goes through all, so what that costs has to follow how many are held, however far apart their numbers lie.
// A number that has ended keeps its place, marked, so that the parts which a copy shares with the original go on meaning the same in both. The places are made anew when the marked ones outnumber the rest.
class ListOfAnswersUntilEnded {
public:
    bool isEmpty() const { return !m_numberHeld; }
    uint32_t size() const { return m_numberHeld; }
    // With those that have ended.
    uint32_t numberOfPlaces() const { return m_places.size(); }
    bool contains(uint32_t number) const
    {
        uint32_t place = firstPlaceNotBelow(number);
        return place < m_places.size() && m_places[place] == number;
    }
    // False: it was a member.
    bool add(uint32_t number);
    // False, and nothing is done: a number as large stands in the list. Whoever adds so moves no place, however long the list is.
    bool appendIfBehindAll(uint32_t number)
    {
        uint32_t size = m_places.size();
        if (size && (m_places[size - 1] & ~hasEnded) >= number)
            return false;
        m_places.append(number);
        ++m_numberHeld;
        return true;
    }
    // False: it was none.
    bool remove(uint32_t number);
    void keepMembersOf(const ListOfAnswersUntilEnded& other)
    {
        if (!m_numberHeld)
            return;
        if (!other.m_numberHeld)
            return clear();
        keepMembersOfListWithMembers(other);
    }
    // Keeps what `other` holds, and what `beside` holds. What goes is appended to `gone`.
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

    // Ascending. The functor leaves the list as it is.
    void forEach(NOESCAPE const Invocable<void(uint32_t number)> auto& functor) const
    {
        for (uint32_t place = 0; place < m_places.size(); ++place) {
            if (uint32_t held = m_places[place]; !(held & hasEnded))
                functor(held);
        }
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
    // So many are borne whatever the list holds.
    static constexpr uint32_t endedPlacesAlwaysBorne = 16;

    uint32_t firstPlaceNotBelow(uint32_t number) const;
    void dropEndedPlacesIfMany();
    void keepMembersOfListWithMembers(const ListOfAnswersUntilEnded&);

    SharedArray<uint32_t, 4> m_places;
    uint32_t m_numberHeld { 0 };
};

// What a state may hold under one number: under this key, this is so. The number stands for the answer whichever way has come by it.
// What is noted may name a record. Whoever notes it has that record in sight, so where every way to a point has noted it, or has got it from a way that has, the record dominates the point.
struct KnownAnswer {
    enum class Family : uint8_t { Note, Operation, Conversion };

    uint64_t noted { 0 };
    // A root. Whatever the answer rests on of a heap of perObjectHeaps lies in this object. Null: it may lie in any.
    Reference holder;
    // In the list of keys of AnswersOfWalk.
    uint32_t key { 0 };
    // Its number and one more. 0: there is none. The answer that was numbered in front of this one under the same key, and for the same holder.
    // Under a key with many answers, of an answer behind the first ones: the newest that the state held under the key where this one was noted first.
    uint32_t older { 0 };
    uint32_t olderOfHolder { 0 };
    // Its place among the masks of AnswersOfWalk and one more. 0: it rests on no heap.
    uint32_t restsOn { 0 };
    Family family { Family::Note };
    // KnownNote of AOTWalkKnown.h.
    uint8_t note { 0 };
    bool endsAtSuspension { false };
};

// The answers of one walk, numbered as they are noted. The states of the walk share this, as they share NumbersOfWalkStates. The walker empties it in front of each unit.
class AnswersOfWalk {
    WTF_MAKE_NONCOPYABLE(AnswersOfWalk);
public:
    struct Key {
        uint32_t hash { 0 };
        uint32_t firstWord { 0 };
        uint32_t numberOfWords { 0 };
        // Its number and one more.
        uint32_t newest { 0 };
        uint32_t numberOfAnswers { 0 };
        // Its place among the keys with many answers and one more. 0: it has few, and whoever asks goes through them.
        uint32_t amongKeysWithManyAnswers { 0 };
        bool hasAnswersThatRestOnNothing { false };
    };
    // One variable that is read in each of many cases, one sum that each of them computes: every way numbers an answer of its own, and none holds those of the others.
    // From here on a state says itself which answer it has noted last under the key: KnownAnswers::Held::newestUnderKeysWithManyAnswers.
    static constexpr uint32_t mostAnswersGoneThroughUnderKey = 8;
    using WordsOfKey = Vector<uint32_t, 16>;

    AnswersOfWalk() = default;

    void clear();

    uint32_t size() const { return m_answers.size(); }
    const KnownAnswer& at(uint32_t number) const LIFETIME_BOUND { return m_answers[number]; }
    const Key& keyAt(uint32_t key) const LIFETIME_BOUND { return m_keys[key]; }
    std::span<const uint32_t> wordsOf(const Key& key) const LIFETIME_BOUND { return m_wordsOfKeys.span().subspan(key.firstWord, key.numberOfWords); }
    // Of an answer that rests on something.
    const LocationMasks& whatAnswerRestsOn(const KnownAnswer& answer) const LIFETIME_BOUND { return m_masks[answer.restsOn - 1]; }

    // Its place in the list of keys and one more. 0: nothing was ever noted under it.
    uint32_t findKey(std::span<const uint32_t> words) const;
    struct PlaceOfKey {
        uint32_t hash { 0 };
        // As findKey() gives it.
        uint32_t found { 0 };
    };
    PlaceOfKey placeOfKey(std::span<const uint32_t> words) const
    {
        uint32_t hash = hashOf(words);
        return { hash, findKey(words, hash) };
    }
    // `answer` says all but key, older, olderOfHolder and restsOn. `mayBeNumbered`: false where what is noted names a record that was made just now, which no answer can hold yet.
    // The newest answer under the key is taken again where it says the same: so two ways that come by one answer hold one number, and a key that is noted anew behind each call has one answer.
    uint32_t numberOf(std::span<const uint32_t> wordsOfKey, PlaceOfKey, KnownAnswer, const LocationMasks& restsOn, bool mayBeNumbered);
    // Of a key with many answers: the newest of the first ones, its number and one more.
    uint32_t newestOfFirstAnswersUnder(const Key& key) const { return m_newestOfFirstAnswers[key.amongKeysWithManyAnswers - 1]; }
    void setOlder(uint32_t number, uint32_t older) { m_answers[number].older = older; }

    // WHAT A BRANCH HAS DECIDED AND WHICH CONVERSIONS WERE MADE are most of what a walk notes, and little of it is asked for again. Each of the two has numbers of its own, found from the record that it speaks of: no key, no answer.
    struct ConversionMade {
        Reference converted;
        // Its number and one more. 0: there is none. The conversion of the same record that was numbered in front of this one.
        // Of a record with many conversions, behind the first ones: the newest of them that the state held where this one was noted.
        uint32_t older { 0 };
        Representation wanted { Representation::Unchosen };
        // Of the same record, up to mostAnswersGoneThroughUnderKey: from there on the record has many, as a key has many answers.
        uint8_t numberInFront { 0 };
    };
    // A condition has two numbers: that it is false, and behind it that it is true. The second of them. 0: no branch of the walk has decided it.
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
    // Its number and one more.
    uint32_t newestConversionOf(Reference value) const { return value.id().index() < m_answersOfHolders.size() ? m_answersOfHolders[value.id().index()].newestConversion : 0; }
    const ConversionMade& conversionAt(uint32_t number) const LIFETIME_BOUND { return m_conversions[number]; }
    uint32_t numberConversion(Reference value, Representation wanted, Reference converted);
    bool isBehindFirstConversions(const ConversionMade& made) const { return made.numberInFront == mostAnswersGoneThroughUnderKey; }
    // Of a record whose newest conversion stands behind the first ones. Its place is one among the keys with many answers.
    uint32_t amongKeysWithManyAnswers(Reference value) const { return m_recordsWithManyConversions.get(value.id().index()); }
    uint32_t newestOfFirstConversions(uint32_t amongKeysWithManyAnswers) const { return m_newestOfFirstAnswers[amongKeysWithManyAnswers - 1]; }
    void setOlderConversion(uint32_t number, uint32_t older) { m_conversions[number].older = older; }
    // All those of a unit, or none: only the frame of a coroutine is ever saved.
    bool conversionsEndAtSuspension() const { return m_conversionsEndAtSuspension; }
    void setConversionsEndAtSuspension(bool end) { m_conversionsEndAtSuspension = end; }

    // Its number and one more, and how many there are.
    uint32_t newestOfHolder(Reference holder) const { return holder.isRecord() && holder.id().index() < m_answersOfHolders.size() ? m_answersOfHolders[holder.id().index()].newest : 0; }
    uint32_t numberOfAnswersOfHolder(Reference holder) const { return holder.isRecord() && holder.id().index() < m_answersOfHolders.size() ? m_answersOfHolders[holder.id().index()].number : 0; }

    // findEqualOperationBuilt() leaves the key of the draft that it was asked about for noteOperationBuilt(), which Walker::build() calls for the same draft where nothing was found.
    WordsOfKey& keyAskedLast() LIFETIME_BOUND { return m_keyAskedLast; }
    // Where that key stood. Only the note of an operation adds the key of one, and it forgets who asked last.
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
    // Where the note that rests on nothing and says the same stands, or where it would.
    uint32_t& placeOfNoteUnderKeyWithManyAnswers(uint32_t key, const KnownAnswer&) LIFETIME_BOUND;
    void makeRoomForNoteUnderKeyWithManyAnswers();
    AnswersOfHolder& rowOf(uint32_t record)
    {
        if (m_answersOfHolders.size() <= record)
            m_answersOfHolders.grow(record + 1);
        return m_answersOfHolders[record];
    }

    Vector<KnownAnswer> m_answers;
    Vector<LocationMasks> m_masks;
    Vector<Key> m_keys;
    Vector<uint32_t> m_wordsOfKeys;
    // Empty, or a power of two of places, at most half of them taken, each found from the hash of its key on: the place of the key in m_keys and one more.
    Vector<uint32_t> m_placesOfKeys;
    Vector<uint32_t> m_newestOfFirstAnswers;
    // As m_placesOfKeys, of the notes that rest on nothing under the keys with many answers: the number and one more.
    Vector<uint32_t> m_placesOfNotes;
    uint32_t m_numberOfPlacesOfNotesTaken { 0 };
    // By record of Code.
    Vector<AnswersOfHolder> m_answersOfHolders;
    Vector<ConversionMade> m_conversions;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_recordsWithManyConversions;
    uint32_t m_numberOfOutcomes { 0 };
    bool m_conversionsEndAtSuspension { false };
    WordsOfKey m_keyAskedLast;
    PlaceOfKey m_placeOfKeyAskedLast;
    const void* m_whoAskedLast { nullptr };
};

// The part of a WalkState that holds answers: copied, met and ended with it. A question and a note cost the same whatever the state holds, but for a key or a holder that was noted on many ways.
// A copy: nothing. A meet: as much as either state has noted or ended since the two were one. Whatever writes goes through the answers that rest on a heap, and only where it writes what one of them rests on.
// What rests on nothing and ends at a suspension (in a coroutine: every conversion, every number that was computed) is many, and no call looks at it: a suspension drops all of it at once.
class KnownAnswers {
public:
    // `isWrittenOutsideObjects`: what is written of what the answer rests on is no part of an object.
    using AsksWhetherAnswerEnds = ScopedLambda<bool(const KnownAnswer&, bool isWrittenOutsideObjects)>;

    KnownAnswers() = default;
    KnownAnswers(const KnownAnswers& other) { *this = other; }

    // A state that has the table keeps it when one that never had it is assigned to it.
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
    // Both have the table afterward if either had it.
    void swap(KnownAnswers& other)
    {
        if (!m_table)
            m_table = other.m_table;
        if (!other.m_table)
            other.m_table = m_table;
        m_held.swap(other.m_held);
    }

    // Without it nothing is noted.
    void setTable(AnswersOfWalk& table) { m_table = &table; }
    AnswersOfWalk* table() const { return m_table; }

    bool isEmpty() const { return !m_held || (m_held->forGood.isEmpty() && m_held->outcomes.isEmpty() && m_held->conversions.isEmpty() && m_held->untilSuspension.isEmpty() && m_held->untilEnded.isEmpty() && m_held->onFreshHolders.isEmpty()); }
    uint32_t size() const { return m_held ? m_held->forGood.size() + m_held->outcomes.size() + m_held->conversions.size() + m_held->untilSuspension.size() + m_held->untilEnded.size() + m_held->onFreshHolders.size() : 0; }
    void clear() { m_held = nullptr; }
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

    // Null: the state holds nothing under the key. It holds one answer at most under the key of a note, and of an operation the newest is found.
    const KnownAnswer* find(std::span<const uint32_t> wordsOfKey) const LIFETIME_BOUND;
    const KnownAnswer* find(AnswersOfWalk::PlaceOfKey) const LIFETIME_BOUND;
    // `answer` and `mayBeNumbered`: as for AnswersOfWalk::numberOf(). What the state held under the key of a note gives way.
    // `holderIsFresh`: WalkState::isFresh() of answer.holder, where the walk stands.
    void note(std::span<const uint32_t> wordsOfKey, const KnownAnswer& answer, const LocationMasks& restsOn, bool mayBeNumbered, bool holderIsFresh)
    {
        if (m_table)
            note(wordsOfKey, m_table->placeOfKey(wordsOfKey), answer, restsOn, mayBeNumbered, holderIsFresh);
    }
    // Of a state that has the table.
    void note(std::span<const uint32_t> wordsOfKey, AnswersOfWalk::PlaceOfKey, const KnownAnswer&, const LocationMasks& restsOn, bool mayBeNumbered, bool holderIsFresh);
    void end(std::span<const uint32_t> wordsOfKey);

    // `condition` and `value` are values. An outcome is known for good, and gives way to the other one. Of the conversions of a value into a form the newest that the state holds is found.
    void noteOutcome(Reference condition, bool isTrue)
    {
        if (!m_table)
            return;
        uint32_t ifTrue = m_table->numberOutcomesOf(condition);
        if (!m_held)
            m_held = makeUniqueWithoutFastMallocCheck<Held>();
        if (m_held->outcomes.add(ifTrue - !isTrue))
            m_held->outcomes.remove(ifTrue - isTrue);
    }
    std::optional<bool> outcomeOf(Reference condition) const;
    void noteConversion(Reference value, Representation wanted, Reference converted, bool endsAtSuspension);
    Reference findConversion(Reference value, Representation wanted) const;

    // WalkState::invalidate(): what rests on a part of a fresh object forgets nothing.
    void endWhatIsWritten(const Effects& effects, const WalkState& state)
    {
        if (!m_held)
            return;
        if (effects.maySuspend)
            endWhatEndsAtSuspension();
        if (!m_held->untilEnded.isEmpty())
            endWhatIsWrittenOutsideFreshObjects(effects, state);
    }
    // `ends` is asked of each answer that rests on something that the effects write. A suspension ends what it ends without a question.
    void endWhatIsWritten(const Effects&, NOESCAPE const AsksWhetherAnswerEnds& ends);
    // Of the answers that rest on a part of that object alone, fresh or not.
    void endWhatIsWrittenIn(Reference holder, const Effects&);
    // WalkState::markEscaped(): from here on what rests on the object ends as what rests on any other does.
    void holderHasLeft(Reference holder);
    // WalkState::noteFresh(), of an object that was let go for the length of an operation: `a.push(x)`, a call of a built-in that keeps nothing. A number that comes back finds its place in the list, so nothing is moved.
    void holderIsFreshAgain(Reference holder);
    // WalkState::invalidateHeap(): a call of code that nobody has seen, a suspension. A default Effects is every effect.
    void endWhatIsNotFresh(const WalkState& state)
    {
        if (!m_held)
            return;
        endWhatEndsAtSuspension();
        if (!m_held->untilEnded.isEmpty())
            endWhatIsWrittenOutsideFreshObjects(Effects(), state);
    }

    // Each answer of the family Note that the state holds and whose holder is that object, the newest first.
    void forEachNoteOf(Reference holder, NOESCAPE const ScopedLambda<void(const KnownAnswer&)>&) const;

private:
    // States are moved for every block and edge, and cleared: a state that holds nothing has none, and a move is a word.
    struct Held {
        // Both hold what rests on no heap.
        SetOfAnswersForGood forGood;
        SetOfAnswersForGood untilSuspension;
        // By the numbers that AnswersOfWalk has for them.
        SetOfAnswersForGood outcomes;
        SetOfAnswersForGood conversions;
        ListOfAnswersUntilEnded untilEnded;
        // What rests on parts of one fresh object and on nothing else, and does not end at a suspension. No write but one into that very object ends it, so whatever ends answers passes all of these by:
        // `const a = [x, y]; f(); n += a.length`, a thousand times over, went through the answers of every list so far at every call.
        // An answer stands here from where it is noted, or from the first write that has asked about it, until its holder leaves: holderHasLeft(). In this list or in untilEnded, never in both.
        ListOfAnswersUntilEnded onFreshHolders;
        // All that the members of untilEnded rest on, and what some that have ended rested on.
        LocationMasks restsOn;
        // By AnswersOfWalk::Key::amongKeysWithManyAnswers: the answer that was noted last, its number and one more, whether or not it is still held. 0: none since the key has many.
        // What the state holds under the key is that one, or is found from it by `older`. A meet leaves it: what is left is a part of what this state held.
        SharedArray<uint32_t, 2> newestUnderKeysWithManyAnswers;
    };

    // Of a state that holds something.
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
    // Its number and one more. 0: none.
    uint32_t findHeldUnder(uint32_t key) const;
    // Where the way through the conversions of a record with many begins for this state: its number and one more.
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
