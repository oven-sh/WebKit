/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkKnown.h"

#if ENABLE(AOT)

#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTOpcodeSets.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

bool SetOfAnswersForGood::addWhereWordIsMissing(uint32_t number)
{
    uint32_t word = number / bitsOfWord;
    uint64_t bit = 1ull << number % bitsOfWord;
    if (!m_size) {
        if (!m_words.isEmpty())
            m_words.clear();
        m_firstWord = word;
    } else if (word < m_firstWord)
        makeRoomFrom(word);

    uint32_t place = word - m_firstWord;
    if (m_words.size() == place) {
        m_words.append(bit);
        ++m_size;
        return true;
    }
    if (m_words.size() < place)
        m_words.grow(0, place + 1);
    if (m_words[place] & bit)
        return false;
    m_words.mutableAt(place) |= bit;
    ++m_size;

    return true;
}

void SetOfAnswersForGood::makeRoomFrom(uint32_t word)
{
    SharedArray<uint64_t, 2> words;
    words.grow(0, m_firstWord - word);
    for (uint32_t place = 0; place < m_words.size(); ++place)
        words.append(m_words[place]);
    m_words.swap(words);
    m_firstWord = word;
}

void SetOfAnswersForGood::keepMembersOfSetWithMembers(const SetOfAnswersForGood& other)
{
    if (other.m_words.size() < m_words.size()) {
        SetOfAnswersForGood shorter = other;
        shorter.keepMembersOf(*this);
        swap(shorter);
        return;
    }

    struct Stretch {
        uint32_t begin;
        uint32_t end;
    };
    Vector<Stretch, 8> stretches;
    if (m_firstWord == other.m_firstWord) {
        m_words.forEachStretchNotSharedWith(other.m_words, [&](uint32_t begin, uint32_t end) {
            stretches.append({ begin, end });
        });
    } else
        stretches.append({ 0, m_words.size() });
    for (Stretch stretch : stretches) {
        for (uint32_t place = stretch.begin; place < stretch.end; ++place) {
            uint64_t held = m_words[place];
            uint64_t kept = held & other.wordAt(m_firstWord + place);
            if (kept == held)
                continue;
            m_words.mutableAt(place) = kept;
            m_size -= std::popcount(held ^ kept);
        }
    }

    uint32_t numberOfWords = m_words.sizeWithoutZerosAtEnd();
    if (numberOfWords != m_words.size())
        m_words.shrink(numberOfWords);
}

uint32_t ListOfAnswersUntilEnded::firstPlaceNotBelow(uint32_t number) const
{
    uint32_t begin = 0;
    uint32_t end = m_places.size();
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2;
        if ((m_places[middle] & ~hasEnded) < number)
            begin = middle + 1;
        else
            end = middle;
    }
    return begin;
}

bool ListOfAnswersUntilEnded::add(uint32_t number)
{
    ASSERT(!(number & hasEnded));
    uint32_t size = m_places.size();
    if (!size || (m_places[size - 1] & ~hasEnded) < number) {
        m_places.append(number);
        ++m_numberHeld;
        return true;
    }

    uint32_t place = firstPlaceNotBelow(number);
    uint32_t held = m_places[place];
    if (held == number)
        return false;
    ++m_numberHeld;
    if ((held & ~hasEnded) == number) {
        m_places.mutableAt(place) = number;
        return true;
    }
    if (held & hasEnded) {
        m_places.mutableAt(place) = number;
        return true;
    }
    if (place && m_places[place - 1] & hasEnded) {
        m_places.mutableAt(place - 1) = number;
        return true;
    }
    uint32_t numberEnded = size - (m_numberHeld - 1);
    uint32_t mostEnded = m_numberHeld + endedPlacesAlwaysBorne;
    uint32_t room = place || number ? std::min(size - place, mostEnded > numberEnded ? mostEnded - numberEnded : 0) : 0;
    uint32_t ended = (place ? m_places[place - 1] & ~hasEnded : 0) | hasEnded;
    m_places.grow(ended, size + room + 1);
    for (uint32_t moved = size; moved > place; --moved) {
        uint32_t behind = m_places[moved - 1];
        m_places.mutableAt(moved + room) = behind;
    }
    for (uint32_t left = place; left < place + room; ++left)
        m_places.mutableAt(left) = ended;
    m_places.mutableAt(place + room) = number;
    return true;
}

bool ListOfAnswersUntilEnded::remove(uint32_t number)
{
    uint32_t place = firstPlaceNotBelow(number);
    if (place >= m_places.size() || m_places[place] != number)
        return false;
    m_places.mutableAt(place) = number | hasEnded;
    --m_numberHeld;
    dropEndedPlacesIfMany();
    return true;
}

void ListOfAnswersUntilEnded::dropEndedPlacesIfMany()
{
    if (!m_numberHeld) {
        if (!m_places.isEmpty())
            m_places.clear();
        return;
    }
    if (m_places.size() - m_numberHeld <= m_numberHeld + endedPlacesAlwaysBorne)
        return;
    SharedArray<uint32_t, 4> places;
    forEach([&](uint32_t number) {
        places.append(number);
    });
    m_places.swap(places);
}

void ListOfAnswersUntilEnded::keepMembersOfListWithMembers(const ListOfAnswersUntilEnded& other)
{
    struct Stretch {
        uint32_t begin;
        uint32_t end;
    };
    Vector<Stretch, 8> stretches;
    m_places.forEachStretchNotSharedWith(other.m_places, [&](uint32_t begin, uint32_t end) {
        stretches.append({ begin, end });
    });
    for (Stretch stretch : stretches) {
        for (uint32_t place = stretch.begin; place < stretch.end; ++place) {
            uint32_t held = m_places[place];
            if (held & hasEnded)
                continue;
            bool liesAtSamePlace = place < other.m_places.size() && other.m_places[place] == held;
            if (liesAtSamePlace || other.contains(held))
                continue;
            m_places.mutableAt(place) = held | hasEnded;
            --m_numberHeld;
        }
    }
    dropEndedPlacesIfMany();
}

ALWAYS_INLINE_INTO_OTHER_FILES void ListOfAnswersUntilEnded::keepMembersOfEither(const ListOfAnswersUntilEnded& other, const ListOfAnswersUntilEnded& beside, Vector<uint32_t, 8>& gone)
{
    if (!m_numberHeld)
        return;
    struct Stretch {
        uint32_t begin;
        uint32_t end;
    };
    Vector<Stretch, 8> stretches;
    m_places.forEachStretchNotSharedWith(other.m_places, [&](uint32_t begin, uint32_t end) {
        stretches.append({ begin, end });
    });
    for (Stretch stretch : stretches) {
        for (uint32_t place = stretch.begin; place < stretch.end; ++place) {
            uint32_t held = m_places[place];
            if (held & hasEnded)
                continue;
            bool liesAtSamePlace = place < other.m_places.size() && other.m_places[place] == held;
            if (liesAtSamePlace || other.contains(held) || beside.contains(held))
                continue;
            m_places.mutableAt(place) = held | hasEnded;
            --m_numberHeld;
            gone.append(held);
        }
    }
    dropEndedPlacesIfMany();
}

