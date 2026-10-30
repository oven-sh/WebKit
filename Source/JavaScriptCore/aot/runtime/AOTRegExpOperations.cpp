/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTImageText.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTProgramStatics.h"
#include "AOTRegExpRecords.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "AOTStubsRegExp.h"
#include "AOTThreshold.h"
#include "AOTVMState.h"
#include "AOTVector.h"
#include "AbstractSlotVisitorInlines.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "InterpreterInlines.h"
#include "JSCInlines.h"
#include "RegExp.h"
#include "RegExpConstructor.h"
#include "RegExpMatchesArray.h"
#include "RegExpObjectInlines.h"
#include "RegExpPrototype.h"
#include "SlotVisitorInlines.h"
#include "StringPrototype.h"
#include "StringPrototypeInlines.h"
#include "VM.h"
#include "YarrFlags.h"
#include "YarrFlatProgram.h"
#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <wtf/HashFunctions.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpExec, EncodedJSValue, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpExec);
    auto* object = cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpExec);
    OPERATION_RETURN(scope, JSValue::encode(object->exec(globalObject, cellArgument<JSString>(string, Entry::operationAOTRegExpExec))));
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

static ALWAYS_INLINE bool testRegExp(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* string)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    RegExp* regExp = object->regExp();
    if (regExp->globalOrSticky() || !object->getLastIndex().isUInt32()) [[unlikely]]
        RELEASE_AND_RETURN(scope, object->test(globalObject, string));
    auto input = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, !!matchFromStart(globalObject, vm, regExp, string, input));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpTest, size_t, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpTest);
    OPERATION_RETURN(scope, testRegExp(globalObject, vm, cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpTest), cellArgument<JSString>(string, Entry::operationAOTRegExpTest)));
}

static bool findMatch(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* string)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto input = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    unsigned lastIndex = getRegExpObjectLastIndexAsUnsigned(globalObject, object, input);
    RETURN_IF_EXCEPTION(scope, false);
    RegExp* regExp = object->regExp();
    bool globalOrSticky = regExp->globalOrSticky();
    if (lastIndex == UINT_MAX && globalOrSticky) {
        scope.release();
        object->setLastIndex(globalObject, 0);
        return false;
    }
    if (!globalOrSticky)
        lastIndex = 0;
    auto offsets = regExp->ovectorSpan();
    int position = regExp->matchInline<Yarr::MatchFrom::VMThread>(globalObject, vm, input, lastIndex, offsets);
    RETURN_IF_EXCEPTION(scope, false);
    if (position == -1) {
        scope.release();
        if (globalOrSticky)
            object->setLastIndex(globalObject, 0);
        return false;
    }
    MatchResult result(position, offsets[1]);
    if (globalOrSticky)
        object->setLastIndex(globalObject, result.end);
    RETURN_IF_EXCEPTION(scope, false);
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, result, false);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpFind, size_t, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpFind);
    OPERATION_RETURN(scope, findMatch(globalObject, vm, cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpFind), cellArgument<JSString>(string, Entry::operationAOTRegExpFind)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpTestBoxed, EncodedJSValue, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpTestBoxed);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(testRegExp(globalObject, vm, cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpTestBoxed), cellArgument<JSString>(string, Entry::operationAOTRegExpTestBoxed)))));
}

static RegExp* regExpOfRecord(VM& vm, uint32_t record)
{
    RegExpRecordID id(record);
    RELEASE_ASSERT(id);
    return vm.aotState().regExpLiterals->regExpFor(vm, id, RegExpRecords(*vm.aotState().image).at(id).flags());
}

static JSValue createMatchValue(Instance* instance, RegExpMatchForm form, RegExp* regExp, JSString* subject, MatchResult result, std::span<const int> offsets)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    switch (form) {
    case RegExpMatchForm::Test:
        return jsBoolean(true);
    case RegExpMatchForm::Search:
        return jsNumber(result.start);
    case RegExpMatchForm::Exec:
    case RegExpMatchForm::Match:
        break;
    }
    unsigned numSubpatterns = regExp->numSubpatterns();
    bool hasNamedCaptures = regExp->hasNamedCaptures();
    bool createIndices = regExp->hasIndices();
    if (hasNamedCaptures || createIndices) [[unlikely]]
        return createRegExpMatchesArrayWithGroupsOrIndices(vm, globalObject, subject, regExp, result, offsets, numSubpatterns, hasNamedCaptures, createIndices);
    if (globalObject->isHavingABadTime()) [[unlikely]]
        return createRegExpMatchesArrayForPlainRegExpHavingABadTime(vm, globalObject, subject, result, offsets, numSubpatterns);
    prepareInlineAllocation<JSArray>(instance, InlineAllocation::RegExpMatchesArray, globalObject->regExpMatchesArrayStructure());
    leaveCellsForRopes(instance);
    return createRegExpMatchesArrayForPlainRegExp(vm, globalObject, subject, result, offsets, numSubpatterns);
}

