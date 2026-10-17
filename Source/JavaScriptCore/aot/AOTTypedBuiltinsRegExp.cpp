/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "JSCInlines.h"
#include "RegExpConstructor.h"
#include "RegExpObjectInlines.h"
#include "RegExpPrototype.h"
#include "StringPrototypeInlines.h"
#include "YarrFlags.h"
#include <wtf/SIMDHelpers.h>

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTTypedRegExpExec, EncodedJSValue, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(uncheckedDowncast<RegExpObject>(regExp)->exec(globalObject, uncheckedDowncast<JSString>(string))));
}

static ALWAYS_INLINE MatchResult matchFromStart(JSGlobalObject* globalObject, VM& vm, RegExp* regExp, JSString* string, StringView input)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    MatchResult result = regExp->matchInline<Yarr::MatchFrom::VMThread>(globalObject, vm, input, 0);
    RETURN_IF_EXCEPTION(scope, MatchResult::failed());
    if (result)
        globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, result, false);
    return result;
}

static ALWAYS_INLINE bool testRegExp(JSGlobalObject* globalObject, VM& vm, JSCell* cell, JSCell* subject)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = uncheckedDowncast<RegExpObject>(cell);
    auto* string = uncheckedDowncast<JSString>(subject);
    RegExp* regExp = object->regExp();
    if (regExp->globalOrSticky() || !object->getLastIndex().isUInt32()) [[unlikely]]
        RELEASE_AND_RETURN(scope, object->test(globalObject, string));
    auto input = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, !!matchFromStart(globalObject, vm, regExp, string, input));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedRegExpTest, size_t, (Instance* instance, JSCell* cell, JSCell* subject))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, testRegExp(globalObject, vm, cell, subject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedRegExpFind, size_t, (Instance* instance, JSCell* cell, JSCell* subject))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* object = uncheckedDowncast<RegExpObject>(cell);
    auto* string = uncheckedDowncast<JSString>(subject);
    auto input = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    unsigned lastIndex = getRegExpObjectLastIndexAsUnsigned(globalObject, object, input);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    RegExp* regExp = object->regExp();
    bool globalOrSticky = regExp->globalOrSticky();
    if (lastIndex == UINT_MAX && globalOrSticky) {
        object->setLastIndex(globalObject, 0);
        OPERATION_RETURN(scope, false);
    }
    if (!globalOrSticky)
        lastIndex = 0;
    auto offsets = regExp->ovectorSpan();
    int position = regExp->matchInline<Yarr::MatchFrom::VMThread>(globalObject, vm, input, lastIndex, offsets);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (position == -1) {
        if (globalOrSticky)
            object->setLastIndex(globalObject, 0);
        OPERATION_RETURN(scope, false);
    }
    MatchResult result(position, offsets[1]);
    if (globalOrSticky)
        object->setLastIndex(globalObject, result.end);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, result, false);
    OPERATION_RETURN(scope, true);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedRegExpTestBoxed, EncodedJSValue, (Instance* instance, JSCell* cell, JSCell* subject))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(testRegExp(globalObject, vm, cell, subject))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedRegExpExecFound, EncodedJSValue, (Instance* instance, JSCell* cell, JSCell* subject))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* string = uncheckedDowncast<JSString>(subject);
    RegExp* regExp = uncheckedDowncast<RegExpObject>(cell)->regExp();
    auto subpatternResults = regExp->ovectorSpan();
    MatchResult result(subpatternResults[0], subpatternResults[1]);
    RELEASE_ASSERT(result.end >= result.start);
    unsigned numSubpatterns = regExp->numSubpatterns();
    bool hasNamedCaptures = regExp->hasNamedCaptures();
    bool createIndices = regExp->hasIndices();
    JSArray* array;
    if (hasNamedCaptures || createIndices) [[unlikely]]
        array = createRegExpMatchesArrayWithGroupsOrIndices(vm, globalObject, string, regExp, result, subpatternResults, numSubpatterns, hasNamedCaptures, createIndices);
    else if (globalObject->isHavingABadTime()) [[unlikely]]
        array = createRegExpMatchesArrayForPlainRegExpHavingABadTime(vm, globalObject, string, result, subpatternResults, numSubpatterns);
    else
        array = createRegExpMatchesArrayForPlainRegExp(vm, globalObject, string, result, subpatternResults, numSubpatterns);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, result, false);
    OPERATION_RETURN(scope, JSValue::encode(array));
}

static ALWAYS_INLINE void recordMatchFound(JSGlobalObject* globalObject, VM& vm, JSCell* cell, JSCell* subject)
{
    RegExp* regExp = uncheckedDowncast<RegExpObject>(cell)->regExp();
    auto subpatternResults = regExp->ovectorSpan();
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, uncheckedDowncast<JSString>(subject), MatchResult(subpatternResults[0], subpatternResults[1]), false);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedRegExpTestFound, size_t, (Instance* instance, JSCell* cell, JSCell* subject))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    recordMatchFound(globalObject, vm, cell, subject);
    OPERATION_RETURN(scope, true);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedRegExpTestFoundBoxed, EncodedJSValue, (Instance* instance, JSCell* cell, JSCell* subject))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    recordMatchFound(globalObject, vm, cell, subject);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(true)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewRegExpLiteral, JSCell*, (Instance* instance, uint32_t source, uint32_t flags, uint32_t offset))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    RegExp* regExp = instance->typed->regExpLiteral(vm, source, flags);
    if (!regExp->isValid()) [[unlikely]] {
        throwException(globalObject, scope, regExp->errorToThrow(globalObject));
        OPERATION_RETURN(scope, nullptr);
    }
    if (offset != std::numeric_limits<uint32_t>::max() && !*reinterpret_cast<JSCell**>(instance->typedStatics + offset)) {
        RegExpObject* shared = RegExpObject::create(vm, globalObject->regExpStructure(), regExp);
        vm.mutatorFence();
        *reinterpret_cast<JSCell**>(instance->typedStatics + offset) = shared;
        instance->didStoreRootInStatics(offset);
    }
    prepareInlineAllocation<RegExpObject>(instance, Instance::InlineAllocation::RegExpObject, globalObject->regExpStructure());
    OPERATION_RETURN(scope, RegExpObject::create(vm, globalObject->regExpStructure(), regExp));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSharedRegExpLiteral, JSCell*, (Instance* instance, uint32_t offset, uint32_t source, uint32_t flags))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    RegExp* regExp = instance->typed->regExpLiteral(vm, source, flags);
    if (!regExp->isValid()) [[unlikely]] {
        throwException(globalObject, scope, regExp->errorToThrow(globalObject));
        OPERATION_RETURN(scope, nullptr);
    }
    RegExpObject* object = RegExpObject::create(vm, globalObject->regExpStructure(), regExp);
    vm.mutatorFence();
    *reinterpret_cast<JSCell**>(instance->typedStatics + offset) = object;
    instance->didStoreRootInStatics(offset);
    OPERATION_RETURN(scope, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewRegExp, JSCell*, (Instance* instance, JSCell* source, uint32_t flags))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* string = uncheckedDowncast<JSString>(source);
    if (string->isRope() && string->length() > ImageRegExp::minLengthStoredAsImageString && vm.useImmutableIntrinsics) {
        if (RegExp* regExp = RegExpObject::tryCreateRegExpFromLongRope(vm, string, OptionSet<Yarr::Flags>::fromRaw(flags)))
            OPERATION_RETURN(scope, RegExpObject::create(vm, globalObject->regExpStructure(), regExp));
    }
    auto pattern = string->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, regExpCreate(globalObject, JSValue(), pattern, OptionSet<Yarr::Flags>::fromRaw(flags)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedRegExpFlags, JSCell*, (Instance* instance, JSCell* regExp))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, jsString(vm, String { Yarr::flagsString(uncheckedDowncast<RegExpObject>(regExp)->regExp()->flags()).span() }));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedRegExpSource, JSCell*, (Instance* instance, JSCell* regExp))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, jsString(vm, uncheckedDowncast<RegExpObject>(regExp)->regExp()->escapedPattern()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringReplaceString, JSCell*, (Instance* instance, JSCell* stringCell, JSCell* searchCell, JSCell* replacementCell))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSString* string = uncheckedDowncast<JSString>(stringCell);
    JSString* search = uncheckedDowncast<JSString>(searchCell);
    JSString* replacement = uncheckedDowncast<JSString>(replacementCell);
    if (JSString* result = tryReplaceOneCharUsingString<DollarCheck::Yes>(globalObject, string, search, replacement))
        OPERATION_RETURN(scope, result);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto stringView = string->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto searchView = search->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto replacementText = replacement->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, (stringReplaceStringString<StringReplaceSubstitutions::Yes, StringReplaceUseTable::No, BoyerMooreHorspoolTable<uint8_t>>(globalObject, string, stringView, searchView, replacement, replacementText, nullptr)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringReplaceRegExp, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, EncodedJSValue replacement))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* regExpObject = uncheckedDowncast<RegExpObject>(regExp);
    if (!regExpObject->isSymbolReplaceFastAndNonObservable(globalObject)) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    OPERATION_RETURN(scope, replaceUsingRegExpSearch(vm, globalObject, uncheckedDowncast<JSString>(string), regExpObject, JSValue::decode(replacement)));
}

