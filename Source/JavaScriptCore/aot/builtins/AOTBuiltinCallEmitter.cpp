/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCallEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTArrayBuiltinEmitter.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTypes.h"
#include "AOTCallEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConvention.h"
#include "AOTConversionEmitter.h"
#include "AOTDiagnostics.h"
#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTIRProgram.h"
#include "AOTNumbering.h"
#include "AOTObjectBuiltinEmitter.h"
#include "AOTOperationData.h"
#include "AOTRegExpFlags.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsArrays.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCallHost.h"
#include "AOTStubsCompiledMethods.h"
#include "AOTStubsPrimitiveFastPaths.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include "CodeSpecializationKind.h"
#include "FunctionRareData.h"
#include "JSCJSValueInlines.h"
#include "JSFunction.h"
#include "NativeExecutable.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static_assert(numberOfArgumentGPRs <= StubKey::maxCount);

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

static std::optional<StubKey> stubThatCallsBuiltin(const BuiltinTable& builtins, const BuiltinOverload& overload, unsigned count)
{
    if (count > numberOfArgumentGPRs)
        return std::nullopt;
    bool constructs = overload.mode == BuiltinCallMode::Construct;
    if (std::optional<StubKey> boxedCall = constructs ? std::nullopt : boxedCallStubFor(builtins, overload.builtin, count))
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

static void failBuiltinOperationWithZero(CodeGenerator& generator, const ValueContext& value, InternalErrorKind kind)
{
    generator.fail(value, kind);
    generator.define(value, generator.emitter<ConstantEmitter>().zeroOf(value.representation()));
}

static std::optional<Wire::SourceLocation> placeWhereRegExpLiteralIsWritten(const Program& program, const Code& code, const Operation& literal)
{
    FunctionID writtenIn = code.inlineFrames()[literal.inlineFrame].callee;
    if (!writtenIn)
        return std::nullopt;
    Wire::SourceLocation place = program.at(writtenIn).locationAt(literal.position);
    if (!hasModule(place))
        return std::nullopt;
    return place;
}

BuiltinCallEmitter::BuiltinCallEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

void BuiltinCallEmitter::stopUnlessNativeResultIsAsWritten(const ValueContext& value, Boxed result, BuiltinID called, bool constructs)
{
    using Written = WrittenNativeResult;
    static constexpr std::array<std::pair<uint16_t, uint32_t>, 7> kindsOfPrimitives { {
        { Primitive::Undefined, Written::undefinedKind }, { Primitive::Null, Written::nullKind }, { Primitive::Boolean, Written::booleanKind }, { Primitive::Number, Written::numberKind },
        { Primitive::AnyString, Written::stringKind }, { Primitive::AnySymbol, Written::symbolKind }, { Primitive::BigInt, Written::bigIntKind },
    } };
    Type written = typeOf(value.id());
    if (!written.isJavaScriptValue() || written.primitives & (Primitive::Absent | Primitive::Unspecified))
        return;
    uint32_t kinds = written.hasObjects() ? Written::objectKind : 0;
    for (auto [primitivesOfKind, kind] : kindsOfPrimitives) {
        if (written.primitives & primitivesOfKind)
            kinds |= kind;
    }
    TypeID tested = types().wholeKindsAndBuiltinClassesOf(typeIDOf(value.id()));
    if (tested == TypeTable::anything)
        return;
    RELEASE_ASSERT(staysInSweptCodeByItself(value.value()));
    TypeID numbers = types().intersectionOf(tested, TypeTable::number);
    RELEASE_ASSERT(called.index() < 1u << 22);
    bool looksAgainOutOfLine = !isNeverType(numbers) && types().isSubset(numbers, TypeTable::integer);
    TypeID besideNumbers = types().withoutPrimitives(tested, Primitive::Number);
    LBasicBlock stops = newBlock();
    LBasicBlock continuation = newBlock();
    ScopeFork fork = m_out.forkScope();
    BranchTargets targets { normalEdge(continuation), rareEdge(stops) };
    auto appendBehindTest = [&](LBasicBlock block) {
        if (block->singlePredecessor() == fork.block)
            m_out.appendToArm(block, fork);
        else
            m_out.appendToJoin(block, fork);
    };
    if (!looksAgainOutOfLine)
        branchOnType(result, typeTestPlanFor(TypeTable::anything, Representation::JSValue, tested), targets);
    else if (isNeverType(besideNumbers))
        branch(isInt32(result), targets);
    else {
        LBasicBlock isNoInt32 = newBlock();
        branch(isInt32(result), { normalEdge(continuation), normalEdge(isNoInt32) });
        m_out.appendToArm(isNoInt32, fork);
        branchOnType(result, typeTestPlanFor(TypeTable::anything, Representation::JSValue, besideNumbers), targets);
    }
    appendBehindTest(stops);
    if (looksAgainOutOfLine) {
        callVoidOperation(value, RuntimeOperation::StopUnlessNativeResultIsIntegerAsWritten, { result, word32(static_cast<int32_t>(Written::encode(kinds, constructs, called.index(), !types().isSubset(numbers, TypeTable::int32)))) });
        m_out.jump(continuation);
        m_out.appendToJoin(continuation, fork);
        return;
    }
    callVoidOperation(value, RuntimeOperation::StopAtNativeResult, { result, word32(static_cast<int32_t>(Written::encode(kinds, constructs, called.index()))) });
    m_out.unreachable();
    appendBehindTest(continuation);
}

std::optional<HostResult> BuiltinCallEmitter::hostResultAsWritten(const ValueContext& value)
{
    const Type& written = typeOf(value.id());
    if (!written.isJavaScriptValue() || written.primitives & (Primitive::Absent | Primitive::Unspecified))
        return std::nullopt;
    TypeID tested = types().wholeKindsAndBuiltinClassesOf(typeIDOf(value.id()));
    if (tested == TypeTable::boolean)
        return HostResult::Boolean;
    if (tested == TypeTable::string)
        return HostResult::String;
    if (tested == TypeTable::symbol)
        return HostResult::Symbol;
    if (tested == TypeTable::object)
        return HostResult::Object;
    if (tested == types().builtinClass(BuiltinClasses::Array))
        return HostResult::Array;
    return std::nullopt;
}

void BuiltinCallEmitter::generateCallBuiltin(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    BuiltinOverloadID id = value->overload();
    const BuiltinOverload& overload = m_builtins.at(id);
    if (requiresWalkLowering(m_builtins, overload.builtin)) {
        failBuiltinOperationWithZero(generator(), value, InternalErrorKind::RequiredPassLeftOperation);
        return;
    }
    ValueID receiver = value->receiverOperand();
    std::span<const ValueID> arguments = value->argumentsOperands();

    bool isDeclaredFresh = m_builtins.contains(overload.result.type)
        && (m_builtins.at(overload.result.type).freshness == BuiltinFreshness::Yes || m_program.builtinInjectedAsFresh == overload.builtin.index());
    bool checksFreshness = ASSERT_ENABLED || AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckResultFreshnessAtRunTime);
    std::optional<Integer> collectionsBefore;
    if (checksFreshness && isDeclaredFresh && value->builtinCallForm() == BuiltinCallForm::Whole && !value->isDoneInPlace()) {
        collectionsBefore = Integer(emitter<RuntimeCallEmitter>().readCollectionCount());
        build().statistics().count(StatisticID::FreshResultsChecked);
    }

    ExpectedResult expected { .result = hostResultAsWritten(value) };
    auto defineResult = [&](Boxed result) {
        if (!expected.isCheckedByCallee)
            stopUnlessNativeResultIsAsWritten(value, result, overload.builtin, overload.mode == BuiltinCallMode::Construct);
        if (collectionsBefore) {
            Boxed none = boxedConstant(JSValue());
            const Operation* receiverOperation = receiver ? &m_code.at(underlyingCellOf(m_code, receiver)) : nullptr;
            bool hasOriginalReceiver = receiverOperation && (receiverOperation->opcode != Opcode::ToObject || receiverOperation->calledBuiltin() != overload.builtin);
            callVoidOperation(value, RuntimeOperation::CheckFreshResult, { result, *collectionsBefore, word32(static_cast<int32_t>(overload.builtin.index())), hasOriginalReceiver ? boxedOperand(value, receiver) : none, arguments.empty() ? none : boxedOperand(value, arguments[0]) });
        }
        defineAs(value, result, typeOf(value.id()));
    };
    auto callFunction = [&] {
        std::optional<uint32_t> number = value->builtinCallForm() == BuiltinCallForm::Whole ? BuiltinBindings::directFunctionNumberOf(id, arguments.size()) : std::nullopt;
        if (number && BuiltinBindings::directFunctionAt(*number).returnsNumber) {
            DirectOperands operands = directOperandsOf(value, BuiltinBindings::directFunctionAt(*number));
            defineAs(value, Number(emitter<RuntimeCallEmitter>().callDirectFunctionReturningNumber(value, *number, operands.words.span(), operands.keptAlive.span())), typeOf(value.id()));
            return;
        }
        if (number && !BuiltinBindings::directFunctionAt(*number).entersHostFunction()) {
            defineResult(callDirectFunction(value, *number));
            return;
        }
        BuiltinID builtin = m_builtins.canonicalID(overload.builtin);
        if (builtin == Builtins::Array && value->isDoneInPlace() && arguments.size() == 1) {
            defineResult(boxCell(callOperation<CellPointer>(value, RuntimeOperation::ArrayOfLengthOrElement, { boxedOperand(value, arguments[0]) })));
            return;
        }
        if (builtin == Builtins::String && overload.mode == BuiltinCallMode::Construct && value->builtinCallForm() == BuiltinCallForm::Whole && arguments.size() == 1 && typeOf(arguments[0]).isAnyString()) {
            defineResult(boxCell(callOperation<CellPointer>(value, RuntimeOperation::NewStringObject, { boxedOperand(value, arguments[0]) })));
            return;
        }
        if (builtin == Builtins::Number && overload.mode == BuiltinCallMode::Call && arguments.size() == 1) {
            defineResult(Boxed(callStub(value, numberOfAnyValue(), { boxedOperand(value, arguments[0]).value() }, { .calledBuiltin = builtin }).first));
            return;
        }
        if (builtin == Builtins::String && overload.mode == BuiltinCallMode::Call && arguments.size() == 1) {
            Boxed operand = boxedOperand(value, arguments[0]);
            auto convertAnyValue = [&] {
                return callOperation<Boxed>(value, RuntimeOperation::StringOfAnyValue, { operand }, { .calledBuiltin = builtin });
            };
            auto convert = [&] {
                if (!generator().usesLoopForms(value.block()))
                    return convertAnyValue();
                return either<Boxed>(isInt32(operand), [&] {
                    return boxCell(emitter<StringEmitter>().integerToString(value, unboxInt32(operand)));
                }, convertAnyValue);
            };
            expected.isCheckedByCallee = expected.result == HostResult::String;
            const TypeTestPlan& isString = typeTestPlanFor(typeIDOf(arguments[0]), representationOf(arguments[0]), TypeTable::string);
            defineResult(either<Boxed>(hasType(get(arguments[0]), isString), [&] {
                return operand;
            }, convert));
            return;
        }
        std::optional<Boxed> boxedReceiver;
        if (receiver)
            boxedReceiver = boxedOperand(value, receiver);
        Vector<Boxed, 8> boxedArguments;
        for (ValueID argument : arguments) {
            bool isStoredAsItIs = builtin == Builtins::ArrayPrototypePush && representationOf(argument) == Representation::JSValue;
            boxedArguments.append(isStoredAsItIs ? emitter<ConversionEmitter>().inFormOfType(get(argument), typeOf(argument)).as<Boxed>() : boxedOperand(value, argument));
        }
        auto argumentOrUndefined = [&](size_t index) {
            return index < boxedArguments.size() ? boxedArguments[index] : undefined();
        };
        auto answerOrCallFunction = [&](RuntimeOperation operation, std::initializer_list<Operand> operands) {
            Boxed answer = callOperation<Boxed>(value, operation, operands, { .calledBuiltin = builtin });
            defineResult(either<Boxed>(isNotEmpty(answer), [&] {
                return answer;
            }, [&] {
                return callBuiltinFunction(value, id, boxedReceiver, boxedArguments.span(), expected.result == HostResult::String ? &expected : nullptr);
            }));
        };
        bool storesUnderObject = builtin == Builtins::WeakMapPrototypeSet && overload.mode == BuiltinCallMode::Call && receiver && arguments.size() == 2 && generator().usesLoopForms(value.block())
            && types().isSubset(typeIDOf(receiver), types().builtinClass(BuiltinClasses::WeakMap)) && types().isSubset(typeIDOf(arguments[0]), TypeTable::object);
        if (storesUnderObject) {
            callStub(value, weakMapSet(), { boxedReceiver->value(), boxedArguments[0].value(), boxedArguments[1].value() }, { .calledBuiltin = builtin });
            defineAs(value, *boxedReceiver, typeOf(value.id()));
            return;
        }
        TypedArrayType constructedView = m_builtins.at(builtin).instanceClass ? m_builtins.typedArrayTypeOf(m_builtins.at(builtin).instanceClass) : NotTypedArray;
        bool makesViewOfLength = overload.mode == BuiltinCallMode::Construct && arguments.size() == 1 && isTypedView(constructedView) && m_builtins.typedArrayClassOf(m_builtins.at(builtin).instanceClass) == m_builtins.at(builtin).instanceClass
            && (typeOf(arguments[0]).primitives & Primitive::Number || typeOf(arguments[0]).hasUnboundedUnknown());
        if (makesViewOfLength && generator().usesLoopForms(value.block())) {
            defineResult(either<Boxed>(isInt32(boxedArguments[0]), [&] {
                return boxCell(CellPointer(callStub(value, newTypedArray(LengthOfTypedArray::WholeNumber), { word32(constructedView).value(), unboxInt32(boxedArguments[0]).value() }, { .calledBuiltin = builtin }).first));
            }, [&] {
                return callBuiltinFunction(value, id, boxedReceiver, boxedArguments.span());
            }));
            return;
        }
        bool isCall = overload.mode == BuiltinCallMode::Call;
        auto holdsOnly = [&](ValueID argument, uint16_t primitives) {
            Type type = typeOf(argument);
            return !type.isNever() && type.hasOnlyPrimitives(primitives);
        };
        bool writesOnePrimitive = builtin == Builtins::JSONStringify && isCall && !arguments.empty()
            && holdsOnly(arguments[0], Primitive::AnyString | Primitive::Number | Primitive::Boolean | Primitive::Null)
            && std::ranges::all_of(arguments.subspan(1), [&](ValueID option) { return holdsOnly(option, Primitive::UndefinedOrNull); });
        if (writesOnePrimitive) {
            defineResult(boxCell(callOperation<CellPointer>(value, RuntimeOperation::JSONStringifyPrimitive, { boxedArguments[0] }, { .calledBuiltin = builtin })));
            return;
        }
        if (builtin == Builtins::ArrayPrototypeJoin && isCall && receiver && arguments.size() <= 1) {
            expected.isCheckedByCallee = expected.result == HostResult::String;
            defineResult(callOperation<Boxed>(value, RuntimeOperation::ArrayJoinOfAnyReceiver, { *boxedReceiver, argumentOrUndefined(0), calleeOf(value, overload) }));
            return;
        }
        if (builtin == Builtins::ArrayPrototypeSlice && isCall && receiver && arguments.empty() && generator().isStoredIntoByName(value.id())) {
            LBasicBlock closed = newBlock();
            OperandJoin answersMeet(m_out, Representation::JSValue);
            answersMeet.add(emitter<ArrayBuiltinEmitter>().copyWithPropertyRoomInFrontOfClosedCall(value, receiver, closed));
            m_out.appendTo(closed);
            answersMeet.add(callBuiltinFunction(value, id, boxedReceiver, boxedArguments.span()));
            defineResult(answersMeet.finish().as<Boxed>());
            return;
        }
        if (std::optional<URICoding> coding = uriCodingOf(builtin); coding && isCall && arguments.size() == 1)
            return answerOrCallFunction(RuntimeOperation::CodeURIOfAnyValue, { boxedArguments[0], word32(static_cast<uint32_t>(*coding)) });
        bool searchesArray = builtin == Builtins::ArrayPrototypeIndexOf || builtin == Builtins::ArrayPrototypeIncludes;
        if (searchesArray && isCall && receiver && arguments.size() == 2 && typeOf(arguments[0]).primitives & Primitive::AnyString)
            return answerOrCallFunction(RuntimeOperation::ArraySearchOfAnyValue, { *boxedReceiver, boxedArguments[0], boxedArguments[1], word32(builtin == Builtins::ArrayPrototypeIndexOf) });
        auto isEncodingForTheHost = [&](ValueID encoding) {
            const Operation& definition = operationAt(definitionOf(encoding));
            if (definition.opcode != Opcode::String)
                return false;
            StringView name { byteCast<Latin1Character>(m_program.string(definition.string())) };
            return name != "utf8"_s && name != "utf-8"_s && name != "ascii"_s && name != "latin1"_s && name != "binary"_s;
        };
        if (builtin == Builtins::BufferPrototypeToString && isCall && receiver && arguments.size() <= 3 && (arguments.empty() || !isEncodingForTheHost(arguments[0])))
            return answerOrCallFunction(RuntimeOperation::TextOfBytes, { *boxedReceiver, argumentOrUndefined(0), argumentOrUndefined(1), argumentOrUndefined(2) });
        auto mayBeView = [&](ValueID view) {
            TypeID type = typeIDOf(view);
            std::array views { BuiltinClasses::Int8Array, BuiltinClasses::Uint8Array, BuiltinClasses::Uint8ClampedArray, BuiltinClasses::Int16Array, BuiltinClasses::Uint16Array, BuiltinClasses::Int32Array, BuiltinClasses::Uint32Array, BuiltinClasses::Float32Array, BuiltinClasses::Float64Array, BuiltinClasses::BigInt64Array, BuiltinClasses::BigUint64Array, BuiltinClasses::Buffer };
            return std::ranges::any_of(views, [&](BuiltinClassID builtinClass) { return types().overlap(type, types().builtinClass(builtinClass)); });
        };
        BuiltinClassID constructed = overload.mode == BuiltinCallMode::Construct && arguments.size() == 1 ? m_builtins.at(builtin).instanceClass : BuiltinClassID();
        if (constructed && m_builtins.typedArrayClassOf(constructed) == constructed && types().overlap(typeIDOf(arguments[0]), types().builtinClass(BuiltinClasses::Array))) {
            TypedArrayType type = m_builtins.typedArrayTypeOf(constructed);
            if (type != TypeInt32 && type != TypeUint32)
                return answerOrCallFunction(RuntimeOperation::TypedArrayOfPlainArray, { boxedArguments[0], word32(type) });
            Boxed answer(callStub(value, newTypedArrayOfArray(), { boxedArguments[0].value(), word32(type).value() }, { .calledBuiltin = builtin }).first);
            defineResult(either<Boxed>(isNotEmpty(answer), [&] {
                return answer;
            }, [&] {
                return callBuiltinFunction(value, id, boxedReceiver, boxedArguments.span(), expected.result == HostResult::String ? &expected : nullptr);
            }));
            return;
        }
        if (builtin == Builtins::TypedArrayPrototypeSet && isCall && receiver && (arguments.size() == 1 || arguments.size() == 2) && types().overlap(typeIDOf(receiver), typeIDOf(arguments[0])))
            return answerOrCallFunction(RuntimeOperation::TypedArraySetOfAnyValue, { *boxedReceiver, boxedArguments[0], argumentOrUndefined(1) });
        if (builtin == Builtins::TypedArrayPrototypeSubarray && isCall && receiver && arguments.size() <= 2 && mayBeView(receiver))
            return answerOrCallFunction(RuntimeOperation::TypedArraySubarrayOfAnyValue, { *boxedReceiver, argumentOrUndefined(0), argumentOrUndefined(1) });
        if (builtin == Builtins::ObjectPrototypeToString && isCall && receiver && arguments.empty()) {
            if (!generator().usesLoopForms(value.block()) || !typeOf(receiver).hasObjects())
                return answerOrCallFunction(RuntimeOperation::ObjectToStringOfAnyValue, { *boxedReceiver });
            LBasicBlock closed = newBlock();
            OperandJoin answersMeet(m_out, Representation::JSValue);
            answersMeet.add(emitter<ObjectBuiltinEmitter>().cachedToStringTagBeforeClosedCall(*boxedReceiver, closed));
            m_out.appendTo(closed);
            Boxed answer = callOperation<Boxed>(value, RuntimeOperation::ObjectToStringOfAnyValue, { *boxedReceiver }, { .calledBuiltin = builtin });
            answersMeet.add(either<Boxed>(isNotEmpty(answer), [&] {
                return answer;
            }, [&] {
                return callBuiltinFunction(value, id, boxedReceiver, boxedArguments.span());
            }));
            defineResult(answersMeet.finish().as<Boxed>());
            return;
        }
        if (builtin == Builtins::BufferByteLength && isCall && (arguments.size() == 1 || arguments.size() == 2))
            return answerOrCallFunction(RuntimeOperation::ByteLengthOfAnyValue, { boxedArguments[0], argumentOrUndefined(1) });
        if (std::optional<uint32_t> entry = hostFunctionEntryOf(m_builtins, overload); entry && receiver) {
            defineResult(callDirectFunction(value, *entry));
            return;
        }
        defineResult(callBuiltinFunction(value, id, boxedReceiver, boxedArguments.span(), &expected));
    };

    switch (value->builtinCallForm()) {
    case BuiltinCallForm::AnyReceiver:
        if (overload.mode == BuiltinCallMode::Get && receiver && !BuiltinCellKey::getter(overload.builtin).hasCell(m_builtins)) {
            defineResult(emitter<AccessEmitter>().getById(value, boxedOperand(value, receiver), keyOf(overload)));
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
            failBuiltinOperationWithZero(generator(), value, InternalErrorKind::OpcodeWithoutCode);
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
            failBuiltinOperationWithZero(generator(), value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
        callFunction();
        return;
    }
    case BuiltinImplementationKind::OwnSlot:
        if (!receiver || overload.mode != BuiltinCallMode::Get) {
            failBuiltinOperationWithZero(generator(), value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
        defineResult(emitter<AccessEmitter>().getById(value, boxedOperand(value, receiver), keyOf(overload)));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PropertyKeyID BuiltinCallEmitter::keyOf(const BuiltinOverload& overload) const
{
    return m_program.keyOf(m_builtins.at(overload.builtin).name);
}

Boxed BuiltinCallEmitter::callCustomGetter(const OperationContext& context, BuiltinOverloadID id, Boxed receiver, bool receiverIsMatched)
{
    const BuiltinOverload& overload = m_builtins.at(id);
    RELEASE_ASSERT(overload.mode == BuiltinCallMode::Get);
    if (!receiverIsMatched)
        return emitter<AccessEmitter>().getById(context, receiver, keyOf(overload));
    return callOperation<Boxed>(context, RuntimeOperation::CallCustomGetter, { receiver, word32(static_cast<int32_t>(id.index())), identifierNumber(keyOf(overload)) });
}

CellPointer BuiltinCallEmitter::builtinCell(const OperationContext& context, BuiltinCellKey key)
{
    return emitter<ConstantEmitter>().builtinCell(context, key);
}

Boxed BuiltinCallEmitter::builtinValue(const OperationContext& context, BuiltinCellKey key, RuntimeOperation fills, const ValueContext* heldAgainstItsRow)
{
    RELEASE_ASSERT(key.kind() == BuiltinCellKey::Kind::Value);
    if (!key.hasCell(m_builtins)) {
        fail(context, InternalErrorKind::OpcodeWithoutCode);
        return undefined();
    }
    static_assert(sizeof(EncodedJSValue) == 1 << logScale(ScaleEight));
    Pointer values = loadImmutablePointer(AOT_INSTANCE_OFFSET(builtins.hostValues));
    Integer number = zeroExtended(numberOf(m_mentions.builtinCell(key)));
    Boxed filled(m_out.load64(m_out.baseIndex(m_heaps.entire(AbstractHeap::RuntimeState), values.value(), number.value(), ScaleEight)));
    return either<Boxed>(isNotEmpty(filled), [&] {
        return filled;
    }, [&] {
        Boxed made = callOperation<Boxed>(context, fills, { word32(static_cast<int32_t>(key.encoded())) }, { .isOnRarePath = true });
        if (heldAgainstItsRow)
            stopUnlessNativeResultIsAsWritten(*heldAgainstItsRow, made, key.builtin(), false);
        return made;
    });
}

static BuiltinCellKey keyOfCalleeOf(const BuiltinOverload& overload)
{
    switch (overload.mode) {
    case BuiltinCallMode::Call:
    case BuiltinCallMode::Construct:
        return BuiltinCellKey::object(overload.builtin);
    case BuiltinCallMode::Get:
        return BuiltinCellKey::getter(overload.builtin);
    case BuiltinCallMode::Set:
        return BuiltinCellKey::setter(overload.builtin);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CellPointer BuiltinCallEmitter::calleeOf(const OperationContext& context, const BuiltinOverload& overload)
{
    return builtinCell(context, keyOfCalleeOf(overload));
}

void BuiltinCallEmitter::createStaticCallee(const ValueContext& value)
{
    const BuiltinOverload& overload = m_builtins.at(value->overload());
    BuiltinID holder = m_builtins.at(overload.builtin).holder;
    ValueID receiver = value->receiverOperand();
    if (receiver && holder && overload.code.implementation.kind == BuiltinImplementationKind::HostCall && keyOfCalleeOf(overload).hasCell(m_builtins) && types().overlap(typeIDOf(receiver), types().forBuiltin(holder)))
        calleeOf(value, overload);
}

Boxed BuiltinCallEmitter::callBuiltinFunction(const OperationContext& context, BuiltinOverloadID id, std::optional<Boxed> receiver, std::span<const Boxed> arguments, ExpectedResult* expected)
{
    const BuiltinOverload& overload = m_builtins.at(id);
    if (overload.code.implementation.callee == BuiltinCallee::CustomGetter) {
        RELEASE_ASSERT(receiver && arguments.empty());
        const Operation* call = context.valueID() ? &operationAt(context.valueID()) : nullptr;
        bool receiverIsMatched = call && call->opcode == Opcode::CallBuiltin && call->overload() == id && call->builtinCallForm() != BuiltinCallForm::AnyReceiver;
        return callCustomGetter(context, id, *receiver, receiverIsMatched);
    }

    if (overload.mode == BuiltinCallMode::Call && m_builtins.at(overload.builtin).kind == BuiltinKind::Accessor) {
        RELEASE_ASSERT(receiver);
        Boxed holder = boxCell(builtinCell(context, BuiltinCellKey::object(m_builtins.at(overload.builtin).holder)));
        Boxed bound = callOperation<Boxed>(context, RuntimeOperation::GetSuperPropertyById, { holder, *receiver, identifierNumber(keyOf(overload)) });
        return emitter<CallEmitter>().callValue(context, { bound, *receiver, arguments });
    }

    bool constructs = overload.mode == BuiltinCallMode::Construct;
    unsigned count = static_cast<unsigned>(arguments.size());
    std::optional<StubKey> stub = stubThatCallsBuiltin(m_builtins, overload, count);
    if (overload.mode == BuiltinCallMode::Get && receiver && !count && stub && stub->family() == StubFamily::CallHostFunctionWithoutBoxedCall && m_builtins.at(overload.builtin).origin == BuiltinOrigin::Host) {
        const Operation* call = context.valueID() ? &operationAt(context.valueID()) : nullptr;
        if (call && call->opcode == Opcode::CallBuiltin && call->overload() == id && call->builtinCallForm() == BuiltinCallForm::Whole)
            stub = callHostGetter();
    }
    if (std::optional<CompiledMethodOfArray> method = !constructs && receiver && count <= maxArgumentCountOfCompiledMethodOfArray ? compiledMethodOfArray(m_builtins.canonicalID(overload.builtin)) : std::nullopt) {
        m_mentions.builtinCell(keyOfCalleeOf(overload));
        Vector<LValue, 4> values;
        for (Boxed argument : arguments)
            values.append(argument.value());
        return emitter<CallEmitter>().callByConvention(context, { .stub = callCompiledMethodOfArray(*method, count), .thisValue = receiver->value(), .arguments = values.span() }, { .numberedImmediate = NumberedImmediate { mentionOfName(keyOf(overload)) } });
    }
    bool leavesFunctionUnfetched = stub && stub->family() == StubFamily::CallBoxedBuiltin && fetchesFunctionOnlyToLeave(static_cast<BoxedCallForm>(stub->form()), count) && keyOfCalleeOf(overload).hasCell(m_builtins);
    Boxed callee = leavesFunctionUnfetched ? Boxed(zeroExtended(numberOf(m_mentions.builtinCell(keyOfCalleeOf(overload)))).value()) : boxCell(calleeOf(context, overload));
    Boxed thisValue = undefined();
    if (constructs)
        thisValue = callee;
    else if (receiver)
        thisValue = *receiver;
    else if (BuiltinID holder = m_builtins.at(overload.builtin).holder; holder && BuiltinCellKey::object(holder).hasCell(m_builtins))
        thisValue = boxCell(builtinCell(context, BuiltinCellKey::object(holder)));

    CallEmitter& calls = emitter<CallEmitter>();
    CallOptions options { .constructs = constructs };
    if (stub && expected && expected->result) {
        if (stub->family() == StubFamily::CallBoxedBuiltin)
            expected->isCheckedByCallee = checkedResultOf(static_cast<BoxedCallForm>(stub->form())) == expected->result;
        else if (stub->family() == StubFamily::CallHostFunctionWithoutBoxedCall) {
            stub = callHostFunctionCheckingResult(*expected->result, count);
            expected->isCheckedByCallee = true;
        }
    }
    if (stub) {
        Vector<LValue, 8> values;
        for (Boxed argument : arguments)
            values.append(argument.value());
        return calls.callByConvention(context, { .stub = *stub, .callee = callee.value(), .thisValue = thisValue.value(), .arguments = values.span() }, options);
    }
    if (constructs)
        return callOperation<Boxed>(context, RuntimeOperation::Construct, { callee, callee, word32(count), argumentBuffer(arguments) }, options);
    return calls.callValue(context, { callee, thisValue, arguments }, options);
}

BuiltinCallEmitter::DirectOperands BuiltinCallEmitter::directOperandsOf(const ValueContext& value, const BuiltinDirectFunctionBinding& binding)
{
    RELEASE_ASSERT(value->argumentsOperands().size() >= binding.minArgumentCount());
    RELEASE_ASSERT(!binding.takesReceiver() || value->receiverOperand());
    std::span<const ValueID> arguments = value->argumentsOperands();
    std::span<const BuiltinDirectValue> parameters = std::span(binding.parameters).first(binding.parameterCount);

    DirectOperands operands;
    size_t next = 0;
    auto takeValue = [&] {
        ValueID argument = arguments[next++];
        LValue word = boxedOperand(value, argument).value();
        if (effectsOf(value).mayCollect && typeOf(argument).hasCells() && !word->hasInt64())
            operands.keptAlive.append(word);
        return word;
    };
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
            operands.words.append(takeValue());
            break;
        case BuiltinDirectValue::ValueOrEmpty:
            operands.words.append(next < arguments.size() ? takeValue() : m_out.constInt64(JSValue::encode(JSValue())));
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
    bool makesErrors = overload.throws() && !overload.onlyPropagatesErrors() && !groupsFunctionsOfCompiledCode(m_builtins.at(overload.builtin).holder);
    bool hasFunctionObject = BuiltinCellKey::object(overload.builtin).hasCell(m_builtins);
    bool canMakeFrame = hasFunctionObject && RuntimeCallEmitter::leavesRegistersForHostFrame(operands.words.span());
    std::optional<RuntimeCallEmitter::HostFrameOfDirectCall> hostFrame;
    if (operands.takesFrame || (makesErrors && canMakeFrame))
        hostFrame = RuntimeCallEmitter::HostFrameOfDirectCall { (value->receiverOperand() ? boxedOperand(value, value->receiverOperand()) : undefined()).value(), calleeOf(value, overload).value() };
    BuiltinID callee = m_builtins.canonicalID(overload.builtin);
    std::optional<BuiltinNativeFunction> nativeFunction = BuiltinBindings::nativeFunctionOf(callee);
    bool hasNameInFrames = hasFunctionObject || (nativeFunction && !nativeFunction->name.isNull());
    BuiltinID listedAtSite = makesErrors && !hostFrame && hasNameInFrames ? callee : BuiltinID();
    return Boxed(emitter<RuntimeCallEmitter>().callDirectFunction(value, number, operands.words.span(), operands.keptAlive.span(), hostFrame, listedAtSite));
}

void BuiltinCallEmitter::generateNewSetFromData(const ValueContext& value)
{
    uint32_t request[] { value->data().index(), false };
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewSetFromData, { numberOf(m_mentions.request(RequestKind::CollectionData, request)) }));
}

void BuiltinCallEmitter::generateNewMapFromData(const ValueContext& value)
{
    uint32_t request[] { value->data().index(), true };
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewMapFromData, { numberOf(m_mentions.request(RequestKind::CollectionData, request)) }));
}

void BuiltinCallEmitter::generateNewFormatterWithPattern(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewFormatterWithPattern, { numberOf(m_mentions.request(RequestKind::FormatPattern, value->formatPattern())) }));
}

void BuiltinCallEmitter::generateSetUpFormatterWithOptions(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::SetUpFormatterWithOptions, { in<CellPointer>(value->formatterOperand()), boxCell(in<CellPointer>(value->optionsOperand())) });
}

