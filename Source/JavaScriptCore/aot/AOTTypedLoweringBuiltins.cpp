/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

// This goes case by case with lowerInline(), lowerContainerBuiltin(), lowerStringBuiltin() and lowerMapOrSetBuiltin(): it says for which
// arguments there is code there.
std::optional<Writes> writesOfInlineBuiltin(InlineBuiltin which, std::span<const Type> arguments)
{
    bool areNumbers = true;
    for (auto& argument : arguments)
        areNumbers &= argument.isNumber();
    auto when = [](bool isInline, Writes writes) -> std::optional<Writes> {
        if (!isInline)
            return std::nullopt;
        return writes;
    };
    switch (which) {
    case InlineBuiltin::MathAbs:
    case InlineBuiltin::MathFloor:
    case InlineBuiltin::MathCeil:
    case InlineBuiltin::MathTrunc:
    case InlineBuiltin::MathSqrt:
    case InlineBuiltin::MathClz32:
        return when(arguments.size() == 1 && areNumbers, Writes::Pure);
    case InlineBuiltin::MathPow:
    case InlineBuiltin::MathMin:
    case InlineBuiltin::MathMax:
    case InlineBuiltin::MathImul:
        return when(arguments.size() == 2 && areNumbers, Writes::Pure);
    case InlineBuiltin::IsNaN:
    case InlineBuiltin::IsFinite:
    case InlineBuiltin::NumberIsNaN:
    case InlineBuiltin::NumberIsFinite:
    case InlineBuiltin::NumberIsInteger:
    case InlineBuiltin::NumberIsSafeInteger: {
        bool converts = which == InlineBuiltin::IsNaN || which == InlineBuiltin::IsFinite;
        bool asksForNaN = which == InlineBuiltin::IsNaN || which == InlineBuiltin::NumberIsNaN;
        if (!converts && !(arguments[0].primitives & Primitive::Number))
            return Writes::Pure;
        switch (repOf(arguments[0])) {
        case Rep::Int64:
        case Rep::Double:
            return Writes::Pure;
        case Rep::OptionalInt64:
            return when(!converts, Writes::Pure);
        // What stands for `undefined` and `null` among doubles is not a number, so it is not finite either. Only to the
        // question whether something is the number that is not one does it give the wrong answer.
        case Rep::OptionalDouble:
            return when(!converts && !asksForNaN, Writes::Pure);
        default:
            return std::nullopt;
        }
    }
    case InlineBuiltin::ToNumber:
        return when(areNumbers, Writes::Pure);
    case InlineBuiltin::This:
        return Writes::Pure;
    case InlineBuiltin::StringLength:
    case InlineBuiltin::ArrayLength:
    case InlineBuiltin::MapSize:
    case InlineBuiltin::SetSize:
        return Writes::Nothing;
    // Looking at the characters flattens a rope.
    case InlineBuiltin::StringAt:
    case InlineBuiltin::StringCharAt:
    case InlineBuiltin::StringCharCodeAt:
    case InlineBuiltin::StringCodePointAt:
        return when(arguments.size() == 1 && areNumbers, Writes::RuntimeState);
    case InlineBuiltin::StringSlice:
    case InlineBuiltin::StringSubstring:
    case InlineBuiltin::StringSubstr:
        return when(!arguments.empty() && areNumbers, Writes::RuntimeState);
    case InlineBuiltin::StringStartsWith:
    case InlineBuiltin::StringEndsWith:
    case InlineBuiltin::StringIncludes:
    case InlineBuiltin::StringIndexOf:
        return when(arguments[0].isString() && (arguments.size() == 1 || arguments[1].isNumber()), Writes::RuntimeState);
    case InlineBuiltin::ArrayPush:
        return when(arguments.size() == 1, Writes::Heap);
    case InlineBuiltin::ArraySetLength:
        return Writes::Heap;
    case InlineBuiltin::ArrayWithLength:
    case InlineBuiltin::ArrayOf:
    case InlineBuiltin::TypedArrayWithLength:
    case InlineBuiltin::MapNew:
    case InlineBuiltin::MapSet:
    case InlineBuiltin::SetAdd:
    // Where the length depends on that of the buffer, the engine is asked.
    case InlineBuiltin::TypedArrayLength:
        return Writes::RuntimeState;
    // What a proxy stands for is asked of the engine, which runs no code of the program for it.
    case InlineBuiltin::ArrayIsArray:
        return arguments[0].hasObjects() ? Writes::RuntimeState : Writes::Pure;
    // Hashing a string flattens a rope.
    case InlineBuiltin::MapGet:
    case InlineBuiltin::MapHas:
    case InlineBuiltin::SetHas:
        return arguments[0].primitives & Primitive::String ? Writes::RuntimeState : Writes::Nothing;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

OperationEffects Lowering::effectsOfBuiltinCall(const Value& value)
{
    auto& builtin = m_builtins.at(value.builtin());
    Writes writes = writesOfBuiltinCall(m_program, m_function, value);
    // Nothing is asked of `undefined` or `null`: that throws.
    bool exitsSideways = writes == Writes::Heap || builtin.effects & Effect::Throws || (builtin.takesReceiver() && typeOf(value.child(0)).mayBeUndefinedOrNull());
    return { writes, exitsSideways };
}

// See Builtins::findCell().
CellPointer Lowering::builtinCell(unsigned index)
{
    return lazilyInitialized<CellPointer>(Instance::offsetOfTypedBuiltins(), index, [&] {
        return AOT_TYPED_OPERATION(BuiltinCell, word32(index));
    });
}

// Calls the function the entry describes. It is not looked for in the receiver, which could have anything under the name.
Boxed Lowering::callFunctionOfEngine(const Value& value, OperationEffects effects)
{
    auto& builtin = m_builtins.at(value.builtin());
    // The typed arrays have their functions in common. Which of them one was found on is what it goes by.
    auto globalOr = [&](BuiltinName name) {
        if (value.engineClass() != Format::none)
            name = m_builtins.classes()[value.engineClass()].name;
        return box(builtinCell(m_builtins.cellOf(name)));
    };
    Boxed callee;
    Boxed thisValue = undefined();
    switch (builtin.kind) {
    case BuiltinKind::Method:
    case BuiltinKind::Getter:
    case BuiltinKind::Setter:
        callee = box(builtinCell(m_builtins.cellOf(value.builtin())));
        if (typeOf(value.child(0)).hasCells())
            thisValue = box(nonNil(value.child(0), m_program.stringOf(builtin.nameName)));
        else
            thisValue = in<Boxed>(value.child(0));
        break;
    case BuiltinKind::StaticFunction:
        callee = box(builtinCell(m_builtins.cellOf(value.builtin())));
        thisValue = globalOr(builtin.holderName);
        break;
    case BuiltinKind::Function:
        callee = globalOr(builtin.nameName);
        break;
    case BuiltinKind::Constructor: {
        auto [count, list] = argumentBuffer(value.arguments);
        return AOT_TYPED_OPERATION_WITH_EFFECTS(Construct, effects, globalOr(builtin.nameName), count, list);
    }
    case BuiltinKind::Constant:
    case BuiltinKind::Operation:
        RELEASE_ASSERT_NOT_REACHED();
    }
    // Then there is nothing to tell the back end, and the stub is the fastest way.
    if (effects.writes == Writes::Heap)
        return callGenerally(callee, thisValue, value.arguments);
    auto [count, list] = argumentBuffer(value.arguments);
    return AOT_TYPED_OPERATION_WITH_EFFECTS(Call, effects, callee, thisValue, count, list);
}

// A method on what is an array for a fact. An array that is no longer as it was made may find something else under the name.
Operand Lowering::callMethodOfArray(const Value& value, const Type& type)
{
    OperationEffects effects = effectsOfBuiltinCall(value);
    StringID name = m_program.stringOf(m_builtins.at(value.builtin()).nameName);
    CellPointer array = nonNil(value.child(0), name);
    auto ofEngine = [&](Boxed yielded) {
        if (vouchesForItsType(m_program, m_function, value))
            return convert(yielded, type, repOf(type));
        return ensure(yielded, type);
    };
    // The operation looks at the array, and does not come back from one that would have it run code of the program.
    if (effects.writes != Writes::Heap) {
        auto [count, list] = argumentBuffer(value.arguments);
        return ofEngine(AOT_TYPED_OPERATION_WITH_EFFECTS(CallReaderOfArray, effects, array, word32(value.builtin().index()), count, list));
    }
    Rep rep = repOf(type);
    LBasicBlock isPlain = m_out.newBlock();
    LBasicBlock hasChanged = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    m_out.branch(isPlainArray(array).value(), usually(isPlain), rarely(hasChanged));
    m_out.appendTo(isPlain);
    join.add(ofEngine(callGenerally(box(builtinCell(m_builtins.cellOf(value.builtin()))), box(array), value.arguments)));
    m_out.appendTo(hasChanged);
    auto [count, list] = argumentBuffer(value.arguments);
    Boxed yielded = AOT_TYPED_OPERATION(CallMethod, box(array), word32(name.index()), count, list);
    // `reverse`, `fill`, `sort` hand back the array, which is taken for one already and would not be again.
    if (m_builtins.at(value.builtin()).result.kinds & BuiltinResult::This) {
        join.add(eitherOperand(equal(yielded, box(array)), rep, [&] {
            return ensure(array, typeOf(value.child(0)).withoutPrimitives(Primitive::UndefinedOrNull), type);
        }, [&] {
            return ensure(yielded, type);
        }));
    } else
        join.add(ensure(yielded, type));
    return join.finish();
}

Operand Lowering::lowerCallBuiltin(ValueID result, const Value& value)
{
    auto& builtin = m_builtins.at(value.builtin());
    auto& type = typeOf(result);
    if (builtin.implementation.kind == BuiltinImplementation::Inline) {
        if (auto inlined = lowerInline(builtin.implementation.inlined, result, value))
            return *inlined;
    }
    if (builtin.kind == BuiltinKind::Constant)
        return ensure(builtinCell(m_builtins.cellOf(value.builtin())), Type::ofPrimitives(Primitive::Symbol), type);
    auto generally = [&] {
        if (auto* engineClass = builtin.kind == BuiltinKind::Method ? typeOf(value.child(0)).onlyEngineClass() : nullptr; engineClass && engineClass->type == ArrayType)
            return callMethodOfArray(value, type);
        Boxed yielded = callFunctionOfEngine(value, effectsOfBuiltinCall(value));
        if (vouchesForItsType(m_program, m_function, value))
            return convert(yielded, type, repOf(type));
        return ensure(yielded, type);
    };
    // The entry holds whatever the arguments are. The code in place may be for what is expected of them.
    ExpectedBuiltin expected;
    expected.call = value;
    bool goesByPredictions = false;
    for (ValueID argument : value.arguments) {
        goesByPredictions |= !!predictionOf(argument);
        expected.arguments.append(expectedTypeOf(argument));
    }
    if (goesByPredictions && chooseArgumentsToExpect(expected))
        return lowerExpectedBuiltin(result, expected, std::nullopt, generally);
    return generally();
}

// The global object of the table that the value is, going by how it is defined, if its properties are as the table says. As resolveBuiltins() has it.
BuiltinName Lowering::globalDefinedBy(ValueID id) const
{
    while (m_function.at(id).opcode == Opcode::Cast)
        id = m_function.at(id).child(0);
    auto& value = m_function.at(id);
    if (value.opcode != Opcode::GetGlobal)
        return { };
    BuiltinName name = m_program.builtinName(value.string());
    if (!m_builtins.isGlobal(name) || (m_program.isExtended(name) && !m_builtins.classNamed(name)))
        return { };
    return name;
}

// resolveBuiltins() goes by types, and has left what it is asked about here. This goes by what is expected: `Math.floor(x)` of a number,
// `s.charCodeAt(i)` of a string, `map.get(key)` of a Map. Only what there is code in place for is of interest: the function of the
// engine is called just as well the general way.
//
// Method, Getter: of the receiver, under the name. A receiver that is a global object of the table is that for a fact, and so is what it
// has under the name. Function: the global function of the name.
std::optional<Lowering::ExpectedBuiltin> Lowering::expectedBuiltin(BuiltinKind kind, ValueID receiver, BuiltinName name, const Arguments& arguments) const
{
    if (!name)
        return std::nullopt;
    ExpectedBuiltin expected;
    bool goesByPredictions = false;
    for (ValueID argument : arguments) {
        goesByPredictions |= !!predictionOf(argument);
        expected.arguments.append(expectedTypeOf(argument));
    }
    BuiltinID first;
    const EngineClass* engineClass = nullptr;
    ValueID thisValue;
    if (kind == BuiltinKind::Function) {
        first = m_builtins.find(kind, { }, name);
        engineClass = m_builtins.classNamed(name);
    } else if (BuiltinName holder = globalDefinedBy(receiver)) {
        if (kind != BuiltinKind::Method)
            return std::nullopt;
        first = m_builtins.findStatic(BuiltinKind::StaticFunction, holder, name);
        engineClass = m_builtins.classNamed(holder);
    } else {
        auto prediction = predictionOf(receiver);
        if (!prediction)
            return std::nullopt;
        engineClass = prediction->isString() ? &m_builtins.stringClass() : prediction->onlyEngineClass();
        if (!engineClass || !(prediction->isString() || canTellWhetherAsMade(*engineClass)))
            return std::nullopt;
        first = m_builtins.findForInstance(kind, *engineClass, name);
        expected.receiver = *prediction;
        thisValue = receiver;
        goesByPredictions = true;
    }
    if (!goesByPredictions)
        return std::nullopt;
    BuiltinID entry = applicableEntry(first, expected.arguments.span());
    if (!entry)
        return std::nullopt;
    expected.call.opcode = Opcode::CallBuiltin;
    expected.call.children = { thisValue, ValueID(), ValueID() };
    expected.call.immediates = { entry.index(), engineClass ? engineClass->index : Format::none };
    expected.call.arguments = arguments;
    if (!chooseArgumentsToExpect(expected))
        return std::nullopt;
    return expected;
}

// Of the entry, with the types that are expected of the arguments: settles which of them are tested for. False: there is no code in place.
bool Lowering::chooseArgumentsToExpect(ExpectedBuiltin& expected) const
{
    auto& builtin = m_builtins.at(expected.call.builtin());
    if (builtin.implementation.kind != BuiltinImplementation::Inline)
        return false;
    InlineBuiltin which = builtin.implementation.inlined;
    auto& arguments = expected.call.arguments;
    // What is taken as it is needs no look, unless the code in place is for one kind of value only.
    Vector<Type, 4> needed = expected.arguments;
    for (unsigned i = 0; i < arguments.size(); ++i) {
        if (builtin.parameters[std::min<size_t>(i, builtin.parameters.size() - 1)].conversion == ArgumentConversion::None)
            needed[i] = typeOf(arguments[i]);
    }
    if (writesOfInlineBuiltin(which, needed.span()))
        expected.arguments = WTF::move(needed);
    else if (!writesOfInlineBuiltin(which, expected.arguments.span()))
        return false;
    // A place in a string is an int32 as a rule.
    if (builtin.holderName == m_builtins.stringClass().name) {
        for (unsigned i = 0; i < arguments.size(); ++i) {
            if (predictionOf(arguments[i]) && expected.arguments[i].isNumber())
                expected.arguments[i] = m_program.at(m_program.types.int32);
        }
    }
    return true;
}

// Nothing: there is no code for these arguments.
std::optional<Operand> Lowering::lowerInline(InlineBuiltin which, ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    Vector<Type, 4> argumentTypes;
    for (ValueID argument : arguments)
        argumentTypes.append(typeOf(argument));
    auto writes = writesOfInlineBuiltin(which, argumentTypes.span());
    if (!writes)
        return std::nullopt;
    auto yieldsNumber = [&](Number yielded) {
        auto& numberType = m_program.at(m_program.types.number);
        // As the function of the engine would hand it back.
        if (repOf(type) == Rep::JSValue)
            return ensure(boxNumber(yielded), numberType, type);
        return ensure(yielded, numberType, type);
    };
    auto yieldsBoolean = [&](Word32 yielded) {
        return ensure(yielded, m_program.at(m_program.types.boolean), type);
    };
    auto yieldsArgument = [&](ValueID argument) {
        return ensure(get(argument), typeOf(argument), type);
    };
    auto isFinite = [&](Number x) {
        return lessThan(abs(x), number(std::numeric_limits<double>::infinity()));
    };
    auto isFiniteInteger = [&](Number x) {
        return logicalAnd(equal(trunc(x), x), isFinite(x));
    };

    switch (which) {
    case InlineBuiltin::MathFloor:
    case InlineBuiltin::MathCeil:
    case InlineBuiltin::MathTrunc:
        if (argumentTypes[0].isInteger())
            return yieldsArgument(arguments[0]);
        [[fallthrough]];
    case InlineBuiltin::MathAbs:
    case InlineBuiltin::MathSqrt: {
        Number x = in<Number>(arguments[0]);
        switch (which) {
        case InlineBuiltin::MathAbs:
            return yieldsNumber(abs(x));
        case InlineBuiltin::MathFloor:
            return yieldsNumber(floor(x));
        case InlineBuiltin::MathCeil:
            return yieldsNumber(ceil(x));
        case InlineBuiltin::MathTrunc:
            return yieldsNumber(trunc(x));
        default:
            return yieldsNumber(sqrt(x));
        }
    }
    case InlineBuiltin::MathPow:
        return yieldsNumber(AOT_TYPED_OPERATION(Pow, in<Number>(arguments[0]), in<Number>(arguments[1])));
    case InlineBuiltin::MathMin:
        return yieldsNumber(min(in<Number>(arguments[0]), in<Number>(arguments[1])));
    case InlineBuiltin::MathMax:
        return yieldsNumber(max(in<Number>(arguments[0]), in<Number>(arguments[1])));
    case InlineBuiltin::MathImul:
        return ensure(signExtended(mul(toInt32(arguments[0]), toInt32(arguments[1]))), m_program.at(m_program.types.int32), type);
    case InlineBuiltin::MathClz32:
        return ensure(zeroExtended(countLeadingZeros(toInt32(arguments[0]))), Type::integer({ 0, 32 }), type);

    case InlineBuiltin::IsNaN:
    case InlineBuiltin::IsFinite:
    case InlineBuiltin::NumberIsNaN:
    case InlineBuiltin::NumberIsFinite:
    case InlineBuiltin::NumberIsInteger:
    case InlineBuiltin::NumberIsSafeInteger: {
        Range safeIntegers { -(maxExactInteger - 1), maxExactInteger - 1 };
        bool converts = which == InlineBuiltin::IsNaN || which == InlineBuiltin::IsFinite;
        bool asksForNaN = which == InlineBuiltin::IsNaN || which == InlineBuiltin::NumberIsNaN;
        if (!converts && !(argumentTypes[0].primitives & Primitive::Number))
            return yieldsBoolean(boolean(false));
        Operand operand = get(arguments[0]);
        if (operand.rep == Rep::Int64) {
            if (which == InlineBuiltin::NumberIsSafeInteger)
                return yieldsBoolean(isWithin(operand.as<Integer>(), safeIntegers));
            return yieldsBoolean(boolean(!asksForNaN));
        }
        if (operand.rep == Rep::OptionalInt64) {
            if (asksForNaN)
                return yieldsBoolean(boolean(false));
            // No marker is within the range.
            if (which == InlineBuiltin::NumberIsSafeInteger)
                return yieldsBoolean(isWithin(assumedInteger(operand.as<OptionalInteger>()), safeIntegers));
            return yieldsBoolean(logicalNot(isMarker(operand)));
        }
        Number x = operand.rep == Rep::Double ? operand.as<Number>() : assumedNumber(operand.as<OptionalNumber>());
        switch (which) {
        case InlineBuiltin::IsNaN:
        case InlineBuiltin::NumberIsNaN:
            return yieldsBoolean(notEqualOrUnordered(x, x));
        case InlineBuiltin::IsFinite:
        case InlineBuiltin::NumberIsFinite:
            return yieldsBoolean(isFinite(x));
        case InlineBuiltin::NumberIsInteger:
            return yieldsBoolean(isFiniteInteger(x));
        default:
            return yieldsBoolean(logicalAnd(isFiniteInteger(x), lessThanOrEqual(abs(x), number(static_cast<double>(safeIntegers.most)))));
        }
    }
    case InlineBuiltin::ToNumber:
        if (arguments.isEmpty())
            return ensure(integer(0), Type::integer(Range::one(0)), type);
        return yieldsArgument(arguments[0]);

    // `valueOf` and the like.
    case InlineBuiltin::This: {
        auto& receiver = typeOf(value.child(0));
        if (!receiver.hasCells())
            return yieldsArgument(value.child(0));
        return ensure(nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName)), receiver.withoutPrimitives(Primitive::UndefinedOrNull), type);
    }

    case InlineBuiltin::StringLength:
    case InlineBuiltin::StringAt:
    case InlineBuiltin::StringCharAt:
    case InlineBuiltin::StringCharCodeAt:
    case InlineBuiltin::StringCodePointAt:
    case InlineBuiltin::StringSlice:
    case InlineBuiltin::StringSubstring:
    case InlineBuiltin::StringSubstr:
    case InlineBuiltin::StringStartsWith:
    case InlineBuiltin::StringEndsWith:
    case InlineBuiltin::StringIncludes:
    case InlineBuiltin::StringIndexOf:
        return lowerStringBuiltin(which, result, value);
    case InlineBuiltin::MapNew:
    case InlineBuiltin::MapGet:
    case InlineBuiltin::MapSet:
    case InlineBuiltin::MapHas:
    case InlineBuiltin::MapSize:
    case InlineBuiltin::SetAdd:
    case InlineBuiltin::SetHas:
    case InlineBuiltin::SetSize:
        return lowerMapOrSetBuiltin(which, result, value, *writes);
    case InlineBuiltin::ArrayLength:
    case InlineBuiltin::ArraySetLength:
    case InlineBuiltin::ArrayPush:
    case InlineBuiltin::ArrayWithLength:
    case InlineBuiltin::ArrayOf:
    case InlineBuiltin::ArrayIsArray:
    case InlineBuiltin::TypedArrayWithLength:
    case InlineBuiltin::TypedArrayLength:
        return lowerContainerBuiltin(which, result, value, *writes);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
