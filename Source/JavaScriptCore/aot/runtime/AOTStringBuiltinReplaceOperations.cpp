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

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceAllString, JSCell*, (Instance* instance, JSCell* stringCell, JSCell* searchCell, JSCell* replacementCell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceAllString);
    JSString* string = cellArgument<JSString>(stringCell, Entry::operationAOTStringReplaceAllString);
    JSString* search = cellArgument<JSString>(searchCell, Entry::operationAOTStringReplaceAllString);
    JSString* replacement = cellArgument<JSString>(replacementCell, Entry::operationAOTStringReplaceAllString);
    auto stringView = string->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto searchView = search->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, replaceUsingStringSearch<StringReplaceMode::Global>(vm, globalObject, string, stringView, searchView, replacement));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringSplitString, JSCell*, (Instance* instance, JSCell* string, JSCell* separator, uint32_t limit))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringSplitString);
    OPERATION_RETURN(scope, stringSplitFast(globalObject, cellArgument<JSString>(string, Entry::operationAOTStringSplitString), cellArgument<JSString>(separator, Entry::operationAOTStringSplitString), limit));
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
        throwTypeError(globalObject, scope, "String.prototype.replaceAll argument must not be a non-global regular expression"_s);
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
