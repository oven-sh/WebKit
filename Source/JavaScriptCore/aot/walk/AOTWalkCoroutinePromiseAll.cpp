/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkCoroutineCalls.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTWalkCoroutineCallsInternal.h"
#include "AOTWalkTextBuilders.h"
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CallOfPromiseAllInPromiseAll {
    ValueID list;
    ValueID all;
    TypeID contents;
};

struct DirectCallInPromiseAll {
    ValueID call;
    Reference handedBack;
};

} // anonymous namespace

static bool isOneBuiltinInPromiseAll(const Walker& walker, TypeID id, BuiltinID wanted)
{
    TypeArena& types = walker.types();
    Type type = types.at(id);
    if (!type.isObject() || !type.isJavaScriptValue())
        return false;
    BuiltinID named = singleBuiltinOf(types, type);
    const BuiltinTable& table = walker.program().builtins();
    return named && table.canonicalID(named) == table.canonicalID(wanted);
}

static TypeID typeOfInputAheadInPromiseAll(Walker& walker, const BodyIndex& index, ValueID input, ValueID firstUnbuilt)
{
    ValueID source = index.stripCasts(input);
    if (source.index() < firstUnbuilt.index()) {
        Reference built = walker.referenceOf(source);
        return built ? walker.typeOf(built) : TypeID(TypeTable::anything);
    }
    BodyValue operation = index.value(source);
    bool takesOperand = false;
    operation.forEachOperand([&](ValueID, uint32_t) {
        takesOperand = true;
    });
    TypeID widest = takesOperand ? TypeID() : widestTypeOfOperation(walker, operation);
    return widest ? widest : TypeID(TypeTable::anything);
}

static bool hasStringInBuilderOperand(Walker& walker, const BodyValue& operation)
{
    if (!walker.hasStringsInBuilders())
        return false;
    bool takesText = false;
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        takesText |= operand && findStringInBuilder(walker, operand);
    });
    return takesText;
}

static std::optional<CallOfPromiseAllInPromiseAll> findCallThatTakesListInPromiseAll(Walker& walker, const BodyIndex& index, BodyBlock block, ValueID list, ValueID firstUnbuilt)
{
    BodyValue literal = index.value(list);
    if (!literal.holes().empty() || literal.elementsOperands().size() > maxComputedElementsOfArrayLiteral || hasStringInBuilderOperand(walker, literal))
        return std::nullopt;
    ValueID name = list;
    ValueID all = followCastsToOnlyTaker(index, name);
    if (!all || index.opcodeOf(all) != Opcode::Call || !block.holds(all) || all.index() - block.firstValue().index() >= walker.endOfStretchBeingWalked())
        return std::nullopt;
    BodyValue call = index.value(all);
    if (call.isDoneInPlace() || !call.thisValueOperand() || call.argumentsOperands().size() != 1 || call.argumentsOperands()[0] != name)
        return std::nullopt;

    TypeArena& types = walker.types();
    TypeID typeOfList = types.builtinClass(BuiltinClasses::Array);
    auto typeOfInput = [&](ValueID input) -> TypeID {
        return index.stripCasts(input) == list ? typeOfList : typeOfInputAheadInPromiseAll(walker, index, input, firstUnbuilt);
    };
    TypeID typeOfThis = typeOfInput(call.thisValueOperand());
    if (!isOneBuiltinInPromiseAll(walker, typeOfInput(call.calleeOperand()), Builtins::PromiseAll) || !isOneBuiltinInPromiseAll(walker, typeOfThis, Builtins::Promise))
        return std::nullopt;
    if (!isArrayIteratorProtocolIntact(walker) || wouldCallBuiltin(walker, call, TypesOfInputs(typeOfInput)) == BuiltinCallAskedAbout::None)
        return std::nullopt;

    const BuiltinTable& table = walker.program().builtins();
    BuiltinOperandTypes passed;
    passed.receiver = typeOfThis;
    passed.arguments.append(typeOfList);
    TypeID typeOfCall = resultTypeOfReachableOverloads(BuiltinTypeContext { walker.program(), types }, table.canonicalID(Builtins::PromiseAll), BuiltinCallMode::Call, passed);
    if (!typeOfCall)
        return std::nullopt;
    const ClassPart* promise = findClassPart(types, types.at(typeOfCall), BuiltinClasses::Promise);
    CallOfPromiseAllInPromiseAll found { list, all, promise ? promise->writtenContentsAt(table, ContentsSlot::Value) : TypeID(TypeTable::anything) };
    Draft operation(Opcode::PromiseAllOfElements);
    operation.setContents(found.contents);
    TypeID typeOfOperation = walker.resultTypeOf(operation);
    if (!typeOfOperation || !types.isSubset(typeOfOperation, typeOfCall))
        return std::nullopt;
    return found;
}