void BuiltinCallEmitter::generateFormatDateWithPattern(const ValueContext& value)
{
    define(value, callOperation<Boxed>(value, RuntimeOperation::FormatWithPattern, { numberOf(m_mentions.request(RequestKind::FormatPattern, value->formatPattern())), boxCell(in<CellPointer>(value->dateOperand())) }));
}

void BuiltinCallEmitter::generateFormatNumberWithPattern(const ValueContext& value)
{
    define(value, callOperation<Boxed>(value, RuntimeOperation::FormatWithPattern, { numberOf(m_mentions.request(RequestKind::FormatPattern, value->formatPattern())), boxedOperand(value, value->numberOperand()) }));
}

void BuiltinCallEmitter::generateNewArgumentList(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewArgumentList, { in<Boxed>(value->arrayLikeOperand()) }));
}

void BuiltinCallEmitter::generateNewArrayOfGiven(const ValueContext& value)
{
    ArrayAllocationEmitter allocation(generator());
    const ArrayStorageForm& seed = arrayStorageFormOf(ArrayStorage::Int32);
    Boxed given = in<Boxed>(value->givenOperand());
    std::optional<Pointer> profile = allocation.profileOfListsOfLengthAtSite(value.origin(), seed);
    if (!profile) {
        define(value, CellPointer(callStub(value, newArrayOfGiven(), { given.value() }).first));
        return;
    }
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    branchUnless(isInt32(given), slowPath);
    Integer length = signExtended(unboxInt32(given));
    CellPointer array = allocation.allocateArrayOfHolesAtSite(value, length, seed, *profile, slowPath);
    allocation.setLengthOfNewArray(array, length);
    join.add(array);

    m_out.appendTo(slowPath);
    join.add(CellPointer(callStub(value, newArrayOfGiven(), { given.value() }).first));
    define(value, join.finish());
}

