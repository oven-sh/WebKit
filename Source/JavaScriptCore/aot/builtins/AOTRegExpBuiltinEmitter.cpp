/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTRegExpFlags.h"
#include "AOTResolvedCallEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsRegExp.h"
#include "AOTTypeArena.h"
#include "RegExp.h"
#include "RegExpObject.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t defaultSplitLimit = UINT32_MAX;

static bool takesRegExp(const InlineCall& call)
{
    return call.parameter<SearchValueKind>(0) == SearchValueKind::RegExp;
}

template<typename T>
T RegExpBuiltinEmitter::matchInPlace(const InlineCall& call, RegExpMatch match, RegExpMatchAnswer answer, T miss, const ScopedLambda<T()>& matchByCall)
{
    if (!call.numberOfParameters())
        return matchByCall();
    match.pattern = StringID::fromBits(call.parameterWord(0));
    match.flags = OptionSet<RegExpFlags>::fromRaw(static_cast<uint8_t>(call.parameterWord(1)));
    match.isHeld = true;
    match.calledBuiltin = functionOf(call.overload());
    match.isPrivate = call.parameterWord(1) & privateRegExpParameter;
    ResolvedCallEmitter& resolved = emitter<ResolvedCallEmitter>();
    const RegExpMatchPlan* plan = resolved.matchPlanOf(call.context().block(), match);
    return plan ? resolved.matchInPlace<T>(call.context(), *plan, match, answer, miss, matchByCall) : matchByCall();
}

RegExpBuiltinEmitter::RegExpBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

TypedPointer RegExpBuiltinEmitter::fieldAt(LValue cell, ptrdiff_t offset)
{
    return m_out.address(m_heaps.entire(AbstractHeap::RegExpState), cell, offset);
}

Pointer RegExpBuiltinEmitter::regExpOf(CellPointer regExpObject)
{
    return Pointer(m_out.bitAnd(m_out.load64(fieldAt(regExpObject.value(), RegExpObject::offsetOfRegExpAndFlags())), RegExpObject::regExpMask));
}

Word32 RegExpBuiltinEmitter::flagsOf(CellPointer regExpObject)
{
    static_assert(sizeof(OptionSet<Yarr::Flags>) == sizeof(uint16_t));
    return Word32(m_out.load16ZeroExt32(fieldAt(regExpOf(regExpObject).value(), RegExp::offsetOfFlags())));
}

void RegExpBuiltinEmitter::generateRegExpGetLastIndex(const InlineCall& call)
{
    CellPointer regExpObject = cellOperand(call.receiver());
    finish(call, Boxed(m_out.load64(fieldAt(regExpObject.value(), RegExpObject::offsetOfLastIndex()))));
}

void RegExpBuiltinEmitter::generateNewRegExp(const InlineCall& call)
{
    OptionSet<Yarr::Flags> flags;
    if (ValueID flagsArgument = call.argumentOrNull(1)) {
        StringID text = singleStringLiteralOf(typeOf(flagsArgument));
        std::optional<OptionSet<Yarr::Flags>> parsed = text ? Yarr::parseFlags(emitter<StringEmitter>().textOf(text)) : std::nullopt;
        if (!parsed) {
            fail(call.context(), InternalErrorKind::OpcodeWithoutCode);
            finish(call, emitter<ConstantEmitter>().zeroOf(resultRepresentationOf(call)), resultTypeOf(call));
            return;
        }
        flags = *parsed;
    }
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::NewRegExp, { cellOperand(call.argument(0)), word32(flags.toRaw()) }, { .calledBuiltin = functionOf(call.overload()) }), types().at(types().builtinClass(BuiltinClasses::RegExp)));
}

void RegExpBuiltinEmitter::generateRegExpExec(const InlineCall& call)
{
    CellPointer regExpObject = cellOperand(call.receiver());
    CellPointer string = cellOperand(call.argument(0));
    RegExpMatch match { .string = string, .regExpObject = regExpObject };
    finish(call, matchInPlace<Boxed>(call, match, RegExpMatchAnswer::Array, boxedConstant(jsNull()), [&] {
        return Boxed(callStub(call.context(), regExpMatch(RegExpMatchResult::Array), { regExpObject.value(), string.value() }, { .calledBuiltin = functionOf(call.overload()) }).first);
    }));
}

void RegExpBuiltinEmitter::generateRegExpTest(const InlineCall& call)
{
    CellPointer regExpObject = cellOperand(call.receiver());
    CellPointer string = cellOperand(call.argument(0));
    RegExpMatch match { .string = string, .regExpObject = regExpObject };
    if (resultRepresentationOf(call) == Representation::JSValue) {
        finish(call, matchInPlace<Boxed>(call, match, RegExpMatchAnswer::Boolean, boxedConstant(jsBoolean(false)), [&] {
            return Boxed(callStub(call.context(), regExpMatch(RegExpMatchResult::BoxedBoolean), { regExpObject.value(), string.value() }, { .calledBuiltin = functionOf(call.overload()) }).first);
        }), m_program.at(TypeTable::boolean));
        return;
    }
    finish(call, matchInPlace<Condition>(call, match, RegExpMatchAnswer::Boolean, boolean(false), [&] {
        LValue isMatch = callStub(call.context(), regExpMatch(RegExpMatchResult::Boolean), { regExpObject.value(), string.value() }, { .calledBuiltin = functionOf(call.overload()) }).first;
        return Condition(m_out.castToInt32(isMatch));
    }));
}