static JSString* tryReplaceEveryAtom(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExpObject* regExpObject, JSString* replacement, const String& replacementText)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    RegExp* regExp = regExpObject->regExp();
    if (!regExp->global() || regExp->sticky() || !regExp->hasValidAtom() || replacementText.isEmpty() || replacementText.find('$') != notFound)
        return nullptr;
    auto source = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (regExp->eitherUnicode() && !source->is8Bit())
        return nullptr;

    regExpObject->setLastIndex(globalObject, 0);
    RETURN_IF_EXCEPTION(scope, nullptr);

    StringView atom { regExp->atom() };
    unsigned atomLength = atom.length();
    unsigned sourceLength = source->length();
    auto findFrom = [&](unsigned start) {
        return atomLength == 1 ? source->find(atom[0], start) : source->find(atom, start);
    };
    size_t found = findFrom(0);
    if (found == notFound)
        return string;

    if (source->is8Bit() && atom.is8Bit() && replacementText.is8Bit()) {
        auto characters = source->span8();
        auto inserted = replacementText.span8();
        Latin1Character first = atom.span8()[0];
        Vector<unsigned, 32> places;
        do {
            if (!places.tryAppend(static_cast<unsigned>(found))) [[unlikely]] {
                throwOutOfMemoryError(globalObject, scope);
                return nullptr;
            }
            found = atomLength == 1 ? WTF::find(characters, first, found + 1) : source->find(atom, found + atomLength);
        } while (found != notFound);

        CheckedUint32 length = places.size();
        length *= inserted.size();
        length += sourceLength - places.size() * atomLength;
        if (length.hasOverflowed() || length.value() > String::MaxLength) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return nullptr;
        }
        std::span<Latin1Character> buffer;
        auto impl = StringImpl::tryCreateUninitialized(length.value(), buffer);
        if (!impl) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return nullptr;
        }
        unsigned copied = 0;
        for (unsigned place : places) {
            StringImpl::copyCharacters(buffer, characters.subspan(copied, place - copied));
            skip(buffer, place - copied);
            StringImpl::copyCharacters(buffer, inserted);
            skip(buffer, inserted.size());
            copied = place + atomLength;
        }
        StringImpl::copyCharacters(buffer, characters.subspan(copied));
        globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, MatchResult { places.last(), places.last() + atomLength }, false);
        return jsString(vm, String { impl.releaseNonNull() });
    }

    Vector<Range<int32_t>, 16> sourceRanges;
    unsigned end = 0;
    size_t last = found;
    do {
        if (!sourceRanges.tryConstructAndAppend(end, found)) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return nullptr;
        }
        last = found;
        end = found + atomLength;
        found = findFrom(end);
    } while (found != notFound);
    int count = sourceRanges.size();
    if (end < sourceLength) {
        if (!sourceRanges.tryConstructAndAppend(end, sourceLength)) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return nullptr;
        }
    }
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, MatchResult { last, last + atomLength }, false);
    RELEASE_AND_RETURN(scope, jsSpliceSubstringsWithSeparator(globalObject, string, source, sourceRanges.span().data(), sourceRanges.size(), replacement, replacementText, count));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringReplaceRegExpString, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, JSCell* replacement))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* regExpObject = uncheckedDowncast<RegExpObject>(regExp);
    if (!regExpObject->isSymbolReplaceFastAndNonObservable(globalObject)) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    CallData callData;
    auto replacementText = uncheckedDowncast<JSString>(replacement)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (JSString* result = tryReplaceEveryAtom(globalObject, vm, uncheckedDowncast<JSString>(string), regExpObject, uncheckedDowncast<JSString>(replacement), replacementText))
        OPERATION_RETURN(scope, result);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, replaceUsingRegExpSearch(vm, globalObject, uncheckedDowncast<JSString>(string), regExpObject, callData, replacementText, replacement));
}

class CharacterReplacer {
public:
    using Steps = Typed::ReplacementSteps;

    CharacterReplacer(const Typed::Data& data, uint32_t countAndForm, const EncodedJSValue* steps, std::span<const Latin1Character> characters)
        : m_data(data)
        , m_steps(steps)
        , m_characters(characters)
        , m_count(countAndForm & Steps::countMask)
        , m_stride(countAndForm & Steps::hasSets ? Steps::wordsWithSet : 1)
    {
        RELEASE_ASSERT(m_count && m_count <= Steps::maxCount);
        static_assert(Steps::maxCount * Steps::wordsWithSet == std::tuple_size_v<decltype(Typed::State::LastReplacement::steps)>);
        for (unsigned step = 0; step < m_count; ++step)
            m_inserted[step] = data.string(static_cast<uint32_t>(steps[step * m_stride]));
        if (m_stride == 1)
            findCharacters();
        else if (m_count == 1 && static_cast<uint64_t>(steps[Steps::wordOfRepeats]) >> Steps::shiftOfNumberOfRanges)
            findRunsOfOneClass();
        else
            findRuns();
    }

    bool findsNothing() const { return m_places.isEmpty(); }
    unsigned lastStep() const { return m_lastStep; }
    unsigned count() const { return m_count; }
    size_t numberOfWords() const { return m_count * m_stride; }
    uint32_t patternOf(unsigned step) const { return static_cast<uint32_t>(m_steps[step * m_stride] >> 32); }

