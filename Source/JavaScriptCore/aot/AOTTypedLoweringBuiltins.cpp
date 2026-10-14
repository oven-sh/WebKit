/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTTypedCompactTable.h"
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

Word32 Lowering::isArrayAsMade(CellPointer cell)
{
    return Word32(m_out.castToInt32(callAllocationStub(Stub::IsArrayAsMade, { { cell.value(), stubTemporaryGPRs[0] } }, { Writes::Nothing, false }, false, true)));
}

bool Lowering::isGuardOfReceiver(const Value& value) const
{
    return value.opcode == Opcode::CallBuiltin && value.builtin() == m_builtins.hasOriginalStructureOperation() && !Options::auditAOTTypedAssertions() && !Options::validateAOTTypedFacts();
}

ValueID Lowering::receiverBehindCasts(ValueID id) const
{
    while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() != CastReason::As && m_function.at(id).castReason() != CastReason::Coerce)
        id = m_function.at(id).child(0);
    return id;
}

void Lowering::branchOnOriginalStructure(const Value& value, const Terminator& terminator, LBasicBlock isOriginal, LBasicBlock hasChanged)
{
    Operand operand = get(value.arguments[0]);
    if (value.engineClass() == Format::none || (operand.rep != Rep::Cell && operand.rep != Rep::JSValue) || isCompact(14)) {
        m_out.jump(hasChanged);
        return;
    }
    CellPointer cell;
    if (operand.rep == Rep::Cell)
        cell = operand.as<CellPointer>();
    else {
        Boxed boxed = operand.as<Boxed>();
        LBasicBlock isCellCase = m_out.newBlock();
        m_out.branch(isCell(boxed).value(), unsure(isCellCase), unsure(hasChanged));
        m_out.appendTo(isCellCase);
        cell = asCell(boxed);
    }
    Word32 holds = hasOriginalStructure(cell, m_builtins.classes()[value.engineClass()]);
    if (value.takesArrayAsMade()) {
        LBasicBlock hasAnother = newColdBlock();
        m_out.branch(holds.value(), unsure(isOriginal), unsure(hasAnother));
        m_out.appendTo(hasAnother);
        holds = isArrayAsMade(cell);
    }
    m_out.branch(holds.value(), unsure(isOriginal), unsure(hasChanged));
    if (terminator.targets[0].block == terminator.targets[1].block)
        return;
    for (unsigned i = 0; i < 2; ++i) {
        BlockID target = terminator.targets[i].block;
        if (target.index() && numberOfPredecessorsOf(target) == 1)
            m_receiversAsked.set(target.index(), ReceiverAsked { receiverBehindCasts(value.arguments[0]), value.engineClass(), !i, value.takesArrayAsMade() });
    }
}

std::optional<bool> Lowering::answerOfGuardAhead(ValueID receiver, const EngineClass& engineClass, AskedFor askedFor)
{
    if (m_receiversAsked.isEmpty() || !m_currentBlock)
        return std::nullopt;
    auto found = m_receiversAsked.find(m_currentBlockID.index());
    if (found == m_receiversAsked.end() || found->value.engineClass != engineClass.index || found->value.receiver != receiverBehindCasts(receiver))
        return std::nullopt;
    if (askedFor == AskedFor::Structure) {
        if (found->value.isOriginal && found->value.mayOnlyBeAsMade)
            return std::nullopt;
        for (size_t i = 0; i < m_currentIndexInBlock; ++i) {
            switch (m_function.at(m_currentBlock->values[i]).opcode) {
            case Opcode::CallDirect:
            case Opcode::Await:
            case Opcode::Yield:
            case Opcode::GetResumeMode:
                return std::nullopt;
            default:
                break;
            }
            if (writesOf(m_program, m_function, *m_currentBlock, i) == Writes::Heap)
                return std::nullopt;
        }
    }
    return found->value.isOriginal;
}