static constexpr size_t fewestPlacesOfKeysInKnownAnswers = 64;

void AnswersOfWalk::clear()
{
    m_whoAskedLast = nullptr;
    if (m_answersOfHolders.isEmpty() && m_keys.isEmpty())
        return;
    m_conversions.shrink(0);
    m_recordsWithManyConversions.clear();
    m_numberOfOutcomes = 0;
    m_answers.shrink(0);
    m_masks.shrink(0);
    m_keys.shrink(0);
    m_wordsOfKeys.shrink(0);
    m_placesOfKeys.shrink(0);
    m_newestOfFirstAnswers.shrink(0);
    m_placesOfNotes.shrink(0);
    m_numberOfPlacesOfNotesTaken = 0;
    m_answersOfHolders.shrink(0);
}

uint32_t AnswersOfWalk::hashOf(std::span<const uint32_t> words)
{
    uint32_t hash = 0x811c9dc5u;
    for (uint32_t word : words) {
        hash = (hash ^ word) * 0x9e3779b1u;
        hash ^= hash >> 15;
    }
    hash *= 0x85ebca77u;
    hash ^= hash >> 13;
    return hash;
}

uint32_t AnswersOfWalk::findKey(std::span<const uint32_t> words, uint32_t hash) const
{
    if (m_placesOfKeys.isEmpty())
        return 0;
    uint32_t mask = m_placesOfKeys.size() - 1;
    for (uint32_t place = hash & mask;; place = (place + 1) & mask) {
        uint32_t found = m_placesOfKeys[place];
        if (!found)
            return 0;
        const Key& key = m_keys[found - 1];
        if (key.hash == hash && std::ranges::equal(wordsOf(key), words))
            return found;
    }
}

uint32_t AnswersOfWalk::findKey(std::span<const uint32_t> words) const
{
    return findKey(words, hashOf(words));
}

void AnswersOfWalk::growPlacesOfKeys()
{
    m_placesOfKeys.fill(0, std::max<size_t>(fewestPlacesOfKeysInKnownAnswers, m_placesOfKeys.size() * 2));
    uint32_t mask = m_placesOfKeys.size() - 1;
    for (uint32_t key = 0; key < m_keys.size(); ++key) {
        uint32_t place = m_keys[key].hash & mask;
        while (m_placesOfKeys[place])
            place = (place + 1) & mask;
        m_placesOfKeys[place] = key + 1;
    }
}

uint32_t AnswersOfWalk::addKey(std::span<const uint32_t> words, uint32_t hash)
{
    if ((m_keys.size() + 1) * 2 > m_placesOfKeys.size())
        growPlacesOfKeys();

    Key key;
    key.hash = hash;
    key.firstWord = m_wordsOfKeys.size();
    key.numberOfWords = words.size();
    m_wordsOfKeys.append(words);
    m_keys.append(key);

    uint32_t mask = m_placesOfKeys.size() - 1;
    uint32_t place = hash & mask;
    while (m_placesOfKeys[place])
        place = (place + 1) & mask;
    m_placesOfKeys[place] = m_keys.size();
    return m_keys.size();
}

ALWAYS_INLINE_INTO_OTHER_FILES uint32_t AnswersOfWalk::numberConversion(Reference value, Representation wanted, Reference converted)
{
    uint32_t number = m_conversions.size();
    uint32_t older = std::exchange(rowOf(value.id().index()).newestConversion, number + 1);
    uint8_t numberInFront = older ? m_conversions[older - 1].numberInFront + 1 : 0;
    if (numberInFront >= mostAnswersGoneThroughUnderKey) [[unlikely]] {
        if (numberInFront == mostAnswersGoneThroughUnderKey) {
            m_newestOfFirstAnswers.append(older);
            m_recordsWithManyConversions.add(value.id().index(), static_cast<uint32_t>(m_newestOfFirstAnswers.size()));
        }
        numberInFront = mostAnswersGoneThroughUnderKey;
        older = 0;
    }
    m_conversions.append(ConversionMade { converted, older, wanted, numberInFront });
    return number;
}

uint32_t& AnswersOfWalk::placeOfNoteUnderKeyWithManyAnswers(uint32_t key, const KnownAnswer& answer)
{
    std::array<uint32_t, 5> words { key, static_cast<uint32_t>(answer.noted), static_cast<uint32_t>(answer.noted >> 32), answer.holder.bits(), answer.endsAtSuspension };
    uint32_t mask = m_placesOfNotes.size() - 1;
    for (uint32_t place = hashOf(words) & mask;; place = (place + 1) & mask) {
        uint32_t found = m_placesOfNotes[place];
        if (!found)
            return m_placesOfNotes[place];
        const KnownAnswer& held = m_answers[found - 1];
        if (held.key == key && held.noted == answer.noted && held.holder == answer.holder && held.endsAtSuspension == answer.endsAtSuspension)
            return m_placesOfNotes[place];
    }
}

void AnswersOfWalk::makeRoomForNoteUnderKeyWithManyAnswers()
{
    if ((m_numberOfPlacesOfNotesTaken + 1) * 2 <= m_placesOfNotes.size())
        return;
    Vector<uint32_t> former = std::exchange(m_placesOfNotes, { });
    m_placesOfNotes.fill(0, std::max<size_t>(fewestPlacesOfKeysInKnownAnswers, former.size() * 2));
    for (uint32_t taken : former) {
        if (taken)
            placeOfNoteUnderKeyWithManyAnswers(m_answers[taken - 1].key, m_answers[taken - 1]) = taken;
    }
}

void AnswersOfWalk::noteThatKeyHasManyAnswers(uint32_t key)
{
    m_newestOfFirstAnswers.append(m_keys[key].newest);
    m_keys[key].amongKeysWithManyAnswers = m_newestOfFirstAnswers.size();
    for (uint32_t place = m_keys[key].newest; place; place = m_answers[place - 1].older) {
        makeRoomForNoteUnderKeyWithManyAnswers();
        if (m_answers[place - 1].family != KnownAnswer::Family::Note || m_answers[place - 1].restsOn)
            continue;
        uint32_t& placeOfNote = placeOfNoteUnderKeyWithManyAnswers(key, m_answers[place - 1]);
        if (placeOfNote)
            continue;
        placeOfNote = place;
        ++m_numberOfPlacesOfNotesTaken;
    }
}