bool BuiltinCallEmitter::canShareRegExpLiteral(const Operation& literal) const
{
    return !!placeWhereRegExpLiteralIsWritten(m_program, m_code, literal);
}
void BuiltinCallEmitter::generateSharedRegExpLiteral(const ValueContext& value)
{
    std::optional<Wire::SourceLocation> written = placeWhereRegExpLiteralIsWritten(m_program, m_code, value.value());
    if (!written) {
        failBuiltinOperationWithZero(generator(), value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    bool countsMatches = onlyCountsMatches(value.id());
    if (countsMatches)
        build().statistics().count(StatisticID::MatchArraysElided);
    define(value, sharedRegExpLiteral(value, value.value(), countsMatches ? countedMatchesParameter : 0));
}

static bool onlyReadsLength(const Program& program, const Code& code, ValueID array)
{
    static constexpr OpcodeSet names { Opcode::Narrowed, Opcode::Box, Opcode::Unbox };
    static constexpr OpcodeSet tests { Opcode::ToBoolean, Opcode::TypeTest, Opcode::ShapeTest, Opcode::GetArrayLength };
    Vector<ValueID, 8> holders { array };
    auto add = [&](ValueID holder) {
        if (!holders.contains(holder))
            holders.append(holder);
    };
    auto isCompiledInstance = [&](const Operation& operation) {
        if (operation.opcode != Opcode::Narrowed || !operation.testOperand() || !operation.isOnTakenEdge() || operation.narrowsHolderOfOperand() || operation.narrowsOtherResultOfOperand() || operation.narrowsTermOfOperand())
            return false;
        const Operation& test = code.at(operation.testOperand());
        return test.opcode == Opcode::ShapeTest && holders.contains(test.objectOperand());
    };
    for (size_t seen = 0; seen != holders.size();) {
        seen = holders.size();
        for (uint32_t index = 0; index < code.numberOfBlocks(); ++index) {
            const BlockID block(index);
            if (!code.isReached(block))
                continue;
            bool isHeld = true;
            for (ValueID id : code.valuesOf(block)) {
                const Operation& operation = code.at(id);
                operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    if (!holders.contains(operand) || tests.contains(operation.opcode))
                        return;
                    if (names.contains(operation.opcode) && !operandIndex) {
                        if (!isCompiledInstance(operation))
                            add(id);
                    } else
                        isHeld = isHeld && operation.opcode == Opcode::PropGet && !operation.isFreeName() && operation.objectOperand() == operand && program.nameClassOf(operation.key()) == NameClass::Length;
                });
            }
            const Terminator& terminator = code.at(block).terminator;
            terminator.forEachOperand([&](ValueID operand, uint32_t) {
                isHeld = isHeld && (!holders.contains(operand) || terminator.isConditionalBranch());
            });
            if (!isHeld)
                return false;
            code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
                std::span<const ValueID> arguments = edge.arguments();
                std::span<const ValueID> parameters = code.at(edge.to).parameters();
                for (size_t place = 0; place < arguments.size(); ++place) {
                    if (holders.contains(arguments[place]))
                        add(parameters[place]);
                }
            });
        }
    }
    return true;
}

