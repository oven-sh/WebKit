/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTRegExpFlags.h"
#include "AOTStringEmitter.h"
#include "AOTStubsRegExp.h"
#include "AOTValueIdentity.h"
#include "RegExp.h"
#include "RegExpObject.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t defaultSplitLimit = UINT32_MAX;

static bool takesRegExp(const InlineCall& call)
{
    return call.parameter<SearchValueKind>(0) == SearchValueKind::RegExp;
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

void RegExpBuiltinEmitter::storeLastIndex(CellPointer regExpObject, Boxed value, LBasicBlock ifNotWritable)
{
    LValue regExpAndFlags = m_out.load64(fieldAt(regExpObject.value(), RegExpObject::offsetOfRegExpAndFlags()));
    branchUnless(Condition(m_out.testIsZero64(regExpAndFlags, m_out.constInt64(RegExpObject::lastIndexIsNotWritableFlag))), ifNotWritable);
    m_out.store64(value.value(), fieldAt(regExpObject.value(), RegExpObject::offsetOfLastIndex()));
}

void RegExpBuiltinEmitter::generateRegExpGetLastIndex(const InlineCall& call)
{
    CellPointer regExpObject = cellOperand(call.receiver());
    finish(call, Boxed(m_out.load64(fieldAt(regExpObject.value(), RegExpObject::offsetOfLastIndex()))));
}

void RegExpBuiltinEmitter::generateRegExpSetLastIndex(const InlineCall& call)
{
    CellPointer regExpObject = cellOperand(call.receiver());
    Boxed value = boxedOperand(call.context(), call.argument(0));
    LBasicBlock notWritable = newBlock();
    LBasicBlock continuation = newBlock();
    storeLastIndex(regExpObject, value, notWritable);
    ifElse(isNotCell(value), [] { }, [&] {
        emitter<BarrierEmitter>().barrier(call.context(), regExpObject);
    });
    m_out.jump(continuation);

    m_out.appendTo(notWritable);
    emitter<AccessEmitter>().putById(call.context(), boxCell(regExpObject), m_program.keyOf(BuiltinNames::lastIndex), value, PutMode::Strict, SiteCacheID());
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    finishWithUndefined(call);
}

void RegExpBuiltinEmitter::generateNewRegExp(const InlineCall& call)
{
    OptionSet<Yarr::Flags> flags;
    if (ValueID flagsArgument = call.argumentOrNull(1)) {
        StringID text = typeOf(sameObjectThroughCasts(m_function, flagsArgument).valueInFrontOfFacts()).singleStringLiteral();
        std::optional<OptionSet<Yarr::Flags>> parsed = text ? Yarr::parseFlags(emitter<StringEmitter>().textOf(text)) : std::nullopt;
        if (!parsed) {
            fail(call.context(), InternalErrorKind::OpcodeWithoutCode);
            finish(call, emitter<ConstantEmitter>().zeroOf(resultRepresentationOf(call)), resultTypeOf(call));
            return;
        }
        flags = *parsed;
    }
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::NewRegExp, { cellOperand(call.argument(0)), word32(flags.toRaw()) }, { .calledBuiltin = functionOf(call.overload()) }), Type::builtinClass(BuiltinClasses::RegExp));
}

void RegExpBuiltinEmitter::generateRegExpExec(const InlineCall& call)
{
    CellPointer regExpObject = cellOperand(call.receiver());
    CellPointer string = cellOperand(call.argument(0));
    finish(call, Boxed(callStub(call.context(), regExpMatch(RegExpMatchResult::Array), { regExpObject.value(), string.value() }).first));
}

void RegExpBuiltinEmitter::generateRegExpTest(const InlineCall& call)
{
    CellPointer regExpObject = cellOperand(call.receiver());
    CellPointer string = cellOperand(call.argument(0));
    if (resultRepresentationOf(call) == Representation::JSValue) {
        Boxed isMatch(callStub(call.context(), regExpMatch(RegExpMatchResult::BoxedBoolean), { regExpObject.value(), string.value() }).first);
        finish(call, isMatch, m_program.at(TypeTable::boolean));
        return;
    }
    LValue isMatch = callStub(call.context(), regExpMatch(RegExpMatchResult::Boolean), { regExpObject.value(), string.value() }).first;
    finish(call, Condition(m_out.castToInt32(isMatch)));
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
        LValue replaced = callStub(call.context(), stringReplaceRegExpMayNotMatch(), { string.value(), regExpObject.value(), cellOperand(replacement).value() }).first;
        finish(call, CellPointer(replaced), stringType);
        return;
    }
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::StringReplaceRegExp, { string, regExpObject, boxedOperand(call.context(), replacement) }, { .calledBuiltin = functionOf(call.overload()) }), stringType);
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
    finish(call, callOperation<CellPointer>(call.context(), operation, { string, separator, limit }), Type::builtinClass(BuiltinClasses::Array));
}

void RegExpBuiltinEmitter::generateStringMatch(const InlineCall& call)
{
    CellPointer string = cellOperand(call.receiver());
    CellPointer regExpObject = cellOperand(call.argument(0));
    finish(call, Boxed(callStub(call.context(), stringMatchRegExp(), { string.value(), regExpObject.value() }).first));
}

void RegExpBuiltinEmitter::generateStringSearch(const InlineCall& call)
{
    CellPointer string = cellOperand(call.receiver());
    CellPointer regExpObject = cellOperand(call.argument(0));
    finish(call, callOperation<Boxed>(call.context(), RuntimeOperation::StringSearchRegExp, { string, regExpObject }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