uint32_t AnswersOfWalk::numberOf(std::span<const uint32_t> wordsOfKey, PlaceOfKey placeOfKey, KnownAnswer answer, const LocationMasks& restsOn, bool mayBeNumbered)
{
    uint32_t key = (placeOfKey.found ? placeOfKey.found : addKey(wordsOfKey, placeOfKey.hash)) - 1;

    uint32_t newest = m_keys[key].newest;
    bool looksAtOlderOnes = answer.family == KnownAnswer::Family::Note && restsOn.isEmpty();
    bool hasManyAnswers = m_keys[key].amongKeysWithManyAnswers;
    if (hasManyAnswers && looksAtOlderOnes) [[unlikely]] {
        if (uint32_t place = mayBeNumbered ? placeOfNoteUnderKeyWithManyAnswers(key, answer) : 0)
            return place - 1;
    } else {
        for (uint32_t place = mayBeNumbered ? newest : 0; place; place = looksAtOlderOnes ? m_answers[place - 1].older : 0) {
            const KnownAnswer& held = m_answers[place - 1];
            bool restsOnSame = restsOn.isEmpty() ? !held.restsOn : held.restsOn && whatAnswerRestsOn(held) == restsOn;
            if (restsOnSame && held.noted == answer.noted && held.holder == answer.holder && held.endsAtSuspension == answer.endsAtSuspension)
                return place - 1;
        }
    }
    if (!hasManyAnswers && m_keys[key].numberOfAnswers == mostAnswersGoneThroughUnderKey) [[unlikely]] {
        noteThatKeyHasManyAnswers(key);
        hasManyAnswers = true;
    }

    uint32_t number = m_answers.size();
    RELEASE_ASSERT(number < 1u << 31);
    answer.key = key;
    answer.older = hasManyAnswers ? 0 : newest;
    answer.olderOfHolder = 0;
    answer.restsOn = 0;
    if (!restsOn.isEmpty()) {
        if (m_masks.isEmpty() || m_masks.last() != restsOn)
            m_masks.append(restsOn);
        answer.restsOn = m_masks.size();
    }
    if (answer.holder.isRecord()) {
        AnswersOfHolder& ofHolder = rowOf(answer.holder.id().index());
        answer.olderOfHolder = std::exchange(ofHolder.newest, number + 1);
        ++ofHolder.number;
    }
    m_answers.append(answer);
    if (hasManyAnswers && looksAtOlderOnes) [[unlikely]] {
        makeRoomForNoteUnderKeyWithManyAnswers();
        uint32_t& placeOfNote = placeOfNoteUnderKeyWithManyAnswers(key, answer);
        m_numberOfPlacesOfNotesTaken += !placeOfNote;
        placeOfNote = number + 1;
    }

    Key& numbered = m_keys[key];
    numbered.newest = number + 1;
    ++numbered.numberOfAnswers;
    numbered.hasAnswersThatRestOnNothing |= !answer.restsOn;
    return number;
}

void KnownAnswers::dropWhatIsHeld()
{
    m_held = nullptr;
}

uint32_t KnownAnswers::findHeldUnder(uint32_t keyAsked) const
{
    const AnswersOfWalk::Key& key = m_table->keyAt(keyAsked);
    if (key.amongKeysWithManyAnswers) [[unlikely]] {
        uint32_t among = key.amongKeysWithManyAnswers - 1;
        uint32_t newest = among < m_held->newestUnderKeysWithManyAnswers.size() ? m_held->newestUnderKeysWithManyAnswers[among] : 0;
        for (uint32_t place = newest ? newest : m_table->newestOfFirstAnswersUnder(key); place; place = m_table->at(place - 1).older) {
            if (holds(place - 1))
                return place;
        }
        return 0;
    }
    if (!key.hasAnswersThatRestOnNothing && m_held->untilEnded.numberOfPlaces() + m_held->onFreshHolders.numberOfPlaces() < key.numberOfAnswers) {
        uint32_t found = 0;
        m_held->untilEnded.forEach([&](uint32_t number) {
            if (m_table->at(number).key == keyAsked)
                found = number + 1;
        });
        m_held->onFreshHolders.forEach([&](uint32_t number) {
            if (m_table->at(number).key == keyAsked)
                found = std::max(found, number + 1);
        });
        return found;
    }
    for (uint32_t place = key.newest; place; place = m_table->at(place - 1).older) {
        if (holds(place - 1))
            return place;
    }
    return 0;
}

const KnownAnswer* KnownAnswers::find(std::span<const uint32_t> wordsOfKey) const
{
    if (!m_table || isEmpty())
        return nullptr;
    uint32_t key = m_table->findKey(wordsOfKey);
    uint32_t held = key ? findHeldUnder(key - 1) : 0;
    return held ? &m_table->at(held - 1) : nullptr;
}

const KnownAnswer* KnownAnswers::find(AnswersOfWalk::PlaceOfKey placeOfKey) const
{
    uint32_t held = placeOfKey.found && !isEmpty() ? findHeldUnder(placeOfKey.found - 1) : 0;
    return held ? &m_table->at(held - 1) : nullptr;
}

void KnownAnswers::note(std::span<const uint32_t> wordsOfKey, AnswersOfWalk::PlaceOfKey placeOfKey, const KnownAnswer& answer, const LocationMasks& restsOn, bool mayBeNumbered, bool holderIsFresh)
{
    uint32_t numberOfAnswersInFront = m_table->size();
    uint32_t number = m_table->numberOf(wordsOfKey, placeOfKey, answer, restsOn, mayBeNumbered);
    const KnownAnswer& numbered = m_table->at(number);
    uint32_t amongKeysWithManyAnswers = m_table->keyAt(numbered.key).amongKeysWithManyAnswers;
    if (numbered.family == KnownAnswer::Family::Note && !isEmpty()) {
        uint32_t held = findHeldUnder(numbered.key);
        if (held == number + 1)
            return;
        if (held)
            drop(held - 1);
    } else if (amongKeysWithManyAnswers && number == numberOfAnswersInFront && !isEmpty()) [[unlikely]]
        m_table->setOlder(number, findHeldUnder(numbered.key));
    if (!m_held)
        m_held = makeUniqueWithoutFastMallocCheck<Held>();
    if (amongKeysWithManyAnswers) [[unlikely]] {
        if (m_held->newestUnderKeysWithManyAnswers.size() < amongKeysWithManyAnswers)
            m_held->newestUnderKeysWithManyAnswers.grow(0, amongKeysWithManyAnswers);
        m_held->newestUnderKeysWithManyAnswers.mutableAt(amongKeysWithManyAnswers - 1) = number + 1;
    }
    if (!numbered.restsOn) {
        (numbered.endsAtSuspension ? m_held->untilSuspension : m_held->forGood).add(number);
        return;
    }
    if (holderIsFresh && !numbered.endsAtSuspension && restsOn.containsOnly(perObjectHeaps) && m_held->onFreshHolders.appendIfBehindAll(number))
        return;
    m_held->untilEnded.add(number);
    m_held->restsOn.merge(restsOn);
}

ALWAYS_INLINE_INTO_OTHER_FILES std::optional<bool> KnownAnswers::outcomeOf(Reference condition) const
{
    if (!m_held || m_held->outcomes.isEmpty())
        return std::nullopt;
    uint32_t ifTrue = m_table->outcomesOf(condition);
    if (!ifTrue)
        return std::nullopt;
    if (m_held->outcomes.contains(ifTrue))
        return true;
    return m_held->outcomes.contains(ifTrue - 1) ? std::optional<bool>(false) : std::nullopt;
}

