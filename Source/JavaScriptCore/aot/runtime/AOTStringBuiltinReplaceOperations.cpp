/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTOperationArguments.h"
#include "InterpreterInlines.h"
#include "JSCInlines.h"
#include "RegExpObjectInlines.h"
#include "RegExpPrototype.h"
#include "StringPrototype.h"
#include "StringPrototypeInlines.h"

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceString, JSCell*, (Instance* instance, JSCell* stringCell, JSCell* searchCell, JSCell* replacementCell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceString);
    JSString* string = cellArgument<JSString>(stringCell, Entry::operationAOTStringReplaceString);
    JSString* search = cellArgument<JSString>(searchCell, Entry::operationAOTStringReplaceString);
    JSString* replacement = cellArgument<JSString>(replacementCell, Entry::operationAOTStringReplaceString);
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

template<typename CharacterType>
static JSString* replaceRanges(JSGlobalObject* globalObject, VM& vm, StringView subject, std::span<const unsigned> matchStarts, unsigned matchLength, StringView replacement, unsigned resultLength)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<CharacterType> buffer;
    auto impl = StringImpl::tryCreateUninitialized(resultLength, buffer);
    if (!impl) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    auto append = [&](StringView piece) ALWAYS_INLINE_LAMBDA {
        piece.getCharacters(buffer.first(piece.length()));
        skip(buffer, piece.length());
    };
    unsigned endOfLastMatch = 0;
    for (unsigned start : matchStarts) {
        append(subject.substring(endOfLastMatch, start - endOfLastMatch));
        append(replacement);
        endOfLastMatch = start + matchLength;
    }
    append(subject.substring(endOfLastMatch));
    RELEASE_AND_RETURN(scope, jsString(vm, impl.releaseNonNull()));
}

// A text that is sought as it stands and a replacement that stands for itself: the two literals of most calls. A piece of another string is searched where it lies,
// a string without the text is its own result, and any other result is written once. Null without an exception: the texts are of another kind.
static JSString* tryReplaceAllWithPlainText(JSGlobalObject* globalObject, VM& vm, JSString* string, JSString* search, JSString* replacement)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    StringImpl* soughtImpl = search->tryGetValueImpl();
    StringImpl* replacementImpl = replacement->tryGetValueImpl();
    if (!soughtImpl || !replacementImpl || !soughtImpl->length())
        return nullptr;
    StringView sought { *soughtImpl };
    StringView replacementText { *replacementImpl };
    if (replacementText.find('$') != notFound)
        return nullptr;

    auto subject = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    unsigned matchLength = sought.length();
    auto findFrom = [&](unsigned start) ALWAYS_INLINE_LAMBDA {
        return matchLength == 1 ? subject->find(sought[0], start) : subject->find(vm.adaptiveStringSearcherTables(), sought, start);
    };
    size_t found = findFrom(0);
    if (found == notFound)
        return string;
    Vector<unsigned, 16> matchStarts;
    do {
        matchStarts.append(static_cast<unsigned>(found));
        found = findFrom(static_cast<unsigned>(found) + matchLength);
    } while (found != notFound);

    size_t resultLength = subject->length() - matchStarts.size() * matchLength + matchStarts.size() * static_cast<size_t>(replacementText.length());
    if (resultLength > JSString::MaxLength) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    if (!resultLength)
        return jsEmptyString(vm);
    if (subject->is8Bit() && replacementText.is8Bit())
        RELEASE_AND_RETURN(scope, replaceRanges<Latin1Character>(globalObject, vm, subject, matchStarts.span(), matchLength, replacementText, static_cast<unsigned>(resultLength)));
    RELEASE_AND_RETURN(scope, replaceRanges<char16_t>(globalObject, vm, subject, matchStarts.span(), matchLength, replacementText, static_cast<unsigned>(resultLength)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceAllString, JSCell*, (Instance* instance, JSCell* stringCell, JSCell* searchCell, JSCell* replacementCell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceAllString);
    JSString* string = cellArgument<JSString>(stringCell, Entry::operationAOTStringReplaceAllString);
    JSString* search = cellArgument<JSString>(searchCell, Entry::operationAOTStringReplaceAllString);
    JSString* replacement = cellArgument<JSString>(replacementCell, Entry::operationAOTStringReplaceAllString);
    if (JSString* result = tryReplaceAllWithPlainText(globalObject, vm, string, search, replacement))
        OPERATION_RETURN(scope, result);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto stringView = string->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto searchView = search->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, replaceUsingStringSearch<StringReplaceMode::Global>(vm, globalObject, string, stringView, searchView, replacement));
}