bool BuiltinCallEmitter::onlyCountsMatches(ValueID literal) const
{
    const OptionSet<RegExpFlags> flags = m_code.at(literal).regExpFlags();
    if (!flags.contains(RegExpFlags::Global) || flags.contains(RegExpFlags::Sticky))
        return false;
    ValueID passed = literal;
    ValueID user = m_code.onlyUserOf(literal);
    if (user && m_code.at(user).opcode == Opcode::Box) {
        passed = user;
        user = m_code.onlyUserOf(user);
    }
    if (!user)
        return false;
    const Operation& call = m_code.at(user);
    switch (call.opcode) {
    case Opcode::Call:
        if (!call.methodOfReceiverAsMade() || m_program.builtins().boxedCallStubOf(call.methodOfReceiverAsMade()) != BuiltinBoxedStub::StringMatch || call.argumentsOperands().size() != 1 || call.argumentsOperands()[0] != passed)
            return false;
        break;
    case Opcode::CallInline:
        if (call.inlineBuiltin() != InlineBuiltin::StringMatch)
            return false;
        break;
    case Opcode::RegExpMatchLiteral:
        if (call.matchForm() != RegExpMatchForm::Match)
            return false;
        break;
    default:
        return false;
    }
    return onlyReadsLength(m_program, m_code, user);
}