static bool isAsyncFunctionInPromiseAll(const Function& function)
{
    return function.has(FunctionFlags::IsAsync) && !function.has(FunctionFlags::IsGenerator);
}

static FunctionID findBodyThatListedCallEntersInPromiseAll(Walker& walker, const BodyValue& call)
{
    FunctionID callee = findCoroutineCalledBy(walker, call);
    if (!callee || !isAsyncFunctionInPromiseAll(walker.program().at(callee)) || walker.plan().conventions().of(callee).coroutineNeverSuspends)
        return { };
    return writtenOperandsMayStartBodyOfCoroutine(walker, walker.draftOf(call)) ? callee : FunctionID();
}

static Reference buildOperationInPromiseAll(Walker& walker, const BodyIndex& index, const CallOfPromiseAllInPromiseAll& written, std::span<const DirectCallInPromiseAll> directCalls)
{
    uint32_t bitsOfDirectCalls = 0;
    static_assert(maxComputedElementsOfArrayLiteral <= 8 * sizeof(bitsOfDirectCalls));
    Vector<Reference, 8> elements;
    for (ValueID element : index.value(written.list).elementsOperands()) {
        ValueID source = index.stripCasts(element);
        size_t direct = directCalls.size();
        for (size_t place = 0; place < directCalls.size(); ++place) {
            if (directCalls[place].call == source)
                direct = place;
        }
        Reference reference = direct < directCalls.size() ? directCalls[direct].handedBack : walker.referenceOf(element);
        if (!reference || isCellOfFrameInCoroutineCalls(walker, reference)) {
            walker.fail("an element of a list that is written for Promise.all is not built, or lies in the frame"_s);
            return { };
        }
        if (direct < directCalls.size())
            bitsOfDirectCalls |= 1u << elements.size();
        elements.append(reference);
    }

    BodyValue call = index.value(written.all);
    Draft operation = draftOfCoroutineCalls(walker, Opcode::PromiseAllOfElements, walker.originOf(call));
    operation.setContents(written.contents);
    operation.setDirectCalls(bitsOfDirectCalls);
    operation.setElementsOperands(elements.span());
    operation.inputValueGiven = written.all;
    return walker.build(WTF::move(operation));
}

static bool buildAtCallOfBuiltinInPromiseAll(Walker& walker, const BodyIndex& index, const BodyValue& call)
{
    if (call.argumentsOperands().size() != 1)
        return false;
    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!callee || !isOneBuiltinInPromiseAll(walker, walker.typeOf(callee), Builtins::PromiseAll))
        return false;
    ValueID list = index.stripCasts(call.argumentsOperands()[0]);
    BodyBlock block = index.block(index.blockOf(call.id()));
    if (index.isParameter(list) || index.opcodeOf(list) != Opcode::NewArray || !block.holds(list))
        return false;
    std::optional<CallOfPromiseAllInPromiseAll> written = findCallThatTakesListInPromiseAll(walker, index, block, list, call.id());
    if (!written || written->all != call.id())
        return false;
    for (ValueID element : index.value(list).elementsOperands()) {
        Reference reference = walker.referenceOf(element);
        if (!reference || isCellOfFrameInCoroutineCalls(walker, reference))
            return false;
    }

    if (Reference promise = buildOperationInPromiseAll(walker, index, *written, { }))
        walker.bind(call.id(), promise);
    return true;
}