// Calls the engine's function. A host function or an InternalFunction is entered through its native call trampoline.
// `calledOn`: what `this` is, where the function was not come by through a receiver or a holder.
bool Lowering::makesTableWithStringKeys(const Value& value)
{
    if (m_builtins.at(value.builtin()).kind != BuiltinKind::Constructor || value.arguments.size() != 1 || value.engineClass() == Format::none)
        return false;
    JSType made = m_builtins.classes()[value.engineClass()].type;
    if (made != JSSetType)
        return false;
    if (value.hasStringKeysByType)
        return true;
    auto elementsOfArray = [&](const Type& type) -> const Type* {
        auto* engineClass = type.onlyEngineClass();
        return engineClass && engineClass->type == ArrayType && type.elements ? &m_program.at(type.elements) : nullptr;
    };
    auto isStringOrNil = [](const Type& type) {
        return type.primitives & Primitive::String && type.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
    };
    ValueID given = value.arguments[0];
    auto& list = m_function.at(originOf(given));
    if (list.opcode == Opcode::NewArray && !list.arguments.isEmpty()) {
        for (ValueID item : list.arguments) {
            if (made == JSSetType) {
                if (!isStringOrNil(typeOf(item)))
                    return false;
                continue;
            }
            auto& pair = m_function.at(originOf(item));
            if (pair.opcode != Opcode::NewArray || pair.arguments.size() < 2 || !isStringOrNil(typeOf(pair.arguments[0])))
                return false;
        }
        return true;
    }
    for (TypeID type : { m_function.at(given).type, m_function.at(given).prediction, list.prediction }) {
        auto* elements = type ? elementsOfArray(m_program.at(type)) : nullptr;
        if (made == JSMapType && elements)
            elements = elementsOfArray(*elements);
        if (elements && isStringOrNil(*elements))
            return true;
    }
    return false;
}