// Leaves the next match in match. Without one match is false.
static ALWAYS_INLINE void findMatchBehind(JSGlobalObject* globalObject, RegExp* regExp, JSString* string, StringView source, MatchResult& match)
{
    uint64_t start = match.empty() ? advanceStringIndex(source, source.length(), match.end, regExp->eitherUnicode()) : match.end;
    if (start > source.length()) {
        match = MatchResult::failed();
        return;
    }
    int* offsets;
    match = globalObject->regExpGlobalData().performMatch(globalObject, regExp, string, source, static_cast<int>(start), &offsets);
}

static void recordLastMatch(JSGlobalObject* globalObject, VM& vm, RegExp* regExp, JSString* string, const Vector<unsigned, 128>& matches)
{
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, MatchResult { matches[matches.size() - 2], matches.last() }, false);
}

// What String.prototype.match answers for a global RegExp that is one set or a run of one set. Empty: it has to search match by match.
static JSValue matchEveryPlaceInOnePass(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* subject)
{
    AOT_THRESHOLD(unsigned, maxMatchesCollected, 100000);
    auto scope = DECLARE_THROW_SCOPE(vm);
    RegExp* regExp = object->regExp();
    if (!regExp->global())
        return { };
    auto source = subject->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    Vector<unsigned, 128> matches;
    if (!regExp->collectMatchesOfOneSetOrRun(vm, source, matches) || matches.size() / 2 > maxMatchesCollected)
        return { };
    object->setLastIndex(globalObject, 0);
    RETURN_IF_EXCEPTION(scope, { });
    if (matches.isEmpty())
        return jsNull();
    JSArray* array = constructEmptyArray(globalObject, nullptr, matches.size() / 2);
    RETURN_IF_EXCEPTION(scope, { });
    for (size_t i = 0; i < matches.size(); i += 2) {
        array->putDirectIndex(globalObject, i / 2, jsSubstringOfResolved(vm, subject, matches[i], matches[i + 1] - matches[i]));
        RETURN_IF_EXCEPTION(scope, { });
    }
    recordLastMatch(globalObject, vm, regExp, subject, matches);
    return array;
}

// What String.prototype.match answers for a global RegExp whose first match in the subject is known.
static JSValue collectMatchesFrom(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* subject, MatchResult match)
{
    // Beyond this RegExpObject::matchGlobal() counts the matches before it creates them.
    AOT_THRESHOLD(unsigned, maxMatchesCreatedUncounted, 100000);
    auto scope = DECLARE_THROW_SCOPE(vm);
    RegExp* regExp = object->regExp();
    object->setLastIndex(globalObject, 0);
    RETURN_IF_EXCEPTION(scope, { });
    auto source = subject->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* array = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, subject, match, false);
    unsigned count = 0;
    do {
        if (count == maxMatchesCreatedUncounted) [[unlikely]]
            RELEASE_AND_RETURN(scope, regExpMatchFast(globalObject, object, subject));
        array->putDirectIndex(globalObject, count++, jsSubstringOfResolved(vm, subject, match.start, match.end - match.start));
        RETURN_IF_EXCEPTION(scope, { });
        findMatchBehind(globalObject, regExp, subject, source, match);
        RETURN_IF_EXCEPTION(scope, { });
    } while (match);
    return array;
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpMatchLiteral, EncodedJSValue, (Instance* instance, JSCell* regExp, JSCell* string, uint32_t regExpRecord, uint32_t form))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpMatchLiteral);
    RegExpObject* object = regExp ? cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpMatchLiteral) : nullptr;
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTRegExpMatchLiteral);
    RegExp* cell = object ? object->regExp() : regExpOfRecord(vm, regExpRecord);
    ASSERT(!RegExpRecordID(regExpRecord) || cell->imageRecord() == &RegExpRecords(*vm.aotState().image).at(RegExpRecordID(regExpRecord)));

    ASSERT(static_cast<size_t>(cell->offsetVectorSize()) <= std::max<size_t>(vm.aotState().image->header().counts.maxRegExpOutputSize, 2 * (VMState::maxGroupsInRegExpMatchOutput + 1)));
    auto offsets = unsafeMakeSpan(vm.aotState().regExpMatchOutput.get(), static_cast<size_t>(cell->offsetVectorSize()));
    MatchResult result(offsets[0], offsets[1]);
    RELEASE_ASSERT(result.end >= result.start);

    auto matchForm = static_cast<RegExpMatchForm>(form);
    if (matchForm == RegExpMatchForm::Match && cell->global())
        OPERATION_RETURN(scope, JSValue::encode(collectMatchesFrom(globalObject, vm, object, subject, result)));

    JSValue value = createMatchValue(instance, matchForm, cell, subject, result, offsets);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!object)
        OPERATION_RETURN(scope, JSValue::encode(value));

    if (matchForm != RegExpMatchForm::Search && cell->globalOrSticky()) {
        object->setLastIndex(globalObject, result.end);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, cell, subject, result, false);
    OPERATION_RETURN(scope, JSValue::encode(value));
}