// A text cut at a flat text that is not empty. The places are found where the characters lie, the array is made once, at its length, and is the program's to write to.
// Null without an exception: the engine's way. It remembers the pieces of an atom.
static JSArray* trySplitAtPlainText(JSGlobalObject* globalObject, VM& vm, JSString* string, JSString* separator, uint32_t limit)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    StringImpl* separatorImpl = separator->tryGetValueImpl();
    if (!separatorImpl || !separatorImpl->length() || !limit || globalObject->isHavingABadTime())
        return nullptr;
    if (StringImpl* impl = string->tryGetValueImpl(); impl && impl->isAtom())
        return nullptr;

    Vector<unsigned, 16> ends;
    unsigned separatorLength = separatorImpl->length();
    {
        auto subject = string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        StringView sought { *separatorImpl };
        size_t found;
        unsigned position = 0;
        while ((found = separatorLength == 1 ? subject->find(sought[0], position) : subject->find(vm.adaptiveStringSearcherTables(), sought, position)) != notFound) {
            ends.append(static_cast<unsigned>(found));
            if (ends.size() == limit)
                break;
            position = static_cast<unsigned>(found) + separatorLength;
        }
        if (found == notFound)
            ends.append(subject->length());
    }
    if (ends.size() >= MIN_SPARSE_ARRAY_INDEX)
        return nullptr;

    JSArray* result = JSArray::tryCreate(vm, globalObject->originalArrayStructureForIndexingType(ArrayWithContiguous), ends.size());
    if (!result) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    if (ends.size() == 1 && ends[0] == string->length()) {
        result->butterfly()->contiguous().at(result, 0).set(vm, result, string);
        return result;
    }
    // view() has made every rope flat but a piece of another string, whose pieces are pieces of that string.
    JSString* base = string;
    unsigned start = 0;
    if (string->isSubstring()) {
        auto* rope = uncheckedDowncast<JSRopeString>(string);
        base = rope->substringBase();
        start = rope->substringOffset();
    }
    unsigned offsetInBase = start;
    for (unsigned i = 0; i < ends.size(); ++i) {
        unsigned end = offsetInBase + ends[i];
        result->butterfly()->contiguous().at(result, i).set(vm, result, jsSubstringOfResolved(vm, base, start, end - start));
        start = end + separatorLength;
    }
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringSplitString, JSCell*, (Instance* instance, JSCell* stringCell, JSCell* separatorCell, uint32_t limit))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringSplitString);
    JSString* string = cellArgument<JSString>(stringCell, Entry::operationAOTStringSplitString);
    JSString* separator = cellArgument<JSString>(separatorCell, Entry::operationAOTStringSplitString);
    if (JSArray* result = trySplitAtPlainText(globalObject, vm, string, separator, limit))
        OPERATION_RETURN(scope, result);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, stringSplitFast(globalObject, string, separator, limit));
}

static JSString* replaceMatchesGeneric(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExpObject* regExpObject, JSValue replacement)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = regExpReplaceGeneric(globalObject, regExpObject, string, replacement);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return asString(result);
}

static JSString* replaceMatches(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExpObject* regExpObject, JSValue replacement)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (regExpObject->isSymbolReplaceFastAndNonObservable(globalObject)) [[likely]]
        RELEASE_AND_RETURN(scope, replaceUsingRegExpSearch(vm, globalObject, string, regExpObject, replacement));
    RELEASE_AND_RETURN(scope, replaceMatchesGeneric(globalObject, vm, string, regExpObject, replacement));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceRegExp, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, EncodedJSValue replacement))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceRegExp);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringReplaceRegExp);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringReplaceRegExp);
    OPERATION_RETURN(scope, replaceMatches(globalObject, vm, subject, regExpObject, JSValue::decode(replacement)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceAllRegExp, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, EncodedJSValue replacement))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceAllRegExp);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringReplaceAllRegExp);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringReplaceAllRegExp);
    if (!regExpObject->regExp()->global()) [[unlikely]] {
        throwException(globalObject, scope, createReplaceAllNonGlobalRegExpError(globalObject));
        OPERATION_RETURN(scope, nullptr);
    }
    OPERATION_RETURN(scope, replaceMatches(globalObject, vm, subject, regExpObject, JSValue::decode(replacement)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceRegExpString, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, JSCell* replacementCell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceRegExpString);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringReplaceRegExpString);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringReplaceRegExpString);
    auto* replacement = cellArgument<JSString>(replacementCell, Entry::operationAOTStringReplaceRegExpString);
    if (!regExpObject->isSymbolReplaceFastAndNonObservable(globalObject)) [[unlikely]]
        OPERATION_RETURN(scope, replaceMatchesGeneric(globalObject, vm, subject, regExpObject, replacement));
    CallData callData;
    auto replacementText = replacement->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, replaceUsingRegExpSearch(vm, globalObject, subject, regExpObject, callData, replacementText, replacement));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringSplitRegExp, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, uint32_t limit))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringSplitRegExp);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringSplitRegExp);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringSplitRegExp);
    if (regExpObject->isSymbolSplitFastAndNonObservable(globalObject)) [[likely]]
        OPERATION_RETURN(scope, regExpSplitFast(globalObject, regExpObject, subject, limit));
    JSValue result = regExpSplitSlow(globalObject, regExpObject, subject, jsNumber(limit));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, result.asCell());
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringMatchRegExp, EncodedJSValue, (Instance* instance, JSCell* string, JSCell* regExp))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringMatchRegExp);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringMatchRegExp);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringMatchRegExp);
    if (regExpObject->isSymbolMatchFastAndNonObservable(globalObject)) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(regExpMatchFast(globalObject, regExpObject, subject)));
    JSValue matcher = globalObject->linkTimeConstant(LinkTimeConstant::regExpPrototypeSymbolMatch);
    auto callData = JSC::getCallData(matcher);
    ASSERT(callData.type != CallData::Type::None);
    auto args = WTF::toArray<EncodedJSValue>({
        JSValue::encode(subject),
    });
    OPERATION_RETURN(scope, JSValue::encode(call(globalObject, matcher, callData, regExpObject, ArgList { args.data(), args.size() })));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringSearchRegExp, EncodedJSValue, (Instance* instance, JSCell* string, JSCell* regExp))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringSearchRegExp);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringSearchRegExp);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringSearchRegExp);
    if (regExpObject->isSymbolSearchFastAndNonObservable(globalObject)) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(regExpSearchFast(globalObject, regExpObject, subject)));
    OPERATION_RETURN(scope, JSValue::encode(regExpSearchGeneric(globalObject, regExpObject, subject)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