    enum class Outcome : uint8_t { Unchanged, Made, TooLong };
    Outcome replaceBelow(unsigned limit, RefPtr<StringImpl>& made, MatchResult& last) const
    {
        CheckedUint32 length = m_characters.size();
        bool changes = false;
        for (auto& place : m_places) {
            if (place.step < limit) {
                length += m_inserted[place.step].size();
                length -= place.length;
                changes = true;
            }
        }
        if (!changes) {
            for (auto& place : m_places) {
                if (place.step == limit)
                    last = MatchResult { place.index, place.index + place.length };
            }
            return Outcome::Unchanged;
        }
        if (length.hasOverflowed() || length.value() > String::MaxLength) [[unlikely]]
            return Outcome::TooLong;
        std::span<Latin1Character> buffer;
        made = StringImpl::tryCreateUninitialized(length.value(), buffer);
        if (!made) [[unlikely]]
            return Outcome::TooLong;
        Latin1Character* to = buffer.data();
        const Latin1Character* from = m_characters.data();
        size_t copied = 0;
        for (auto& place : m_places) {
            if (place.step > limit)
                continue;
            copy(to, from + copied, place.index - copied);
            to += place.index - copied;
            copied = place.index;
            if (place.step == limit) {
                size_t start = to - buffer.data();
                last = MatchResult { start, start + place.length };
                continue;
            }
            auto inserted = m_inserted[place.step];
            copy(to, inserted.data(), inserted.size());
            to += inserted.size();
            copied += place.length;
        }
        copy(to, from + copied, m_characters.size() - copied);
        return Outcome::Made;
    }

private:
    struct Place {
        uint32_t index;
        uint32_t length;
        uint32_t step;
    };

    static ALWAYS_INLINE void copy(Latin1Character* to, const void* source, size_t length)
    {
        auto* from = static_cast<const uint8_t*>(source);
        auto copyEnds = [&]<typename Word>(Word) ALWAYS_INLINE_LAMBDA {
            Word first = WTF::unalignedLoad<Word>(from);
            Word last = WTF::unalignedLoad<Word>(from + length - sizeof(Word));
            WTF::unalignedStore<Word>(to, first);
            WTF::unalignedStore<Word>(to + length - sizeof(Word), last);
        };
        if (length > 16) {
            memcpy(to, from, length);
            return;
        }
        if (length >= 8)
            copyEnds(uint64_t());
        else if (length >= 4)
            copyEnds(uint32_t());
        else if (length >= 2)
            copyEnds(uint16_t());
        else if (length)
            *to = *from;
    }

    static ALWAYS_INLINE bool has(const uint64_t* set, Latin1Character character)
    {
        return set[character / 64] >> character % 64 & 1;
    }

    ALWAYS_INLINE void didFind(size_t index, size_t length, unsigned step)
    {
        m_places.append(Place { static_cast<uint32_t>(index), static_cast<uint32_t>(length), step });
        m_lastStep = std::max(m_lastStep, step);
    }

    void findCharacters()
    {
        constexpr size_t stride = SIMD::stride<uint8_t>;
        constexpr unsigned bitsPerLane = SIMD::bitsPerLaneInMask<uint8_t>;
        constexpr uint64_t oneBitPerLane = bitsPerLane == 1 ? 0xffffull : 0x1111111111111111ull;
        std::array<Latin1Character, Steps::maxCount> replaced;
        std::array<simde_uint8x16_t, Steps::maxCount> replacedInEveryLane;
        for (unsigned step = 0; step < m_count; ++step) {
            replaced[step] = m_data.string(patternOf(step)).back();
            replacedInEveryLane[step] = SIMD::splat<uint8_t>(replaced[step]);
        }
        auto findIn = [&](const Latin1Character* lanes, size_t index, uint64_t lanesInUse) ALWAYS_INLINE_LAMBDA {
            auto input = SIMD::load(std::bit_cast<const uint8_t*>(lanes));
            auto mask = SIMD::equal(input, replacedInEveryLane[0]);
            for (unsigned step = 1; step < m_count; ++step)
                mask = SIMD::bitOr(mask, SIMD::equal(input, replacedInEveryLane[step]));
            for (uint64_t bits = SIMD::laneMask(mask) & lanesInUse; bits; bits &= bits - 1) {
                unsigned lane = WTF::ctz(bits) / bitsPerLane;
                unsigned step = 0;
                while (replaced[step] != lanes[lane])
                    ++step;
                didFind(index + lane, 1, step);
            }
        };
        size_t size = m_characters.size();
        size_t index = 0;
        for (; index + stride <= size; index += stride)
            findIn(m_characters.data() + index, index, oneBitPerLane);
        if (size_t rest = size - index) {
            std::array<Latin1Character, stride> lanes { };
            copy(lanes.data(), m_characters.data() + index, rest);
            findIn(lanes.data(), index, oneBitPerLane & ((1ull << rest * bitsPerLane) - 1));
        }
    }

    void findRunsOfOneClass()
    {
        constexpr size_t stride = SIMD::stride<uint8_t>;
        constexpr unsigned bitsPerLane = SIMD::bitsPerLaneInMask<uint8_t>;
        constexpr uint64_t oneBitPerLane = bitsPerLane == 1 ? 0xffffull : 0x1111111111111111ull;
        constexpr uint64_t everyLane = bitsPerLane == 1 ? 0xffffull : ~0ull;
        constexpr uint64_t firstLane = (1ull << bitsPerLane) - 1;
        uint64_t form = m_steps[Steps::wordOfRepeats];
        uint64_t bounds = m_steps[Steps::wordOfRanges];
        unsigned numberOfRanges = form >> Steps::shiftOfNumberOfRanges;
        bool repeats = form & Steps::repeats;
        uint64_t flipped = form & Steps::rangesAreOfTheOthers ? everyLane : 0;
        std::array<simde_uint8x16_t, Steps::maxRanges> first;
        std::array<simde_uint8x16_t, Steps::maxRanges> width;
        for (unsigned range = 0; range < numberOfRanges; ++range) {
            uint8_t low = bounds >> range * 16;
            uint8_t high = bounds >> (range * 16 + 8);
            first[range] = SIMD::splat<uint8_t>(low);
            width[range] = SIMD::splat<uint8_t>(static_cast<uint8_t>(high - low));
        }
        size_t start = 0;
        bool isInRun = false;
        auto findIn = [&](const Latin1Character* lanes, size_t index, uint64_t lanesInUse) ALWAYS_INLINE_LAMBDA {
            auto input = SIMD::load(std::bit_cast<const uint8_t*>(lanes));
            auto mask = SIMD::lessThanOrEqual(SIMD::sub(input, first[0]), width[0]);
            for (unsigned range = 1; range < numberOfRanges; ++range)
                mask = SIMD::bitOr(mask, SIMD::lessThanOrEqual(SIMD::sub(input, first[range]), width[range]));
            uint64_t members = (SIMD::laneMask(mask) ^ flipped) & lanesInUse;
            if (!repeats) {
                for (uint64_t bits = members & oneBitPerLane; bits; bits &= bits - 1)
                    didFind(index + WTF::ctz(bits) / bitsPerLane, 1, 0);
                return;
            }
            uint64_t previous = (members << bitsPerLane | (isInRun ? firstLane : 0)) & everyLane;
            for (uint64_t bits = (members ^ previous) & oneBitPerLane; bits; bits &= bits - 1) {
                size_t place = index + WTF::ctz(bits) / bitsPerLane;
                if (isInRun)
                    didFind(start, place - start, 0);
                else
                    start = place;
                isInRun = !isInRun;
            }
        };
        size_t size = m_characters.size();
        size_t index = 0;
        for (; index + stride <= size; index += stride)
            findIn(m_characters.data() + index, index, everyLane);
        if (size_t rest = size - index) {
            std::array<Latin1Character, stride> lanes { };
            copy(lanes.data(), m_characters.data() + index, rest);
            findIn(lanes.data(), index, (1ull << rest * bitsPerLane) - 1);
        }
        if (isInRun)
            didFind(start, size - start, 0);
    }