CellPointer BuiltinCallEmitter::sharedRegExpLiteral(const ValueContext& at, const Operation& literal, uint32_t parameters)
{
    Wire::SourceLocation written = *placeWhereRegExpLiteralIsWritten(m_program, m_code, literal);
    OptionSet<Yarr::Flags> flags = toYarrFlags(literal.regExpFlags());
    uint32_t cell = m_mentions.regExpLiteralCell(written, literal.pattern(), flags.toRaw() | parameters);
    uint32_t record = m_mentions.regExp(literal.pattern(), flags);
    Integer shared(m_out.load64(instanceWord(m_heaps.entire(AbstractHeap::AllocatorState), cell)));
    return either<CellPointer>(isNotZero(shared), [&] {
        return CellPointer(shared.value());
    }, [&] {
        return callOperation<CellPointer>(at, RuntimeOperation::SharedRegExpLiteral, { numberOf(cell), numberOf(record), word32(flags.toRaw() | parameters) }, { .isOnRarePath = true });
    });
}

void BuiltinCallEmitter::generateGetBuiltinValue(const ValueContext& value)
{
    defineAs(value, builtinValue(value, BuiltinCellKey::value(m_builtins.canonicalID(value->builtin())), RuntimeOperation::FillBuiltinValue, &value), typeOf(value.id()));
}

