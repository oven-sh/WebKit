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

std::optional<Writes> writesOfInlineBuiltin(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (familyOf(which)) {
#define AOT_TYPED_WRITES_OF_FAMILY(family) \
    case BuiltinFamily::family: \
        return writesOfInline ## family(which, arguments);
    FOR_EACH_AOT_TYPED_BUILTIN_FAMILY(AOT_TYPED_WRITES_OF_FAMILY)
#undef AOT_TYPED_WRITES_OF_FAMILY
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Of what the entry describes. The generic fallback is a call like any other.
OperationEffects Lowering::effectsOfBuiltinCall(const Value& value)
{
    auto& builtin = m_builtins.at(value.builtin());
    Writes writes = writesOfResolvedBuiltinCall(m_program, m_function, value);
    // Nothing is asked of `undefined` or `null`: that throws.
    bool exitsSideways = writes == Writes::Heap || builtin.effects & Effect::MayThrow || (builtin.takesReceiver() && typeOf(value.child(0)).mayBeUndefinedOrNull());
    return { writes, exitsSideways };
}

// See Builtins::findCell().
CellPointer Lowering::builtinCell(unsigned index)
{
    if (std::ranges::contains(m_builtins.cellsFilledFromStart(), index))
        return CellPointer(m_out.load64(m_out.address(m_heaps.AOTInstance_fixedFields, fixedPointer(Instance::offsetOfTypedBuiltins()), static_cast<ptrdiff_t>(index) * sizeof(void*))));
    return lazilyInitialized<CellPointer>(Stub::TypedBuiltinCell, Instance::offsetOfTypedBuiltins(), index, [&] {
        return AOT_TYPED_OPERATION(BuiltinCell, word32(index));
    });
}

// The classes whose structure is in Instance::receiverStructureIDs from the start.
static std::optional<Receiver> receiverWithStructureID(JSType type)
{
    switch (type) {
    case JSMapType:
        return Receiver::Map;
    case JSSetType:
        return Receiver::Set;
    case JSWeakMapType:
        return Receiver::WeakMap;
    case JSWeakSetType:
        return Receiver::WeakSet;
    case RegExpObjectType:
        return Receiver::RegExp;
    case JSDateType:
        return Receiver::Date;
    default:
        return std::nullopt;
    }
}

// Of any cell. See EngineClass::canCheckOriginalStructure(). Of the class of strings: whether the cell is a string.
Word32 Lowering::hasOriginalStructure(CellPointer cell, const EngineClass& engineClass)
{
    if (engineClass.type == ArrayType)
        return Word32(isOriginalArray(cell.value()));
    if (engineClass.index == m_builtins.stringClass().index)
        return equal(typeOfCell(cell), word32(StringType));
    Word32 structureID = loadField<Word32>(cell, m_heaps.JSCell_structureID);
    if (auto receiver = receiverWithStructureID(engineClass.type))
        return equal(structureID, Word32(fixed32(Instance::offsetOfReceiverStructureIDs() + static_cast<unsigned>(*receiver) * sizeof(uint32_t))));
    auto isStructure = [&](unsigned which) {
        return equal(structureID, low32(box(builtinCell(m_builtins.cellOfOriginalStructure(engineClass.firstOriginalStructure + which)))));
    };
    if (engineClass.numberOfOriginalStructures == 1)
        return isStructure(0);
    RELEASE_ASSERT(engineClass.numberOfOriginalStructures == 2);
    return either<Word32>(isStructure(0), [&] {
        return boolean(true);
    }, [&] {
        return isStructure(1);
    });
}

// Calls the engine's function. A host function or an InternalFunction is entered through its native call trampoline.
// `calledOn`: what `this` is, where the function was not come by through a receiver or a holder.
Boxed Lowering::callBuiltinFunction(const Value& value, OperationEffects effects, std::optional<Boxed> calledOn)
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
        if (calledOn)
            thisValue = *calledOn;
        else if (typeOf(value.child(0)).hasCells())
            thisValue = box(nonNil(value.child(0), m_program.stringOf(builtin.nameName)));
        else
            thisValue = in<Boxed>(value.child(0));
        break;
    case BuiltinKind::StaticFunction:
        callee = box(builtinCell(m_builtins.cellOf(value.builtin())));
        // Of a replaceable global: the object the function was read from.
        thisValue = calledOn ? *calledOn : value.child(0) ? in<Boxed>(value.child(0)) : globalOr(builtin.holderName);
        break;
    case BuiltinKind::Function:
        callee = globalOr(builtin.nameName);
        break;
    // `new.target` is in the place of `this`.
    case BuiltinKind::Constructor:
        callee = globalOr(builtin.nameName);
        thisValue = callee;
        break;
    case BuiltinKind::Constant:
    case BuiltinKind::Operation:
        RELEASE_ASSERT_NOT_REACHED();
    }
    bool isConstruct = builtin.kind == BuiltinKind::Constructor;
    std::optional<Stub> stub;
    switch (builtin.implementation.callee) {
    case BuiltinCallee::None:
        RELEASE_ASSERT_NOT_REACHED();
    case BuiltinCallee::Function:
        if (!isConstruct)
            stub = Stub::Call;
        break;
    case BuiltinCallee::HostFunction:
        RELEASE_ASSERT(!isConstruct);
        stub = Stub::CallHostFunction;
        break;
    case BuiltinCallee::InternalFunction:
        stub = isConstruct ? Stub::ConstructInternalFunction : Stub::CallInternalFunction;
        break;
    }
    if (!stub || value.arguments.size() > numberOfArgumentGPRs) {
        auto [count, list] = argumentBuffer(value.arguments);
        if (isConstruct)
            return AOT_TYPED_OPERATION_WITH_EFFECTS(Construct, effects, callee, count, list);
        return AOT_TYPED_OPERATION_WITH_EFFECTS(Call, effects, callee, thisValue, count, list);
    }
    Vector<LValue, 8> operands { callee.value(), thisValue.value() };
    for (ValueID argument : value.arguments)
        operands.append(in<Boxed>(argument).value());
    return callValue(*stub, operands, effects);
}