static bool buildFromListedCallInPromiseAll(Walker& walker, const BodyIndex& index, const BodyValue& first)
{
    ValueID name = first.id();
    ValueID list = followCastsToOnlyTaker(index, name);
    if (!list || index.opcodeOf(list) != Opcode::NewArray)
        return false;
    BodyBlock block = index.block(index.blockOf(first.id()));
    if (!block.holds(list))
        return false;
    FunctionID firstCallee = findBodyThatListedCallEntersInPromiseAll(walker, first);
    if (!firstCallee)
        return false;
    ValueID firstUnbuilt = first.id();
    std::optional<CallOfPromiseAllInPromiseAll> written = findCallThatTakesListInPromiseAll(walker, index, block, list, firstUnbuilt);
    if (!written)
        return false;

    for (uint32_t value = first.id().index() + 1; value < written->all.index(); ++value) {
        BodyValue between = index.value(ValueID(value));
        Opcode opcode = between.opcode();
        if (between.id() == list || opcodesThatStateTypes.contains(opcode))
            continue;
        if (opcode != Opcode::Call) {
            if (!declaredBoundInTable(opcode).isUnobservable())
                return false;
            continue;
        }
        ValueID nameOfBetween = between.id();
        if (between.isDoneInPlace() || followCastsToOnlyTaker(index, nameOfBetween) != list || hasStringInBuilderOperand(walker, between))
            return false;
        FunctionID callee = findOnlyCallee(walker, between, typeOfInputAheadInPromiseAll(walker, index, between.calleeOperand(), firstUnbuilt));
        if (!callee || !isAsyncFunctionInPromiseAll(walker.program().at(callee)))
            return false;
    }

    std::optional<Vector<Reference, 8>> firstOperands = layOutEntryOperandsOfCall(walker, first, firstCallee, { firstCallee, CodeRole::CoroutineBody });
    if (!firstOperands)
        return false;
    if (firstOperands->size() != walker.program().at(firstCallee).numberOfEntryParameters())
        return true;

    Vector<DirectCallInPromiseAll, 8> directCalls;
    Reference handedBack = buildStartOfBodyOfCoroutine(walker, first, firstCallee, firstOperands->span(), undefinedOfCoroutineCalls(walker));
    if (!handedBack)
        return true;
    directCalls.append({ first.id(), handedBack });

    uint32_t firstValue = block.firstValue().index();
    {
        SetForScope takesListApart(keptByCallsOfCoroutines(walker).isTakingListOfPromiseAllApart, true);
        for (uint32_t value = first.id().index() + 1; value < written->all.index(); ++value) {
            BodyValue between = index.value(ValueID(value));
            if (between.id() == list)
                continue;
            if (opcodesThatStateTypes.contains(between.opcode())) {
                ValueID source = index.stripCasts(between.id());
                if (source == list || directCalls.containsIf([&](const DirectCallInPromiseAll& direct) { return direct.call == source; }))
                    continue;
            }
            if (FunctionID callee = between.opcode() == Opcode::Call ? findBodyThatListedCallEntersInPromiseAll(walker, between) : FunctionID()) {
                if (std::optional<Vector<Reference, 8>> operands = layOutEntryOperandsOfCall(walker, between, callee, { callee, CodeRole::CoroutineBody })) {
                    if (operands->size() != walker.program().at(callee).numberOfEntryParameters())
                        return true;
                    handedBack = buildStartOfBodyOfCoroutine(walker, between, callee, operands->span(), undefinedOfCoroutineCalls(walker));
                    if (!handedBack)
                        return true;
                    directCalls.append({ between.id(), handedBack });
                    continue;
                }
            }
            walker.walkStretch(block.id(), value - firstValue, value - firstValue + 1);
            if (walker.isUnreachable() || walker.hasFailed())
                return true;
        }
    }

    Reference promise = buildOperationInPromiseAll(walker, index, *written, directCalls.span());
    if (!promise)
        return true;
    walker.bind(written->all, promise);
    leaveAnythingForSwallowedOperation(walker, written->all);
    walker.leaveArrivals(firstCallee, firstOperands->span());
    uint32_t behind = written->all.index() - firstValue + 1;
    walker.walkStretch(block.id(), behind, behind);
    return true;
}

bool buildPromiseAllOfElements(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::BuiltinCalls))
        return false;
    KeptByCallsOfCoroutines* kept = findKeptByCallsOfCoroutines(walker);
    if (kept && kept->isTakingListOfPromiseAllApart)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    return buildAtCallOfBuiltinInPromiseAll(walker, index, call) || buildFromListedCallInPromiseAll(walker, index, call);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