Boxed Lowering::callBuiltinFunction(const Value& value, OperationEffects effects, std::optional<Boxed> calledOn)
{
    if (!m_isMakingTableGenerally && makesTableWithStringKeys(value)) {
        Boxed list = in<Boxed>(value.arguments[0]);
        bool isSet = m_builtins.classes()[value.engineClass()].type == JSSetType;
        LValue made = isSet ? AOT_TYPED_OPERATION(TryNewSetOfStrings, list).value() : AOT_TYPED_OPERATION(TryNewMapWithStringKeys, list).value();
        return either<Boxed>(Word32(m_out.notZero64(made)), [&] {
            return box(CellPointer(made));
        }, [&] {
            SetForScope isMakingTableGenerally(m_isMakingTableGenerally, true);
            return callBuiltinFunction(value, effects, calledOn);
        });
    }
    if (auto& called = m_builtins.at(value.builtin()); !m_isApplyingGenerally && called.kind == BuiltinKind::StaticFunction && called.holder == "Reflect"_s && called.name == "apply"_s && value.arguments.size() == 3) {
        Boxed function = in<Boxed>(value.arguments[0]);
        Boxed list = in<Boxed>(value.arguments[2]);
        LBasicBlock generally = newUnlikelyBlock();
        Join join(m_out, Rep::JSValue, Int64);
        orElse(isCell(function), generally);
        orElse(hasTypeOfFunctionFlag(asCell(function)), generally);
        orElse(isCell(list), generally);
        orElse(isPlainArray(asCell(list)), generally);
        orElse(belowOrEqual(publicLength(butterflyOf(asCell(list))), integer(numberOfArgumentGPRs)), generally);
        join.add(callValueWithArray(Stub::ApplyArray, function, in<Boxed>(value.arguments[1]), list));
        m_out.appendTo(generally);
        SetForScope isApplyingGenerally(m_isApplyingGenerally, true);
        join.add(callBuiltinFunction(value, effects, calledOn));
        return join.finish().as<Boxed>();
    }
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
bool Lowering::needsOnlyItsMethod(const Value& value) const
{
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    switch (implementation.kind) {
    case BuiltinImplementation::Call:
        return true;
    case BuiltinImplementation::Inline:
        return isExactForEveryReceiver(implementation.inlined) || familyOf(implementation.inlined) == BuiltinFamily::MapSet;
    default:
        return false;
    }
}

Operand Lowering::lowerGenericFallback(const Value& value, CellPointer receiver, const Type& type, bool methodWasFound)
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
            yielded = name ? callMethodGenerally(box(receiver), name, value.arguments) : callGenerally(box(builtinCell(m_builtins.cellOf(value.builtin()))), box(receiver), value.arguments);
            break;
        }
        if (methodWasFound) {
            yielded = callGenerally(in<Boxed>(value.methodRead()), box(receiver), value.arguments);
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
    if (auto& builtin = m_builtins.at(value.builtin()); builtin.takesReceiver()) {
        auto& receiver = typeOf(value.child(0));
        if (!receiver.hasObjects() && receiver.mayBeUndefinedOrNull() && receiver.primitives & (Primitive::Number | Primitive::Boolean)) {
            Type withoutNil = receiver.withoutPrimitives(Primitive::UndefinedOrNull);
            Rep rep = repOf(typeOf(result));
            return ifAsExpected({ { value.child(0), &withoutNil } }, rep, [&](LBasicBlock) {
                return lowerResolvedBuiltin(result, value);
            }, [&] {
                throwAt(ThrowKind::NullishRead, Operand(thrownNil(in<Boxed>(value.child(0)))), m_program.stringOf(builtin.nameName).index());
                return placeholder(rep);
            });
        }
    }
    if (!hasGenericFallback(m_program, m_function, value))
        return lowerResolvedBuiltin(result, value);
    auto answer = answerOfGuardAhead(value.child(0), *typeOf(value.child(0)).onlyEngineClass(), AskedFor::Structure);
    if (answer.value_or(true)) {
        if (auto searched = lowerArraySearchAsOneCall(result, value))
            return *searched;
    }
    if (answer.value_or(false))
        return lowerResolvedBuiltin(result, value);
    auto& type = typeOf(result);
    Rep rep = repOf(type);
    CellPointer receiver = nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    auto kindOfResolvedWay = m_builtins.at(value.builtin()).implementation.kind;
    if (answer || isCompact(kindOfResolvedWay == BuiltinImplementation::Call || kindOfResolvedWay == BuiltinImplementation::Prelude ? 5 : 14))
        return lowerGenericFallback(value, receiver, type);
    if (tableStubAsksReceiver(result, value)) {
        SetForScope isInArm(m_isInArm, true);
        m_tableStubAsksReceiver = true;
        Operand yielded = lowerResolvedBuiltin(result, value);
        RELEASE_ASSERT(!m_tableStubAsksReceiver);
        return yielded;
    }
    if (holdsResultOfMapGetBehindJoin(value)) {
        LBasicBlock isOriginal = m_out.newBlock();
        LBasicBlock hasChanged = newColdBlock();
        Join join(m_out, Rep::JSValue, Int64);
        m_out.branch(hasOriginalStructure(receiver, *typeOf(value.child(0)).onlyEngineClass()).value(), usually(isOriginal), rarely(hasChanged));
        m_out.appendTo(isOriginal);
        {
            SetForScope isInArm(m_isInArm, true);
            m_mapGetYieldsAnything = true;
            join.add(lowerResolvedBuiltin(result, value));
            RELEASE_ASSERT(!m_mapGetYieldsAnything);
            m_out.appendTo(hasChanged);
            join.add(lowerGenericFallback(value, receiver, m_program.at(m_program.types.anything)));
        }
        return ensure(join.finish().as<Boxed>(), type);
    }
    LBasicBlock isOriginal = m_out.newBlock();
    LBasicBlock hasChanged = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    bool methodWasFound = value.methodRead() && needsOnlyItsMethod(value);
    m_out.branch((methodWasFound ? isEmpty(in<Boxed>(value.methodRead())) : hasOriginalStructure(receiver, *typeOf(value.child(0)).onlyEngineClass())).value(), usually(isOriginal), rarely(hasChanged));
    m_out.appendTo(isOriginal);
    SetForScope isInArm(m_isInArm, true);
    Operand resolved;
    {
        SetForScope offer(m_generalArmOnOffer, !methodWasFound ? hasChanged : nullptr);
        SetForScope expectedMethodThatWasRead(m_expectedMethodThatWasRead, methodWasFound ? value.builtin() : m_expectedMethodThatWasRead);
        SetForScope builtinOfOffer(m_builtinOfGeneralArmOnOffer, value.builtin());
        resolved = lowerResolvedBuiltin(result, value);
    }
    if (rep != Rep::Double || m_lastIntegerMadeNumber.number != resolved.value) {
        join.add(resolved);
        m_out.appendTo(hasChanged);
        join.add(lowerGenericFallback(value, receiver, type, methodWasFound));
        return join.finish();
    }
    int64_t least = m_lastIntegerMadeNumber.least;
    ValueFromBlock integerInPlace = m_out.anchor(m_lastIntegerMadeNumber.integer);
    join.add(resolved);
    m_out.appendTo(hasChanged);
    Operand general = lowerGenericFallback(value, receiver, type, methodWasFound);
    ValueFromBlock noInteger = m_out.anchor(integer(INT64_MIN).value());
    join.add(general);
    Operand joined = join.finish();
    m_likelyIntegersOfNumbers.add(result.index(), IntegerMadeNumber { joined.value, m_out.phi(Int64, integerInPlace, noInteger), least });
    return joined;
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
        if (m_classExpectedOfReceiver)
            prediction = m_classExpectedOfReceiver == &m_builtins.stringClass() ? m_program.at(m_program.types.string) : Type::engineClass(*m_classExpectedOfReceiver);
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