void RegExpBuiltinEmitter::generateRegExpFlags(const InlineCall& call)
{
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::RegExpFlags, { cellOperand(call.receiver()) }), m_program.at(TypeTable::string));
}

void RegExpBuiltinEmitter::generateRegExpSource(const InlineCall& call)
{
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::RegExpSource, { cellOperand(call.receiver()) }), m_program.at(TypeTable::string));
}

void RegExpBuiltinEmitter::generateRegExpFlag(const InlineCall& call)
{
    Yarr::Flags flag = toYarrFlag(call.parameter<RegExpFlags>(0));
    finish(call, hasBits(flagsOf(cellOperand(call.receiver())), static_cast<uint32_t>(flag)));
}

void RegExpBuiltinEmitter::generateStringReplace(const InlineCall& call)
{
    CellPointer string = cellOperand(call.receiver());
    ValueID pattern = call.argument(0);
    ValueID replacement = call.argument(1);
    const Type& stringType = m_program.at(TypeTable::string);
    if (!takesRegExp(call)) {
        finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::StringReplaceString, { string, cellOperand(pattern), cellOperand(replacement) }), stringType);
        return;
    }
    CellPointer regExpObject = cellOperand(pattern);
    if (typeOf(replacement).isAnyString()) {
        LValue replaced = callStub(call.context(), stringReplaceRegExpMayNotMatch(), { string.value(), regExpObject.value(), cellOperand(replacement).value() }, { .calledBuiltin = functionOf(call.overload()) }).first;
        finish(call, CellPointer(replaced), stringType);
        return;
    }
    LValue replaced = callStub(call.context(), stringReplaceRegExpMayNotMatch(ReplacementOfMatches::AnyValue), { string.value(), regExpObject.value(), boxedOperand(call.context(), replacement).value() }, { .calledBuiltin = functionOf(call.overload()) }).first;
    finish(call, CellPointer(replaced), stringType);
}

void RegExpBuiltinEmitter::generateStringReplaceAll(const InlineCall& call)
{
    CellPointer string = cellOperand(call.receiver());
    CellPointer pattern = cellOperand(call.argument(0));
    ValueID replacement = call.argument(1);
    const Type& stringType = m_program.at(TypeTable::string);
    if (takesRegExp(call)) {
        finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::StringReplaceAllRegExp, { string, pattern, boxedOperand(call.context(), replacement) }, { .calledBuiltin = functionOf(call.overload()) }), stringType);
        return;
    }
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::StringReplaceAllString, { string, pattern, cellOperand(replacement) }), stringType);
}

void RegExpBuiltinEmitter::generateStringSplit(const InlineCall& call)
{
    CellPointer string = cellOperand(call.receiver());
    CellPointer separator = cellOperand(call.argument(0));
    Word32 limit = word32(static_cast<int32_t>(defaultSplitLimit));
    ValueID limitArgument = call.argumentOrNull(1);
    if (limitArgument && !typeOf(limitArgument).hasOnlyPrimitives(Primitive::Undefined)) {
        limit = int32Operand(limitArgument);
        if (typeOf(limitArgument).integers.minimum < 0)
            missUnless(call, greaterThanOrEqual(limit, word32(0)));
    }
    RuntimeOperation operation = takesRegExp(call) ? RuntimeOperation::StringSplitRegExp : RuntimeOperation::StringSplitString;
    finish(call, callOperation<CellPointer>(call.context(), operation, { string, separator, limit }, { .calledBuiltin = takesRegExp(call) ? functionOf(call.overload()) : BuiltinID() }), types().at(types().builtinClass(BuiltinClasses::Array)));
}

void RegExpBuiltinEmitter::generateStringMatch(const InlineCall& call)
{
    CellPointer string = cellOperand(call.receiver());
    CellPointer regExpObject = cellOperand(call.argument(0));
    RegExpMatch match { .string = string, .regExpObject = regExpObject, .isArgument = true };
    finish(call, matchInPlace<Boxed>(call, match, RegExpMatchAnswer::Array, boxedConstant(jsNull()), [&] {
        return Boxed(callStub(call.context(), regExpMatch(RegExpMatchResult::ArrayForStringMatch), { string.value(), regExpObject.value() }, { .calledBuiltin = functionOf(call.overload()) }).first);
    }));
}

void RegExpBuiltinEmitter::generateStringSearch(const InlineCall& call)
{
    CellPointer string = cellOperand(call.receiver());
    CellPointer regExpObject = cellOperand(call.argument(0));
    RegExpMatch match { .string = string, .regExpObject = regExpObject, .isArgument = true, .resetsLastIndex = true };
    finish(call, matchInPlace<Boxed>(call, match, RegExpMatchAnswer::Index, boxedConstant(jsNumber(-1)), [&] {
        return Boxed(callStub(call.context(), regExpMatch(RegExpMatchResult::IndexForStringSearch), { string.value(), regExpObject.value() }, { .calledBuiltin = functionOf(call.overload()) }).first);
    }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