    void findRuns()
    {
        std::array<std::array<uint64_t, 4>, Steps::maxCount> replacedBy;
        std::array<uint64_t, 4> isReplaced { };
        for (unsigned step = 0; step < m_count; ++step) {
            for (unsigned word = 0; word < 4; ++word) {
                replacedBy[step][word] = static_cast<uint64_t>(m_steps[step * m_stride + Steps::firstWordOfSet + word]) & ~isReplaced[word];
                isReplaced[word] |= replacedBy[step][word];
            }
        }
        size_t size = m_characters.size();
        for (size_t index = 0; index < size; ++index) {
            if (!has(isReplaced.data(), m_characters[index])) [[likely]]
                continue;
            unsigned step = 0;
            while (!has(replacedBy[step].data(), m_characters[index]))
                ++step;
            size_t end = index + 1;
            if (m_steps[step * m_stride + Steps::wordOfRepeats] & Steps::repeats) {
                while (end < size && has(replacedBy[step].data(), m_characters[end]))
                    ++end;
            }
            didFind(index, end - index, step);
            index = end - 1;
        }
    }

    const Typed::Data& m_data;
    const EncodedJSValue* m_steps;
    std::span<const Latin1Character> m_characters;
    unsigned m_count;
    unsigned m_stride;
    unsigned m_lastStep { 0 };
    std::array<std::span<const uint8_t>, Steps::maxCount> m_inserted;
    Vector<Place, 32> m_places;
};

static void reconstructLastReplacement(JSGlobalObject* globalObject, RegExpCachedResult& cachedResult, JSString* original, void* context)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* instance = static_cast<Instance*>(context);
    auto& replacement = instance->typed->lastReplacement;
    auto source = original->view(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    CharacterReplacer replacer(instance->typed->data, replacement.countAndForm, replacement.steps.data(), source->span8());
    RefPtr<StringImpl> made;
    MatchResult last;
    JSString* subject = original;
    switch (replacer.replaceBelow(replacement.lastStep, made, last)) {
    case CharacterReplacer::Outcome::Unchanged:
        break;
    case CharacterReplacer::Outcome::Made:
        subject = jsString(vm, String { made.releaseNonNull() });
        break;
    case CharacterReplacer::Outcome::TooLong:
        throwOutOfMemoryError(globalObject, scope);
        return;
    }
    RegExp* regExp = instance->typed->regExpLiteral(vm, replacer.patternOf(replacement.lastStep), OptionSet<Yarr::Flags> { Yarr::Flags::Global }.toRaw());
    cachedResult.record(vm, globalObject, regExp, subject, last, false);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringReplaceCharacters, JSCell*, (Instance* instance, JSCell* stringCell, uint32_t countAndForm, const EncodedJSValue* steps))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (!globalObject->regExpPrimordialPropertiesWatchpointSet().isStillValid() || !globalObject->stringSymbolReplaceWatchpointSet().isStillValid()) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    JSString* string = uncheckedDowncast<JSString>(stringCell);
    auto source = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (!source->is8Bit())
        OPERATION_RETURN(scope, nullptr);
    CharacterReplacer replacer(instance->typed->data, countAndForm, steps, source->span8());
    if (replacer.findsNothing())
        OPERATION_RETURN(scope, string);
    RefPtr<StringImpl> made;
    MatchResult unused;
    if (replacer.replaceBelow(replacer.count(), made, unused) != CharacterReplacer::Outcome::Made) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    auto& replacement = instance->typed->lastReplacement;
    memcpy(replacement.steps.data(), steps, replacer.numberOfWords() * sizeof(EncodedJSValue));
    replacement.countAndForm = countAndForm;
    replacement.lastStep = replacer.lastStep();
    globalObject->regExpGlobalData().cachedResult().recordToBeReconstructed(vm, globalObject, string, reconstructLastReplacement, instance);
    OPERATION_RETURN(scope, jsString(vm, String { made.releaseNonNull() }));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringReplaceAllString, JSCell*, (Instance* instance, JSCell* stringCell, JSCell* search, JSCell* replacement))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSString* string = uncheckedDowncast<JSString>(stringCell);
    auto stringView = string->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto searchView = uncheckedDowncast<JSString>(search)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, replaceUsingStringSearch<StringReplaceMode::Global>(vm, globalObject, string, stringView, searchView, replacement));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringReplaceAllRegExp, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, EncodedJSValue replacement))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* regExpObject = uncheckedDowncast<RegExpObject>(regExp);
    if (!regExpObject->isSymbolReplaceFastAndNonObservable(globalObject) || !regExpObject->regExp()->global()) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    OPERATION_RETURN(scope, replaceUsingRegExpSearch(vm, globalObject, uncheckedDowncast<JSString>(string), regExpObject, JSValue::decode(replacement)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringSplitString, JSCell*, (Instance* instance, JSCell* string, JSCell* separator, uint32_t limit))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, stringSplitFast(globalObject, uncheckedDowncast<JSString>(string), uncheckedDowncast<JSString>(separator), limit));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringSplitRegExp, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, uint32_t limit))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* regExpObject = uncheckedDowncast<RegExpObject>(regExp);
    if (!regExpObject->isSymbolSplitFastAndNonObservable(globalObject)) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    OPERATION_RETURN(scope, regExpSplitFast(globalObject, regExpObject, uncheckedDowncast<JSString>(string), limit));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringMatchRegExp, EncodedJSValue, (Instance* instance, JSCell* string, JSCell* regExp))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* regExpObject = uncheckedDowncast<RegExpObject>(regExp);
    if (!regExpObject->isSymbolMatchFastAndNonObservable(globalObject)) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(regExpMatchFast(globalObject, regExpObject, uncheckedDowncast<JSString>(string))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringSearchRegExp, EncodedJSValue, (Instance* instance, JSCell* string, JSCell* regExp))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* regExpObject = uncheckedDowncast<RegExpObject>(regExp);
    if (!regExpObject->isSymbolSearchFastAndNonObservable(globalObject)) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    auto* subject = uncheckedDowncast<JSString>(string);
    auto input = subject->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    MatchResult result = matchFromStart(globalObject, vm, regExpObject->regExp(), subject, input);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(result ? jsNumber(result.start) : jsNumber(-1)));
}