ALWAYS_INLINE_INTO_OTHER_FILES void KnownAnswers::noteConversion(Reference value, Representation wanted, Reference converted, bool endsAtSuspension)
{
    if (!m_table)
        return;
    m_table->setConversionsEndAtSuspension(endsAtSuspension);
    if (!m_held)
        m_held = makeUniqueWithoutFastMallocCheck<Held>();
    uint32_t number = m_table->numberConversion(value, wanted, converted);
    if (m_table->isBehindFirstConversions(m_table->conversionAt(number))) [[unlikely]] {
        uint32_t held = newestConversionNotedOf(value);
        while (held && !m_held->conversions.contains(held - 1))
            held = m_table->conversionAt(held - 1).older;
        m_table->setOlderConversion(number, held);
        uint32_t among = m_table->amongKeysWithManyAnswers(value);
        if (m_held->newestUnderKeysWithManyAnswers.size() < among)
            m_held->newestUnderKeysWithManyAnswers.grow(0, among);
        m_held->newestUnderKeysWithManyAnswers.mutableAt(among - 1) = number + 1;
    }
    m_held->conversions.add(number);
}

Reference KnownAnswers::findConversion(Reference value, Representation wanted) const
{
    if (!m_held || m_held->conversions.isEmpty())
        return { };
    uint32_t newest = m_table->newestConversionOf(value);
    if (newest && m_table->isBehindFirstConversions(m_table->conversionAt(newest - 1))) [[unlikely]]
        newest = newestConversionNotedOf(value);
    for (uint32_t place = newest; place; place = m_table->conversionAt(place - 1).older) {
        const AnswersOfWalk::ConversionMade& made = m_table->conversionAt(place - 1);
        if (made.wanted == wanted && m_held->conversions.contains(place - 1))
            return made.converted;
    }
    return { };
}

void KnownAnswers::end(std::span<const uint32_t> wordsOfKey)
{
    if (!m_table || isEmpty())
        return;
    uint32_t key = m_table->findKey(wordsOfKey);
    while (uint32_t held = key ? findHeldUnder(key - 1) : 0)
        drop(held - 1);
    if (m_held->untilEnded.isEmpty())
        forgetWhatRestsOnHeaps();
}

void KnownAnswers::endWhatIsWritten(const Effects& effects, NOESCAPE const AsksWhetherAnswerEnds& ends)
{
    if (!m_held)
        return;
    if (effects.maySuspend)
        endWhatEndsAtSuspension();
    if (m_held->untilEnded.isEmpty())
        return;
    bool runsOtherCode = effects.mayRunOtherCode();
    if (!runsOtherCode && !effects.writes.overlaps(m_held->restsOn))
        return;

    LocationMasks writtenOutsideObjects = effects.writes.without(perObjectHeaps);
    LocationMasks whatRestRestsOn;
    m_held->untilEnded.removeAllMatching([&](uint32_t number) {
        const KnownAnswer& answer = m_table->at(number);
        if (effects.maySuspend && answer.endsAtSuspension)
            return true;
        const LocationMasks& restsOn = m_table->whatAnswerRestsOn(answer);
        bool isWritten = runsOtherCode || effects.writes.overlaps(restsOn);
        bool isWrittenOutsideObjects = runsOtherCode ? !restsOn.containsOnly(perObjectHeaps) : writtenOutsideObjects.overlaps(restsOn);
        if (isWritten && ends(answer, isWrittenOutsideObjects))
            return true;
        whatRestRestsOn.merge(restsOn);
        return false;
    });
    m_held->restsOn = whatRestRestsOn;
}

void KnownAnswers::meetWhatRestsOnHeapsWith(const Held& other)
{
    Vector<uint32_t, 8> gone;
    m_held->untilEnded.keepMembersOfEither(other.untilEnded, other.onFreshHolders, gone);
    gone.shrink(0);
    m_held->onFreshHolders.keepMembersOfEither(other.onFreshHolders, { }, gone);
    for (uint32_t number : gone) {
        if (!other.untilEnded.contains(number))
            continue;
        m_held->untilEnded.add(number);
        m_held->restsOn.merge(m_table->whatAnswerRestsOn(m_table->at(number)));
    }
}

void KnownAnswers::endWhatIsWrittenOutsideFreshObjects(const Effects& effects, const WalkState& state)
{
    auto ends = [&](const KnownAnswer& answer, bool isWrittenOutsideObjects) {
        if (isWrittenOutsideObjects || !answer.holder || !state.isFresh(answer.holder))
            return true;
        return !answer.endsAtSuspension && m_table->whatAnswerRestsOn(answer).containsOnly(perObjectHeaps) && m_held->onFreshHolders.appendIfBehindAll(static_cast<uint32_t>(&answer - &m_table->at(0)));
    };
    endWhatIsWritten(effects, ends);
}

void KnownAnswers::endWhatIsWrittenIn(Reference holder, const Effects& effects)
{
    if (!m_held)
        return;
    auto isWritten = [&](uint32_t number) {
        const KnownAnswer& answer = m_table->at(number);
        return answer.holder == holder && answer.restsOn && effects.writes.overlaps(m_table->whatAnswerRestsOn(answer));
    };
    auto endIn = [&](ListOfAnswersUntilEnded& list) {
        if (list.isEmpty())
            return;
        if (list.numberOfPlaces() <= m_table->numberOfAnswersOfHolder(holder)) {
            list.removeAllMatching(isWritten);
            return;
        }
        for (uint32_t place = m_table->newestOfHolder(holder); place; place = m_table->at(place - 1).olderOfHolder) {
            if (isWritten(place - 1))
                list.remove(place - 1);
        }
    };
    if (effects.writes.containsAny(perObjectHeaps))
        endIn(m_held->onFreshHolders);
    if (m_held->untilEnded.isEmpty() || !effects.writes.overlaps(m_held->restsOn))
        return;
    if (m_held->untilEnded.numberOfPlaces() <= m_table->numberOfAnswersOfHolder(holder)) {
        m_held->untilEnded.removeAllMatching(isWritten);
        return;
    }
    for (uint32_t place = m_table->newestOfHolder(holder); place; place = m_table->at(place - 1).olderOfHolder) {
        if (isWritten(place - 1))
            m_held->untilEnded.remove(place - 1);
    }
}

void KnownAnswers::holderIsFreshAgain(Reference holder)
{
    if (!m_held || m_held->untilEnded.isEmpty())
        return;
    auto restsOnHolderAlone = [&](uint32_t number) {
        const KnownAnswer& answer = m_table->at(number);
        return answer.holder == holder && answer.restsOn && !answer.endsAtSuspension && m_table->whatAnswerRestsOn(answer).containsOnly(perObjectHeaps);
    };
    if (m_held->untilEnded.numberOfPlaces() <= m_table->numberOfAnswersOfHolder(holder)) {
        m_held->untilEnded.removeAllMatching([&](uint32_t number) {
            if (!restsOnHolderAlone(number))
                return false;
            m_held->onFreshHolders.add(number);
            return true;
        });
        return;
    }
    for (uint32_t place = m_table->newestOfHolder(holder); place; place = m_table->at(place - 1).olderOfHolder) {
        if (restsOnHolderAlone(place - 1) && m_held->untilEnded.remove(place - 1))
            m_held->onFreshHolders.add(place - 1);
    }
}

