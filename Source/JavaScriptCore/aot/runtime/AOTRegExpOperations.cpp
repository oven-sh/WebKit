/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramStatics.h"
#include "AOTRegExpLiterals.h"
#include "AOTRegExpRecords.h"
#include "AOTState.h"
#include "AOTStringBuiltinOperationData.h"
#include "AOTStubsRegExp.h"
#include "AOTVMState.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "InterpreterInlines.h"
#include "JSCInlines.h"
#include "RegExpConstructor.h"
#include "RegExpMatchesArray.h"
#include "RegExpObjectInlines.h"
#include "RegExpPrototype.h"
#include "StringPrototypeInlines.h"
#include "YarrFlags.h"
#include "YarrFlatProgram.h"
#include <algorithm>
#include <span>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

struct FoundMatch {
    RegExp* regExp;
    JSString* subject;
    MatchResult result;
    std::span<const int> offsets;
};

} // anonymous namespace

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

// What test answers for a RegExp without the flags g and y. RegExp.$1 and its like stay what the last match of the program made them.
JSC_DEFINE_JIT_OPERATION(operationAOTMatchesUnrecorded, size_t, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMatchesUnrecorded);
    auto input = cellArgument<JSString>(string, Entry::operationAOTMatchesUnrecorded)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, !!cellArgument<RegExpObject>(regExp, Entry::operationAOTMatchesUnrecorded)->regExp()->matchInline<Yarr::MatchFrom::VMThread>(globalObject, vm, input, 0));
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

static RegExp* resolveMatchedRegExp(VM& vm, RegExpObject* object, RegExpRecordID record)
{
    if (!object) {
        RELEASE_ASSERT(record);
        return vm.aotState().regExpLiterals->regExpFor(vm, record, RegExpRecords(*vm.aotState().image).at(record).flags());
    }
    RegExp* regExp = object->regExp();
    ASSERT(!record || regExp->imageRecord() == &RegExpRecords(*vm.aotState().image).at(record));
    return regExp;
}

static JSArray* createMatchesArray(Instance* instance, JSGlobalObject* globalObject, VM& vm, const FoundMatch& match)
{
    unsigned numSubpatterns = match.regExp->numSubpatterns();
    bool hasNamedCaptures = match.regExp->hasNamedCaptures();
    bool createIndices = match.regExp->hasIndices();
    if (hasNamedCaptures || createIndices) [[unlikely]]
        return createRegExpMatchesArrayWithGroupsOrIndices(vm, globalObject, match.subject, match.regExp, match.result, match.offsets, numSubpatterns, hasNamedCaptures, createIndices);
    if (globalObject->isHavingABadTime()) [[unlikely]]
        return createRegExpMatchesArrayForPlainRegExpHavingABadTime(vm, globalObject, match.subject, match.result, match.offsets, numSubpatterns);
    prepareInlineAllocation<JSArray>(instance, InlineAllocation::RegExpMatchesArray, globalObject->regExpMatchesArrayStructure());
    return createRegExpMatchesArrayForPlainRegExp(vm, globalObject, match.subject, match.result, match.offsets, numSubpatterns);
}