namespace Typed {

using namespace B3;

std::optional<uint64_t> regExpLiteralKey(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode != Opcode::CallBuiltin || value.arguments.isEmpty() || value.arguments.size() > 2)
        return std::nullopt;
    auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline || implementation.inlined != InlineBuiltin::RegExpNew)
        return std::nullopt;
    auto textOf = [&](ValueID id) -> const Value* {
        while (function.at(id).opcode == Opcode::Cast && program.typeOf(function, id).primitives & Primitive::String)
            id = function.at(id).child(0);
        return function.at(id).opcode == Opcode::String ? &function.at(id) : nullptr;
    };
    auto* source = textOf(value.arguments[0]);
    if (!source)
        return std::nullopt;
    OptionSet<Yarr::Flags> flags;
    if (value.arguments.size() > 1) {
        auto* text = textOf(value.arguments[1]);
        auto parsed = text ? Yarr::parseFlags(StringView { byteCast<Latin1Character>(program.string(text->string())) }) : std::nullopt;
        if (!parsed)
            return std::nullopt;
        flags = *parsed;
    }
    if (flags.contains(Yarr::Flags::Sticky))
        return std::nullopt;
    return static_cast<uint64_t>(source->string().index()) << 32 | flags.toRaw();
}

bool Lowering::isUnreferenced(ValueID literal, bool isGlobal)
{
    auto isNamed = [&](const Value& value, std::initializer_list<ASCIILiteral> names) {
        auto text = m_program.string(value.string());
        for (ASCIILiteral name : names) {
            if (equalSpans(text, byteCast<uint8_t>(name.span8())))
                return true;
        }
        return false;
    };
    Vector<ValueID, 4> aliases { literal };
    Vector<ValueID, 2> methods;
    auto canReplaceParameter = [&](ValueID id) {
        return id && aliases.contains(id);
    };
    for (unsigned next = 0; next < aliases.size(); ++next) {
        ValueID previous;
        for (ValueID userID : usersOf(aliases[next])) {
            if (userID == std::exchange(previous, userID))
                continue;
            auto& user = m_function.at(userID);
            if (user.child(0) != aliases[next])
                continue;
            if (user.opcode == Opcode::Cast && user.castReason() != CastReason::Coerce && user.castReason() != CastReason::As)
                aliases.append(userID);
            else if (user.opcode == Opcode::GetById && !isGlobal && isNamed(user, { "test"_s, "exec"_s }))
                methods.append(userID);
        }
    }
    Vector<ValueID, 6> used;
    used.appendVector(aliases);
    used.appendVector(methods);
    for (ValueID one : used) {
        for (ValueID userID : usersOf(one)) {
            auto& user = m_function.at(userID);
            unsigned objectUses = 0;
            unsigned methodUses = 0;
            user.forEachChild([&](ValueID child) {
                objectUses += canReplaceParameter(child);
                methodUses += methods.contains(child);
            });
            if (!objectUses && !methodUses)
                continue;
            if (aliases.contains(userID) || methods.contains(userID))
                continue;
            bool isReceiver = objectUses == 1 && canReplaceParameter(user.child(0));
            bool isFirstArgument = objectUses == 1 && !user.arguments.isEmpty() && canReplaceParameter(user.arguments[0]);
            switch (user.opcode) {
            case Opcode::CallMethod:
                if (!methodUses && isReceiver && !isGlobal && isNamed(user, { "test"_s, "exec"_s }))
                    continue;
                return false;
            case Opcode::Call:
                if (methodUses == 1 && methods.contains(user.child(0)) && objectUses == 1 && canReplaceParameter(user.child(1)))
                    continue;
                return false;
            case Opcode::CallBuiltin: {
                auto& implementation = m_builtins.at(user.builtin()).implementation;
                if (implementation.kind != BuiltinImplementation::Inline)
                    return false;
                switch (implementation.inlined) {
                case InlineBuiltin::RegExpTest:
                case InlineBuiltin::RegExpExec:
                    if (isReceiver && !isGlobal && (!methodUses || (methodUses == 1 && methods.contains(user.methodGetById()))))
                        continue;
                    return false;
                case InlineBuiltin::StringReplace:
                case InlineBuiltin::StringReplaceAll:
                case InlineBuiltin::StringSplit:
                case InlineBuiltin::StringMatch:
                case InlineBuiltin::StringSearch:
                    if (!methodUses && isFirstArgument && !typeOf(user.child(0)).hasObjects())
                        continue;
                    return false;
                default:
                    return false;
                }
            }
            default:
                return false;
            }
        }
        if (isUsedByTerminator(one))
            return false;
    }
    return true;
}

static bool isRegExp(const Type& type)
{
    auto* engineClass = type.singleEngineClass();
    return engineClass && engineClass->type == RegExpObjectType && !type.primitives;
}

static bool isLimit(const Type& type)
{
    return type.isInteger() && type.integers.isSubsetOf({ 0, UINT32_MAX });
}

std::optional<Writes> writesOfInlineRegExp(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    case InlineBuiltin::RegExpExec:
    case InlineBuiltin::RegExpTest:
        return writesIf(arguments.size() == 1 && arguments[0].isString(), Writes::RuntimeState);
    case InlineBuiltin::RegExpNew:
        return writesIf(!arguments.empty() && arguments.size() <= 2 && arguments[0].isString() && arguments.back().isString(), Writes::RuntimeState);
    case InlineBuiltin::RegExpFlag:
        return writesIf(arguments.empty(), Writes::Nothing);
    case InlineBuiltin::RegExpFlags:
    case InlineBuiltin::RegExpSource:
        return writesIf(arguments.empty(), Writes::RuntimeState);
    case InlineBuiltin::StringReplace:
    case InlineBuiltin::StringReplaceAll:
        if (arguments.size() != 2)
            return std::nullopt;
        if (isRegExp(arguments[0]))
            return arguments[1].hasObjects() ? Writes::Heap : Writes::RuntimeState;
        return writesIf(arguments[0].isString() && arguments[1].isString(), Writes::RuntimeState);
    case InlineBuiltin::StringSplit:
        return writesIf(!arguments.empty() && arguments.size() <= 2 && (isRegExp(arguments[0]) || arguments[0].isString()) && (arguments.size() == 1 || isLimit(arguments[1])), Writes::RuntimeState);
    case InlineBuiltin::StringMatch:
    case InlineBuiltin::StringSearch:
        return writesIf(arguments.size() == 1 && isRegExp(arguments[0]), Writes::RuntimeState);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool Lowering::isNamedLastIndex(StringID name) const
{
    return equalSpans(m_program.string(name), asBytes("lastIndex"_s.span8()));
}

CellPointer Lowering::regExpObjectOrElse(Boxed value, LBasicBlock otherwise)
{
    orElse(isCell(value), otherwise);
    CellPointer cell = asCell(value);
    orElse(equal(loadCellType(cell), word32(RegExpObjectType)), otherwise);
    return cell;
}

Boxed Lowering::getLastIndex(Boxed base, StringID name, bool isCalled)
{
    LBasicBlock otherwise = newColdBlock();
    Join join(m_out, Rep::JSValue, Int64);
    CellPointer regExp = regExpObjectOrElse(base, otherwise);
    join.add(Boxed(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, regExp.value(), RegExpObject::offsetOfLastIndex()))));
    m_out.appendTo(otherwise);
    join.add(emitCachedGetById(base, name, isCalled));
    return join.finish().as<Boxed>();
}