void KnownAnswers::holderHasLeft(Reference holder)
{
    if (!m_held || m_held->onFreshHolders.isEmpty())
        return;
    auto goesBack = [&](uint32_t number) {
        m_held->untilEnded.add(number);
        m_held->restsOn.merge(m_table->whatAnswerRestsOn(m_table->at(number)));
    };
    if (m_held->onFreshHolders.numberOfPlaces() <= m_table->numberOfAnswersOfHolder(holder)) {
        m_held->onFreshHolders.removeAllMatching([&](uint32_t number) {
            if (m_table->at(number).holder != holder)
                return false;
            goesBack(number);
            return true;
        });
        return;
    }
    for (uint32_t place = m_table->newestOfHolder(holder); place; place = m_table->at(place - 1).olderOfHolder) {
        if (m_held->onFreshHolders.remove(place - 1))
            goesBack(place - 1);
    }
}

static constexpr uint32_t shiftOfFamilyInKeyOfKnownAnswer = 30;
static constexpr uint32_t shiftOfFlagsInKeyOfKnownAnswer = 16;
static constexpr uint32_t shiftOfValuesByContentInKeyOfKnownAnswer = 24;
static constexpr uint32_t mostValuesInKeyOfKnownAnswer = 2;
static constexpr uint32_t mostWordsOfContentInKeyOfKnownAnswer = 255;
static_assert(numberOfEnumerators<Opcode> <= 1u << shiftOfFlagsInKeyOfKnownAnswer);
static_assert(shiftOfValuesByContentInKeyOfKnownAnswer + mostValuesInKeyOfKnownAnswer <= shiftOfFamilyInKeyOfKnownAnswer);
static_assert(8 * sizeof(Draft::flags) <= shiftOfValuesByContentInKeyOfKnownAnswer - shiftOfFlagsInKeyOfKnownAnswer);
static_assert(mostWordsOfContentInKeyOfKnownAnswer <= UINT32_MAX >> shiftOfValuesByContentInKeyOfKnownAnswer);

static uint32_t headOfKeyOfKnownAnswer(KnownAnswer::Family family, uint32_t kind, uint32_t flags = 0)
{
    return static_cast<uint32_t>(family) << shiftOfFamilyInKeyOfKnownAnswer | flags << shiftOfFlagsInKeyOfKnownAnswer | kind;
}

static Reference referenceToRecordInKnownAnswers(const Code& code, ValueID record)
{
    return code.isConstant(record) ? Reference::constant(record) : Reference::value(record);
}

static ALWAYS_INLINE Reference rootInKnownAnswers(const Code& code, Reference name)
{
    return name.isValue() ? referenceToRecordInKnownAnswers(code, underlyingCellOf(code, name.id())) : name;
}

static ALWAYS_INLINE Reference valueBehindFormsInKnownAnswers(const Code& code, Reference name)
{
    if (!name.isValue())
        return name;
    return referenceToRecordInKnownAnswers(code, code.valueBehindNames<valuePreservingOpcodes>(name.id()));
}

static ALWAYS_INLINE void appendValueToKeyOfKnownAnswer(const Code& code, Reference value, uint32_t position, AnswersOfWalk::WordsOfKey& words)
{
    ASSERT(position < mostValuesInKeyOfKnownAnswer);
    if (value.isConstant()) {
        const Operation& record = code.at(value.id());
        std::span<const uint32_t> content = record.payload();
        if (!record.numberOfOperands() && content.size() <= mostWordsOfContentInKeyOfKnownAnswer) {
            words[0] |= 1u << (shiftOfValuesByContentInKeyOfKnownAnswer + position);
            words.append(static_cast<uint32_t>(record.opcode) | static_cast<uint32_t>(record.flags) << shiftOfFlagsInKeyOfKnownAnswer | static_cast<uint32_t>(content.size()) << shiftOfValuesByContentInKeyOfKnownAnswer);
            words.append(content);
            return;
        }
    }
    words.append(value.bits());
}

static ALWAYS_INLINE void collectKeyOfNoteInKnownAnswers(const Code& code, KnownNote kind, Reference subject, Reference other, uint32_t word, AnswersOfWalk::WordsOfKey& words)
{
    words.append(headOfKeyOfKnownAnswer(KnownAnswer::Family::Note, static_cast<uint32_t>(kind)));
    words.append(word);
    appendValueToKeyOfKnownAnswer(code, subject, 0, words);
    appendValueToKeyOfKnownAnswer(code, other, 1, words);
}

static bool frameMayBeSavedInKnownAnswers(const Walker& walker)
{
    return walker.program().at(walker.unit().function).isCoroutine();
}

static bool isOutcomeOfValueInKnownAnswers(KnownNote kind, Reference holder, Reference other, uint32_t word)
{
    return kind == KnownNote::Outcome && holder.isValue() && !other && !word;
}

void noteKnown(Walker& walker, KnownNote kind, Reference subject, Reference other, uint32_t word, const LocationMasks& restsOn, uint64_t noted, bool endsAtSuspension)
{
    if (!walker.has(Mechanism::Folding))
        return;
    const Code& code = walker.code();
    Reference holder = rootInKnownAnswers(code, subject);
    if (isOutcomeOfValueInKnownAnswers(kind, holder, other, word)) {
        RELEASE_ASSERT(restsOn.isEmpty() && !endsAtSuspension);
        walker.state().answers().noteOutcome(holder, noted);
        return;
    }
    AnswersOfWalk::WordsOfKey words;
    collectKeyOfNoteInKnownAnswers(code, kind, holder, rootInKnownAnswers(code, other), word, words);

    KnownAnswer answer;
    answer.noted = noted;
    answer.holder = holder;
    answer.note = static_cast<uint8_t>(kind);
    answer.endsAtSuspension = endsAtSuspension && frameMayBeSavedInKnownAnswers(walker);
    walker.state().answers().note(words.span(), answer, restsOn, true, holder && walker.state().isFresh(holder));
}

std::optional<uint64_t> findKnown(const Walker& walker, KnownNote kind, Reference subject, Reference other, uint32_t word)
{
    const KnownAnswers& answers = walker.state().answers();
    if (answers.isEmpty() || !walker.has(Mechanism::Folding) || !walker.state().remembersPlaces())
        return std::nullopt;
    const Code& code = walker.code();
    Reference holder = rootInKnownAnswers(code, subject);
    if (isOutcomeOfValueInKnownAnswers(kind, holder, other, word)) {
        std::optional<bool> outcome = answers.outcomeOf(holder);
        return outcome ? std::optional<uint64_t>(*outcome) : std::nullopt;
    }
    if (holder.isValue() && !answers.table()->newestOfHolder(holder))
        return std::nullopt;
    AnswersOfWalk::WordsOfKey words;
    collectKeyOfNoteInKnownAnswers(code, kind, holder, rootInKnownAnswers(code, other), word, words);
    const KnownAnswer* answer = answers.find(words.span());
    return answer ? std::optional<uint64_t>(answer->noted) : std::nullopt;
}