// Answers as the code of a pattern in the image does: where the match starts, -1 where there is none, less where the match has to be given to somebody else.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRegExpMatchFlatProgram8, int64_t, (void* program, const Latin1Character* characters, uint32_t length, uint32_t* output))
{
    switch (Yarr::matchFlatProgram(*static_cast<const Yarr::FlatProgram*>(program), std::span { characters, length }, 0, output)) {
    case Yarr::JSRegExpResult::Match:
        return output[0];
    case Yarr::JSRegExpResult::NoMatch:
        return -1;
    default:
        return static_cast<int64_t>(Yarr::JSRegExpResult::JITCodeFailure);
    }
}

static JSValue valueOfFailedMatch(RegExpMatchForm form)
{
    switch (form) {
    case RegExpMatchForm::Test:
        return jsBoolean(false);
    case RegExpMatchForm::Exec:
    case RegExpMatchForm::Match:
        return jsNull();
    case RegExpMatchForm::Search:
        return jsNumber(-1);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpMatchLiteralWithoutObject, EncodedJSValue, (Instance* instance, JSCell* string, uint32_t regExpRecord, uint32_t form))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpMatchLiteralWithoutObject);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTRegExpMatchLiteralWithoutObject);
    RegExp* regExp = regExpOfRecord(vm, regExpRecord);
    RELEASE_ASSERT(!regExp->globalOrSticky());

    auto input = subject->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto offsets = regExp->ovectorSpan();
    int position = regExp->matchInline<Yarr::MatchFrom::VMThread>(globalObject, vm, input, 0, offsets);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (position == -1)
        OPERATION_RETURN(scope, JSValue::encode(valueOfFailedMatch(static_cast<RegExpMatchForm>(form))));

    MatchResult result(position, offsets[1]);
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, subject, result, false);
    OPERATION_RETURN(scope, JSValue::encode(createMatchValue(instance, static_cast<RegExpMatchForm>(form), regExp, subject, result, offsets)));
}

static size_t fillPrefixOfMatch(JSGlobalObject* globalObject, VM& vm, RegExp* regExp, JSString* string, uint32_t count, EncodedJSValue* elements)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto offsets = regExp->ovectorSpan();
    unsigned groups = regExp->numSubpatterns();
    for (unsigned i = 0; i < count && i <= groups; ++i) {
        if (offsets[2 * i] < 0)
            continue;
        JSString* piece = jsSubstring(globalObject, string, offsets[2 * i], offsets[2 * i + 1] - offsets[2 * i]);
        RETURN_IF_EXCEPTION(scope, 0);
        elements[i] = JSValue::encode(piece);
    }
    return groups + 1;
}

static size_t findPrefixOfMatch(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* string, uint32_t count, EncodedJSValue* elements)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isFound = findMatch(globalObject, vm, object, string);
    RETURN_IF_EXCEPTION(scope, 0);
    if (!isFound)
        return noMatchPrefixLength;
    RELEASE_AND_RETURN(scope, fillPrefixOfMatch(globalObject, vm, object->regExp(), string, count, elements));
}