void Lowering::putLastIndex(Boxed base, StringID name, Boxed value)
{
    LBasicBlock otherwise = newColdBlock();
    LBasicBlock storesCell = newUnlikelyBlock();
    LBasicBlock continuation = m_out.newBlock();
    CellPointer regExp = regExpObjectOrElse(base, otherwise);
    LValue regExpAndFlags = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, regExp.value(), RegExpObject::offsetOfRegExpAndFlags()));
    orElse(Word32(m_out.testIsZero64(regExpAndFlags, m_out.constInt64(RegExpObject::lastIndexIsNotWritableFlag))), otherwise);
    didEmitWrite(Writes::RuntimeState);
    m_out.store64(value.value(), m_out.address(m_heaps.AOTTyped_runtimeState, regExp.value(), RegExpObject::offsetOfLastIndex()));
    m_out.branch(isCell(value).value(), rarely(storesCell), usually(continuation));
    m_out.appendTo(storesCell);
    barrier(regExp);
    m_out.jump(continuation);
    m_out.appendTo(otherwise);
    emitCachedPutById(base, name, value);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

std::optional<Operand> Lowering::lowerMatchOnlyTakenApart(ValueID result, const Value& value)
{
    auto taken = m_matchesOnlyTakenApart.find(result.index());
    if (taken == m_matchesOnlyTakenApart.end())
        return std::nullopt;
    if (!isOnMainPath()) {
        m_matchesOnlyTakenApart.remove(taken);
        return std::nullopt;
    }
    auto& builtin = m_builtins.at(value.builtin());
    bool isExec = builtin.implementation.inlined == InlineBuiltin::RegExpExec;
    CellPointer receiver = nonNullish(value.child(0), m_program.stringOf(builtin.nameName));
    CellPointer regExpObject = isExec ? receiver : in<CellPointer>(value.arguments[0]);
    CellPointer string = isExec ? toStringCell(value.arguments[0]) : receiver;
    Word32 isFound = AOT_TYPED_OPERATION(RegExpFind, regExpObject, string);
    LValue regExp = m_out.bitAnd(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, regExpObject.value(), RegExpObject::offsetOfRegExpAndFlags())), m_out.constInt64(RegExpObject::regExpMask));
    LValue offsets = m_out.loadPtr(m_out.address(m_heaps.AOTTyped_runtimeState, regExp, RegExp::offsetOfOvector() + FixedVector<int>::offsetOfStorage()));
    auto match = makeUniqueWithoutFastMallocCheck<MatchInFrame>();
    match->subject = stringReceiver(string);
    if (!isExec || typeOf(value.arguments[0]).isString())
        match->subject.id = isExec ? value.arguments[0] : value.child(0);
    match->numberOfGroups = zeroExtended(Word32(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, regExp, RegExp::offsetOfNumSubpatterns()))));
    for (unsigned group = 0; group < taken->value; ++group) {
        Integer index = group ? select(lessThanOrEqual(integer(group), match->numberOfGroups), integer(group), integer(0)) : integer(0);
        LValue pair = m_out.add(offsets, m_out.shl(index.value(), m_out.constInt32(3)));
        match->starts.append(signExtended(Word32(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, pair, FixedVector<int>::Storage::offsetOfData())))));
        match->ends.append(signExtended(Word32(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, pair, FixedVector<int>::Storage::offsetOfData() + sizeof(int))))));
    }
    ensureStillAliveHere(regExpObject);
    m_matchesInFrame.set(result.index(), WTF::move(match));
    return convert(select(isFound, box(regExpObject), boxedConstant(jsNull())), typeOf(result), value.rep);
}

struct OneCharacterPattern {
    std::array<uint64_t, 4> set { };
    bool repeats { false };
};

static std::optional<OneCharacterPattern> parseOneCharacterPattern(std::span<const uint8_t> text)
{
    using Set = std::array<uint64_t, 4>;
    auto add = [](Set& set, unsigned first, unsigned last) {
        for (unsigned character = first; character <= last; ++character)
            set[character / 64] |= 1ull << character % 64;
    };
    auto invert = [](Set& set) {
        for (auto& word : set)
            word = ~word;
    };
    auto isSyntaxCharacter = [](uint8_t character) {
        return character && strchr("^$\\.*+?()[]{}|/", character);
    };
    size_t position = 0;
    auto parseEscape = [&](Set& set, bool isInClass, std::optional<uint8_t>& single) {
        if (position == text.size())
            return false;
        uint8_t escaped = text[position++];
        Set named { };
        switch (escaped) {
        case 'd':
        case 'D':
            add(named, '0', '9');
            break;
        case 'w':
        case 'W':
            add(named, '0', '9');
            add(named, 'A', 'Z');
            add(named, 'a', 'z');
            add(named, '_', '_');
            break;
        case 's':
        case 'S':
            add(named, '\t', '\r');
            add(named, ' ', ' ');
            add(named, 0xa0, 0xa0);
            break;
        case 'n':
            single = '\n';
            break;
        case 'r':
            single = '\r';
            break;
        case 't':
            single = '\t';
            break;
        case 'f':
            single = '\f';
            break;
        case 'v':
            single = '\v';
            break;
        default:
            if (!isSyntaxCharacter(escaped) && !(isInClass && escaped == '-'))
                return false;
            single = escaped;
            break;
        }
        if (single) {
            add(set, *single, *single);
            return true;
        }
        if (isASCIIUpper(escaped))
            invert(named);
        for (unsigned word = 0; word < 4; ++word)
            set[word] |= named[word];
        return true;
    };

    OneCharacterPattern pattern;
    if (text.empty())
        return std::nullopt;
    uint8_t first = text[position++];
    std::optional<uint8_t> single;
    if (first == '\\') {
        if (!parseEscape(pattern.set, false, single))
            return std::nullopt;
    } else if (first == '.') {
        add(pattern.set, '\n', '\n');
        add(pattern.set, '\r', '\r');
        invert(pattern.set);
    } else if (first == '[') {
        bool isInverted = position < text.size() && text[position] == '^';
        position += isInverted;
        bool isClosed = false;
        while (position < text.size()) {
            uint8_t character = text[position++];
            if (character == ']') {
                isClosed = true;
                break;
            }
            single = std::nullopt;
            if (character == '\\') {
                if (!parseEscape(pattern.set, true, single))
                    return std::nullopt;
            } else if (character == '-' || character == '[')
                return std::nullopt;
            else {
                single = character;
                add(pattern.set, character, character);
            }
            if (position + 1 >= text.size() || text[position] != '-')
                continue;
            uint8_t last = text[position + 1];
            if (!single || !isASCIIAlphanumeric(*single) || !isASCIIAlphanumeric(last) || last < *single)
                return std::nullopt;
            add(pattern.set, *single, last);
            position += 2;
        }
        if (!isClosed)
            return std::nullopt;
        if (isInverted)
            invert(pattern.set);
    } else if (isSyntaxCharacter(first))
        return std::nullopt;
    else
        add(pattern.set, first, first);
    if (position < text.size() && text[position] == '+') {
        pattern.repeats = true;
        ++position;
    }
    if (position != text.size())
        return std::nullopt;
    return pattern;
}