void endKnown(Walker& walker, KnownNote kind, Reference subject, Reference other, uint32_t word)
{
    KnownAnswers& answers = walker.state().answers();
    if (answers.isEmpty())
        return;
    const Code& code = walker.code();
    Reference holder = rootInKnownAnswers(code, subject);
    RELEASE_ASSERT(!isOutcomeOfValueInKnownAnswers(kind, holder, other, word));
    if (holder.isValue() && !answers.table()->newestOfHolder(holder))
        return;
    AnswersOfWalk::WordsOfKey words;
    collectKeyOfNoteInKnownAnswers(code, kind, holder, rootInKnownAnswers(code, other), word, words);
    answers.end(words.span());
}

static constexpr OpcodeSet testsNumberedInKnownAnswers = allEqualities | allRelations | OpcodeSet { Opcode::ToBoolean, Opcode::HasArrayStorage };
static constexpr OpcodeSet valuesNumberedInKnownAnswers = allNumericOperators | OpcodeSet { Opcode::WholeDivisor, Opcode::RemainderByWholeDivisor, Opcode::ToInt32, Opcode::ToUint32, Opcode::GetStringLength, Opcode::GetTypedArrayLength, Opcode::PrivateGet, Opcode::GetValueAtSiteOrAbsent, Opcode::CallInline };
static constexpr OpcodeSet operationsThatDoNothingAgainInKnownAnswers { Opcode::GetBuiltinValue, Opcode::ModuleBuiltin };
static bool doesNothingAgainInKnownAnswers(const Walker& walker, const Draft& draft)
{
    if (!operationsThatDoNothingAgainInKnownAnswers.contains(draft.opcode))
        return false;
    return draft.opcode != Opcode::GetBuiltinValue || !walker.program().builtins().has(BuiltinMark::IsReplacedByHostThatSaysSo, draft.builtin());
}
static constexpr OpcodeSet readsAnsweredByNotesInKnownAnswers { Opcode::GetArrayLength };
static constexpr OpcodeSet operationsWithKeysInKnownAnswers = testsNumberedInKnownAnswers | valuesNumberedInKnownAnswers | operationsThatDoNothingAgainInKnownAnswers;
static constexpr OpcodeSet operationsFoundInKnownAnswers = operationsWithKeysInKnownAnswers | readsAnsweredByNotesInKnownAnswers;
static_assert(!operationsFoundInKnownAnswers.contains(Opcode::TryFindInPrototypeChain));
static constexpr OpcodeSet operationsThatTakeOperandsInEitherOrderInKnownAnswers = allEqualities | OpcodeSet { Opcode::Add, Opcode::Mul, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor };
static_assert(!operationsFoundInKnownAnswers.intersects(conversionsOfForm));

static constexpr AbstractHeapSet heapsThatNoAnswerRestsOnInKnownAnswers { AbstractHeap::StringStorage };

static bool givesSameAgainInKnownAnswers(const Effects& effects)
{
    return !effects.mayRunOtherCode() && !effects.mayBeTerminated && effects.writes.containsOnly(unobservableWriteHeaps) && !effects.reads.containsAny(bookkeepingHeaps);
}

static bool givesNewAnswerUnaskedInKnownAnswers(const Walker& walker, const Draft& draft)
{
    return draft.opcode == Opcode::GetTypedArrayLength && viewMayFollowSharedMemoryThatGrows(walker, draft.viewOperand());
}

static bool collectKeyOfOperationInKnownAnswers(const Code& code, const Draft& draft, AnswersOfWalk::WordsOfKey& words)
{
    words.shrink(0);
    Opcode opcode = draft.opcode;
    uint32_t numberOfOperands = opcode == Opcode::RemainderByWholeDivisor ? OperandIndex::RemainderByWholeDivisor::wholeDivisor : draft.numberOfOperands();
    if (numberOfOperands > mostValuesInKeyOfKnownAnswer)
        return false;

    words.append(headOfKeyOfKnownAnswer(KnownAnswer::Family::Operation, static_cast<uint32_t>(opcode)));
    switch (opcode) {
    case Opcode::GetBuiltinValue:
    case Opcode::ModuleBuiltin:
        words.append(draft.payload());
        break;
    case Opcode::PrivateGet:
        words.append(draft.layout().index());
        words.append(draft.field());
        break;
    case Opcode::GetTypedArrayLength:
        words.append(static_cast<uint32_t>(draft.typedArrayType()));
        break;
    case Opcode::GetValueAtSiteOrAbsent:
        words.append(draft.key().bits());
        break;
    case Opcode::CallInline:
        words.append(static_cast<uint32_t>(draft.inlineBuiltin()));
        break;
    case Opcode::ToInt32:
        words.append(draft.flags);
        break;
    case Opcode::HasArrayStorage:
        words.append(static_cast<uint32_t>(draft.arrayStorage()));
        words.append(draft.flags);
        words.append(draft.data() ? draft.data().index() + 1 : 0);
        break;
    default:
        break;
    }

    std::array<Reference, mostValuesInKeyOfKnownAnswer> operands;
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex)
        operands[operandIndex] = valueBehindFormsInKnownAnswers(code, draft.operand(operandIndex));
    if (numberOfOperands == 2 && operationsThatTakeOperandsInEitherOrderInKnownAnswers.contains(opcode)) {
        bool constantStandsInFront = operands[0].isConstant() && !operands[1].isConstant();
        bool areOfOneKind = operands[0].isConstant() == operands[1].isConstant();
        if (constantStandsInFront || (areOfOneKind && operands[0].bits() > operands[1].bits()))
            std::swap(operands[0], operands[1]);
    }
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex)
        appendValueToKeyOfKnownAnswer(code, operands[operandIndex], operandIndex, words);
    return true;
}

static Reference unlessWiderThanDraftInKnownAnswers(Walker& walker, const Draft& draft, Reference earlier)
{
    if (!earlier.isRecord())
        return { };
    if (draft.representation != Representation::Unchosen && walker.representationOf(earlier) != draft.representation)
        return { };
    TypeID held = walker.typeOf(earlier);
    TypeID asked = walker.resultTypeOf(draft);
    if (!held || !asked)
        return { };
    if (walker.types().isSubset(held, asked))
        return earlier;
    Reference narrowed = walker.nameNarrowedBehindTests(earlier);
    bool answers = narrowed != earlier && narrowed.isRecord() && walker.representationOf(narrowed) == walker.representationOf(earlier) && walker.types().isSubset(walker.typeOf(narrowed), asked);
    return answers ? narrowed : Reference();
}

static Reference booleanInKnownAnswers(Walker& walker, bool value)
{
    Draft constant(Opcode::Boolean);
    constant.setBoolean(value);
    return walker.constant(WTF::move(constant));
}

static bool isAsWellAskedAgainInKnownAnswers(const Draft& test, const Effects& effects)
{
    return effects.isPure() && test.form() != OperatorForm::Generic;
}