// Does what RegExpObject::matchGlobal() does, without the list.
static size_t findPrefixOfGlobalMatch(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* string, uint32_t count, EncodedJSValue* elements)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    RegExp* regExp = object->regExp();
    object->setLastIndex(globalObject, 0);
    RETURN_IF_EXCEPTION(scope, 0);
    auto source = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    Vector<unsigned, 128> matches;
    if (regExp->collectMatchesOfOneSetOrRun(vm, source, matches) && matches.size() / 2 <= MAX_STORAGE_VECTOR_LENGTH) {
        if (matches.isEmpty())
            return noMatchPrefixLength;
        size_t length = matches.size() / 2;
        for (size_t i = 0; i < std::min<size_t>(length, count); ++i)
            elements[i] = JSValue::encode(jsSubstringOfResolved(vm, string, matches[2 * i], matches[2 * i + 1] - matches[2 * i]));
        recordLastMatch(globalObject, vm, regExp, string, matches);
        return length;
    }
    MatchResult match = globalObject->regExpGlobalData().performMatch(globalObject, regExp, string, source, 0);
    RETURN_IF_EXCEPTION(scope, 0);
    if (!match)
        return noMatchPrefixLength;

    size_t length = 0;
    do {
        if (length == MAX_STORAGE_VECTOR_LENGTH) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return 0;
        }
        if (length < count)
            elements[length] = JSValue::encode(jsSubstringOfResolved(vm, string, match.start, match.end - match.start));
        ++length;
        findMatchBehind(globalObject, regExp, string, source, match);
        RETURN_IF_EXCEPTION(scope, 0);
    } while (match);
    return length;
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpExecPrefix, size_t, (Instance* instance, JSCell* regExp, JSCell* string, uint32_t count, uint32_t* bounds))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpExecPrefix);
    std::fill_n(bounds, 2 * static_cast<size_t>(count), static_cast<uint32_t>(absentBound));
    auto* object = cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpExecPrefix);
    bool isFound = findMatch(globalObject, vm, object, cellArgument<JSString>(string, Entry::operationAOTRegExpExecPrefix));
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!isFound)
        OPERATION_RETURN(scope, static_cast<size_t>(noMatchPrefixLength));

    static_assert(absentBound == -1, "A group that took no part in the match has the offsets -1.");
    size_t length = static_cast<size_t>(object->regExp()->numSubpatterns()) + 1;
    std::ranges::copy(object->regExp()->ovectorSpan().first(2 * std::min<size_t>(count, length)), bounds);
    OPERATION_RETURN(scope, length);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringMatchPrefix, size_t, (Instance* instance, JSCell* string, JSCell* regExp, uint32_t count, EncodedJSValue* elements))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringMatchPrefix);
    std::fill_n(elements, count, JSValue::encode(jsUndefined()));
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringMatchPrefix);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringMatchPrefix);
    if (regExpObject->regExp()->global())
        OPERATION_RETURN(scope, findPrefixOfGlobalMatch(globalObject, vm, regExpObject, subject, count, elements));
    OPERATION_RETURN(scope, findPrefixOfMatch(globalObject, vm, regExpObject, subject, count, elements));
}

static void setSharedLiteral(VM& vm, Instance* instance, uint32_t staticsOffset, RegExpObject* object)
{
    vm.mutatorFence();
    instance->state->statics->setCell(staticsOffset, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewRegExpLiteral, JSCell*, (Instance* instance, uint32_t regExpRecord, uint32_t flags, uint32_t staticsOffset))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewRegExpLiteral);
    RegExp* regExp = vm.aotState().regExpLiterals->regExpFor(vm, RegExpRecordID(regExpRecord), OptionSet<Yarr::Flags>::fromRaw(flags));
    ASSERT(regExp->isValid());
    if (staticsOffset != noSharedRegExpOffset && !instance->state->statics->at<JSCell*>(staticsOffset))
        setSharedLiteral(vm, instance, staticsOffset, RegExpObject::create(vm, globalObject->regExpStructure(), regExp));
    prepareInlineAllocation<RegExpObject>(instance, InlineAllocation::RegExpObject, globalObject->regExpStructure());
    OPERATION_RETURN(scope, RegExpObject::create(vm, globalObject->regExpStructure(), regExp));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSharedRegExpLiteral, JSCell*, (Instance* instance, uint32_t staticsOffset, uint32_t regExpRecord, uint32_t flags))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTSharedRegExpLiteral);
    DeferTerminationForAWhile deferTermination(vm);
    RegExp* regExp = vm.aotState().regExpLiterals->regExpFor(vm, RegExpRecordID(regExpRecord), OptionSet<Yarr::Flags>::fromRaw(flags));
    ASSERT(regExp->isValid());
    RegExpObject* object = RegExpObject::create(vm, globalObject->regExpStructure(), regExp);
    setSharedLiteral(vm, instance, staticsOffset, object);
    OPERATION_RETURN(scope, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewRegExp, JSCell*, (Instance* instance, JSCell* patternCell, uint32_t flags))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewRegExp);
    auto pattern = cellArgument<JSString>(patternCell, Entry::operationAOTNewRegExp)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, regExpCreate(globalObject, JSValue(), pattern, OptionSet<Yarr::Flags>::fromRaw(flags)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpFlags, JSCell*, (Instance* instance, JSCell* regExp))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpFlags);
    OPERATION_RETURN(scope, jsString(vm, String { Yarr::flagsString(cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpFlags)->regExp()->flags()).span() }));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpSource, JSCell*, (Instance* instance, JSCell* regExp))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpSource);
    OPERATION_RETURN(scope, jsString(vm, cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpSource)->regExp()->escapedPattern()));
}