std::optional<Lowering::CharacterReplacement> Lowering::characterReplacementBy(ValueID id)
{
    auto& value = m_function.at(id);
    if (value.opcode != Opcode::CallBuiltin || !value.child(0) || value.arguments.size() != 2 || value.methodGetById() || typeOf(value.child(0)).hasObjects())
        return std::nullopt;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline || implementation.inlined != InlineBuiltin::StringReplace)
        return std::nullopt;
    if (!typeOf(value.arguments[1]).isString() || useCountOf(value.arguments[0]) != 1)
        return std::nullopt;
    auto& literal = m_function.at(value.arguments[0]);
    if (literal.opcode != Opcode::CallBuiltin || literal.arguments.size() != 2)
        return std::nullopt;
    OptionSet<Yarr::Flags> flags { Yarr::Flags::Global };
    auto key = regExpLiteralKey(m_program, m_function, literal);
    if (!key || static_cast<uint32_t>(*key) != flags.toRaw())
        return std::nullopt;
    auto text = constantASCIIString(literal.arguments[0]);
    auto inserted = constantASCIIString(value.arguments[1]);
    if (!text || !inserted || !stringConstantOf(value.arguments[1]) || std::ranges::find(*inserted, '$') != inserted->end())
        return std::nullopt;

    auto pattern = parseOneCharacterPattern(*text);
    if (!pattern)
        return std::nullopt;
    CharacterReplacement replacement { pattern->set, pattern->repeats, false, *inserted };
    uint8_t last = text->back();
    std::array<uint64_t, 4> onlyLast { };
    onlyLast[last / 64] = 1ull << last % 64;
    replacement.isLastOfPattern = !pattern->repeats && pattern->set == onlyLast;
    return replacement;
}

void Lowering::findReplacementChains()
{
    for (auto& block : m_function.blocks) {
        ReplacementChain chain;
        size_t indexOfLast = 0;
        auto close = [&] {
            if (chain.size() > 1 || (chain.size() == 1 && !characterReplacementBy(chain[0])->isLastOfPattern)) {
                for (ValueID step : chain) {
                    if (step != chain.last()) {
                        m_isDoneByLastReplacement.set(step.index());
                        m_isLazilyFetched.set(step.index());
                        for (ValueID user : usersOf(step)) {
                            if (m_function.at(user).opcode == Opcode::Cast) {
                                m_isDoneByLastReplacement.set(user.index());
                                m_isLazilyFetched.set(user.index());
                            }
                        }
                    }
                    m_isDoneByLastReplacement.set(m_function.at(step).arguments[0].index());
                    m_isLazilyFetched.set(m_function.at(step).arguments[0].index());
                }
                m_replacementChains.add(chain.last().index(), chain);
            }
            chain.clear();
        };
        for (size_t index = 0; index < block.values.size(); ++index) {
            ValueID id = block.values[index];
            auto& value = m_function.at(id);
            auto replacement = value.opcode == Opcode::CallBuiltin ? characterReplacementBy(id) : std::nullopt;
            if (!replacement)
                continue;
            bool follows = !chain.isEmpty() && value.child(0) == chain.last() && chain.size() < maxReplacementsInOnePass;
            if (follows) {
                for (ValueID user : usersOf(chain.last()))
                    follows &= user == id || (m_function.at(user).opcode == Opcode::Cast && !useCountOf(user));
                for (size_t between = indexOfLast + 1; between < index; ++between) {
                    ValueID other = block.values[between];
                    follows &= m_function.at(other).opcode == Opcode::String || other == value.arguments[0] || (m_function.at(other).opcode == Opcode::Cast && m_function.at(other).child(0) == chain.last());
                }
                for (ValueID earlier : chain) {
                    auto inserted = characterReplacementBy(earlier)->inserted;
                    follows &= !replacement->repeats || !inserted.empty();
                    for (uint8_t character : inserted)
                        follows &= !(replacement->set[character / 64] >> character % 64 & 1);
                }
            }
            if (!follows)
                close();
            chain.append(id);
            indexOfLast = index;
        }
        close();
    }
}

std::optional<Operand> Lowering::tryLowerReplacementChain(ValueID result, const Value& value)
{
    if (m_isLoweringReplacementsOneByOne)
        return std::nullopt;
    auto found = m_replacementChains.find(result.index());
    if (found == m_replacementChains.end())
        return std::nullopt;
    ReplacementChain chain = found->value;
    auto& type = typeOf(result);
    Rep rep = repOf(type);
    CellPointer receiver = nonNullish(m_function.at(chain[0]).child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    bool hasSets = false;
    for (ValueID step : chain)
        hasSets |= !characterReplacementBy(step)->isLastOfPattern;
    unsigned stride = hasSets ? ReplacementSteps::wordsWithSet : 1;
    LValue steps = transientBuffer(chain.size() * stride * sizeof(EncodedJSValue));
    auto store = [&](unsigned word, uint64_t bits) {
        m_out.store64(integer(static_cast<int64_t>(bits)).value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, steps, word * sizeof(EncodedJSValue)));
    };
    for (unsigned i = 0; i < chain.size(); ++i) {
        auto& step = m_function.at(chain[i]);
        uint64_t pattern = m_function.at(stringConstantOf(m_function.at(step.arguments[0]).arguments[0])).string().index();
        uint64_t inserted = m_function.at(stringConstantOf(step.arguments[1])).string().index();
        store(i * stride, pattern << 32 | inserted);
        if (!hasSets)
            continue;
        auto replacement = *characterReplacementBy(chain[i]);
        uint64_t form = replacement.repeats ? ReplacementSteps::repeats : 0;
        uint64_t bounds = 0;
        for (bool ofTheOthers : { false, true }) {
            Vector<std::pair<unsigned, unsigned>, 8> ranges;
            for (unsigned character = 0; character < 256; ++character) {
                if (static_cast<bool>(replacement.set[character / 64] >> character % 64 & 1) == ofTheOthers)
                    continue;
                if (!ranges.isEmpty() && ranges.last().second + 1 == character)
                    ranges.last().second = character;
                else
                    ranges.append({ character, character });
            }
            if (ranges.isEmpty() || ranges.size() > ReplacementSteps::maxRanges)
                continue;
            form |= static_cast<uint64_t>(ranges.size()) << ReplacementSteps::shiftOfNumberOfRanges | (ofTheOthers ? ReplacementSteps::rangesAreOfTheOthers : 0);
            for (unsigned range = 0; range < ranges.size(); ++range)
                bounds |= static_cast<uint64_t>(ranges[range].first | ranges[range].second << 8) << range * 16;
            break;
        }
        store(i * stride + ReplacementSteps::wordOfRepeats, form);
        store(i * stride + ReplacementSteps::wordOfRanges, bounds);
        for (unsigned word = 0; word < 4; ++word)
            store(i * stride + ReplacementSteps::firstWordOfSet + word, replacement.set[word]);
    }
    CellPointer replaced = AOT_TYPED_OPERATION(StringReplaceCharacters, receiver, word32(chain.size() | (hasSets ? ReplacementSteps::hasSets : 0)), Pointer(steps));
    LBasicBlock oneByOne = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    orElse(Word32(m_out.notZero64(replaced.value())), oneByOne);
    join.add(ensure(replaced, m_program.at(m_program.types.string), type));
    m_out.appendTo(oneByOne);
    SetForScope isLoweringOneByOne(m_isLoweringReplacementsOneByOne, true);
    SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
    size_t numberOfAssumptions = m_assumptions.size();
    Operand last;
    for (ValueID step : chain) {
        ValueID pattern = m_function.at(step).arguments[0];
        m_assumptions.append(Assumption { pattern, lowerCallBuiltin(pattern, m_function.at(pattern)), &typeOf(pattern) });
        last = lowerCallBuiltin(step, m_function.at(step));
        m_assumptions.append(Assumption { step, last, &typeOf(step) });
    }
    m_assumptions.shrink(numberOfAssumptions);
    join.add(last);
    return join.finish();
}

