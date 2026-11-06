/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeHelpers.h"
#include "AOTPlainURL.h"
#include "IterationKind.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSString.h"
#include "KeyAtomStringCacheInlines.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/ASCIICType.h>
#include <wtf/IterationStatus.h>


namespace JSC { namespace AOT {

static std::span<const Latin1Character> flatLatin1Span(JSValue value)
{
    if (!value.isString())
        return { };
    const StringImpl* impl = asString(value)->tryGetValueImpl();
    if (!impl || !impl->is8Bit())
        return { };
    return impl->span8();
}

static bool isCanonicalIPv4(std::span<const Latin1Character> host)
{
    unsigned parts = 0;
    size_t at = 0;
    while (at < host.size()) {
        unsigned value = 0;
        unsigned digits = 0;
        for (; at < host.size() && host[at] != '.'; ++at) {
            if (!isASCIIDigit(host[at]) || (digits == 1 && !value))
                return false;
            value = value * 10 + (host[at] - '0');
            if (++digits > 3)
                return false;
        }
        if (!digits || value > 255)
            return false;
        ++parts;
        if (at < host.size() && ++at == host.size())
            return false;
    }
    return parts == 4;
}

static std::optional<size_t> findEndOfHostAndPort(std::span<const Latin1Character> text, size_t start)
{
    size_t at = start;
    size_t label = start;
    for (; at < text.size(); ++at) {
        Latin1Character character = text[at];
        if (isASCIILower(character) || isASCIIDigit(character))
            continue;
        if (character == '-') {
            if (at == label || (at - label == 3 && text[at - 1] == '-'))
                return std::nullopt;
            continue;
        }
        if (character != '.')
            break;
        if (at == label || text[at - 1] == '-')
            return std::nullopt;
        label = at + 1;
    }
    if (at == label || text[at - 1] == '-')
        return std::nullopt;
    if (isASCIIDigit(text[label]) && !isCanonicalIPv4(text.subspan(start, at - start)))
        return std::nullopt;
    if (at < text.size() && text[at] == ':') {
        unsigned port = 0;
        size_t digits = ++at;
        for (; at < text.size() && isASCIIDigit(text[at]); ++at) {
            port = port * 10 + (text[at] - '0');
            if (port > 65535)
                return std::nullopt;
        }
        if (at == digits)
            return std::nullopt;
    }
    return at;
}

static std::optional<size_t> findStartOfHost(std::span<const Latin1Character> text)
{
    if (text.size() < 8 || text[0] != 'h' || text[1] != 't' || text[2] != 't' || text[3] != 'p')
        return std::nullopt;
    size_t colon = text[4] == 's' ? 5 : 4;
    if (text[colon] != ':' || text[colon + 1] != '/' || text[colon + 2] != '/')
        return std::nullopt;
    return colon + 3;
}

static std::optional<size_t> findStartOfPath(std::span<const Latin1Character> text)
{
    auto host = findStartOfHost(text);
    if (!host)
        return std::nullopt;
    auto at = findEndOfHostAndPort(text, *host);
    if (!at || *at >= text.size() || text[*at] != '/')
        return std::nullopt;
    return at;
}

bool isPlainBaseURL(std::span<const Latin1Character> text)
{
    auto host = findStartOfHost(text);
    if (!host)
        return false;
    auto at = findEndOfHostAndPort(text, *host);
    return at && (*at == text.size() || text[*at] == '/' || text[*at] == '?' || text[*at] == '#');
}

enum URLCharacterClass : uint8_t {
    AllowedInPath = 1,
    AllowedInQuery = 2,
    IsDotOrPercent = 4,
    IsQueryDelimiterOrEscape = 8,
};

static constexpr std::array<uint8_t, 256> urlCharacterClasses = [] {
    std::array<uint8_t, 256> classes { };
    auto isIn = [](unsigned character, uint32_t below64) {
        uint32_t mask = character < 64 ? (character < 32 ? 0 : below64) : character < 96 ? 0x87ffffffu : character < 128 ? 0x47fffffeu : 0;
        return mask >> (character & 31) & 1;
    };
    for (unsigned character = 0; character < 256; ++character) {
        if (isIn(character, 0x2fffff72u))
            classes[character] |= AllowedInPath;
        if (isIn(character, 0xafffff72u))
            classes[character] |= AllowedInQuery;
    }
    classes['.'] |= IsDotOrPercent;
    classes['%'] |= IsDotOrPercent | IsQueryDelimiterOrEscape;
    classes['+'] |= IsQueryDelimiterOrEscape;
    classes['&'] |= IsQueryDelimiterOrEscape;
    classes['='] |= IsQueryDelimiterOrEscape;
    return classes;
}();

static int32_t scanPathAndQuery(std::span<const Latin1Character> text, std::optional<size_t> path)
{
    if (!path || *path > Typed::PlainURL::maxStartOfPath || text.size() > Typed::PlainURL::maxOffset)
        return Typed::PlainURL::notPlain;
    size_t at = *path + 1;
    for (; at < text.size(); ++at) {
        uint8_t classes = urlCharacterClasses[text[at]];
        if (!(classes & AllowedInPath))
            break;
        if (classes & IsDotOrPercent && text[at - 1] == '/')
            return Typed::PlainURL::notPlain;
    }
    size_t query = at;
    if (at < text.size() && text[at] == '?') {
        for (++at; at < text.size() && urlCharacterClasses[text[at]] & AllowedInQuery; ++at) { }
    }
    if (at < text.size() && text[at] != '#')
        return Typed::PlainURL::notPlain;
    return Typed::PlainURL::packOffsets(*path, query, at);
}

static int32_t scanWithPlainBase(std::span<const Latin1Character> text)
{
    if (text.empty() || text[0] != '/')
        return scanPathAndQuery(text, findStartOfPath(text));
    if (text.size() > 1 && text[1] == '/')
        return Typed::PlainURL::notPlain;
    return scanPathAndQuery(text, 0);
}

static int32_t findQueryValue(std::span<const Latin1Character> text, int32_t offsets, JSValue nameValue)
{
    auto name = flatLatin1Span(nameValue);
    if (offsets < 0 || !nameValue.isString() || (name.empty() && asString(nameValue)->length()))
        return Typed::PlainURL::needsObject;
    size_t end = Typed::PlainURL::endOfQuery(offsets);
    size_t at = Typed::PlainURL::endOfPath(offsets) + 1;
    if (end > text.size())
        return Typed::PlainURL::needsObject;
    while (at < end) {
        size_t stop = at;
        for (; stop < end && !(urlCharacterClasses[text[stop]] & IsQueryDelimiterOrEscape); ++stop) { }
        if (stop < end && text[stop] != '&' && text[stop] != '=')
            return Typed::PlainURL::needsObject;
        bool hasValue = stop < end && text[stop] == '=';
        bool matchesName = (hasValue || stop != at) && stop - at == name.size() && equalSpans(text.subspan(at, stop - at), name);
        size_t value = stop;
        if (hasValue) {
            for (value = ++stop; stop < end && text[stop] != '&'; ++stop) {
                if (matchesName && (text[stop] == '%' || text[stop] == '+'))
                    return Typed::PlainURL::needsObject;
            }
        }
        if (matchesName)
            return Typed::PlainURL::packRange(value, stop);
        at = stop + 1;
    }
    return Typed::PlainURL::absent;
}

static constexpr std::array<bool, 256> isUnchangedByFormEncoding = [] {
    std::array<bool, 256> result { };
    for (unsigned character = 0; character < 128; ++character)
        result[character] = isASCIIAlphanumeric(static_cast<char>(character)) || character == '*' || character == '-' || character == '.' || character == '_';
    return result;
}();

template<typename Functor>
static std::optional<unsigned> forEachPlainQueryPair(std::span<const Latin1Character> text, const Functor& functor)
{
    if (text.size() > Typed::PlainURL::maxOffset)
        return std::nullopt;
    size_t at = !text.empty() && text[0] == '?';
    if (at == text.size())
        return 0;
    for (unsigned count = 1;; ++count) {
        size_t name = at;
        for (; at < text.size() && isUnchangedByFormEncoding[text[at]]; ++at) { }
        if (at == text.size() || text[at] != '=')
            return std::nullopt;
        size_t value = ++at;
        for (; at < text.size() && isUnchangedByFormEncoding[text[at]]; ++at) { }
        if (functor(text.subspan(name, value - 1 - name), value, at) == IterationStatus::Done || at == text.size())
            return count;
        if (text[at] != '&' || ++at == text.size())
            return std::nullopt;
    }
}

using NarrowCopy = Vector<Latin1Character, 128>;

// The width is that of the storage now: what was 8-bit at one call may be 16-bit at the next, and its Latin-1 characters are copied then.
static std::optional<std::span<const Latin1Character>> latin1SpanWithoutResolving(JSGlobalObject* globalObject, JSValue value, NarrowCopy& copy)
{
    if (!value.isString() || asString(value)->isNonSubstringRope())
        return std::nullopt;
    auto view = asString(value)->view(globalObject);
    if (view->is8Bit())
        return view->span8();
    if (view->length() > Typed::PlainURL::maxOffset || !WTF::charactersAreAllLatin1(view->span16()))
        return std::nullopt;
    copy.grow(view->length());
    view->getCharacters(copy.mutableSpan());
    return copy.span();
}

static int32_t scanQuery(JSGlobalObject* globalObject, JSValue textValue)
{
    NarrowCopy copy;
    auto text = latin1SpanWithoutResolving(globalObject, textValue, copy);
    if (!text)
        return Typed::PlainURL::notPlain;
    auto count = forEachPlainQueryPair(*text, [](auto, size_t, size_t) {
        return IterationStatus::Continue;
    });
    return count ? Typed::PlainURL::packQuery(*count, !text->empty() && text->front() == '?', text->size()) : Typed::PlainURL::notPlain;
}

static int32_t findInQuery(JSGlobalObject* globalObject, JSValue textValue, JSValue nameValue)
{
    NarrowCopy copyOfText;
    NarrowCopy copyOfName;
    auto text = latin1SpanWithoutResolving(globalObject, textValue, copyOfText);
    auto name = latin1SpanWithoutResolving(globalObject, nameValue, copyOfName);
    if (!text || !name)
        return Typed::PlainURL::needsObject;
    int32_t found = Typed::PlainURL::absent;
    forEachPlainQueryPair(*text, [&](auto candidate, size_t value, size_t end) {
        if (!equalSpans(candidate, *name))
            return IterationStatus::Continue;
        found = Typed::PlainURL::packRange(value, end);
        return IterationStatus::Done;
    });
    return found;
}

static JSString* keyString(VM& vm, std::span<const Latin1Character> name)
{
    if (name.size() > KeyAtomStringCache::maxStringLengthForCache)
        return jsString(vm, String(AtomString(name)));
    WTF::HashTranslatorCharBuffer<Latin1Character> buffer { name };
    return vm.keyAtomStringCache.make(vm, buffer, [](VM& vm, auto& buffer) {
        return jsString(vm, String(AtomString(buffer.characters)));
    });
}

static EncodedJSValue listQueryPairs(JSGlobalObject* globalObject, JSValue textValue, JSValue query, IterationKind kind)
{
    VM& vm = globalObject->vm();
    NarrowCopy copy;
    auto text = *latin1SpanWithoutResolving(globalObject, textValue, copy);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    JSArray* list = JSArray::create(vm, structure, Typed::PlainURL::numberOfPairs(query.asInt32()));
    unsigned index = 0;
    forEachPlainQueryPair(text, [&](auto name, size_t value, size_t end) {
        JSString* key = kind == IterationKind::Values ? nullptr : keyString(vm, name);
        JSString* valueString = kind == IterationKind::Keys ? nullptr : jsString(vm, String(text.subspan(value, end - value)));
        JSValue entry = key ? key : valueString;
        if (kind == IterationKind::Entries) {
            JSArray* pair = JSArray::create(vm, structure, 2);
            pair->setIndexQuickly(vm, 0, key);
            pair->setIndexQuickly(vm, 1, valueString);
            entry = pair;
        }
        list->setIndexQuickly(vm, index++, entry);
        return IterationStatus::Continue;
    });
    ensureStillAliveHere(textValue);
    return JSValue::encode(list);
}

} } // namespace JSC::AOT

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__scanPlainQuery(JSC::JSGlobalObject* globalObject, JSC::EncodedJSValue encodedText)
{
    return JSC::JSValue::encode(JSC::jsNumber(JSC::AOT::scanQuery(globalObject, JSC::JSValue::decode(encodedText))));
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__findInPlainQuery(JSC::JSGlobalObject* globalObject, JSC::EncodedJSValue encodedText, JSC::EncodedJSValue encodedName)
{
    return JSC::JSValue::encode(JSC::jsNumber(JSC::AOT::findInQuery(globalObject, JSC::JSValue::decode(encodedText), JSC::JSValue::decode(encodedName))));
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__plainQueryEntries(JSC::JSGlobalObject* globalObject, JSC::EncodedJSValue encodedText, JSC::EncodedJSValue encodedQuery)
{
    return JSC::AOT::listQueryPairs(globalObject, JSC::JSValue::decode(encodedText), JSC::JSValue::decode(encodedQuery), JSC::IterationKind::Entries);
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__plainQueryKeys(JSC::JSGlobalObject* globalObject, JSC::EncodedJSValue encodedText, JSC::EncodedJSValue encodedQuery)
{
    return JSC::AOT::listQueryPairs(globalObject, JSC::JSValue::decode(encodedText), JSC::JSValue::decode(encodedQuery), JSC::IterationKind::Keys);
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__plainQueryValues(JSC::JSGlobalObject* globalObject, JSC::EncodedJSValue encodedText, JSC::EncodedJSValue encodedQuery)
{
    return JSC::AOT::listQueryPairs(globalObject, JSC::JSValue::decode(encodedText), JSC::JSValue::decode(encodedQuery), JSC::IterationKind::Values);
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__scanPlainURL(JSC::EncodedJSValue encodedText)
{
    auto text = JSC::AOT::flatLatin1Span(JSC::JSValue::decode(encodedText));
    return JSC::JSValue::encode(JSC::jsNumber(JSC::AOT::scanPathAndQuery(text, JSC::AOT::findStartOfPath(text))));
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__scanPlainURLWithPlainBase(JSC::EncodedJSValue encodedText)
{
    return JSC::JSValue::encode(JSC::jsNumber(JSC::AOT::scanWithPlainBase(JSC::AOT::flatLatin1Span(JSC::JSValue::decode(encodedText)))));
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__scanPlainURLWithBase(JSC::EncodedJSValue encodedText, JSC::EncodedJSValue encodedBase)
{
    if (!JSC::AOT::isPlainBaseURL(JSC::AOT::flatLatin1Span(JSC::JSValue::decode(encodedBase))))
        return JSC::JSValue::encode(JSC::jsNumber(JSC::AOT::Typed::PlainURL::notPlain));
    return AOT__scanPlainURLWithPlainBase(encodedText);
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__findPlainQueryValue(JSC::EncodedJSValue encodedText, JSC::EncodedJSValue encodedOffsets, JSC::EncodedJSValue encodedName)
{
    JSC::JSValue offsets = JSC::JSValue::decode(encodedOffsets);
    return JSC::JSValue::encode(JSC::jsNumber(JSC::AOT::findQueryValue(JSC::AOT::flatLatin1Span(JSC::JSValue::decode(encodedText)), offsets.isInt32() ? offsets.asInt32() : JSC::AOT::Typed::PlainURL::notPlain, JSC::JSValue::decode(encodedName))));
}

#endif // ENABLE(AOT)
