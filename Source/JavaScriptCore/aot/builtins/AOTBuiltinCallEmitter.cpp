/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCallEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinLocationWatch.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTCallEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTBuiltinTypes.h"
#include "AOTConvention.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTInlineBuiltinEmitter.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTOperationData.h"
#include "AOTPlacement.h"
#include "AOTRegExpFlags.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTStaticsEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCallHost.h"
#include "AOTVector.h"
#include "CodeSpecializationKind.h"
#include "FunctionRareData.h"
#include "JSCJSValueInlines.h"
#include "MathCommon.h"
#include "NativeExecutable.h"
#include "PageCount.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(numberOfArgumentGPRs <= StubKey::maxCount);
static_assert(sizeof(WatchedLocationState) == sizeof(uint8_t));

// The built-in's own stub for boxed operands. A call by value finds it through the callee's executable; a call that names the built-in finds it in the table.
// The stub takes the call's registers and falls back to the host function by itself.
static std::optional<StubKey> boxedCallStubFor(const BuiltinTable& builtins, BuiltinID builtin, unsigned count)
{
    std::optional<BuiltinBoxedStub> boxedStub = builtins.boxedCallStubOf(builtin);
    if (!boxedStub || !exists(callBoxedBuiltin(boxedCallFormOf(*boxedStub), count)) || !acceptsArgumentCount(boxedCallFormOf(*boxedStub), count))
        return std::nullopt;
    return callBoxedBuiltin(boxedCallFormOf(*boxedStub), count);
}

static StubKey hostFunctionStubFor(unsigned count)
{
    StubKey withoutBoxedCall = callHostFunctionWithoutBoxedCall(count);
    return exists(withoutBoxedCall) ? withoutBoxedCall : callHostFunction(count);
}