// The receiver does not have its original structure: the call, read or store by name.
Operand Lowering::lowerGenericFallback(const Value& value, CellPointer receiver, const Type& type)
{
    auto& builtin = m_builtins.at(value.builtin());
    StringID name = m_program.stringOf(builtin.nameName);
    Boxed yielded;
    switch (builtin.kind) {
    case BuiltinKind::Getter:
        yielded = emitGetById(box(receiver), name);
        break;
    case BuiltinKind::Setter:
        emitPutById(box(receiver), name, in<Boxed>(value.arguments[0]));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case BuiltinKind::Method:
        if (!value.methodRead()) {
            yielded = callMethodGenerally(box(receiver), name, value.arguments);
            break;
        }
        // See Value::readsMethodFor().
        yielded = callGenerally(either<Boxed>(isNotEmpty(in<Boxed>(value.methodRead())), [&] {
            return in<Boxed>(value.methodRead());
        }, [&] {
            return box(builtinCell(m_builtins.cellOf(value.builtin())));
        }), box(receiver), value.arguments);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    // An array that was taken for one would not be again.
    if (builtin.result.kinds & BuiltinResult::This) {
        return eitherOperand(equal(yielded, box(receiver)), repOf(type), [&] {
            return ensure(receiver, typeOf(value.child(0)).withoutPrimitives(Primitive::UndefinedOrNull), type);
        }, [&] {
            return ensure(yielded, type);
        });
    }
    return ensure(yielded, type);
}

// A GetById with Value::readsMethodFor().
Boxed Lowering::lowerMethodRead(const Value& value)
{
    CellPointer receiver = nonNil(value.child(0), value.string());
    return either<Boxed>(hasOriginalStructure(receiver, *typeOf(value.child(0)).onlyEngineClass()), [&] {
        return boxedConstant(JSValue());
    }, [&] {
        return emitGetById(box(receiver), value.string());
    });
}

Operand Lowering::lowerCallBuiltin(ValueID result, const Value& value)
{
    if (!hasGenericFallback(m_program, m_function, value))
        return lowerResolvedBuiltin(result, value);
    auto& type = typeOf(result);
    Rep rep = repOf(type);
    CellPointer receiver = nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    LBasicBlock isOriginal = m_out.newBlock();
    LBasicBlock hasChanged = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    m_out.branch(hasOriginalStructure(receiver, *typeOf(value.child(0)).onlyEngineClass()).value(), usually(isOriginal), rarely(hasChanged));
    m_out.appendTo(isOriginal);
    join.add(lowerResolvedBuiltin(result, value));
    m_out.appendTo(hasChanged);
    join.add(lowerGenericFallback(value, receiver, type));
    return join.finish();
}

Operand Lowering::lowerResolvedBuiltin(ValueID result, const Value& value)
{
    auto& builtin = m_builtins.at(value.builtin());
    auto& type = typeOf(result);
    switch (builtin.implementation.kind) {
    case BuiltinImplementation::Inline:
        if (auto inlined = lowerInline(builtin.implementation.inlined, result, value))
            return *inlined;
        break;
    case BuiltinImplementation::Number:
        return yieldNumber(number(m_builtins.numberOf(value.builtin())), type);
    case BuiltinImplementation::Cell:
        return ensure(builtinCell(m_builtins.cellOf(value.builtin())), Type::ofPrimitives(Primitive::Symbol), type);
    case BuiltinImplementation::Call:
    case BuiltinImplementation::Prelude:
        break;
    case BuiltinImplementation::Generic:
    case BuiltinImplementation::Operation:
        RELEASE_ASSERT_NOT_REACHED();
    }
    auto generally = [&] {
        Boxed yielded = callBuiltinFunction(value, effectsOfBuiltinCall(value));
        if (vouchesForItsType(m_program, m_function, value))
            return convert(yielded, type, repOf(type));
        return ensure(yielded, type);
    };
    // The entry holds whatever the arguments are. The code in place may be for what is expected of them.
    ExpectedBuiltin expected;
    expected.call = value;
    for (ValueID argument : value.arguments) {
        Type type = expectedTypeOf(argument);
        // What may be `undefined` or `null` by its type seldom is: a parameter, or what has come by one.
        if (type.mayBeUndefinedOrNull() && !type.isUndefinedOrNull())
            type = type.withoutPrimitives(Primitive::UndefinedOrNull);
        expected.arguments.append(WTF::move(type));
    }
    if (chooseArgumentsToExpect(expected))
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
    return m_builtins.isGlobal(name) ? name : BuiltinName();
}

// resolveBuiltins() goes by types, and has left what it is asked about here. This goes by what is expected: `Math.floor(x)` of a number,
// `s.charCodeAt(i)` of a string, `map.get(key)` of a Map. Only what there is code in place for is of interest: the function of the
// engine is called just as well the general way.
//
// Method, Getter: of the receiver, under the name. A receiver that is a global object of the table is that for a fact, and so is what it
// has under the name. Function: the global function of the name.
// By the name as the program has it. A private name of the engine finds what the public one of the same spelling does where the table says
// so, and nothing anywhere else: `weakMap.$get(key)` throws. Whoever falls back goes by the name of the program.
std::optional<Lowering::ExpectedBuiltin> Lowering::expectedBuiltin(BuiltinKind kind, ValueID receiver, StringID name, const Arguments& arguments) const
{
    auto bytes = m_program.string(name);
    if (!isEnginePrivateName(bytes))
        return expectedBuiltin(kind, receiver, m_program.builtinName(name), arguments);
    auto expected = expectedBuiltin(kind, receiver, m_builtins.nameOf(bytes.subspan(1)), arguments);
    if (expected && !m_builtins.at(expected->call.builtin()).hasPrivateTwin)
        return std::nullopt;
    return expected;
}

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
        if (!engineClass || !(prediction->isString() || engineClass->canCheckOriginalStructure()))
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

// A call of a function of the table that was not come by through the receiver: `Array.prototype.push.call(o, x)`, a constant that holds
// `Object.keys`. Which function is called is known, what it is called on is not: `o` need not be an array, and may have a `push` of its own.
// So `thisValue` is expected to be of the class the function is listed under, with its original structure, and the arguments what the code
// in place takes. Else that function is called, on that value. The code in place of a static function is for a call on its holder; some
// look at `this` (`Array.of.call(List, 1)` makes a List). So it is only taken where `this` is the holder, or where it is `undefined` or `null`
// and that comes to the same.
// Nothing: the callee is no such function.
std::optional<Operand> Lowering::lowerCallOfBuiltinFunction(ValueID result, ValueID callee, ValueID thisValue, const Arguments& arguments)
{
    ValueID definition = callee;
    while (m_function.at(definition).opcode == Opcode::Cast)
        definition = m_function.at(definition).child(0);
    if (m_function.at(definition).opcode != Opcode::BuiltinFunction)
        return std::nullopt;
    BuiltinID first = m_function.at(definition).builtin();
    auto& builtin = m_builtins.at(first);
    bool isMethod = builtin.kind == BuiltinKind::Method;
    if ((!isMethod && builtin.kind != BuiltinKind::StaticFunction) || builtin.implementation.callee == BuiltinCallee::None)
        return std::nullopt;
    const EngineClass* engineClass = m_builtins.classNamed(builtin.holderName);
    ExpectedBuiltin expected;
    expected.call.opcode = Opcode::CallBuiltin;
    expected.call.immediates = { first.index(), engineClass ? engineClass->index : Format::none };
    expected.call.arguments = arguments;
    // What it is called on may be anything, so code of the program may run.
    auto generally = [&, direct = expected.call] {
        return ensure(callBuiltinFunction(direct, OperationEffects { Writes::Heap, true }, thisValue ? in<Boxed>(thisValue) : undefined()), typeOf(result));
    };
    if (isMethod) {
        bool isString = engineClass == &m_builtins.stringClass();
        if (!thisValue || !engineClass || !(isString || engineClass->canCheckOriginalStructure()))
            return generally();
        expected.receiver = isString ? m_program.at(m_program.types.string) : Type::engineClass(*engineClass);
        expected.call.children = { thisValue, ValueID(), ValueID() };
    } else if (!thisValue || globalDefinedBy(thisValue) != builtin.holderName) {
        // Array.of and Array.from make an array where `this` is no constructor. All of Promise, and `of` and `from` of the typed arrays, throw.
        bool throwsWithoutHolder = engineClass == &m_builtins.promiseClass() || builtin.holderName == BuiltinName(AOT_TYPED_BUILTIN_NAME_TypedArray);
        if (throwsWithoutHolder || (thisValue && !typeOf(thisValue).isUndefinedOrNull()))
            return generally();
    }
    Vector<Type, 4> types;
    for (ValueID argument : arguments) {
        types.append(typeOf(argument));
        Type type = expectedTypeOf(argument);
        if (type.mayBeUndefinedOrNull() && !type.isUndefinedOrNull())
            type = type.withoutPrimitives(Primitive::UndefinedOrNull);
        expected.arguments.append(WTF::move(type));
    }
    BuiltinID entry = applicableEntry(first, expected.arguments.span());
    if (!entry || m_builtins.at(entry).implementation.kind != BuiltinImplementation::Inline)
        return generally();
    expected.call.immediates[0] = entry.index();
    if (!chooseArgumentsToExpect(expected)) {
        // Nothing is left to test for: the code takes the arguments as they are.
        if (!writesOfInlineBuiltin(m_builtins.at(entry).implementation.inlined, types.span()))
            return generally();
        expected.arguments = WTF::move(types);
    }
    return lowerExpectedBuiltin(result, expected, std::nullopt, generally);
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
    else if (!writesOfInlineBuiltin(which, expected.arguments.span())) {
        // A place in an array or a string, a count, a radix: a number is an int32 there as a rule.
        for (auto& type : expected.arguments) {
            if (type.isNumber() && !type.isInteger())
                type = m_program.at(m_program.types.int32);
        }
        if (!writesOfInlineBuiltin(which, expected.arguments.span()))
            return false;
    }
    // A place in a string is an int32 as a rule.
    if (builtin.holderName == m_builtins.stringClass().name) {
        for (unsigned i = 0; i < arguments.size(); ++i) {
            if (predictionOf(arguments[i]) && expected.arguments[i].isNumber())
                expected.arguments[i] = m_program.at(m_program.types.int32);
        }
    }
    // Where nothing is left to test for, the code in place has been asked already.
    for (unsigned i = 0; i < arguments.size(); ++i) {
        if (expected.arguments[i] != typeOf(arguments[i]))
            return true;
    }
    return !expected.receiver.isNever();
}

// Nothing: there is no code for these arguments.
std::optional<Operand> Lowering::lowerInline(InlineBuiltin which, ValueID result, const Value& value)
{
    Vector<Type, 4> argumentTypes;
    for (ValueID argument : value.arguments)
        argumentTypes.append(typeOf(argument));
    auto writes = writesOfInlineBuiltin(which, argumentTypes.span());
    if (!writes)
        return std::nullopt;
    switch (familyOf(which)) {
#define AOT_TYPED_LOWER_FAMILY(family) \
    case BuiltinFamily::family: \
        return lowerInline ## family(which, result, value, *writes);
    FOR_EACH_AOT_TYPED_BUILTIN_FAMILY(AOT_TYPED_LOWER_FAMILY)
#undef AOT_TYPED_LOWER_FAMILY
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand Lowering::yieldNumber(Number yielded, const Type& type)
{
    auto& numberType = m_program.at(m_program.types.number);
    // As the engine's function would hand it back.
    if (repOf(type) == Rep::JSValue)
        return ensure(boxNumber(yielded), numberType, type);
    return ensure(yielded, numberType, type);
}

Operand Lowering::yieldBoolean(Word32 yielded, const Type& type)
{
    return ensure(yielded, m_program.at(m_program.types.boolean), type);
}

Operand Lowering::yieldValue(ValueID yielded, const Type& type)
{
    return ensure(get(yielded), typeOf(yielded), type);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