std::optional<Operand> Lowering::lowerInlineRegExp(InlineBuiltin which, ValueID result, const Value& value, Writes writes)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    auto& builtin = m_builtins.at(value.builtin());
    auto& stringType = m_program.at(m_program.types.string);
    OperationEffects effects { writes, true };
    auto genericPath = [&] {
        return ensure(callBuiltinFunction(value, effects), type);
    };
    auto orGeneric = [&](Operand yielded, const Type& guarantee) {
        Rep rep = repOf(type);
        LBasicBlock callsFunction = newBlockForGenericCall(value);
        Join join(m_out, rep, b3TypeOf(rep));
        orElse(Word32(m_out.notZero64(yielded.value)), callsFunction);
        join.add(ensure(yielded, guarantee, type));
        if (!isSharedGenericPath(callsFunction)) {
            m_out.appendTo(callsFunction);
            join.add(genericPath());
        }
        return join.finish();
    };

    if (which == InlineBuiltin::RegExpNew) {
        Type regExpClass = Type::engineClass(m_builtins.classes()[builtin.result.engineClass]);
        OptionSet<Yarr::Flags> flags;
        if (arguments.size() > 1) {
            auto text = constantASCIIString(arguments[1]);
            auto parsed = text ? Yarr::parseFlags(StringView { byteCast<Latin1Character>(*text) }) : std::nullopt;
            if (!parsed)
                return genericPath();
            flags = *parsed;
        }
        Word32 rawFlags = word32(flags.toRaw());
        if (ValueID constant = stringConstantOf(arguments[0])) {
            auto& source = m_function.at(constant);
            int32_t wordIndex = -1;
            if (auto key = regExpLiteralKey(m_program, m_function, value)) {
                if (auto place = m_program.sharedRegExpObjectOffsets.find(*key); place != m_program.sharedRegExpObjectOffsets.end()) {
                    wordIndex = place->value;
                    if (isUnreferenced(result, flags.contains(Yarr::Flags::Global))) {
                        LValue shared = m_out.load64(inStatics(m_heaps.AOTTyped_collectorState, place->value));
                        return ensure(either<CellPointer>(Word32(m_out.notZero64(shared)), [&] {
                            return CellPointer(shared);
                        }, [&] {
                            return AOT_TYPED_OPERATION(SharedRegExpLiteral, word32(place->value), word32(source.string().index()), rawFlags);
                        }), regExpClass, type);
                    }
                }
            }
            return ensure(AOT_TYPED_OPERATION(NewRegExpLiteral, word32(source.string().index()), rawFlags, word32(wordIndex)), regExpClass, type);
        }
        return ensure(AOT_TYPED_OPERATION(NewRegExp, in<CellPointer>(arguments[0]), rawFlags), regExpClass, type);
    }

    CellPointer receiver = nonNullish(value.child(0), m_program.stringOf(builtin.nameName));
    auto limit = [&] {
        return arguments.size() > 1 ? low32(in<Integer>(arguments[1])) : word32(-1);
    };
    switch (which) {
    case InlineBuiltin::RegExpExec:
        return ensure(AOT_TYPED_OPERATION(RegExpExec, receiver, in<CellPointer>(arguments[0])), type);
    case InlineBuiltin::RegExpTest:
        return booleanResult(AOT_TYPED_OPERATION(RegExpTest, receiver, in<CellPointer>(arguments[0])), type);
    case InlineBuiltin::RegExpFlag: {
        std::optional<Yarr::Flags> flag;
#define AOT_TYPED_FIND_REGEXP_FLAG(key, capitalizedName, lowerCaseName, index) \
        if (builtin.name == ASCIILiteral::fromLiteralUnsafe(#lowerCaseName)) \
            flag = Yarr::Flags::capitalizedName;
        JSC_REGEXP_FLAGS(AOT_TYPED_FIND_REGEXP_FLAG)
#undef AOT_TYPED_FIND_REGEXP_FLAG
        RELEASE_ASSERT(flag);
        static_assert(sizeof(OptionSet<Yarr::Flags>) == sizeof(uint16_t));
        LValue regExp = m_out.bitAnd(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, receiver.value(), RegExpObject::offsetOfRegExpAndFlags())), m_out.constInt64(RegExpObject::regExpMask));
        LValue flags = m_out.load16ZeroExt32(m_out.address(m_heaps.AOTTyped_runtimeState, regExp, RegExp::offsetOfFlags()));
        Word32 isSet = hasBits(Word32(flags), static_cast<uint32_t>(*flag));
        ensureStillAliveHere(receiver);
        return booleanResult(isSet, type);
    }
    case InlineBuiltin::RegExpFlags:
        return ensure(AOT_TYPED_OPERATION(RegExpFlags, receiver), stringType, type);
    case InlineBuiltin::RegExpSource:
        return ensure(AOT_TYPED_OPERATION(RegExpSource, receiver), stringType, type);
    case InlineBuiltin::StringReplace:
        if (isRegExp(typeOf(arguments[0])) && typeOf(arguments[1]).isString())
            return orGeneric(AOT_TYPED_OPERATION(StringReplaceRegExpString, receiver, in<CellPointer>(arguments[0]), in<CellPointer>(arguments[1])), stringType);
        if (isRegExp(typeOf(arguments[0])))
            return orGeneric(AOT_TYPED_OPERATION_WITH_EFFECTS(StringReplaceRegExp, effects, receiver, in<CellPointer>(arguments[0]), in<Boxed>(arguments[1])), stringType);
        return ensure(AOT_TYPED_OPERATION(StringReplaceString, receiver, in<CellPointer>(arguments[0]), in<CellPointer>(arguments[1])), stringType, type);
    case InlineBuiltin::StringReplaceAll:
        if (isRegExp(typeOf(arguments[0])))
            return orGeneric(AOT_TYPED_OPERATION_WITH_EFFECTS(StringReplaceAllRegExp, effects, receiver, in<CellPointer>(arguments[0]), in<Boxed>(arguments[1])), stringType);
        return ensure(AOT_TYPED_OPERATION(StringReplaceAllString, receiver, in<CellPointer>(arguments[0]), in<CellPointer>(arguments[1])), stringType, type);
    case InlineBuiltin::StringSplit: {
        Type arrayClass = Type::engineClass(m_builtins.arrayClass());
        if (isRegExp(typeOf(arguments[0])))
            return orGeneric(AOT_TYPED_OPERATION(StringSplitRegExp, receiver, in<CellPointer>(arguments[0]), limit()), arrayClass);
        if (auto takenApart = lowerSplitOnlyTakenApart(result, value, receiver))
            return takenApart;
        return ensure(AOT_TYPED_OPERATION(StringSplitString, receiver, in<CellPointer>(arguments[0]), limit()), arrayClass, type);
    }
    case InlineBuiltin::StringMatch:
        return orGeneric(AOT_TYPED_OPERATION(StringMatchRegExp, receiver, in<CellPointer>(arguments[0])), m_program.at(m_program.types.anything));
    case InlineBuiltin::StringSearch:
        return orGeneric(AOT_TYPED_OPERATION(StringSearchRegExp, receiver, in<CellPointer>(arguments[0])), m_program.at(m_program.types.anything));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