static JSString* replaceMatchesGeneric(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExpObject* regExpObject, JSValue replacement)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = regExpReplaceGeneric(globalObject, regExpObject, string, replacement);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return asString(result);
}

// A global replace with plain text of a pattern that is one set or a run of one set. hasReplaced false: it has to search match by match.
static JSString* replaceInOnePass(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExpObject* object, JSString* replacement, const String& replacementText, bool& hasReplaced)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    hasReplaced = false;
    RegExp* regExp = object->regExp();
    if (!regExp->global() || replacementText.find('$') != notFound)
        return nullptr;
    auto source = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    Vector<unsigned, 128> matches;
    if (!regExp->collectMatchesOfOneSetOrRun(vm, source, matches))
        return nullptr;
    hasReplaced = true;
    object->setLastIndex(globalObject, 0);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (matches.isEmpty())
        return string;
    Vector<Range<int32_t>, 64> kept;
    bool removes = replacementText.isEmpty();
    size_t endOfLastMatch = 0;
    for (size_t i = 0; i < matches.size(); i += 2) {
        if ((!removes || endOfLastMatch < matches[i]) && !kept.tryConstructAndAppend(endOfLastMatch, matches[i])) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);
        endOfLastMatch = matches[i + 1];
    }
    if (endOfLastMatch < source->length() && !kept.tryConstructAndAppend(endOfLastMatch, source->length())) [[unlikely]]
        OUT_OF_MEMORY(globalObject, scope);
    recordLastMatch(globalObject, vm, regExp, string, matches);
    if (removes)
        RELEASE_AND_RETURN(scope, jsSpliceSubstrings(globalObject, string, source, kept.span()));
    RELEASE_AND_RETURN(scope, jsSpliceSubstringsWithSeparator(globalObject, string, source, kept.span().data(), kept.size(), replacement, replacementText, matches.size() / 2));
}

static JSString* replaceMatches(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExpObject* regExpObject, JSValue replacement)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (regExpObject->isSymbolReplaceFastAndNonObservable(globalObject)) [[likely]] {
        if (replacement.isString()) {
            auto text = asString(replacement)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
            bool hasReplaced;
            JSString* result = replaceInOnePass(globalObject, vm, string, regExpObject, asString(replacement), text, hasReplaced);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (hasReplaced)
                return result;
        }
        RELEASE_AND_RETURN(scope, replaceUsingRegExpSearch(vm, globalObject, string, regExpObject, replacement));
    }
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

static JSString* replaceOneMatch(JSGlobalObject* globalObject, VM& vm, JSString* string, StringView source, RegExp* regExp, const String& replacement, MatchResult match, const int* offsets)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (replacement.isEmpty()) {
        if (!match.start)
            return jsSubstringOfResolved(vm, string, match.end, source.length() - match.end);
        if (match.end == source.length())
            return jsSubstringOfResolved(vm, string, 0, match.start);
    }
    StringBuilder substituted(OverflowPolicy::RecordOverflow);
    StringView inserted = replacement;
    if (size_t dollar = replacement.find('$'); dollar != notFound) {
        substituteBackreferencesSlow(substituted, replacement, source, offsets, regExp, dollar);
        if (substituted.hasOverflowed()) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);
        inserted = substituted;
    }
    auto result = tryMakeString(source.left(match.start), inserted, source.substring(match.end));
    if (!result) [[unlikely]]
        OUT_OF_MEMORY(globalObject, scope);
    RELEASE_AND_RETURN(scope, jsString(vm, WTF::move(result)));
}

