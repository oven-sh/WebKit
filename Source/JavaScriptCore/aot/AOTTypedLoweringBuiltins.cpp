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

OperationEffects Lowering::builtinCallEffects(const Value& value)
{
    auto& builtin = m_builtins.at(value.builtin());
    Writes writes = writesOfResolvedBuiltinCall(m_program, m_function, value);
    bool exitsSideways = writes == Writes::Heap || builtin.effects & Effect::MayThrow || (builtin.hasReceiver() && typeOf(value.child(0)).mayBeUndefinedOrNull());
    return { writes, exitsSideways };
}

CellPointer Lowering::builtinCell(unsigned index)
{
    if (std::ranges::contains(m_builtins.eagerCells(), index) || m_program.comparedBuiltinCells.get(index))
        return CellPointer(m_out.load64(m_out.address(m_heaps.AOTInstance_fixedFields, fixedPointer(Instance::offsetOfTypedBuiltins()), static_cast<ptrdiff_t>(index) * sizeof(void*))));
    return lazilyInitialized<CellPointer>(Stub::TypedBuiltinCell, Instance::offsetOfTypedBuiltins(), index, [&] {
        return AOT_TYPED_OPERATION(BuiltinCell, word32(index));
    });
}

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

Word32 Lowering::hasOriginalStructure(CellPointer cell, const EngineClass& engineClass)
{
    if (engineClass.type == ArrayType)
        return isPlainArray(cell);
    if (engineClass.index == m_builtins.stringClass().index)
        return equal(loadCellType(cell), word32(StringType));
    Word32 structureID = loadField<Word32>(cell, m_heaps.JSCell_structureID);
    if (auto receiver = receiverWithStructureID(engineClass.type))
        return equal(structureID, Word32(fixed32(Instance::offsetOfReceiverStructureID(*receiver))));
    auto isStructure = [&](unsigned which) {
        return equal(structureID, low32(box(builtinCell(m_builtins.originalStructureCell(engineClass.firstOriginalStructure + which)))));
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

void Lowering::validateReceiverHasOriginalStructure(Word32 holds, CellPointer receiver, ValueID at)
{
    LBasicBlock doesNotHold = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(holds.value(), usually(continuation), rarely(doesNotHold));
    m_out.appendTo(doesNotHold);
    {
        SetForScope isReporting(m_isReportingFactViolation, true);
        AOT_TYPED_OPERATION(FactViolated, box(receiver), word32(m_id.index()), word32(at.index()));
    }
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

bool Lowering::isReceiverGuard(const Value& value) const
{
    return value.opcode == Opcode::CallBuiltin && value.builtin() == m_builtins.hasOriginalStructureOperation() && !Options::auditAOTTypedAssertions() && !Options::validateAOTTypedFacts();
}

ValueID Lowering::receiverWithoutCasts(ValueID id) const
{
    while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() != CastReason::As && m_function.at(id).castReason() != CastReason::Coerce)
        id = m_function.at(id).child(0);
    return id;
}

void Lowering::branchOnOriginalStructure(const Value& value, const Terminator& terminator, LBasicBlock isOriginal, LBasicBlock hasChanged)
{
    Operand operand = get(value.arguments[0]);
    if (value.engineClass() == Format::none || (operand.rep != Rep::Cell && operand.rep != Rep::JSValue)) {
        m_out.jump(hasChanged);
        return;
    }
    CellPointer cell;
    if (operand.rep == Rep::Cell)
        cell = operand.as<CellPointer>();
    else {
        Boxed boxed = operand.as<Boxed>();
        LBasicBlock isCellCase = m_out.newBlock();
        if (hasChanged->frequency())
            m_out.branch(isCell(boxed).value(), unsure(isCellCase), unsure(hasChanged));
        else
            m_out.branch(isCell(boxed).value(), usually(isCellCase), rarely(hasChanged));
        m_out.appendTo(isCellCase);
        cell = asCell(boxed);
    }
    if (hasChanged->frequency())
        m_out.branch(hasOriginalStructure(cell, m_builtins.classes()[value.engineClass()]).value(), unsure(isOriginal), unsure(hasChanged));
    else
        m_out.branch(hasOriginalStructure(cell, m_builtins.classes()[value.engineClass()]).value(), usually(isOriginal), rarely(hasChanged));
    if (terminator.targets[0].block == terminator.targets[1].block)
        return;
    for (unsigned i = 0; i < 2; ++i) {
        BlockID target = terminator.targets[i].block;
        if (target.index() && numberOfPredecessorsOf(target) == 1)
            m_checkedReceivers.set(target.index(), CheckedReceiver { receiverWithoutCasts(value.arguments[0]), value.engineClass(), !i });
    }
}

std::optional<bool> Lowering::resultOfDominatingGuard(ValueID receiver, const EngineClass& engineClass, GuardKind requested)
{
    if (m_checkedReceivers.isEmpty() || !m_currentBlock)
        return std::nullopt;
    auto found = m_checkedReceivers.find(m_currentBlockID.index());
    if (found == m_checkedReceivers.end() || found->value.engineClass != engineClass.index || found->value.receiver != receiverWithoutCasts(receiver))
        return std::nullopt;
    if (requested == GuardKind::Structure) {
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

bool Lowering::createsStringKeyedTable(const Value& value)
{
    if (m_builtins.at(value.builtin()).kind != BuiltinKind::Constructor || value.arguments.size() != 1 || value.engineClass() == Format::none)
        return false;
    JSType created = m_builtins.classes()[value.engineClass()].type;
    if (created != JSSetType)
        return false;
    if (value.hasStringKeysByType)
        return true;
    auto arrayElements = [&](const Type& type) -> const Type* {
        auto* engineClass = type.singleEngineClass();
        return engineClass && engineClass->type == ArrayType && type.elements ? &m_program.at(type.elements) : nullptr;
    };
    auto isStringOrNullish = [](const Type& type) {
        return type.primitives & Primitive::String && type.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
    };
    ValueID given = value.arguments[0];
    auto& list = m_function.at(originOf(given));
    if (list.opcode == Opcode::NewArray && !list.arguments.isEmpty()) {
        for (ValueID item : list.arguments) {
            if (created == JSSetType) {
                if (!isStringOrNullish(typeOf(item)))
                    return false;
                continue;
            }
            auto& pair = m_function.at(originOf(item));
            if (pair.opcode != Opcode::NewArray || pair.arguments.size() < 2 || !isStringOrNullish(typeOf(pair.arguments[0])))
                return false;
        }
        return true;
    }
    for (TypeID type : { m_function.at(given).type, m_function.at(given).prediction(), list.prediction() }) {
        auto* elements = type ? arrayElements(m_program.at(type)) : nullptr;
        if (created == JSMapType && elements)
            elements = arrayElements(*elements);
        if (elements && isStringOrNullish(*elements))
            return true;
    }
    return false;
}

bool Lowering::isUnmodifiedSinceAllocation(ValueID object) const
{
    if (m_function.at(object).opcode != Opcode::NewObjectWithShape)
        return false;
    auto& values = m_currentBlock->values;
    for (size_t i = m_currentIndexInBlock; i--;) {
        if (values[i] == object)
            return true;
        switch (m_function.at(values[i]).opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::FunctionReference:
        case Opcode::BuiltinFunction:
        case Opcode::NewClosure:
            continue;
        case Opcode::PutField:
        case Opcode::PutById:
            if (m_function.at(values[i]).child(0) == object && isInitializingStore(m_program, m_function, *m_currentBlock, i))
                continue;
            return false;
        default:
            return false;
        }
    }
    return false;
}

Boxed Lowering::callBuiltinFunction(const Value& value, OperationEffects effects, std::optional<Boxed> calledOn)
{
    if (!m_isCreatingTableGenerically && createsStringKeyedTable(value)) {
        Boxed list = in<Boxed>(value.arguments[0]);
        bool isSet = m_builtins.classes()[value.engineClass()].type == JSSetType;
        LValue created = isSet ? AOT_TYPED_OPERATION(TryNewStringSet, list).value() : AOT_TYPED_OPERATION(TryNewMapWithStringKeys, list).value();
        return either<Boxed>(Word32(m_out.notZero64(created)), [&] {
            return box(CellPointer(created));
        }, [&] {
            SetForScope isCreatingTableGenerically(m_isCreatingTableGenerically, true);
            return callBuiltinFunction(value, effects, calledOn);
        });
    }
    if (auto& called = m_builtins.at(value.builtin()); !m_isFreezingGenerically && called.kind == BuiltinKind::StaticFunction && called.holder == "Object"_s && called.name == "freeze"_s && value.arguments.size() == 1) {
        auto& type = typeOf(value.arguments[0]);
        bool containsOnlyFixedLayoutObjects = !type.anyObject && !type.objectLayouts.isEmpty() && !type.acceptsUntypedObjects(m_program) && std::ranges::all_of(type.objectLayouts, [&](LayoutID supertype) {
            return std::ranges::all_of(m_program.at(supertype).subtypes, [&](LayoutID subtype) {
                auto& layout = m_program.at(subtype);
                return isObjectLayout(layout.kind) && layout.shape && layout.mayBeFrozen && !layout.mayDespecialize && !layout.acceptsUntypedObjects;
            });
        });
        Word32 frozen = word32(static_cast<int32_t>(orderWordIsFrozen | orderWordIsSealed | orderWordIsNotExtensible));
        if (containsOnlyFixedLayoutObjects && isUnmodifiedSinceAllocation(value.arguments[0])) {
            CellPointer object = in<CellPointer>(value.arguments[0]);
            storeField(object, m_heaps.AOTTypedObject_orderWord, bitOr(orderWordOf(object), frozen));
            return box(object);
        }
        if (containsOnlyFixedLayoutObjects) {
            Boxed object = in<Boxed>(value.arguments[0]);
            LBasicBlock genericPath = newUnlikelyBlock();
            Join join(m_out, Rep::JSValue, Int64);
            orElse(isCell(object, type), genericPath);
            Word32 word = orderWordOf(asCell(object));
            orElse(below(word, word32(orderWordHasExtraProperties)), genericPath);
            static_assert(orderWordHasExtraProperties < orderWordIsFrozen && orderWordIsFrozen < orderWordIsSealed && orderWordIsSealed < orderWordIsNotExtensible);
            storeField(asCell(object), m_heaps.AOTTypedObject_orderWord, bitOr(word, frozen));
            join.add(object);
            m_out.appendTo(genericPath);
            SetForScope isFreezingGenerically(m_isFreezingGenerically, true);
            join.add(callBuiltinFunction(value, effects, calledOn));
            return join.finish().as<Boxed>();
        }
    }
    if (auto& called = m_builtins.at(value.builtin()); !m_isApplyingGenerically && called.kind == BuiltinKind::StaticFunction && called.holder == "Reflect"_s && called.name == "apply"_s && value.arguments.size() == 3) {
        Boxed function = in<Boxed>(value.arguments[0]);
        Boxed list = in<Boxed>(value.arguments[2]);
        LBasicBlock genericPath = newUnlikelyBlock();
        Join join(m_out, Rep::JSValue, Int64);
        orElse(isCell(function), genericPath);
        orElse(hasTypeOfFunctionFlag(asCell(function)), genericPath);
        orElse(isCell(list), genericPath);
        orElse(isPlainArray(asCell(list)), genericPath);
        orElse(belowOrEqual(publicLength(butterflyOf(asCell(list))), integer(numberOfArgumentGPRs)), genericPath);
        join.add(callValueWithArray(Stub::ApplyArray, function, in<Boxed>(value.arguments[1]), list));
        m_out.appendTo(genericPath);
        SetForScope isApplyingGenerically(m_isApplyingGenerically, true);
        join.add(callBuiltinFunction(value, effects, calledOn));
        return join.finish().as<Boxed>();
    }
    if (auto& called = m_builtins.at(value.builtin()); !m_isCreatingArrayGenerically && (called.kind == BuiltinKind::Constructor || called.kind == BuiltinKind::Function) && called.name == "Array"_s && value.arguments.size() == 1) {
        Boxed length = in<Boxed>(value.arguments[0]);
        LBasicBlock genericPath = newUnlikelyBlock();
        Join join(m_out, Rep::JSValue, Int64);
        orElse(isInt32(length), genericPath);
        Integer count = zeroExtended(low32(length));
        orElse(below(count, integer(maxNumberArrayCapacity)), genericPath);
        LValue room = m_out.shl(m_out.bitOr(count.value(), m_out.constInt64(1)), m_out.constInt32(numberArrayCapacityShift));
        LValue created = callAllocationStub(Stub::NewArrayWithContiguous, { { room, stubTemporaryGPRs[0] } }, effectsForOperation<operationAOTTypedNewNumberArray>(), true);
        orElse(m_out.notZero64(created), genericPath);
        m_out.store32(low32(length).value(), butterflyOf(CellPointer(created)).value(), m_heaps.Butterfly_publicLength);
        join.add(box(CellPointer(created)));
        m_out.appendTo(genericPath);
        SetForScope isCreatingArrayGenerically(m_isCreatingArrayGenerically, true);
        join.add(callBuiltinFunction(value, effects, calledOn));
        return join.finish().as<Boxed>();
    }
    auto& builtin = m_builtins.at(value.builtin());
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
            thisValue = box(nonNullish(value.child(0), m_program.stringOf(builtin.nameName)));
        else
            thisValue = in<Boxed>(value.child(0));
        break;
    case BuiltinKind::StaticFunction:
        callee = box(builtinCell(m_builtins.cellOf(value.builtin())));
        thisValue = calledOn ? *calledOn : value.child(0) ? in<Boxed>(value.child(0)) : globalOr(builtin.holderName);
        break;
    case BuiltinKind::Function:
        callee = globalOr(builtin.nameName);
        break;
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

bool Lowering::needsOnlyMethod(const Value& value) const
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
    Boxed returnedValue;
    switch (builtin.kind) {
    case BuiltinKind::Getter:
        returnedValue = emitGetById(box(receiver), name);
        break;
    case BuiltinKind::Setter:
        emitPutById(box(receiver), name, in<Boxed>(value.arguments[0]));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case BuiltinKind::Method:
        if (!value.methodGetById()) {
            returnedValue = name ? callMethodGeneric(box(receiver), name, value.arguments) : callGeneric(box(builtinCell(m_builtins.cellOf(value.builtin()))), box(receiver), value.arguments);
            break;
        }
        if (methodWasFound) {
            returnedValue = callGeneric(in<Boxed>(value.methodGetById()), box(receiver), value.arguments);
            break;
        }
        returnedValue = callGeneric(either<Boxed>(isNotEmpty(in<Boxed>(value.methodGetById())), [&] {
            return in<Boxed>(value.methodGetById());
        }, [&] {
            return box(builtinCell(m_builtins.cellOf(value.builtin())));
        }), box(receiver), value.arguments);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    if (builtin.result.kinds & BuiltinResult::This) {
        return eitherOperand(equal(returnedValue, box(receiver)), repOf(type), [&] {
            return ensure(receiver, typeOf(value.child(0)).withoutPrimitives(Primitive::UndefinedOrNull), type);
        }, [&] {
            return ensure(returnedValue, type);
        });
    }
    return ensure(returnedValue, type);
}

Boxed Lowering::lowerMethodGetById(const Value& value)
{
    if (value.predictedReceiverClassPlusOne && !typeOf(value.child(0)).singleEngineClass()) {
        auto& engineClass = m_builtins.classes()[value.predictedReceiverClassPlusOne - 1];
        Type expected = &engineClass == &m_builtins.stringClass() ? m_program.at(m_program.types.string) : Type::engineClass(engineClass);
        return ifPredictionHolds({ Expectation { value.child(0), &expected, true } }, Rep::JSValue, [&](LBasicBlock) {
            return Operand(boxedConstant(JSValue()));
        }, [&] {
            return Operand(emitGetById(in<Boxed>(value.child(0)), value.string()));
        }).as<Boxed>();
    }
    CellPointer receiver = nonNullish(value.child(0), value.string());
    return either<Boxed>(hasOriginalStructure(receiver, *typeOf(value.child(0)).singleEngineClass()), [&] {
        return boxedConstant(JSValue());
    }, [&] {
        return emitGetById(box(receiver), value.string());
    });
}

Operand Lowering::lowerCallBuiltin(ValueID result, const Value& value)
{
    if (auto& builtin = m_builtins.at(value.builtin()); builtin.hasReceiver()) {
        auto& receiver = typeOf(value.child(0));
        if (!receiver.hasObjects() && receiver.mayBeUndefinedOrNull() && receiver.primitives & (Primitive::Number | Primitive::Boolean)) {
            Type withoutNullish = receiver.withoutPrimitives(Primitive::UndefinedOrNull);
            Rep rep = repOf(typeOf(result));
            return ifPredictionHolds({ { value.child(0), &withoutNullish } }, rep, [&](LBasicBlock) {
                return lowerResolvedBuiltin(result, value);
            }, [&] {
                throwAt(ThrowKind::NullishRead, Operand(thrownNullish(in<Boxed>(value.child(0)))), m_program.stringOf(builtin.nameName).index());
                return placeholder(rep);
            });
        }
    }
    if (value.receiverHasOriginalStructure && value.child(0) && Options::validateAOTTypedFacts()) [[unlikely]] {
        CellPointer receiver = nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
        validateReceiverHasOriginalStructure(hasOriginalStructure(receiver, *typeOf(value.child(0)).singleEngineClass()), receiver, result);
    }
    if (!hasGenericFallback(m_program, m_function, value))
        return lowerResolvedBuiltin(result, value);
    auto answer = value.receiverHasOriginalStructure ? std::optional<bool>(true) : resultOfDominatingGuard(value.child(0), *typeOf(value.child(0)).singleEngineClass(), GuardKind::Structure);
    if (answer.value_or(true)) {
        if (auto searched = lowerArraySearchAsSingleCall(result, value))
            return *searched;
    }
    if (answer.value_or(false))
        return lowerResolvedBuiltin(result, value);
    auto& type = typeOf(result);
    Rep rep = repOf(type);
    CellPointer receiver = nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    if (answer)
        return lowerGenericFallback(value, receiver, type);
    if (tableStubChecksReceiver(result, value)) {
        SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
        m_tableStubChecksReceiver = true;
        Operand returnedValue = lowerResolvedBuiltin(result, value);
        RELEASE_ASSERT(!m_tableStubChecksReceiver);
        return returnedValue;
    }
    if (checksMapGetResultAfterJoin(value)) {
        LBasicBlock isOriginal = m_out.newBlock();
        LBasicBlock hasChanged = newColdBlock();
        Join join(m_out, Rep::JSValue, Int64);
        m_out.branch(hasOriginalStructure(receiver, *typeOf(value.child(0)).singleEngineClass()).value(), usually(isOriginal), rarely(hasChanged));
        m_out.appendTo(isOriginal);
        {
            SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
            m_mapGetReturnsAnything = true;
            join.add(lowerResolvedBuiltin(result, value));
            RELEASE_ASSERT(!m_mapGetReturnsAnything);
            m_out.appendTo(hasChanged);
            join.add(lowerGenericFallback(value, receiver, m_program.at(m_program.types.anything)));
        }
        return ensure(join.finish().as<Boxed>(), type);
    }
    LBasicBlock isOriginal = m_out.newBlock();
    LBasicBlock hasChanged = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    bool methodWasFound = value.methodGetById() && needsOnlyMethod(value);
    m_out.branch((methodWasFound ? isEmpty(in<Boxed>(value.methodGetById())) : hasOriginalStructure(receiver, *typeOf(value.child(0)).singleEngineClass())).value(), usually(isOriginal), rarely(hasChanged));
    m_out.appendTo(isOriginal);
    SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
    Operand resolved;
    {
        SetForScope offer(m_sharedGenericPath, !methodWasFound ? hasChanged : nullptr);
        SetForScope predictedMethodAlreadyRead(m_predictedMethodAlreadyRead, methodWasFound ? value.builtin() : m_predictedMethodAlreadyRead);
        SetForScope sharedPathBuiltin(m_sharedGenericPathBuiltin, value.builtin());
        resolved = lowerResolvedBuiltin(result, value);
    }
    if (rep != Rep::Double || m_lastIntegerConvertedToNumber.number != resolved.value) {
        join.add(resolved);
        m_out.appendTo(hasChanged);
        join.add(lowerGenericFallback(value, receiver, type, methodWasFound));
        return join.finish();
    }
    int64_t minimum = m_lastIntegerConvertedToNumber.minimum;
    ValueFromBlock inlineInteger = m_out.anchor(m_lastIntegerConvertedToNumber.integer);
    join.add(resolved);
    m_out.appendTo(hasChanged);
    Operand general = lowerGenericFallback(value, receiver, type, methodWasFound);
    ValueFromBlock notInteger = m_out.anchor(integer(INT64_MIN).value());
    join.add(general);
    Operand joined = join.finish();
    m_likelyIntegerNumbers.add(result.index(), IntegerConvertedToNumber { joined.value, m_out.phi(Int64, inlineInteger, notInteger), minimum });
    return joined;
}

Operand Lowering::lowerResolvedBuiltin(ValueID result, const Value& value)
{
    auto& builtin = m_builtins.at(value.builtin());
    auto& type = typeOf(result);
    if (value.copiesOwnPropertyDescriptors() && builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "getOwnPropertyDescriptors"_s)
        return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(OwnPropertyDescriptorsUnlessCopied, builtinCallEffects(value), in<Boxed>(value.arguments[0])), type);
    if (value.copiesOwnPropertyDescriptors() && builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "defineProperties"_s) {
        Boxed target = in<Boxed>(value.arguments[0]);
        AOT_TYPED_OPERATION_WITH_EFFECTS(CopyOwnPropertyDescriptors, builtinCallEffects(value), target, in<Boxed>(value.children[2]), in<Boxed>(value.arguments[1]));
        return ensure(target, type);
    }
    switch (builtin.implementation.kind) {
    case BuiltinImplementation::Inline:
        if (auto inlined = lowerInline(builtin.implementation.inlined, result, value))
            return *inlined;
        break;
    case BuiltinImplementation::Number:
        return numberResult(number(m_builtins.numberOf(value.builtin())), type);
    case BuiltinImplementation::Cell:
        return ensure(builtinCell(m_builtins.cellOf(value.builtin())), Type::fromPrimitives(Primitive::Symbol), type);
    case BuiltinImplementation::Call:
    case BuiltinImplementation::Prelude:
        break;
    case BuiltinImplementation::Generic:
    case BuiltinImplementation::Operation:
        RELEASE_ASSERT_NOT_REACHED();
    }
    auto genericPath = [&] {
        Boxed returnedValue = callBuiltinFunction(value, builtinCallEffects(value));
        if (guaranteesResultType(m_program, m_function, value))
            return convert(returnedValue, type, repOf(type));
        return ensure(returnedValue, type);
    };
    PredictedBuiltin expected;
    expected.call = value;
    for (ValueID argument : value.arguments) {
        Type type = predictedTypeOf(argument);
        if (type.mayBeUndefinedOrNull() && !type.isUndefinedOrNull())
            type = type.withoutPrimitives(Primitive::UndefinedOrNull);
        expected.arguments.append(WTF::move(type));
    }
    if (builtin.implementation.kind == BuiltinImplementation::Call && !builtin.hasReceiver()) {
        BuiltinID sibling = applicableEntry(m_builtins.find(builtin.kind, builtin.holderName, builtin.nameName), expected.arguments.span());
        if (sibling && m_builtins.at(sibling).implementation.kind == BuiltinImplementation::Inline)
            expected.call.immediates[0] = sibling.index();
    }
    if (choosePredictedArguments(expected))
        return lowerPredictedBuiltin(result, expected, std::nullopt, genericPath);
    return genericPath();
}

BuiltinName Lowering::globalNameOf(ValueID id) const
{
    while (m_function.at(id).opcode == Opcode::Cast)
        id = m_function.at(id).child(0);
    auto& value = m_function.at(id);
    if (value.opcode != Opcode::GetGlobal)
        return { };
    BuiltinName name = m_program.builtinName(value.string());
    return m_builtins.isGlobal(name) ? name : BuiltinName();
}

std::optional<Lowering::PredictedBuiltin> Lowering::predictedBuiltin(BuiltinKind kind, ValueID receiver, StringID name, const Arguments& arguments) const
{
    auto bytes = m_program.string(name);
    if (!isEnginePrivateName(bytes))
        return predictedBuiltin(kind, receiver, m_program.builtinName(name), arguments);
    auto expected = predictedBuiltin(kind, receiver, m_builtins.nameOf(bytes.subspan(1)), arguments);
    if (expected && !m_builtins.at(expected->call.builtin()).hasPrivateAlias)
        return std::nullopt;
    return expected;
}

std::optional<Lowering::PredictedBuiltin> Lowering::predictedBuiltin(BuiltinKind kind, ValueID receiver, BuiltinName name, const Arguments& arguments) const
{
    if (!name)
        return std::nullopt;
    PredictedBuiltin expected;
    bool usesPredictions = false;
    for (ValueID argument : arguments) {
        usesPredictions |= !!predictionOf(argument);
        expected.arguments.append(predictedTypeOf(argument));
    }
    BuiltinID first;
    const EngineClass* engineClass = nullptr;
    ValueID thisValue;
    if (kind == BuiltinKind::Function) {
        first = m_builtins.find(kind, { }, name);
        engineClass = m_builtins.classNamed(name);
    } else if (BuiltinName holder = globalNameOf(receiver)) {
        if (kind != BuiltinKind::Method)
            return std::nullopt;
        first = m_builtins.findStatic(BuiltinKind::StaticFunction, holder, name);
        engineClass = m_builtins.classNamed(holder);
    } else {
        auto prediction = predictionOf(receiver);
        if (!prediction)
            return std::nullopt;
        engineClass = prediction->isString() ? &m_builtins.stringClass() : prediction->singleEngineClass();
        if (!engineClass || !(prediction->isString() || engineClass->canCheckOriginalStructure()))
            return std::nullopt;
        first = m_builtins.findForInstance(kind, *engineClass, name);
        expected.receiver = *prediction;
        thisValue = receiver;
        usesPredictions = true;
    }
    if (!usesPredictions)
        return std::nullopt;
    BuiltinID entry = applicableEntry(first, expected.arguments.span());
    if (!entry)
        return std::nullopt;
    expected.call.opcode = Opcode::CallBuiltin;
    expected.call.children = { thisValue, ValueID(), ValueID() };
    expected.call.immediates = { entry.index(), engineClass ? engineClass->index : Format::none };
    expected.call.arguments = arguments;
    if (!choosePredictedArguments(expected))
        return std::nullopt;
    return expected;
}

std::optional<Operand> Lowering::lowerBuiltinFunctionCall(ValueID result, ValueID callee, ValueID thisValue, const Arguments& arguments)
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
    PredictedBuiltin expected;
    expected.isFunctionCalledAsValue = true;
    expected.call.opcode = Opcode::CallBuiltin;
    expected.call.immediates = { first.index(), engineClass ? engineClass->index : Format::none };
    expected.call.arguments = arguments;
    auto genericPath = [&, direct = expected.call] {
        return ensure(callBuiltinFunction(direct, OperationEffects { Writes::Heap, true }, thisValue ? in<Boxed>(thisValue) : undefined()), typeOf(result));
    };
    if (first == m_builtins.hasOwnProperty(builtin.nameName) && thisValue && arguments.size() == 1) {
        auto& type = typeOf(result);
        auto& keyType = typeOf(arguments[0]);
        if (!keyType.hasObjects())
            return booleanResult(lowerHasOwnProperty(thisValue, arguments[0]), type);
        LBasicBlock keyIsCell = m_out.newBlock();
        LBasicBlock keyIsPrimitive = m_out.newBlock();
        LBasicBlock keyIsObject = newColdBlock();
        Join join(m_out, repOf(type), b3TypeOf(repOf(type)));
        Boxed key = in<Boxed>(arguments[0]);
        m_out.branch(isCell(key, keyType).value(), unsure(keyIsCell), unsure(keyIsPrimitive));
        m_out.appendTo(keyIsCell);
        orElse(below(loadCellType(asCell(key)), word32(ObjectType)), keyIsObject);
        m_out.jump(keyIsPrimitive);
        m_out.appendTo(keyIsPrimitive);
        join.add(booleanResult(lowerHasOwnProperty(thisValue, arguments[0]), type));
        m_out.appendTo(keyIsObject);
        join.add(genericPath());
        return join.finish();
    }
    if (isMethod && thisValue && arguments.isEmpty() && builtin.holder == "Object"_s && builtin.name == "toString"_s) {
        auto& type = typeOf(result);
        LBasicBlock isNotRetained = newColdBlock();
        Join join(m_out, repOf(type), b3TypeOf(repOf(type)));
        CellPointer object = objectOrElse(thisValue, isNotRetained);
        LValue previousOrRareData = m_out.loadPtr(structureOf(object.value()), m_heaps.Structure_previousOrRareData);
        orElse(m_out.notNull(previousOrRareData), isNotRetained);
        orElse(m_out.logicalNot(isCellOfType(previousOrRareData, StructureType)), isNotRetained);
        LValue cache = m_out.loadPtr(previousOrRareData, m_heaps.StructureRareData_specialPropertyCache);
        orElse(m_out.notNull(cache), isNotRetained);
        LValue text = m_out.loadPtr(cache, m_heaps.SpecialPropertyCache_cachedToStringTagValue);
        orElse(m_out.above(text, m_out.constIntPtr(std::bit_cast<uintptr_t>(JSCell::seenMultipleCalleeObjects()))), isNotRetained);
        join.add(ensure(CellPointer(text), m_program.at(m_program.types.string), type));
        m_out.appendTo(isNotRetained);
        join.add(genericPath());
        return join.finish();
    }
    if (isMethod) {
        bool isString = engineClass == &m_builtins.stringClass();
        if (!thisValue || !engineClass || !(isString || engineClass->canCheckOriginalStructure()))
            return genericPath();
        expected.receiver = isString ? m_program.at(m_program.types.string) : Type::engineClass(*engineClass);
        expected.call.children = { thisValue, ValueID(), ValueID() };
    } else if (!thisValue || globalNameOf(thisValue) != builtin.holderName) {
        bool throwsWithoutHolder = engineClass == &m_builtins.promiseClass() || builtin.holderName == BuiltinName(AOT_TYPED_BUILTIN_NAME_TypedArray);
        if (throwsWithoutHolder || (thisValue && !typeOf(thisValue).isUndefinedOrNull()))
            return genericPath();
    }
    Vector<Type, 4> types;
    for (ValueID argument : arguments) {
        types.append(typeOf(argument));
        Type type = predictedTypeOf(argument);
        if (type.mayBeUndefinedOrNull() && !type.isUndefinedOrNull())
            type = type.withoutPrimitives(Primitive::UndefinedOrNull);
        expected.arguments.append(WTF::move(type));
    }
    if (isMethod && engineClass->type == RegExpObjectType && expected.arguments.size() == 1 && expected.arguments[0].primitives & Primitive::String && !expected.arguments[0].isString())
        expected.arguments[0] = m_program.at(m_program.types.string);
    BuiltinID entry = applicableEntry(first, expected.arguments.span());
    if (!entry || m_builtins.at(entry).implementation.kind != BuiltinImplementation::Inline)
        return genericPath();
    expected.call.immediates[0] = entry.index();
    if (!choosePredictedArguments(expected)) {
        if (!writesOfInlineBuiltin(m_builtins.at(entry).implementation.inlined, types.span()))
            return genericPath();
        expected.arguments = WTF::move(types);
    }
    return lowerPredictedBuiltin(result, expected, std::nullopt, genericPath);
}

bool Lowering::choosePredictedArguments(PredictedBuiltin& expected) const
{
    auto& builtin = m_builtins.at(expected.call.builtin());
    if (builtin.implementation.kind != BuiltinImplementation::Inline)
        return false;
    InlineBuiltin which = builtin.implementation.inlined;
    auto& arguments = expected.call.arguments;
    Vector<Type, 4> needed = expected.arguments;
    for (unsigned i = 0; i < arguments.size(); ++i) {
        if (builtin.parameters[std::min<size_t>(i, builtin.parameters.size() - 1)].conversion == ArgumentConversion::None)
            needed[i] = typeOf(arguments[i]);
    }
    if (writesOfInlineBuiltin(which, needed.span()))
        expected.arguments = WTF::move(needed);
    else if (!writesOfInlineBuiltin(which, expected.arguments.span())) {
        for (auto& type : expected.arguments) {
            if (type.isNumber() && !type.isInteger())
                type = m_program.at(m_program.types.int32);
        }
        if (!writesOfInlineBuiltin(which, expected.arguments.span()))
            return false;
    }
    if (builtin.holderName == m_builtins.stringClass().name) {
        for (unsigned i = 0; i < arguments.size(); ++i) {
            if (predictionOf(arguments[i]) && expected.arguments[i].isNumber())
                expected.arguments[i] = m_program.at(m_program.types.int32);
        }
    }
    if (which == InlineBuiltin::ArrayWithLength && predictionOf(arguments[0]) && expected.arguments[0].isNumber())
        expected.arguments[0] = m_program.at(m_program.types.int32);
    for (unsigned i = 0; i < arguments.size(); ++i) {
        if (expected.arguments[i] != typeOf(arguments[i]))
            return true;
    }
    return !expected.receiver.isNever();
}

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

Operand Lowering::numberResult(Number returnedValue, const Type& type)
{
    auto& numberType = m_program.at(m_program.types.number);
    if (repOf(type) == Rep::JSValue)
        return ensure(boxNumber(returnedValue), numberType, type);
    return ensure(returnedValue, numberType, type);
}

Operand Lowering::booleanResult(Word32 returnedValue, const Type& type)
{
    return ensure(returnedValue, m_program.at(m_program.types.boolean), type);
}

Operand Lowering::valueResult(ValueID returnedValue, const Type& type)
{
    return ensure(get(returnedValue), typeOf(returnedValue), type);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