static Reference answerOfTestBuiltInKnownAnswers(Walker& walker, const Draft& test, const Effects& effects, Reference built)
{
    std::optional<bool> outcome = outcomeKnownOf(walker, built);
    if (!walker.isInLoop())
        return outcome ? booleanInKnownAnswers(walker, *outcome) : isAsWellAskedAgainInKnownAnswers(test, effects) ? Reference() : built;
    const Code& code = walker.code();
    ValueID firstTaken = code.at(built.id()).operand(0);
    if (valueBehindFormsInKnownAnswers(code, referenceToRecordInKnownAnswers(code, firstTaken)) != valueBehindFormsInKnownAnswers(code, test.operand(0)))
        return { };
    return !outcome && isAsWellAskedAgainInKnownAnswers(test, effects) ? Reference() : built;
}

constinit const OpcodeSet opcodesThatFindEqualOperationBuiltTakes = operationsFoundInKnownAnswers;
constinit const OpcodeSet opcodesThatNoteOperationBuiltTakes = operationsFoundInKnownAnswers;

static bool isNumberedInKnownAnswers(const Draft& draft)
{
    return draft.opcode != Opcode::CallInline || isLookupInTable(draft.inlineBuiltin());
}

static bool makesTextOfItsKeyInKnownAnswers(const Draft& draft, const Effects& effects)
{
    return draft.opcode == Opcode::CallInline && (effects.reads.containsAny(bookkeepingHeaps) || effects.writes.containsAny(bookkeepingHeaps));
}

static Effects besideTextOfItsKeyInKnownAnswers(const Effects& effects)
{
    Effects besideText = effects;
    besideText.reads.remove(bookkeepingHeaps);
    besideText.writes.remove(bookkeepingHeaps);
    return besideText;
}

static bool touchesItsSiteInKnownAnswers(const Draft& draft, const Effects& effects)
{
    return draft.opcode == Opcode::GetValueAtSiteOrAbsent && (effects.reads.containsAny(AbstractHeapSet { AbstractHeap::RuntimeState }) || effects.writes.containsAny(AbstractHeapSet { AbstractHeap::RuntimeState }));
}

static Effects besideItsSiteInKnownAnswers(const Effects& effects)
{
    Effects besideItsSite = effects;
    besideItsSite.reads.remove(AbstractHeapSet { AbstractHeap::RuntimeState });
    besideItsSite.writes.remove(AbstractHeapSet { AbstractHeap::RuntimeState });
    return besideItsSite;
}

Reference findEqualOperationBuiltOfItsOpcodes(Walker& walker, const Draft& draft, const Effects& effects)
{
    if (touchesItsSiteInKnownAnswers(draft, effects))
        return findEqualOperationBuiltOfItsOpcodes(walker, draft, besideItsSiteInKnownAnswers(effects));
    if (!isNumberedInKnownAnswers(draft))
        return { };
    if (makesTextOfItsKeyInKnownAnswers(draft, effects))
        return findEqualOperationBuiltOfItsOpcodes(walker, draft, besideTextOfItsKeyInKnownAnswers(effects));
    Opcode opcode = draft.opcode;
    if (!walker.has(Mechanism::Folding) || !walker.state().remembersPlaces())
        return { };
    KnownAnswers& answers = walker.state().answers();
    AnswersOfWalk* table = answers.table();
    if (!table)
        return { };
    table->setWhoAskedLast(nullptr);

    if (opcode == Opcode::GetArrayLength) {
        std::optional<uint64_t> length = findKnown(walker, KnownNote::LengthOfList, draft.arrayOperand());
        return length ? unlessWiderThanDraftInKnownAnswers(walker, draft, Reference::fromBits(static_cast<uint32_t>(*length))) : Reference();
    }

    if ((!doesNothingAgainInKnownAnswers(walker, draft) && !givesSameAgainInKnownAnswers(effects)) || givesNewAnswerUnaskedInKnownAnswers(walker, draft))
        return { };
    AnswersOfWalk::WordsOfKey& words = table->keyAskedLast();
    if (!collectKeyOfOperationInKnownAnswers(walker.code(), draft, words))
        return { };
    table->setWhoAskedLast(&draft);
    table->placeOfKeyAskedLast() = table->placeOfKey(words.span());

    const KnownAnswer* answer = answers.find(table->placeOfKeyAskedLast());
    Reference built = answer ? unlessWiderThanDraftInKnownAnswers(walker, draft, Reference::fromBits(static_cast<uint32_t>(answer->noted))) : Reference();
    if (!built || !testsNumberedInKnownAnswers.contains(opcode))
        return built;
    return answerOfTestBuiltInKnownAnswers(walker, draft, effects, built);
}

void noteOperationBuiltOfItsOpcodes(Walker& walker, const Draft& draft, const Effects& effects, Reference built)
{
    if (touchesItsSiteInKnownAnswers(draft, effects))
        return noteOperationBuiltOfItsOpcodes(walker, draft, besideItsSiteInKnownAnswers(effects), built);
    if (!isNumberedInKnownAnswers(draft))
        return;
    if (makesTextOfItsKeyInKnownAnswers(draft, effects))
        return noteOperationBuiltOfItsOpcodes(walker, draft, besideTextOfItsKeyInKnownAnswers(effects), built);
    Opcode opcode = draft.opcode;
    if (!walker.has(Mechanism::Folding) || !built.isValue())
        return;
    KnownAnswers& answers = walker.state().answers();
    AnswersOfWalk* table = answers.table();
    if (!table)
        return;
    const void* whoAskedLast = table->whoAskedLast();
    table->setWhoAskedLast(nullptr);

    if (opcode == Opcode::GetArrayLength) {
        noteKnown(walker, KnownNote::LengthOfList, draft.arrayOperand(), { }, 0, placesUnderLengthsOfLists, built.bits());
        return;
    }
    if (!operationsWithKeysInKnownAnswers.contains(opcode))
        return;

    bool doesNothingAgain = doesNothingAgainInKnownAnswers(walker, draft);
    if ((!doesNothingAgain && !givesSameAgainInKnownAnswers(effects)) || givesNewAnswerUnaskedInKnownAnswers(walker, draft))
        return;
    const Code& code = walker.code();
    AnswersOfWalk::WordsOfKey& words = table->keyAskedLast();
    bool isKeyAskedLast = whoAskedLast == &draft;
    if (!isKeyAskedLast && !collectKeyOfOperationInKnownAnswers(code, draft, words))
        return;

    LocationMasks restsOn = doesNothingAgain ? LocationMasks() : effects.reads.without(heapsThatNoAnswerRestsOnInKnownAnswers);
    KnownAnswer answer;
    answer.family = KnownAnswer::Family::Operation;
    answer.noted = built.bits();
    if (restsOn.containsAny(perObjectHeaps) && draft.numberOfOperands() && opcode != Opcode::GetValueAtSiteOrAbsent)
        answer.holder = rootInKnownAnswers(code, draft.operand(0));
    answer.endsAtSuspension = valuesNumberedInKnownAnswers.contains(opcode) && frameMayBeSavedInKnownAnswers(walker);
    answers.note(words.span(), isKeyAskedLast ? table->placeOfKeyAskedLast() : table->placeOfKey(words.span()), answer, restsOn, false, answer.holder && walker.state().isFresh(answer.holder));
}