static JSString* removeEveryMatch(JSGlobalObject* globalObject, VM& vm, JSString* string, StringView source, RegExp* regExp, MatchResult match)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<Range<int32_t>, 64> kept;
    size_t endOfLastMatch = 0;
    do {
        if (endOfLastMatch < match.start) {
            if (!kept.tryConstructAndAppend(endOfLastMatch, match.start)) [[unlikely]]
                OUT_OF_MEMORY(globalObject, scope);
        }
        endOfLastMatch = match.end;
        findMatchBehind(globalObject, regExp, string, source, match);
        RETURN_IF_EXCEPTION(scope, nullptr);
    } while (match);
    if (!endOfLastMatch)
        return string;
    if (endOfLastMatch < source.length()) {
        if (!kept.tryConstructAndAppend(endOfLastMatch, source.length())) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);
    }
    RELEASE_AND_RETURN(scope, jsSpliceSubstrings(globalObject, string, source, kept.span()));
}

static JSString* replaceEveryMatchWithPlainText(JSGlobalObject* globalObject, VM& vm, JSString* string, StringView source, RegExp* regExp, JSString* replacement, const String& replacementText, MatchResult match)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<Range<int32_t>, 16> kept;
    size_t endOfLastMatch = 0;
    int matches = 0;
    do {
        if (!kept.tryConstructAndAppend(endOfLastMatch, match.start)) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);
        ++matches;
        endOfLastMatch = match.end;
        findMatchBehind(globalObject, regExp, string, source, match);
        RETURN_IF_EXCEPTION(scope, nullptr);
    } while (match);
    if (endOfLastMatch < source.length()) {
        if (!kept.tryConstructAndAppend(endOfLastMatch, source.length())) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);
    }
    RELEASE_AND_RETURN(scope, jsSpliceSubstringsWithSeparator(globalObject, string, source, kept.span().data(), kept.size(), replacement, replacementText, matches));
}

static JSString* replaceEveryMatchWithTemplate(JSGlobalObject* globalObject, VM& vm, JSString* string, StringView source, RegExp* regExp, const String& replacementText, size_t dollar, MatchResult match, const int* offsets)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    StringReplaceTemplateParts parts;
    parseReplacementTemplate(parts, replacementText, regExp, dollar);

    MatchResult first = match;
    StringBuilder firstReplacement(OverflowPolicy::RecordOverflow);
    appendReplacementUsingTemplate(firstReplacement, parts.span(), replacementText, source, offsets, regExp);
    if (firstReplacement.hasOverflowed()) [[unlikely]]
        OUT_OF_MEMORY(globalObject, scope);
    findMatchBehind(globalObject, regExp, string, source, match);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!match) {
        auto resolved = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        RELEASE_AND_RETURN(scope, jsString(globalObject, resolved->substringSharingImpl(0, first.start), firstReplacement.toString(), resolved->substringSharingImpl(first.end)));
    }

    StringBuilder result(OverflowPolicy::RecordOverflow);
    result.reserveCapacity(source.length());
    result.append(source.left(first.start));
    result.append(firstReplacement);
    size_t endOfLastMatch = first.end;
    offsets = regExp->ovectorSpan().data();
    do {
        result.append(source.substring(endOfLastMatch, match.start - endOfLastMatch));
        appendReplacementUsingTemplate(result, parts.span(), replacementText, source, offsets, regExp);
        if (result.hasOverflowed()) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);
        endOfLastMatch = match.end;
        findMatchBehind(globalObject, regExp, string, source, match);
        RETURN_IF_EXCEPTION(scope, nullptr);
    } while (match);
    result.append(source.substring(endOfLastMatch));
    if (result.hasOverflowed()) [[unlikely]]
        OUT_OF_MEMORY(globalObject, scope);
    RELEASE_AND_RETURN(scope, jsString(vm, result.toString()));
}

// The caller has searched the subject from its start for a RegExp that is not sticky, and offsets are those of the match. lastIndex of a global RegExp is 0.
static JSString* replaceFromFirstMatch(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExp* regExp, JSString* replacement, const String& replacementText, const int* offsets)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto source = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    MatchResult match(offsets[0], offsets[1]);
    RELEASE_ASSERT(match.start <= match.end && match.end <= source->length());
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, match, false);
    if (!regExp->global())
        RELEASE_AND_RETURN(scope, replaceOneMatch(globalObject, vm, string, source, regExp, replacementText, match, offsets));
    if (replacementText.isEmpty())
        RELEASE_AND_RETURN(scope, removeEveryMatch(globalObject, vm, string, source, regExp, match));
    size_t dollar = replacementText.find('$');
    if (dollar == notFound)
        RELEASE_AND_RETURN(scope, replaceEveryMatchWithPlainText(globalObject, vm, string, source, regExp, replacement, replacementText, match));
    RELEASE_AND_RETURN(scope, replaceEveryMatchWithTemplate(globalObject, vm, string, source, regExp, replacementText, dollar, match, offsets));
}