void BuiltinCallEmitter::generateGetWatchedBuiltinValue(const ValueContext& value)
{
    defineAs(value, builtinValue(value, BuiltinCellKey::value(m_builtins.canonicalID(value->builtin())), RuntimeOperation::FillWatchedBuiltinValue), typeOf(value.id()));
}

void BuiltinCallEmitter::generateGetWatchedBuiltinValueOrAbsent(const ValueContext& value)
{
    BuiltinCellKey key = BuiltinCellKey::value(m_builtins.canonicalID(value->builtin()));
    if (!key.hasCell(m_builtins)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, undefined());
        return;
    }
    Pointer values = loadImmutablePointer(AOT_INSTANCE_OFFSET(builtins.hostValues));
    Integer number = zeroExtended(numberOf(m_mentions.builtinCell(key)));
    defineAs(value, Boxed(m_out.load64(m_out.baseIndex(m_heaps.entire(AbstractHeap::RuntimeState), values.value(), number.value(), ScaleEight))), typeOf(value.id()));
}

void BuiltinCallEmitter::generateInitializeBuiltin(const ValueContext& value)
{
    BuiltinCellKey read = BuiltinCellKey::value(value->builtin());
    emitter<ConstantEmitter>().builtinCell(value, read.hasCell(m_builtins) ? read : BuiltinCellKey::object(m_builtins.canonicalID(value->builtin())));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