static JSValue createMatchValue(Instance* instance, JSGlobalObject* globalObject, VM& vm, RegExpMatchForm form, const FoundMatch& match)
{
    switch (form) {
    case RegExpMatchForm::Test:
        return jsBoolean(true);
    case RegExpMatchForm::Exec:
        return createMatchesArray(instance, globalObject, vm, match);
    case RegExpMatchForm::Search:
        return jsNumber(match.result.start);
    case RegExpMatchForm::Match:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool advancesLastIndex(RegExpMatchForm form, RegExp* regExp)
{
    switch (form) {
    case RegExpMatchForm::Test:
    case RegExpMatchForm::Exec:
        return regExp->globalOrSticky();
    case RegExpMatchForm::Search:
        return false;
    case RegExpMatchForm::Match:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What String.prototype.match answers for a global RegExp whose first match in the subject is known.
static JSValue collectMatchesFrom(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* subject, MatchResult match)
{
    // Beyond this RegExpObject::matchGlobal() counts the matches before it creates them.
    constexpr unsigned maxMatchesCreatedUncounted = 100000;
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
        uint64_t start = match.empty() ? advanceStringIndex(source, source->length(), match.end, regExp->eitherUnicode()) : match.end;
        if (start > source->length())
            break;
        int* offsets;
        match = globalObject->regExpGlobalData().performMatch(globalObject, regExp, subject, source, static_cast<int>(start), &offsets);
        RETURN_IF_EXCEPTION(scope, { });
    } while (match);
    return array;
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpMatchLiteral, EncodedJSValue, (Instance* instance, JSCell* regExp, JSCell* string, uint32_t regExpRecord, uint32_t form))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpMatchLiteral);
    RegExpObject* object = regExp ? cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpMatchLiteral) : nullptr;
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTRegExpMatchLiteral);
    RegExp* cell = resolveMatchedRegExp(vm, object, RegExpRecordID(regExpRecord));

    ASSERT(static_cast<size_t>(cell->offsetVectorSize()) <= std::max<size_t>(vm.aotState().image->header().counts.maxRegExpOutputSize, 2 * (VMState::maxGroupsInRegExpMatchOutput + 1)));
    auto offsets = unsafeMakeSpan(vm.aotState().regExpMatchOutput.get(), static_cast<size_t>(cell->offsetVectorSize()));
    MatchResult result(offsets[0], offsets[1]);
    RELEASE_ASSERT(result.end >= result.start);

    auto matchForm = static_cast<RegExpMatchForm>(form);
    if (matchForm == RegExpMatchForm::Match) {
        if (cell->global())
            OPERATION_RETURN(scope, JSValue::encode(collectMatchesFrom(globalObject, vm, object, subject, result)));
        matchForm = RegExpMatchForm::Exec;
    }

    JSValue value = createMatchValue(instance, globalObject, vm, matchForm, { cell, subject, result, offsets });
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!object)
        OPERATION_RETURN(scope, JSValue::encode(value));

    if (advancesLastIndex(matchForm, cell)) {
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
    RegExp* regExp = resolveMatchedRegExp(vm, nullptr, RegExpRecordID(regExpRecord));
    RELEASE_ASSERT(!regExp->globalOrSticky());

    auto input = subject->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto offsets = regExp->ovectorSpan();
    int position = regExp->matchInline<Yarr::MatchFrom::VMThread>(globalObject, vm, input, 0, offsets);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());

    auto matchForm = static_cast<RegExpMatchForm>(form);
    if (position == -1)
        OPERATION_RETURN(scope, JSValue::encode(valueOfFailedMatch(matchForm)));

    MatchResult result(position, offsets[1]);
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, subject, result, false);
    if (matchForm == RegExpMatchForm::Match)
        matchForm = RegExpMatchForm::Exec;
    OPERATION_RETURN(scope, JSValue::encode(createMatchValue(instance, globalObject, vm, matchForm, { regExp, subject, result, offsets })));
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

static size_t findPrefixOfGlobalMatch(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* string, uint32_t count, EncodedJSValue* elements)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = regExpMatchFast(globalObject, object, string);
    RETURN_IF_EXCEPTION(scope, 0);
    if (result.isNull())
        return noMatchPrefixLength;
    auto* array = uncheckedDowncast<JSArray>(result);
    unsigned length = array->length();
    for (unsigned i = 0; i < std::min(count, length); ++i) {
        JSValue piece = array->getIndex(globalObject, i);
        RETURN_IF_EXCEPTION(scope, 0);
        elements[i] = JSValue::encode(piece);
    }
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
    if (!regExp->isValid()) [[unlikely]] {
        throwException(globalObject, scope, regExp->errorToThrow(globalObject));
        OPERATION_RETURN(scope, nullptr);
    }
    if (staticsOffset != noSharedRegExpOffset && !instance->state->statics->at<JSCell*>(staticsOffset))
        setSharedLiteral(vm, instance, staticsOffset, RegExpObject::create(vm, globalObject->regExpStructure(), regExp));
    prepareInlineAllocation<RegExpObject>(instance, InlineAllocation::RegExpObject, globalObject->regExpStructure());
    OPERATION_RETURN(scope, RegExpObject::create(vm, globalObject->regExpStructure(), regExp));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSharedRegExpLiteral, JSCell*, (Instance* instance, uint32_t staticsOffset, uint32_t regExpRecord, uint32_t flags))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSharedRegExpLiteral);
    DeferTerminationForAWhile deferTermination(vm);
    DeferGCForAWhile deferGC(vm);
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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