static std::optional<StubKey> stubKeyFor(const BuiltinTable& builtins, const BuiltinOverload& overload, unsigned count, bool validates)
{
    if (count > numberOfArgumentGPRs)
        return std::nullopt;
    bool constructs = overload.mode == BuiltinCallMode::Construct;
    // A validating build calls the host function every time, so that each call is compared with the built-in's table entry.
    if (std::optional<StubKey> boxedCall = constructs || validates ? std::nullopt : boxedCallStubFor(builtins, overload.builtin, count))
        return boxedCall;
    switch (overload.code.implementation.callee) {
    case BuiltinCallee::None:
    case BuiltinCallee::Function:
    case BuiltinCallee::CustomGetter:
        return std::nullopt;
    case BuiltinCallee::HostFunction:
        if (constructs)
            return std::nullopt;
        return hostFunctionStubFor(count);
    case BuiltinCallee::InternalFunction:
        return callInternalFunction(constructs ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall, count);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<uint32_t> hostFunctionEntryOf(const BuiltinTable& builtins, const BuiltinOverload& called)
{
    for (BuiltinOverloadID id : builtins.overloadIDsOf(called.builtin)) {
        if (builtins.at(id).mode != called.mode)
            continue;
        std::optional<uint32_t> number = BuiltinBindings::directFunctionNumberOf(id);
        if (number && BuiltinBindings::directFunctionAt(*number).entersHostFunction())
            return number;
    }
    return std::nullopt;
}

static std::optional<URICoding> uriCodingOf(BuiltinID builtin)
{
    if (builtin == Builtins::DecodeURI)
        return URICoding::DecodeURI;
    if (builtin == Builtins::DecodeURIComponent)
        return URICoding::DecodeURIComponent;
    if (builtin == Builtins::EncodeURI)
        return URICoding::EncodeURI;
    if (builtin == Builtins::EncodeURIComponent)
        return URICoding::EncodeURIComponent;
    return std::nullopt;
}

static void failAndDefineZero(CodeGenerator& generator, const ValueContext& value, InternalErrorKind kind)
{
    generator.fail(value, kind);
    generator.define(value, generator.emitter<ConstantEmitter>().zeroOf(value.representation()));
}

BuiltinCallEmitter::BuiltinCallEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

void BuiltinCallEmitter::generateCallBuiltin(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    if (isCallThatRequiredPassRemoves(m_program, value.value())) {
        failAndDefineZero(generator(), value, InternalErrorKind::RequiredPassLeftOperation);
        return;
    }
    BuiltinOverloadID id = value->overload();
    const BuiltinOverload& overload = m_builtins.at(id);
    ValueID receiver = value->receiverOperand();
    std::span<const ValueID> arguments = value->argumentsOperands();

    auto defineResult = [&](Boxed result) {
        defineAs(value, result, typeOf(value.id()));
    };
    auto callFunction = [&] {
        std::optional<uint32_t> number = value->builtinCallForm() == BuiltinCallForm::Whole ? BuiltinBindings::directFunctionNumberOf(id, arguments.size()) : std::nullopt;
        // A direct entry that answers fields makes no object. Where the program keeps the object and every view is whole, the function itself makes it.
        if (number && BuiltinBindings::directFunctionAt(*number).returnsFields) {
            if (BuiltinBindings::directFunctionAt(*number).numberOfBounds()) {
                defineResult(boxCell(makeObjectOfDirectEntry(value, *number)));
                return;
            }
            number = std::nullopt;
        }
        if (number && BuiltinBindings::directFunctionAt(*number).returnsNumber) {
            DirectOperands operands = directOperandsOf(value, BuiltinBindings::directFunctionAt(*number));
            defineAs(value, Number(emitter<RuntimeCallEmitter>().callDirectFunctionReturningNumber(value, *number, operands.words.span(), operands.keptAlive.span())), typeOf(value.id()));
            return;
        }
        // A validating build makes no host frame for a direct call, so what needs one goes through the host function there.
        bool needsHostFrame = number && (BuiltinBindings::directFunctionAt(*number).takesFrame() || (receiver && overload.throws() && !overload.onlyPropagatesErrors() && BuiltinCellKey::object(overload.builtin).hasCell(m_builtins)));
        if (number && !BuiltinBindings::directFunctionAt(*number).entersHostFunction() && !(generator().validates() && needsHostFrame)) {
            defineResult(callDirectFunction(value, *number));
            return;
        }
        BuiltinID builtin = m_builtins.canonicalID(overload.builtin);
        // Bytecode makes the array of `Array(length)` in the caller, so the stack has no frame of Array.
        if (builtin == Builtins::Array && value->isDoneInPlace() && arguments.size() == 1) {
            defineResult(boxCell(callOperation<CellPointer>(value, RuntimeOperation::ArrayOfLengthOrElement, { boxedOperand(value, arguments[0]) })));
            return;
        }
        if (builtin == Builtins::Number && overload.mode == BuiltinCallMode::Call && arguments.size() == 1) {
            defineResult(callOperation<Boxed>(value, RuntimeOperation::NumberOfAnyValue, { boxedOperand(value, arguments[0]) }, { .calledBuiltin = builtin }));
            return;
        }
        if (builtin == Builtins::String && overload.mode == BuiltinCallMode::Call && arguments.size() == 1) {
            Boxed operand = boxedOperand(value, arguments[0]);
            auto convert = [&] {
                return callOperation<Boxed>(value, RuntimeOperation::StringOfAnyValue, { operand }, { .calledBuiltin = builtin });
            };
            if (value.shape() == CodeShape::Compact) {
                defineResult(convert());
                return;
            }
            const TypeTestPlan& isString = typeTestPlanFor(m_function.at(arguments[0]).type, representationOf(arguments[0]), TypeTable::string);
            defineResult(either<Boxed>(hasType(get(arguments[0]), isString), [&] {
                return operand;
            }, convert));
            return;
        }
        std::optional<Boxed> boxedReceiver;
        if (receiver)
            boxedReceiver = boxedOperand(value, receiver);
        Vector<Boxed, 8> boxedArguments;
        for (ValueID argument : arguments)
            boxedArguments.append(boxedOperand(value, argument));
        auto argumentOrUndefined = [&](size_t index) {
            return index < boxedArguments.size() ? boxedArguments[index] : undefined();
        };
        auto answerOrCallFunction = [&](RuntimeOperation operation, std::initializer_list<Operand> operands) {
            Boxed answer = callOperation<Boxed>(value, operation, operands, { .calledBuiltin = builtin });
            defineResult(either<Boxed>(isNotEmpty(answer), [&] {
                return answer;
            }, [&] {
                return callBuiltinFunction(value, id, boxedReceiver, boxedArguments.span());
            }));
        };
        bool isCall = overload.mode == BuiltinCallMode::Call;
        if (std::optional<URICoding> coding = uriCodingOf(builtin); coding && isCall && arguments.size() == 1)
            return answerOrCallFunction(RuntimeOperation::CodeURIOfAnyValue, { boxedArguments[0], word32(static_cast<uint32_t>(*coding)) });
        bool searchesArray = builtin == Builtins::ArrayPrototypeIndexOf || builtin == Builtins::ArrayPrototypeIncludes;
        if (searchesArray && isCall && receiver && arguments.size() == 2 && typeOf(arguments[0]).primitives & Primitive::AnyString)
            return answerOrCallFunction(RuntimeOperation::ArraySearchOfAnyValue, { *boxedReceiver, boxedArguments[0], boxedArguments[1], word32(builtin == Builtins::ArrayPrototypeIndexOf) });
        auto isEncodingForTheHost = [&](ValueID encoding) {
            const Value* definition = &m_function.at(encoding);
            while (isCast(definition->opcode))
                definition = &m_function.at(definition->operand(0));
            if (definition->opcode != Opcode::String)
                return false;
            StringView name { byteCast<Latin1Character>(m_program.strings[definition->string()]) };
            return name != "utf8"_s && name != "utf-8"_s && name != "ascii"_s && name != "latin1"_s && name != "binary"_s;
        };
        if (builtin == Builtins::BufferPrototypeToString && isCall && receiver && arguments.size() <= 3 && (arguments.empty() || !isEncodingForTheHost(arguments[0])))
            return answerOrCallFunction(RuntimeOperation::TextOfBytes, { *boxedReceiver, argumentOrUndefined(0), argumentOrUndefined(1), argumentOrUndefined(2) });
        bool isCalledOnEncoder = isCall && receiver && typeOf(receiver).isSubsetOf(Type::builtinClass(BuiltinClasses::TextEncoder), m_program);
        if (builtin == Builtins::TextEncoderPrototypeEncode && isCalledOnEncoder && arguments.size() == 1)
            return answerOrCallFunction(RuntimeOperation::EncodeTextToBytes, { boxedArguments[0] });
        if (builtin == Builtins::TextEncoderPrototypeEncodeInto && isCalledOnEncoder && arguments.size() == 2)
            return answerOrCallFunction(RuntimeOperation::EncodeTextIntoBytes, { boxedArguments[0], boxedArguments[1] });
        auto mayBeBytes = [&](ValueID id) {
            const Type& type = typeOf(id);
            return type.overlaps(Type::builtinClass(BuiltinClasses::Uint8Array), m_program) || type.overlaps(Type::builtinClass(BuiltinClasses::Buffer), m_program);
        };
        if (builtin == Builtins::TypedArrayPrototypeSet && isCall && receiver && (arguments.size() == 1 || arguments.size() == 2) && typeOf(receiver).overlaps(typeOf(arguments[0]), m_program))
            return answerOrCallFunction(RuntimeOperation::TypedArraySetOfAnyValue, { *boxedReceiver, boxedArguments[0], argumentOrUndefined(1) });
        if (builtin == Builtins::TypedArrayPrototypeSubarray && isCall && receiver && arguments.size() <= 2 && mayBeBytes(receiver))
            return answerOrCallFunction(RuntimeOperation::TypedArraySubarrayOfAnyValue, { *boxedReceiver, argumentOrUndefined(0), argumentOrUndefined(1) });
        if (builtin == Builtins::ObjectPrototypeToString && isCall && receiver && arguments.empty())
            return answerOrCallFunction(RuntimeOperation::ObjectToStringOfAnyValue, { *boxedReceiver });
        if (builtin == Builtins::BufferByteLength && isCall && (arguments.size() == 1 || arguments.size() == 2))
            return answerOrCallFunction(RuntimeOperation::ByteLengthOfAnyValue, { boxedArguments[0], argumentOrUndefined(1) });
        if (std::optional<uint32_t> entry = hostFunctionEntryOf(m_builtins, overload); entry && receiver && !generator().validates()) {
            defineResult(callDirectFunction(value, *entry));
            return;
        }
        defineResult(callBuiltinFunction(value, id, boxedReceiver, boxedArguments.span()));
    };

    switch (value->builtinCallForm()) {
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix: {
        std::optional<InlineBuiltin> builtin = inlineImplementationOf(m_program, value.value());
        if (!builtin) {
            fail(value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
        if (*builtin == InlineBuiltin::FieldsOfDirectEntry) {
            generateFieldsOfDirectEntry(value);
            return;
        }
        InlineCall call(value, *builtin, { });
        emitter<InlineBuiltinEmitter>().generate(call);
        return;
    }
    case BuiltinCallForm::List:
        if (receiver || arguments.size() != 1 || overload.mode != BuiltinCallMode::Call) {
            failAndDefineZero(generator(), value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
        defineResult(callOperation<Boxed>(value, RuntimeOperation::Apply, { boxCell(calleeOf(value, overload)), undefined(), boxedOperand(value, arguments[0]) }));
        return;
    case BuiltinCallForm::AnyReceiver:
        if (m_builtins.canonicalID(overload.builtin) == Builtins::ArrayPrototypeJoin && overload.code.implementation.kind == BuiltinImplementationKind::Inline && overload.code.implementation.inlineBuiltin == InlineBuiltin::ArrayJoin && overload.mode == BuiltinCallMode::Call && receiver && arguments.size() <= 1) {
            Boxed separator = arguments.empty() ? undefined() : boxedOperand(value, arguments[0]);
            defineResult(callOperation<Boxed>(value, RuntimeOperation::ArrayJoinOfAnyReceiver, { boxedOperand(value, receiver), separator, calleeOf(value, overload) }));
            return;
        }
        // A member that is a slot of the object itself, a number or a cell has no accessor function to call.
        if (overload.mode == BuiltinCallMode::Get && receiver && !BuiltinCellKey::getter(overload.builtin).hasCell(m_builtins)) {
            defineResult(emitter<AccessEmitter>().getById(value, boxedOperand(value, receiver), keyOf(overload), SiteCacheID()));
            return;
        }
        callFunction();
        return;
    case BuiltinCallForm::Whole:
        break;
    }

    switch (overload.code.implementation.kind) {
    case BuiltinImplementationKind::Generic:
    case BuiltinImplementationKind::HostCall:
    case BuiltinImplementationKind::Compiled:
    case BuiltinImplementationKind::Inline:
        callFunction();
        return;
    case BuiltinImplementationKind::Number: {
        std::optional<double> number = m_builtins.numberOf(overload.builtin);
        if (!number) {
            failAndDefineZero(generator(), value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
        defineResult(emitter<ConstantEmitter>().constantOf(jsNumber(*number), Representation::JSValue).as<Boxed>());
        return;
    }
    case BuiltinImplementationKind::Cell:
        defineResult(boxCell(builtinCell(value, BuiltinCellKey::object(overload.builtin))));
        return;
    case BuiltinImplementationKind::CachedGetter: {
        if (!receiver) {
            failAndDefineZero(generator(), value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
        std::optional<uint32_t> offsetOfCachedValue = BuiltinBindings::cachedValueOffsetOf(id);
        if (!offsetOfCachedValue) {
            callFunction();
            return;
        }
        defineResult(readCachedGetter(value, overload, *offsetOfCachedValue));
        return;
    }
    case BuiltinImplementationKind::OwnSlot:
        if (!receiver) {
            failAndDefineZero(generator(), value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
        defineResult(readOwnSlot(value, overload));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed BuiltinCallEmitter::readCachedGetter(const ValueContext& value, const BuiltinOverload& overload, uint32_t offsetOfCachedValue)
{
    RELEASE_ASSERT(overload.mode == BuiltinCallMode::Get);
    Boxed receiver = boxedOperand(value, value->receiverOperand());
    Boxed cached(m_out.load64(m_out.address(m_heaps.hostObjectCachedValues, unboxCell(receiver).value(), offsetOfCachedValue)));
    return either<Boxed>(isNotEmpty(cached), [&] {
        return cached;
    }, [&] {
        return callBuiltinFunction(value, value->overload(), receiver, { });
    });
}

PropertyKeyID BuiltinCallEmitter::keyOf(const BuiltinOverload& overload) const
{
    return m_program.keyOf(m_builtins.at(overload.builtin).name);
}

Boxed BuiltinCallEmitter::loadProperty(CellPointer object, PropertyOffset offset)
{
    const B3::AbstractHeap& properties = m_heaps.entire(AbstractHeap::NamedProperties);
    if (isInlineOffset(offset))
        return Boxed(m_out.load64(m_out.address(properties, object.value(), JSObject::offsetOfInlineStorage() + offset * static_cast<ptrdiff_t>(sizeof(EncodedJSValue)))));

    Pointer butterfly = loadField<Pointer>(object, m_heaps.JSObject_butterfly);
    Boxed property(m_out.load64(m_out.address(properties, butterfly.value(), offsetInButterfly(offset) * static_cast<ptrdiff_t>(sizeof(EncodedJSValue)))));
    ensureStillAliveHere(object);
    return property;
}

// One for each class that the receiver may be an instance of, where every class has the slot and one load reads them all. Otherwise the one that the call names.
Vector<BuiltinOverloadID, 4> BuiltinCallEmitter::ownSlotsReadBy(const ValueContext& value, const BuiltinOverload& overload)
{
    auto hasWord = [&](BuiltinOverloadID id) {
        return m_builtins.at(id).code.implementation.ownSlot.structure.index() < BuiltinWords::numberOfOwnSlotStructures;
    };
    BuiltinOverloadID named = value->overload();
    if (!m_builtins.receiverClassOf(overload.builtin) || !hasWord(named))
        return { };

    Vector<BuiltinOverloadID, 4> slots;
    const Type& receiver = typeOf(value->receiverOperand());
    bool everyClassHasSlot = receiver.isObject() && receiver.hasOnlyClassObjects() && std::ranges::all_of(receiver.classes, [&](const ClassPart& part) {
        BuiltinOverloadID slot = findOwnSlotOfInstances(m_builtins, part.builtinClass, m_builtins.at(overload.builtin).name);
        if (!slot || !hasWord(slot) || !liesAtSamePlace(m_builtins, slot, named))
            return false;
        slots.append(slot);
        return true;
    });
    if (!everyClassHasSlot || slots.isEmpty())
        return { named };
    return slots;
}

Boxed BuiltinCallEmitter::readOwnSlot(const ValueContext& value, const BuiltinOverload& overload)
{
    RELEASE_ASSERT(overload.mode == BuiltinCallMode::Get);
    Boxed receiver = boxedOperand(value, value->receiverOperand());
    auto readByName = [&] {
        return emitter<AccessEmitter>().getById(value, receiver, keyOf(overload), SiteCacheID());
    };
    Vector<BuiltinOverloadID, 4> slots = ownSlotsReadBy(value, overload);
    if (slots.isEmpty())
        return readByName();

    CellPointer object = unboxCell(receiver);
    Word32 structureOfObject = loadStructureID(object);
    LBasicBlock hasSlot = newBlock();
    LBasicBlock hasOtherStructure = newBlock();
    Word32 lastPublished = word32(0);
    uint32_t wordsAsked = 0;
    for (BuiltinOverloadID slot : slots) {
        unsigned word = m_builtins.at(slot).code.implementation.ownSlot.structure.index();
        wordsAsked |= 1u << word;
        lastPublished = loadMutable32(AOT_INSTANCE_OFFSET(builtins.ownSlotStructureIDs) + word * static_cast<ptrdiff_t>(sizeof(uint32_t)));
        bool isLast = slot == slots.last();
        LBasicBlock next = isLast ? hasOtherStructure : newBlock();
        m_out.branch(equal(structureOfObject, lastPublished).value(), normalEdge(hasSlot), isLast ? rareEdge(next) : normalEdge(next));
        m_out.appendTo(next);
    }

    auto wereLookedFor = [&] {
        if (slots.size() == 1)
            return isNotZero(lastPublished);
        Word32 asked = word32(static_cast<int32_t>(wordsAsked));
        return equal(bitAnd(loadMutable32(AOT_INSTANCE_OFFSET(builtins.ownSlotStructuresLookedFor)), asked), asked);
    };
    LBasicBlock looksFor = newBlock();
    LBasicBlock readsByName = newBlock();
    m_out.branch(wereLookedFor().value(), normalEdge(readsByName), rareEdge(looksFor));
    m_out.appendTo(looksFor);
    for (BuiltinOverloadID slot : slots)
        callVoidOperation(value, RuntimeOperation::FindOwnSlotStructure, { word32(static_cast<int32_t>(slot.index())) }, { .isOnRarePath = true });
    m_out.jump(readsByName);

    Join<Boxed> join(m_out);
    m_out.appendTo(readsByName);
    join.add(readByName());
    m_out.appendTo(hasSlot);
    const BuiltinOwnSlot& place = overload.code.implementation.ownSlot;
    join.add(loadProperty(object, propertyOffsetOfOwnSlot(m_builtins.at(m_builtins.receiverClassOf(overload.builtin)).type, place.offset)));
    return join.finish();
}

Boxed BuiltinCallEmitter::callCustomGetter(const OperationContext& context, BuiltinOverloadID id, Boxed receiver, bool receiverIsMatched)
{
    const BuiltinOverload& overload = m_builtins.at(id);
    RELEASE_ASSERT(overload.mode == BuiltinCallMode::Get);
    if (!receiverIsMatched)
        return emitter<AccessEmitter>().getById(context, receiver, keyOf(overload), SiteCacheID());
    return callOperation<Boxed>(context, RuntimeOperation::CallCustomGetter, { receiver, word32(static_cast<int32_t>(id.index())), identifierNumber(keyOf(overload)) });
}

CellPointer BuiltinCallEmitter::builtinCell(const OperationContext& context, BuiltinCellKey key)
{
    return emitter<ConstantEmitter>().builtinCell(context, key);
}

Boxed BuiltinCallEmitter::builtinValue(const OperationContext& context, BuiltinCellKey key)
{
    RELEASE_ASSERT(key.kind() == BuiltinCellKey::Kind::Value);
    std::optional<uint32_t> number = m_placement.builtinCell(key);
    if (!number) {
        fail(context, InternalErrorKind::PlacedPayloadMissing);
        return undefined();
    }
    const B3::AbstractHeap& heap = m_heaps.entire(AbstractHeap::RuntimeState);
    ptrdiff_t offsetInTable = *number * static_cast<ptrdiff_t>(sizeof(EncodedJSValue));
    Boxed filled(m_out.load64(InstanceLayout::holdsBuiltinCells(m_placement.builtinCells().size())
        ? instanceField(heap, static_cast<ptrdiff_t>(InstanceLayout::offsetOfBuiltinValues) + offsetInTable)
        : m_out.address(heap, loadImmutablePointer(AOT_INSTANCE_OFFSET(builtins.hostValues)).value(), offsetInTable)));
    return either<Boxed>(isNotEmpty(filled), [&] {
        return filled;
    }, [&] {
        return callOperation<Boxed>(context, RuntimeOperation::FillBuiltinValue, { word32(static_cast<int32_t>(key.encoded())) }, { .isOnRarePath = true });
    });
}

CellPointer BuiltinCallEmitter::calleeOf(const OperationContext& context, const BuiltinOverload& overload)
{
    switch (overload.mode) {
    case BuiltinCallMode::Call:
    case BuiltinCallMode::Construct:
        return builtinCell(context, BuiltinCellKey::object(overload.builtin));
    case BuiltinCallMode::Get:
        return builtinCell(context, BuiltinCellKey::getter(overload.builtin));
    case BuiltinCallMode::Set:
        return builtinCell(context, BuiltinCellKey::setter(overload.builtin));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed BuiltinCallEmitter::callBuiltinFunction(const OperationContext& context, BuiltinOverloadID id, std::optional<Boxed> receiver, std::span<const Boxed> arguments)
{
    const BuiltinOverload& overload = m_builtins.at(id);
    const Value* call = context.valueID() ? &m_function.at(context.valueID()) : nullptr;
    bool takesAnyReceiver = call && call->opcode == Opcode::CallBuiltin && call->builtinCallForm() == BuiltinCallForm::AnyReceiver;
    if (overload.code.implementation.callee == BuiltinCallee::CustomGetter) {
        RELEASE_ASSERT(receiver && arguments.empty());
        bool receiverIsMatched = call && call->opcode == Opcode::CallBuiltin && call->overload() == id && !takesAnyReceiver;
        return callCustomGetter(context, id, *receiver, receiverIsMatched);
    }

    // The function that an accessor has bound to the receiver. The accessor is asked for it, whatever the receiver has under the name by now.
    if (overload.mode == BuiltinCallMode::Call && m_builtins.at(overload.builtin).kind == BuiltinKind::Accessor) {
        RELEASE_ASSERT(receiver);
        Boxed holder = boxCell(builtinCell(context, BuiltinCellKey::object(m_builtins.at(overload.builtin).holder)));
        Boxed bound = callOperation<Boxed>(context, RuntimeOperation::GetSuperPropertyById, { holder, *receiver, identifierNumber(keyOf(overload)) });
        return emitter<CallEmitter>().callValue(context, { bound, *receiver, arguments });
    }

    bool constructs = overload.mode == BuiltinCallMode::Construct;
    Boxed callee = boxCell(calleeOf(context, overload));
    Boxed thisValue = undefined();
    if (constructs)
        thisValue = callee;
    else if (receiver)
        thisValue = *receiver;
    else if (BuiltinID holder = m_builtins.at(overload.builtin).holder; holder && BuiltinCellKey::object(holder).hasCell(m_builtins))
        thisValue = boxCell(builtinCell(context, BuiltinCellKey::object(holder)));

    CallEmitter& calls = emitter<CallEmitter>();
    CallOptions options { .constructs = constructs };
    CalledBuiltin calledBuiltin { id, takesAnyReceiver };
    unsigned count = static_cast<unsigned>(arguments.size());
    if (std::optional<StubKey> stub = stubKeyFor(m_builtins, overload, count, generator().validates())) {
        Vector<LValue, 8> values;
        for (Boxed argument : arguments)
            values.append(argument.value());
        return calls.callByConvention(context, { .stub = *stub, .callee = callee.value(), .thisValue = thisValue.value(), .arguments = values.span(), .builtin = calledBuiltin }, options);
    }
    if (constructs)
        return callOperation<Boxed>(context, RuntimeOperation::Construct, { callee, callee, word32(count), argumentBuffer(arguments) }, options);
    return calls.callValue(context, { callee, thisValue, arguments, calledBuiltin }, options);
}

// The address of the first byte and the number of bytes, as they are now. A view that has lost its memory has none.
// Bounds were taken when the program cut the part out. The view had a fixed length then, so since then it can only have lost its memory.
void BuiltinCallEmitter::appendBytesOf(const ValueContext& value, DirectOperands& operands, ValueID id, std::span<const ValueID> bounds)
{
    CellPointer view = cellOperand(id);
    bool checksPart = generator().validates() && !bounds.empty();
    // The pass that moved the call to the overload with bounds had the proof that this is a view.
    if (checksPart)
        emitter<RuntimeValidationEmitter>().checkPrecondition(value, hasCellTypeIn(view, { Int8ArrayType, DataViewType }));
    TypedArrayType type = typedArrayTypeOf(m_builtins, typeOf(id));
    auto askView = [&] {
        return callLeaf<Integer>(RuntimeOperation::ViewByteLength, { view });
    };
    Integer size;
    if (isTypedView(type)) {
        LBasicBlock isNotFixedLength = newBlock();
        Join<Integer> join(m_out);
        Integer length = emitter<ElementAccessEmitter>().typedArrayElementsOf(view, isNotFixedLength).length;
        join.add(Integer(m_out.shl(length.value(), m_out.constInt32(logElementSize(type)))));
        m_out.appendTo(isNotFixedLength);
        join.add(askView());
        size = join.finish();
    } else
        size = askView();
    Pointer first = loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector);
    if (!bounds.empty()) {
        auto atMost = [&](Integer value, Integer limit) {
            return select(below(value, limit), value, limit);
        };
        if (checksPart) {
            // A part lies inside its view. A whole view is written as an end that no part reaches.
            Integer givenStart = integerOperand(bounds[0]);
            Integer givenEnd = integerOperand(bounds[1]);
            Condition partEndsInsideView = belowOrEqual(givenEnd, size);
            Condition endsInsideView = logicalOr(partEndsInsideView, aboveOrEqual(givenEnd, static_cast<int64_t>(MAX_ARRAY_BUFFER_SIZE)));
            RuntimeValidationEmitter& validation = emitter<RuntimeValidationEmitter>();
            validation.checkPrecondition(value, belowOrEqual(givenStart, givenEnd));
            validation.checkPrecondition(value, logicalOr(endsInsideView, isZero(size)));
        }
        Integer end = atMost(integerOperand(bounds[1]), size);
        Integer start = atMost(integerOperand(bounds[0]), end);
        first = offsetBy(first, start, 0);
        size = sub(end, start);
    }
    operands.words.append(first.value());
    operands.words.append(size.value());
    operands.keptAlive.append(view.value());
}

BuiltinCallEmitter::DirectOperands BuiltinCallEmitter::directOperandsOf(const ValueContext& value, const BuiltinDirectFunctionBinding& binding)
{
    RELEASE_ASSERT(value->argumentsOperands().size() >= binding.minArgumentCount());
    RELEASE_ASSERT(!binding.takesReceiver() || value->receiverOperand());
    std::span<const ValueID> bounds = value->argumentsOperands().last(binding.numberOfBounds());
    std::span<const ValueID> arguments = value->argumentsOperands().first(value->argumentsOperands().size() - bounds.size());
    auto takeBounds = [&] {
        std::span<const ValueID> taken = bounds.first(2);
        bounds = bounds.subspan(2);
        return taken;
    };
    std::span<const BuiltinDirectValue> parameters = std::span(binding.parameters).first(binding.parameterCount);

    // A rope is resolved first: that allocates, and no address is taken before it.
    Vector<CharacterView, 1> characters;
    size_t next = 0;
    for (BuiltinDirectValue parameter : parameters) {
        if (parameter == BuiltinDirectValue::Characters)
            characters.append(emitter<StringEmitter>().viewOf(value, arguments[next]));
        next += BuiltinDirectFunctionBinding::takesArgument(parameter) && next < arguments.size();
    }

    DirectOperands operands;
    size_t nextCharacters = 0;
    next = 0;
    for (BuiltinDirectValue parameter : parameters) {
        switch (parameter) {
        case BuiltinDirectValue::Realm:
            operands.words.append(loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject)).value());
            break;
        case BuiltinDirectValue::NativeObjectOfReceiver:
            operands.words.append(m_out.loadPtr(m_out.address(m_heaps.root, cellOperand(value->receiverOperand()).value(), binding.offsetOfNativeObject)));
            break;
        case BuiltinDirectValue::Receiver:
            operands.words.append(boxedOperand(value, value->receiverOperand()).value());
            break;
        case BuiltinDirectValue::Value:
            operands.words.append(boxedOperand(value, arguments[next++]).value());
            break;
        case BuiltinDirectValue::ValueOrEmpty:
            operands.words.append(next < arguments.size() ? boxedOperand(value, arguments[next++]).value() : m_out.constInt64(JSValue::encode(JSValue())));
            break;
        case BuiltinDirectValue::RestOfValues: {
            Vector<Boxed, 8> rest;
            for (ValueID argument : arguments.subspan(next))
                rest.append(boxedOperand(value, argument));
            operands.words.append(argumentBuffer(rest.span()).value());
            operands.words.append(m_out.constIntPtr(rest.size()));
            next = arguments.size();
            break;
        }
        case BuiltinDirectValue::FrameOfValues: {
            Vector<Boxed, 8> words;
            words.append(Boxed(m_out.constInt64(static_cast<int64_t>(arguments.size() - next + 1))));
            words.append(boxedOperand(value, value->receiverOperand()));
            for (ValueID argument : arguments.subspan(next))
                words.append(boxedOperand(value, argument));
            operands.words.append(m_out.sub(argumentBuffer(words.span()).value(), CallFrameSlot::argumentCountIncludingThis * sizeof(Register)));
            next = arguments.size();
            operands.takesFrame = true;
            break;
        }
        case BuiltinDirectValue::Boolean:
            operands.words.append(asWord32(booleanOperand(arguments[next++])).value());
            break;
        case BuiltinDirectValue::Int32:
            operands.words.append(int32Operand(arguments[next++]).value());
            break;
        case BuiltinDirectValue::Double:
            operands.words.append(numberOperand(arguments[next++]).value());
            break;
        case BuiltinDirectValue::Bytes:
            appendBytesOf(value, operands, arguments[next++]);
            break;
        case BuiltinDirectValue::BytesOfReceiver:
            appendBytesOf(value, operands, value->receiverOperand());
            break;
        case BuiltinDirectValue::BytesInRange:
            appendBytesOf(value, operands, arguments[next++], takeBounds());
            break;
        case BuiltinDirectValue::BytesOfReceiverInRange:
            appendBytesOf(value, operands, value->receiverOperand(), takeBounds());
            break;
        case BuiltinDirectValue::Characters: {
            const CharacterView& view = characters[nextCharacters++];
            Integer address(view.characters.value());
            operands.words.append(select(view.is8Bit, address, bitOr(address, integer(static_cast<int64_t>(wideCharactersBit)))).value());
            operands.words.append(zeroExtended(view.length).value());
            operands.keptAlive.append(view.owner.value());
            ++next;
            break;
        }
        }
    }
    if (binding.takesReceiver())
        operands.keptAlive.append(boxedOperand(value, value->receiverOperand()).value());
    build().statistics().count(StatisticID::NativeFunctionsCalledDirectly);
    return operands;
}

Boxed BuiltinCallEmitter::callDirectFunction(const ValueContext& value, uint32_t number)
{
    DirectOperands operands = directOperandsOf(value, BuiltinBindings::directFunctionAt(number));
    const BuiltinOverload& overload = m_builtins.at(value->overload());
    // An error that the function makes lists the function in its stack, so the function needs a frame of its own.
    bool makesErrors = overload.throws() && !overload.onlyPropagatesErrors() && !groupsFunctionsOfCompiledCode(m_builtins.at(overload.builtin).holder);
    bool hasFunctionObject = BuiltinCellKey::object(overload.builtin).hasCell(m_builtins);
    bool canMakeFrame = hasFunctionObject && !generator().validates() && RuntimeCallEmitter::leavesRegistersForHostFrame(operands.words.span());
    std::optional<RuntimeCallEmitter::HostFrameOfDirectCall> hostFrame;
    if (operands.takesFrame || (makesErrors && canMakeFrame))
        hostFrame = RuntimeCallEmitter::HostFrameOfDirectCall { (value->receiverOperand() ? boxedOperand(value, value->receiverOperand()) : undefined()).value(), calleeOf(value, overload).value() };
    // Where there is no such frame, the site says which function it calls.
    BuiltinID callee = m_builtins.canonicalID(overload.builtin);
    std::optional<BuiltinNativeFunction> nativeFunction = BuiltinBindings::nativeFunctionOf(callee);
    bool hasNameInFrames = hasFunctionObject || (nativeFunction && !nativeFunction->name.isNull());
    BuiltinID listedAtSite = makesErrors && !hostFrame && hasNameInFrames ? callee : BuiltinID();
    return Boxed(emitter<RuntimeCallEmitter>().callDirectFunction(value, number, operands.words.span(), operands.keptAlive.span(), hostFrame, listedAtSite));
}

StubCallResult BuiltinCallEmitter::callDirectFunctionReturningFields(const ValueContext& value, uint32_t number)
{
    DirectOperands operands = directOperandsOf(value, BuiltinBindings::directFunctionAt(number));
    return emitter<RuntimeCallEmitter>().callDirectFunctionReturningFields(value, number, operands.words.span(), operands.keptAlive.span());
}

// For a program that keeps the object.
CellPointer BuiltinCallEmitter::makeObjectOfDirectEntry(const ValueContext& value, uint32_t number)
{
    const BuiltinType& result = m_builtins.at(m_builtins.at(value->overload()).result.type);
    RELEASE_ASSERT(result.parts.size() == 1 && result.parts[0].kind == BuiltinTypePartKind::Layout);
    StubCallResult fields = callDirectFunctionReturningFields(value, number);
    auto boxed = [&](LValue field) {
        return emitter<ConversionEmitter>().boxInteger(Integer(field), Range { 0, static_cast<int64_t>(maxSafeInteger()) });
    };
    return callOperation<CellPointer>(value, RuntimeOperation::NewObjectOfBuiltinLayout, { word32(static_cast<int32_t>(result.parts[0].layout.index())), boxed(fields.first), boxed(fields.second) });
}

void BuiltinCallEmitter::generateFieldsOfDirectEntry(const ValueContext& value)
{
    std::optional<uint32_t> number = BuiltinBindings::directFunctionNumberOf(value->overload(), value->argumentsOperands().size());
    std::optional<uint32_t> numberOfFields = numberOfResultsOfOverload(m_program, value.value());
    if (!number || !BuiltinBindings::directFunctionAt(*number).returnsFields || !numberOfFields || !*numberOfFields || *numberOfFields > 2) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    StubCallResult fields = callDirectFunctionReturningFields(value, *number);
    std::array<Operand, 2> results { Integer(fields.first), Integer(fields.second) };
    finishWithResults(InlineCall(value, InlineBuiltin::FieldsOfDirectEntry, { }), std::span<const Operand>(results).first(*numberOfFields));
}

void BuiltinCallEmitter::generateNewSetFromData(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    if (!request) {
        failAndDefineZero(generator(), value, InternalErrorKind::PlacedPayloadMissing);
        return;
    }
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewSetFromData, { word32(request->value()) }));
}

void BuiltinCallEmitter::generateNewMapFromData(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    if (!request) {
        failAndDefineZero(generator(), value, InternalErrorKind::PlacedPayloadMissing);
        return;
    }
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewMapFromData, { word32(request->value()) }));
}

void BuiltinCallEmitter::generateNewFormatterWithPattern(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    if (!request) {
        failAndDefineZero(generator(), value, InternalErrorKind::PlacedPayloadMissing);
        return;
    }
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewFormatterWithPattern, { word32(request->value()) }));
}

void BuiltinCallEmitter::generateSetUpFormatterWithoutPattern(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::SetUpFormatterWithoutPattern, { in<CellPointer>(value->formatterOperand()) });
}

void BuiltinCallEmitter::generateFormatDateWithPattern(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    if (!request) {
        failAndDefineZero(generator(), value, InternalErrorKind::PlacedPayloadMissing);
        return;
    }
    define(value, callOperation<Boxed>(value, RuntimeOperation::FormatWithPattern, { word32(request->value()), boxCell(in<CellPointer>(value->dateOperand())) }));
}

void BuiltinCallEmitter::generateFormatNumberWithPattern(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    if (!request) {
        failAndDefineZero(generator(), value, InternalErrorKind::PlacedPayloadMissing);
        return;
    }
    define(value, callOperation<Boxed>(value, RuntimeOperation::FormatWithPattern, { word32(request->value()), boxedOperand(value, value->numberOperand()) }));
}

void BuiltinCallEmitter::generateNewArgumentList(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewArgumentList, { in<Boxed>(value->calleeOperand()), in<Boxed>(value->arrayLikeOperand()) }));
}

void BuiltinCallEmitter::generateSharedRegExpLiteral(const ValueContext& value)
{
    std::optional<StaticsOffset> staticsOffset = value->staticsOffset();
    RegExpRecordID record = value->regExpRecord();
    if (!staticsOffset || !record) {
        failAndDefineZero(generator(), value, InternalErrorKind::PlacedPayloadMissing);
        return;
    }
    Integer shared(m_out.load64(emitter<StaticsEmitter>().inStatics(m_heaps.entire(AbstractHeap::AllocatorState), *staticsOffset)));
    define(value, either<CellPointer>(isNotZero(shared), [&] {
        return CellPointer(shared.value());
    }, [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::SharedRegExpLiteral, { word32(staticsOffset->value()), word32(record.index()), word32(toYarrFlags(value->regExpFlags()).toRaw()) }, { .isOnRarePath = true });
    }));
}

void BuiltinCallEmitter::generateIsBuiltinLocationUnchanged(const ValueContext& value)
{
    BuiltinID member = value->member();
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock isNotKnownUnchanged = newBlock();
        LBasicBlock isNotArmed = newBlock();
        const B3::AbstractHeap& heap = m_heaps.entire(AbstractHeap::NamedProperties);
        Word32 state(m_out.load8ZeroExt32(instanceField(heap, InstanceLayout::offsetOfBuiltinLocationStates + member.index())));
        branch(equal(state, word32(static_cast<int32_t>(WatchedLocationState::Unchanged))), { targets.taken, rareEdge(isNotKnownUnchanged) });

        m_out.appendTo(isNotKnownUnchanged);
        Pointer states = loadImmutablePointer(AOT_INSTANCE_OFFSET(builtins.locationStates));
        branch(isZero(Word32(m_out.load8ZeroExt32(m_out.address(heap, states.value(), member.index())))), { rareEdge(isNotArmed), targets.notTaken });

        m_out.appendTo(isNotArmed);
        branch(callWithoutSite<Condition>(RuntimeOperation::ArmBuiltinLocation, { word32(static_cast<int32_t>(member.index())) }, { .isOnRarePath = true }), targets);
    });
}

void BuiltinCallEmitter::generateIsFunctionOfBuiltin(const ValueContext& value)
{
    build().statistics().count(StatisticID::NativeFunctionTestsEmitted);
    std::optional<uint32_t> number = BuiltinBindings::nativeFunctionNumberOf(value->member());
    Boxed operand = boxedOperand(value, value->valueOperand());
    defineTestByBranching(value, [&](BranchTargets targets) {
        if (!number) {
            m_out.jump(targets.notTaken.block());
            return;
        }
        LBasicBlock isCellCase = newBlock();
        LBasicBlock isFunction = newBlock();
        LBasicBlock holdsExecutable = newBlock();
        LBasicBlock holdsRareData = newBlock();
        LBasicBlock isHostFunction = newBlock();
        branch(isCell(operand), { normalEdge(isCellCase), targets.notTaken });

        m_out.appendTo(isCellCase);
        CellPointer function = unboxCell(operand);
        branch(hasCellType(function, JSFunctionType), { normalEdge(isFunction), targets.notTaken });

        // The edge not taken says that the value is another function, so the answer may not depend on the properties that the function has got since it was made.
        m_out.appendTo(isFunction);
        const B3::AbstractHeap& fixed = m_heaps.AOTInstance_fixedFields;
        Pointer executableOrRareData = loadField<Pointer>(function, m_heaps.JSFunction_executableOrRareData);
        OperandJoin executables(m_out, CellPointer::representation);
        branch(Condition(m_out.testIsZeroPtr(executableOrRareData.value(), m_out.constIntPtr(JSFunction::rareDataTag))), { normalEdge(holdsExecutable), normalEdge(holdsRareData) });

        m_out.appendTo(holdsExecutable);
        executables.add(CellPointer(executableOrRareData.value()));

        m_out.appendTo(holdsRareData);
        executables.add(CellPointer(m_out.loadPtr(m_out.address(fixed, executableOrRareData.value(), FunctionRareData::offsetOfExecutable() - static_cast<ptrdiff_t>(JSFunction::rareDataTag)))));

        CellPointer executable = executables.finish().as<CellPointer>();
        branch(hasCellType(executable, NativeExecutableType), { normalEdge(isHostFunction), targets.notTaken });

        m_out.appendTo(isHostFunction);
        Pointer code(m_out.loadPtr(m_out.address(fixed, executable.value(), NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall))));
        Pointer bindings = loadImmutablePointer(AOT_INSTANCE_OFFSET(builtinBindings.nativeFunctionsOfHost));
        ptrdiff_t offsetOfExpectedCode = *number * static_cast<ptrdiff_t>(sizeof(BuiltinNativeFunctionBinding)) + static_cast<ptrdiff_t>(OBJECT_OFFSETOF(BuiltinNativeFunctionBinding, function));
        Pointer expected(m_out.loadPtr(m_out.address(fixed, bindings.value(), offsetOfExpectedCode)));
        branch(equal(code, expected), targets);
    });
}

void BuiltinCallEmitter::generateGetBuiltinValue(const ValueContext& value)
{
    defineAs(value, builtinValue(value, BuiltinCellKey::value(m_builtins.canonicalID(value->builtin()))), typeOf(value.id()));
}

void BuiltinCallEmitter::generateInitializeBuiltin(const ValueContext& value)
{
    BuiltinCellKey read = BuiltinCellKey::value(value->builtin());
    emitter<ConstantEmitter>().builtinCell(value, read.hasCell(m_builtins) ? read : BuiltinCellKey::object(m_builtins.canonicalID(value->builtin())));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