Reference findConversionMade(Walker& walker, Reference value, Representation wanted)
{
    if (!value.isValue() || !walker.has(Mechanism::Folding))
        return { };
    const Code& code = walker.code();
    const Operation& record = code.at(value.id());
    if (conversionsOfForm.contains(record.opcode) && code.at(record.operand(0)).representation == wanted)
        return referenceToRecordInKnownAnswers(code, record.operand(0));

    return walker.state().answers().findConversion(value, wanted);
}

void noteConversionMade(Walker& walker, Reference value, Representation wanted, Reference converted)
{
    if (!value.isValue() || !converted.isValue() || converted == value || !walker.has(Mechanism::Folding))
        return;
    walker.state().answers().noteConversion(value, wanted, converted, frameMayBeSavedInKnownAnswers(walker));
}

std::optional<bool> outcomeKnownOf(Walker& walker, Reference condition)
{
    const KnownAnswers& answers = walker.state().answers();
    if (answers.isEmpty() || !walker.has(Mechanism::Folding))
        return std::nullopt;
    const Code& code = walker.code();
    bool isNegated = false;
    while (condition.isValue()) {
        condition = rootInKnownAnswers(code, condition);
        if (!condition.isValue())
            break;
        if (std::optional<bool> outcome = answers.outcomeOf(condition))
            return *outcome != isNegated;
        const Operation& record = code.at(condition.id());
        if (record.opcode != Opcode::LogicalNot)
            break;
        isNegated = !isNegated;
        condition = referenceToRecordInKnownAnswers(code, record.operand(0));
    }
    return std::nullopt;
}

static void noteRelationInKnownAnswers(Walker& walker, Reference test, const Operation& comparison, bool isTrue)
{
    const Code& code = walker.code();
    Reference left = referenceToRecordInKnownAnswers(code, comparison.operand(0));
    Reference right = referenceToRecordInKnownAnswers(code, comparison.operand(1));
    TypeArena& types = walker.types();
    TypeID typeOfLeft = walker.typeOf(left);
    TypeID typeOfRight = walker.typeOf(right);
    if (!typeOfLeft || !typeOfRight || !types.at(typeOfLeft).isNumber() || !types.at(typeOfRight).isNumber())
        return;
    bool isWrittenOtherWayRound = comparison.opcode == Opcode::Greater || comparison.opcode == Opcode::GreaterEq;
    bool admitsEqual = comparison.opcode == Opcode::LessEq || comparison.opcode == Opcode::GreaterEq;
    noteKnown(walker, KnownNote::Relation, isWrittenOtherWayRound ? right : left, isWrittenOtherWayRound ? left : right, admitsEqual, LocationMasks(), static_cast<uint64_t>(test.bits()) << 32 | isTrue);
}

void recordEdgeOutcome(Walker& walker, BlockID from, uint32_t targetIndex)
{
    if (!walker.has(Mechanism::Folding))
        return;
    const Code& code = walker.code();
    const Terminator& end = code.at(from).terminator;
    if (!end.isSet || !end.isConditionalBranch() || end.numberOfEdges != 2 || targetIndex > 1)
        return;

    bool isTrue = !targetIndex;
    for (Reference condition = referenceToRecordInKnownAnswers(code, end.conditionOperand()); condition.isValue();) {
        condition = rootInKnownAnswers(code, condition);
        if (!condition.isValue())
            return;
        const Operation& record = code.at(condition.id());
        walker.state().answers().noteOutcome(condition, isTrue);
        if (record.opcode == Opcode::LogicalNot) {
            isTrue = !isTrue;
            condition = referenceToRecordInKnownAnswers(code, record.operand(0));
            continue;
        }
        if (allRelations.contains(record.opcode))
            noteRelationInKnownAnswers(walker, condition, record, isTrue);
        if (isTrue && record.opcode == Opcode::ToBoolean)
            noteSlotFilledWhereAnswerOfItsReadIsTruthy(walker, rootInKnownAnswers(code, referenceToRecordInKnownAnswers(code, record.operand(0))));
        return;
    }
}

std::optional<uint32_t> onlyTargetDecidedOnWay(Walker& walker, const BodyTerminator& terminator)
{
    if (terminator.kind() != TerminatorKind::Branch)
        return std::nullopt;
    Reference condition = walker.referenceOf(terminator.operand(0));
    if (!condition || !condition.isValue())
        return std::nullopt;
    std::optional<bool> outcome = outcomeKnownOf(walker, condition);
    if (!outcome)
        return std::nullopt;
    uint32_t target = *outcome ? 0 : 1;
    return target;
}

std::optional<PassedTest> Walker::findUpperBoundProof(Reference value, Reference bound)
{
    TypeID typeOfValue = typeOf(value);
    TypeID typeOfBound = typeOf(bound);
    if (!typeOfValue || !typeOfBound)
        return std::nullopt;
    Type valueType = m_types.at(typeOfValue);
    Type boundType = m_types.at(typeOfBound);
    bool areIntegers = valueType.isInteger() && boundType.isInteger();
    if (areIntegers && valueType.integers.maximum < boundType.integers.minimum)
        return PassedTest();
    auto comparisonThatAnswered = [&](std::optional<uint64_t> noted, bool answer) -> std::optional<PassedTest> {
        if (!noted || static_cast<bool>(*noted & 1) != answer)
            return std::nullopt;
        return findPassedTest(Reference::fromBits(static_cast<uint32_t>(*noted >> 32)), answer);
    };
    if (std::optional<PassedTest> isLess = comparisonThatAnswered(findKnown(*this, KnownNote::Relation, value, bound, 0), true))
        return isLess;
    return areIntegers ? comparisonThatAnswered(findKnown(*this, KnownNote::Relation, bound, value, 1), false) : std::nullopt;
}

std::optional<PassedTest> Walker::findNonNegativeProof(Reference value)
{
    TypeID type = typeOf(value);
    if (!type || !m_types.at(type).isInteger())
        return std::nullopt;
    if (m_types.at(type).integers.minimum >= 0)
        return PassedTest();
    if (m_state.answers().isEmpty())
        return std::nullopt;
    Draft zero(Opcode::Integer);
    zero.setInteger(0);
    std::optional<uint64_t> comparison = findKnown(*this, KnownNote::Relation, constant(WTF::move(zero)), value, 1);
    if (!comparison || !(*comparison & 1))
        return std::nullopt;
    return findPassedTest(Reference::fromBits(static_cast<uint32_t>(*comparison >> 32)), true);
}

bool isKnownToLieBelow(Walker& walker, Reference value, Reference bound)
{
    return !!walker.findUpperBoundProof(value, bound);
}

bool isKnownToLieInside(Walker& walker, Reference position, Reference length)
{
    return walker.findNonNegativeProof(position) && walker.findUpperBoundProof(position, length);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