// oneBehindStartOfFirstMatch is 0 where nobody has searched. Else the caller has searched the subject from its start; the code of a pattern has left the offsets of the match in the output of the VM.
JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceRegExpString, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, JSCell* replacementCell, uint32_t oneBehindStartOfFirstMatch))
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
    RegExp* pattern = regExpObject->regExp();
    bool isAtom = pattern->hasValidAtom();
    // Every place of a text that is sought as it stands and removed is found in one pass.
    if (!oneBehindStartOfFirstMatch) {
        bool hasReplaced;
        JSString* result = replaceInOnePass(globalObject, vm, subject, regExpObject, replacement, replacementText, hasReplaced);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        if (hasReplaced)
            OPERATION_RETURN(scope, result);
    }
    if (!oneBehindStartOfFirstMatch || (isAtom && pattern->global() && replacementText->isEmpty()))
        OPERATION_RETURN(scope, replaceUsingRegExpSearch(vm, globalObject, subject, regExpObject, callData, replacementText, replacement));
    if (!isAtom)
        OPERATION_RETURN(scope, replaceFromFirstMatch(globalObject, vm, subject, pattern, replacement, replacementText, vm.aotState().regExpMatchOutput.get()));
    int start = static_cast<int>(oneBehindStartOfFirstMatch - 1);
    std::array offsets { start, start + static_cast<int>(pattern->atom().length()) };
    OPERATION_RETURN(scope, replaceFromFirstMatch(globalObject, vm, subject, pattern, replacement, replacementText, offsets.data()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceRegExpWithFunction, EncodedJSValue, (Instance* instance, JSCell* string, JSCell* regExp, JSCell* replacement, JSCell* hostFunction))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceRegExpWithFunction);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringReplaceRegExpWithFunction);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringReplaceRegExpWithFunction);
    RegExp* pattern = regExpObject->regExp();
    if (!pattern->sticky() && regExpObject->getLastIndex().isNumber() && (!pattern->global() || regExpObject->lastIndexIsWritable())) {
        auto input = subject->view(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        MatchResult first = pattern->matchInline<Yarr::MatchFrom::VMThread>(globalObject, vm, input, 0);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!first) {
            if (pattern->global())
                regExpObject->setLastIndex(globalObject, 0);
            OPERATION_RETURN(scope, JSValue::encode(subject));
        }
    }
    auto callData = JSC::getCallData(hostFunction);
    ASSERT(callData.type == CallData::Type::Native);
    auto args = WTF::toArray<EncodedJSValue>({
        JSValue::encode(regExpObject),
        JSValue::encode(replacement),
    });
    OPERATION_RETURN(scope, JSValue::encode(call(globalObject, hostFunction, callData, subject, ArgList { args.data(), args.size() })));
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
    if (regExpObject->isSymbolMatchFastAndNonObservable(globalObject)) [[likely]] {
        JSValue value = matchEveryPlaceInOnePass(globalObject, vm, regExpObject, subject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (value)
            OPERATION_RETURN(scope, JSValue::encode(value));
        OPERATION_RETURN(scope, JSValue::encode(regExpMatchFast(globalObject, regExpObject, subject)));
    }
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

WTF_MAKE_TZONE_ALLOCATED_IMPL(RegExpLiterals);

RegExpRecords::RegExpRecords(const Image& image)
    : m_records(image.section<ImageRegExp>(ImageSection::RegExps))
{
}

const ImageRegExp* findRegExpRecord(VM& vm, const String& pattern, OptionSet<Yarr::Flags> flags)
{
    if (!vm.hasCompiledCode())
        return nullptr;

    uint16_t rawFlags = ImageRegExp::significantFlags(flags).toRaw();
    // The records of two images for one pattern say the same.
    for (const Image* each = &vm.aotState().image->first(); each; each = each->above()) {
        const Image& image = *each;
        RegExpRecords records(image);
        for (const ImageRegExpHash& entry : std::ranges::equal_range(image.section<ImageRegExpHash>(ImageSection::RegExpHashes), ImageRegExp::hashOf(pattern.hash(), flags), { }, &ImageRegExpHash::hash)) {
            const ImageRegExp& record = records.at(RegExpRecordID(entry.record));
            if (record.rawFlags == rawFlags && image.text().equals(StringID(record.pattern), pattern))
                return &record;
        }
    }
    count(vm, RuntimeCounterID::RegExpsCreatedWithoutRecord);
    return nullptr;
}

RegExpEntrypoints regExpEntrypointsOf(VM& vm, const ImageRegExp& record)
{
    RELEASE_ASSERT(record.form() == RegExpRecordForm::Code);
    std::span<const uint8_t> code = vm.aotState().image->holderOf(&record).regExpCode();
    return { code.subspan(record.codeFor8Bit).data(), code.subspan(record.codeFor16Bit).data() };
}

static std::span<const uint8_t> detailsOf(VM& vm, const ImageRegExp& record)
{
    return vm.aotState().image->holderOf(&record).section<uint8_t>(ImageSection::RegExpDetails).subspan(record.details);
}

Yarr::CharacterSetView regExpCharacterSetOf(VM& vm, const ImageRegExp& record)
{
    RELEASE_ASSERT(record.hasCharacterSet());
    std::span<const uint8_t> rest = detailsOf(vm, record);
    const auto& set = consumeAndReinterpretCastTo<ImageRegExpCharacterSet>(rest);
    return Yarr::CharacterSetView(set.bits, spanReinterpretCast<const Yarr::CharacterRange>(rest.first(set.bits.wideRangeCount * sizeof(Yarr::CharacterRange))));
}

String regExpPatternOf(VM& vm, const ImageRegExp& record)
{
    return vm.aotState().image->holderOf(&record).text().createString(StringID(record.pattern));
}

String regExpAtomOf(VM& vm, const ImageRegExp& record)
{
    RELEASE_ASSERT(record.form() == RegExpRecordForm::Atom);
    return vm.aotState().image->holderOf(&record).text().createString(StringID(record.atom));
}

RegExpNamedGroups decodeRegExpNamedGroups(VM& vm, const ImageRegExp& record)
{
    RELEASE_ASSERT(record.hasNamedGroups);
    const ImageText& text = vm.aotState().image->holderOf(&record).text();
    std::span<const uint8_t> rest = detailsOf(vm, record);
    const auto& header = consumeAndReinterpretCastTo<ImageRegExpNamedGroups>(rest);
    auto words = spanReinterpretCast<const uint32_t>(rest.first(header.numberOfWords * sizeof(uint32_t)));

    RegExpNamedGroups groups;
    groups.numDuplicateNamedCaptureGroups = header.numDuplicateNamedCaptureGroups;
    groups.captureGroupNames = FixedVector<AtomString>(header.numberOfCaptureGroupNames);
    for (AtomString& name : groups.captureGroupNames) {
        if (uint32_t stringPlusOne = consume(words))
            name = AtomString(text.createAtom(StringID(stringPlusOne - 1)));
    }
    for (uint32_t group = 0; group < header.numberOfNamedGroups; ++group) {
        String name = text.createAtom(StringID(consume(words)));
        uint32_t numberOfIndices = consume(words);
        groups.namedGroupToParenIndices.add(WTF::move(name), Vector<unsigned>(consumeSpan(words, numberOfIndices)));
    }
    RELEASE_ASSERT(words.empty());
    return groups;
}

template<typename Visitor>
void RegExpLiterals::visitRoots(Visitor& visitor)
{
    // A cell lasts as long as the program. One that a collection has marked is old, and only the next full collection has to mark it again.
    size_t first = 0;
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        if (visitor.collectionScope() == CollectionScope::Full && m_lastFullVisitVersion != visitor.markingVersion())
            m_lastFullVisitVersion = visitor.markingVersion();
        else
            first = m_numberOfCellsVisited;
        m_numberOfCellsVisited = m_cellsInOrderOfCreation.size();
    }
    for (RegExp* regExp : m_cellsInOrderOfCreation.span().subspan(first))
        visitor.appendUnbarriered(regExp);
}

RegExpLiterals::RegExpLiterals(CacheRegistry& registry)
{
    registry.add(CacheKind::StrongRoots, "regexp literals"_s, *this, RootVolatility::GreyedByExecution);
}

RegExp* RegExpLiterals::regExpFor(VM& vm, RegExpRecordID id, OptionSet<Yarr::Flags> flags)
{
    if (!m_records) [[unlikely]]
        m_records = &RegExpRecords(*vm.aotState().image).at(id) - id.index();
    const ImageRegExp& record = m_records[id.index()];
    static_assert(sizeof(flags.toRaw()) <= sizeof(uint32_t));
    uint64_t key = static_cast<uint64_t>(id.index()) << 32 | flags.toRaw();
    if (auto known = m_cells.find(key); known != m_cells.end())
        return known->value;

    RegExp* regExp = RegExp::createFromImage(vm, regExpPatternOf(vm, record), flags, record);
    m_cells.add(key, regExp);
    m_cellsInOrderOfCreation.append(regExp);
    return regExp;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
