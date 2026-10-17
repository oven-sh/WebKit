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

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringReplaceRegExpString, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, JSCell* replacement))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* regExpObject = uncheckedDowncast<RegExpObject>(regExp);
    if (!regExpObject->isSymbolReplaceFastAndNonObservable(globalObject)) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    CallData callData;
    auto replacementText = uncheckedDowncast<JSString>(replacement)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, replaceUsingRegExpSearch(vm, globalObject, uncheckedDowncast<JSString>(string), regExpObject, callData, replacementText, replacement));
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
