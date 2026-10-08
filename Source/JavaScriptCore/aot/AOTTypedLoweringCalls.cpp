/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTTypedObject.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "JSCInlines.h"
#include "VMTraps.h"
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

static RegisterSet registersClobberedByCalls()
{
#if CPU(ARM64)
    return RegisterSet { callMarkerGPR };
#else
    RegisterSet result;
    for (GPRReg reg : stubTemporaryGPRs)
        result.add(reg, IgnoreVectors);
    result.add(callMarkerGPR, IgnoreVectors);
    return result;
#endif
}

void Lowering::didEmitWrite(Writes writes, bool writesShapeWords)
{
    if (writes == Writes::Fields && m_isStoringToLocalAllocation)
        writes = Writes::RuntimeState;
    if (writes <= m_function.writes && (!writesShapeWords || m_function.writesShapeWords))
        return;
    dataLogLn("AOT: f", m_id.index(), " changes more (", static_cast<unsigned>(writes), writesShapeWords ? ", shape words" : "", ") than was summed up for it (", static_cast<unsigned>(m_function.writes), m_function.writesShapeWords ? ", shape words" : "", ")");
    m_program.dump(WTF::dataFile(), m_id);
    CRASH();
}

// The heaps of the program are numbered last: fields and variables, the bytes of buffers, the state of the collector, the state of the
// run time, order words, shape words. So whatever a call writes is one range. See B3AbstractHeapRepository.cpp.
void Lowering::applyEffects(PatchpointValue* patchpoint, OperationEffects effects)
{
    bool writesShapeWords = effects.writesShapeWords && m_program.castsMayRetag;
    didEmitWrite(effects.writes, writesShapeWords);
    patchpoint->effects = Effects::none();
    if (effects.writes == Writes::Pure)
        return;
    // Whatever may collect garbage or unwind looks at all there is.
    patchpoint->effects.reads = HeapRange::top();
    patchpoint->effects.exitsSideways = effects.exitsSideways;
    patchpoint->effects.controlDependent = true;
    // Any call may make a string, say, and so come to collect garbage: what a write barrier goes by is read again after it.
    const AbstractHeap* first = &m_heaps.AOTTyped_collectorState;
    const AbstractHeap* last = &m_heaps.AOTTypedObject_orderWord;
    switch (effects.writes) {
    case Writes::Pure:
    case Writes::Nothing:
        last = first;
        break;
    case Writes::RuntimeState:
        last = &m_heaps.AOTTyped_runtimeState;
        break;
    case Writes::TypedArrayProperties:
        first = &m_heaps.TypedArrayProperties;
        last = effects.writesOnlyTypedArrayProperties ? first : &m_heaps.AOTTyped_runtimeState;
        break;
    case Writes::Fields:
        if (!effects.writesOnlyOrderWords)
            first = &m_heaps.AOTTyped_fields;
        break;
    case Writes::Heap:
        first = &m_heaps.root;
        break;
    }
    // Whoever gives an object another shape gives it an order of that shape.
    if (writesShapeWords)
        last = &m_heaps.AOTTypedObject_shapeWord;
    m_heaps.decoratePatchpointWrite(first, last, patchpoint);
}

LValue Lowering::callEntry(Entry entry, LType type, bool mayThrow, OperationEffects effects, const Vector<LValue, 8>& arguments)
{
    m_context.emitsCalls = true;
    bool withInstance = takesInstance(entry);
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    applyEffects(patchpoint, effects);
    unsigned nextGPR = withInstance;
    unsigned nextFPR = 0;
    for (LValue argument : arguments) {
        if (argument->type() == Double) {
            RELEASE_ASSERT(nextFPR < FPRInfo::numberOfArgumentRegisters);
            patchpoint->append(ConstrainedValue(argument, ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++))));
            continue;
        }
        RELEASE_ASSERT(nextGPR < numberOfOperationArgumentGPRs);
        patchpoint->append(ConstrainedValue(argument, ValueRep::reg(operationArgumentGPR(nextGPR++))));
    }
    Stub stub;
    if (!mayThrow)
        stub = withInstance ? Stub::PlainOperationWithInstance : Stub::PlainOperation;
    else if (type == Double)
        stub = withInstance ? Stub::OperationDoubleWithInstance : Stub::OperationDouble;
    else if (type == Void)
        stub = withInstance ? Stub::OperationVoidWithInstance : Stub::OperationVoid;
    else
        stub = withInstance ? Stub::OperationValueWithInstance : Stub::OperationValue;
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
#if CPU(X86_64)
    patchpoint->clobberLate(registersClobberedByCalls());
#endif
    if (type == Double)
        patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::returnValueFPR) };
    else if (type != Void)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site;
    if (mayThrow)
        site.bits = nextSite();
    uint32_t immediate = static_cast<uint32_t>(static_cast<unsigned>(entry) * sizeof(void*));
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, stub, immediate, site](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        stubCalls->call(jit, stub, immediate, site);
    });
    return patchpoint;
}

LValue Lowering::callFunction(FunctionID function, LType type, const Vector<LValue, 8>& arguments)
{
    m_context.emitsCalls = true;
    uint32_t callee = m_context.calleeIndices.ensure(function.index(), [&] {
        m_context.knownCallees.append(keyOf(function));
        return m_context.knownCallees.size() - 1;
    }).iterator->value;
    // What goes on the stack takes a whole word there.
    Vector<LValue, 8> widened;
    for (LValue argument : arguments)
        widened.append(argument->type() == Int32 ? m_out.zeroExt(argument, Int64) : argument);
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    // Where no handler watches, what follows the call only runs if the call comes back. It is pure only if it never throws either.
    auto& summary = m_program.at(function);
    OperationEffects effects { summary.writes, true, summary.writesShapeWords };
    if (!m_currentCatchEntrypoint && summary.writes != Writes::Pure)
        effects = { std::max(summary.writesIfItReturns, Writes::Nothing), true, summary.writesShapeWordsIfItReturns };
    applyEffects(patchpoint, effects);
    unsigned nextGPR = 0;
    unsigned nextFPR = 0;
    unsigned onStack = 0;
    for (unsigned i = 0; i < arguments.size(); ++i) {
        LValue argument = arguments[i];
        if (argument->type() == Double && nextFPR < FPRInfo::numberOfArgumentRegisters)
            patchpoint->append(ConstrainedValue(argument, ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++))));
        else if (argument->type() != Double && nextGPR < numberOfArgumentGPRs)
            patchpoint->append(ConstrainedValue(argument, ValueRep::reg(argumentGPR(nextGPR++))));
        else
            patchpoint->append(ConstrainedValue(widened[i], ValueRep::stackArgument(static_cast<int32_t>(onStack++ * sizeof(Register)))));
    }
    if (onStack)
        m_proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(onStack * sizeof(Register)));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(registersClobberedByCalls());
    if (type == Double)
        patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::returnValueFPR) };
    else if (type != Void)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, callee, site](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        stubCalls->callFunction(jit, callee, site);
    });
    return patchpoint;
}

// The operands: the callee, `this`, the arguments.
Boxed Lowering::callValue(Stub stub, const Vector<LValue, 8>& operands)
{
    m_context.emitsCalls = true;
    unsigned count = operands.size() - 2;
    RELEASE_ASSERT(count <= numberOfArgumentGPRs);
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    applyEffects(patchpoint, { });
    patchpoint->append(ConstrainedValue(operands[0], ValueRep::reg(calleeGPR)));
    patchpoint->append(ConstrainedValue(operands[1], ValueRep::reg(thisGPR)));
    for (unsigned i = 0; i < count; ++i)
        patchpoint->append(ConstrainedValue(operands[i + 2], ValueRep::reg(argumentGPR(i))));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(registersClobberedByCalls());
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, stub, count, site](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        stubCalls->call(jit, stub, count, site);
    });
    return Boxed(patchpoint);
}

CellPointer Lowering::allocate(uint32_t allocation)
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    // The first allocation, which takes the slow path, fills the entry.
    LValue entry = m_out.add(fixedPointer(Instance::offsetOfTypedAllocations()), m_out.constIntPtr(static_cast<intptr_t>(allocation) * sizeof(Instance::TypedAllocation)));
    auto inEntry = [&](ptrdiff_t offset) {
        return m_out.address(m_heaps.AOTTyped_runtimeState, entry, offset);
    };
    LValue cell = allocateHeapCell(m_out.loadPtr(inEntry(OBJECT_OFFSETOF(Instance::TypedAllocation, allocator))), slowPath);
    applyEffects(cell->as<PatchpointValue>(), { Writes::RuntimeState, false });
    m_out.store64(m_out.load64(inEntry(OBJECT_OFFSETOF(Instance::TypedAllocation, header))), cell, m_heaps.JSCell_header);
    if (isObjectLayout(m_program.at(m_program.allocations[allocation].layout).kind))
        m_out.store64(m_out.load64(inEntry(OBJECT_OFFSETOF(Instance::TypedAllocation, words))), cell, m_heaps.AOTTypedObject_words);
    ValueFromBlock fast = m_out.anchor(cell);
    m_out.jump(continuation);
    m_out.appendTo(slowPath);
    ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(AllocSlow, word32(allocation)).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, fast, slow));
}

CellPointer Lowering::stringConstant(StringID string)
{
    return lazilyInitialized<CellPointer>(Instance::offsetOfTypedStrings(), string.index(), [&] {
        return AOT_TYPED_OPERATION(StringConstant, word32(string.index()));
    });
}

void Lowering::poll()
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    PatchpointValue* bits = m_out.patchpoint(Int32);
    bits->append(m_vm, ValueRep::SomeRegister);
    bits->effects = Effects::none();
    bits->effects.reads = HeapRange::top();
    bits->effects.writesLocalState = true;
    bits->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.load32(CCallHelpers::Address(params[1].gpr(), VM::offsetOfTrapsBits()), params[0].gpr());
    });
    m_out.branch(m_out.testNonZero32(bits, m_out.constInt32(VMTraps::AsyncEvents)), rarely(slowPath), usually(continuation));
    m_out.appendTo(slowPath);
    AOT_TYPED_OPERATION(PollSlow);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

// The values, boxed, one after the other on the stack: their number and the address of the first.
std::pair<Word32, Pointer> Lowering::argumentBuffer(const Arguments& arguments)
{
    LValue slot = m_out.lockedStackSlot(std::max<size_t>(arguments.size(), 1) * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < arguments.size(); ++i)
        m_out.store64(in<Boxed>(arguments[i]).value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, slot, i * sizeof(EncodedJSValue)));
    return { word32(arguments.size()), Pointer(slot) };
}

// What the function is handed as the parameter of that number of its entry block: what it is declared to take. The representation of that
// is how it is passed. See lowerEntryParameters().
const Type& Lowering::typePassedAs(const Function& callee, unsigned index) const
{
    unsigned at = index;
    if (callee.environment && !at--)
        return m_program.at(callee.at(callee.blocks[0].parameters[index]).type);
    TypeID declared;
    if (callee.has(Format::FunctionHasThis) && !at--)
        declared = callee.thisType;
    else if (at < callee.parameterTypes.size())
        declared = callee.parameterTypes[at];
    else
        return m_program.at(callee.at(callee.blocks[0].parameters[index]).type);
    return m_program.at(declared ? declared : m_program.types.anything);
}

// A call of a function of the program. An argument that is of the type of the parameter, which is what the verifier has seen to for a
// CallDirect, is handed over as it is.
Operand Lowering::callCompiled(FunctionID function, const Vector<Argument, 8>& arguments, const Type& result)
{
    auto& callee = m_program.at(function);
    if (callee.blocks.isEmpty())
        return failWith("a call of a function without a body"_s);
    unsigned numberOfParameters = callee.blocks[0].parameters.size();
    RELEASE_ASSERT(arguments.size() <= numberOfParameters);
    auto& undefinedType = m_program.at(m_program.types.undefined);
    Vector<LValue, 8> passed;
    for (unsigned i = 0; i < numberOfParameters; ++i) {
        Argument given = i < arguments.size() ? arguments[i] : Argument { undefined(), &undefinedType };
        passed.append(ensure(given.operand, *given.type, typePassedAs(callee, i)).value);
    }
    Rep rep = resultRepOf(m_program, function);
    LValue value = callFunction(function, b3TypeOf(rep), passed);
    if (rep == Rep::Void)
        return ensure(undefined(), undefinedType, result);
    return ensure(Operand(rep, value), m_program.at(callee.resultType ? callee.resultType : m_program.types.anything), result);
}

Operand Lowering::lowerCallDirect(ValueID result, const Value& value)
{
    Vector<Argument, 8> arguments;
    for (ValueID id : value.arguments)
        arguments.append(argument(id));
    return callCompiled(value.function(), arguments, typeOf(result));
}

FunctionID Lowering::genericEntrypointFor(FunctionID function)
{
    auto found = m_program.genericEntrypoints.find(function.index());
    if (found == m_program.genericEntrypoints.end()) {
        fail("a function value without a general entry"_s);
        return function;
    }
    return found->value;
}

// That the value is a closure of the program was found by a test, which the load stays behind.
CellPointer Lowering::scopeOfClosure(CellPointer closure)
{
    return loadField<CellPointer>(closure, m_heaps.AOTTypedClosure_environment);
}

// A function that captures nothing as a value: one object, which is made when it is first asked for.
CellPointer Lowering::closureConstant(FunctionID function)
{
    uint32_t general = genericEntrypointFor(function).index();
    return lazilyInitialized<CellPointer>(Instance::offsetOfTypedClosures(), general, [&] {
        return AOT_TYPED_OPERATION(ClosureConstant, word32(general));
    });
}

// A function value with an environment.
CellPointer Lowering::lowerNewClosure(const Value& value)
{
    FunctionID general = genericEntrypointFor(value.function());
    CellPointer environment = in<CellPointer>(value.child(0));
    Pointer entry = lazilyInitialized<Pointer>(Instance::offsetOfTypedFunctions(), general.index(), [&] {
        return AOT_TYPED_OPERATION(FunctionEntry, word32(general.index()));
    });
    // Closures are allocated with number 0: see linkAllocations().
    CellPointer cell = allocate(0);
    // That is as of a plain function. An async function or a generator function has another prototype, and so another Structure.
    if (unsigned which = closureStructureOf(m_program.at(value.function()).declaredAs))
        storeField(cell, m_heaps.JSCell_structureID, Word32(loadRuntimeState32(Instance::offsetOfTypedClosureStructureID(which))));
    storeField(cell, m_heaps.JSObject_butterfly, integer(0));
    storeField(cell, m_heaps.AOTTypedClosure_entry, entry);
    storeField(cell, m_heaps.AOTTypedClosure_environment, environment);
    // All 64 bits: what follows the number says that no property has been made of `length`, `name` or `prototype`.
    storeField(cell, m_heaps.AOTTypedClosure_function, integer(general.index()));
    fenceAfterInitialization();
    return cell;
}

// A call of a value of which nothing is known.
Boxed Lowering::callGenerally(Boxed callee, Boxed thisValue, const Arguments& arguments)
{
    if (arguments.size() > numberOfArgumentGPRs) {
        auto [count, list] = argumentBuffer(arguments);
        return AOT_TYPED_OPERATION(Call, callee, thisValue, count, list);
    }
    Vector<LValue, 8> operands { callee.value(), thisValue.value() };
    for (ValueID argument : arguments)
        operands.append(in<Boxed>(argument).value());
    return callValue(Stub::Call, operands);
}

Boxed Lowering::callMethodGenerally(Boxed object, StringID name, const Arguments& arguments)
{
    if (arguments.size() > numberOfArgumentGPRs) {
        auto [count, list] = argumentBuffer(arguments);
        return AOT_TYPED_OPERATION(CallMethod, object, word32(name.index()), count, list);
    }
    return callGenerally(emitGetById(object, name), object, arguments);
}

// Whether a call of a closure of the function with so many arguments comes down to a call of the function: what its general entry does
// in between is to take the environment out of the closure, to hold `this` and the arguments to the types of the parameters, with
// `undefined` for those that are missing, and to make an array of the rest.
bool Lowering::mayBeCalledInPlaceOfItsClosure(FunctionID function, unsigned numberOfArguments) const
{
    auto& callee = m_program.at(function);
    if (callee.blocks.isEmpty() || callee.isGenericEntrypoint() || !m_program.genericEntrypoints.contains(function.index()))
        return false;
    // To call a class throws.
    if (callee.kind == FunctionKind::Constructor || callee.kind == FunctionKind::Module)
        return false;
    bool hasRest = callee.has(Format::FunctionHasRestParameter);
    if (hasRest && callee.parameterTypes.isEmpty())
        return false;
    auto& undefinedType = m_program.at(m_program.types.undefined);
    for (unsigned i = 0; i < callee.parameterTypes.size(); ++i) {
        TypeID type = callee.parameterTypes[i];
        if (!type)
            continue;
        if (hasRest && i + 1 == callee.parameterTypes.size()) {
            if (!Type::engineClass(m_builtins.arrayClass()).isWithin(m_program.at(type), m_program))
                return false;
        } else if (i >= numberOfArguments && !undefinedType.isWithin(m_program.at(type), m_program))
            return false;
        // An array that is coerced is gone through even though it is an array: see coerce().
        if (callee.reasonToHoldParameter(i) != CastReason::As)
            continue;
        for (TypeID elements = m_program.at(type).elements; elements; elements = m_program.at(elements).elements) {
            if (!m_program.at(elements).views.isEmpty())
                return false;
        }
    }
    return true;
}

// The cell is a closure of the function. Where `this` or an argument is not of the type of the parameter: to the block, before anything is done.
Operand Lowering::callClosure(FunctionID function, CellPointer closure, ValueID thisValue, const Arguments& arguments, const Type& result, LBasicBlock otherwise)
{
    auto& callee = m_program.at(function);
    auto& undefinedType = m_program.at(m_program.types.undefined);
    Type arrayType = Type::engineClass(m_builtins.arrayClass());
    Vector<Argument, 8> all;
    // What the general entry would coerce is for the general entry, also where it would pass for the type as it is.
    auto pass = [&](ValueID id, CastReason reason) {
        auto& type = typePassedAs(callee, all.size());
        SetForScope isCoerced(m_foreignObjectsDoNotFit, reason == CastReason::As);
        all.append(Argument { fitOrElse(get(id), typeOf(id), type, otherwise), &type });
    };
    if (callee.environment)
        all.append(Argument { scopeOfClosure(closure), &typePassedAs(callee, 0) });
    if (callee.has(Format::FunctionHasThis))
        pass(thisValue, CastReason::Store);
    bool hasRest = callee.has(Format::FunctionHasRestParameter);
    unsigned numberOfPlainParameters = callee.parameterTypes.size() - hasRest;
    for (unsigned i = 0; i < numberOfPlainParameters; ++i) {
        if (i < arguments.size())
            pass(arguments[i], callee.reasonToHoldParameter(i));
        else
            all.append(Argument { undefined(), &undefinedType });
    }
    if (hasRest) {
        Arguments rest;
        for (unsigned i = numberOfPlainParameters; i < arguments.size(); ++i)
            rest.append(arguments[i]);
        auto [count, list] = argumentBuffer(rest);
        all.append(Argument { AOT_TYPED_OPERATION(NewArray, count, list), &arrayType });
    }
    return callCompiled(function, all, result);
}

Operand Lowering::lowerCall(ValueID result, const Value& value)
{
    ValueID callee = value.child(0);
    ValueID thisValue = value.child(1);
    auto& type = typeOf(result);
    auto generally = [&] {
        return ensure(callGenerally(in<Boxed>(callee), in<Boxed>(thisValue), value.arguments), type);
    };
    // A closure of one function of the program.
    if (FunctionID expected = m_function.at(callee).predictedClosure; expected && mayBeCalledInPlaceOfItsClosure(expected, value.arguments.size())) {
        auto& functionType = m_program.at(m_program.types.function);
        return ifAsExpected({ }, repOf(type), [&](LBasicBlock otherwise) {
            Boxed boxed = in<Boxed>(callee);
            if (!typeOf(callee).isWithin(functionType, m_program))
                orElse(isCell(boxed), otherwise);
            CellPointer closure = asCell(boxed);
            orElse(isClosureOf(closure, expected), otherwise);
            return callClosure(expected, closure, thisValue, value.arguments, type, otherwise);
        }, generally);
    }
    // A function of the engine: a global one, or, `o.m(..)` being written as a read and a call, what `o` was found to have under the name.
    auto definitionOf = [&](ValueID id) -> const Value& {
        while (m_function.at(id).opcode == Opcode::Cast)
            id = m_function.at(id).child(0);
        return m_function.at(id);
    };
    if (BuiltinName global = globalDefinedBy(callee)) {
        if (auto builtin = expectedBuiltin(BuiltinKind::Function, { }, global, value.arguments))
            return lowerExpectedBuiltin(result, *builtin, std::nullopt, generally);
    } else if (auto& read = definitionOf(callee); read.opcode == Opcode::GetById && &definitionOf(read.child(0)) == &definitionOf(thisValue)) {
        if (auto builtin = expectedBuiltin(BuiltinKind::Method, thisValue, m_program.builtinName(read.string()), value.arguments))
            return lowerExpectedBuiltin(result, *builtin, in<Boxed>(callee), generally);
    }
    return generally();
}

Operand Lowering::lowerConstruct(ValueID result, const Value& value)
{
    Boxed callee = in<Boxed>(value.child(0));
    auto [count, list] = argumentBuffer(value.arguments);
    // What comes of it is an object, whatever was constructed.
    return ensure(AOT_TYPED_OPERATION(Construct, callee, count, list), m_program.at(m_program.types.object), typeOf(result));
}

Operand Lowering::lowerConstructWith(ValueID result, const Value& value)
{
    Boxed callee = in<Boxed>(value.arguments[0]);
    CellPointer object = in<CellPointer>(value.arguments[1]);
    Arguments passed;
    for (unsigned i = 2; i < value.arguments.size(); ++i)
        passed.append(value.arguments[i]);
    auto [count, list] = argumentBuffer(passed);
    return ensure(AOT_TYPED_OPERATION(ConstructWith, callee, object, count, list), m_program.at(m_program.types.object), typeOf(result));
}

// The methods of a class that is laid out cannot be replaced, but for those the program is seen to replace, which are left to the call by
// name. An object of the class can be given a property of the name of any other only if that is the method, bound to the object.
std::optional<Lowering::MethodTargets> Lowering::methodTargets(LayoutID view, StringID name) const
{
    static constexpr unsigned maxTargets = 32;
    static constexpr unsigned maxShapes = 256;
    auto& members = m_program.at(view).members;
    if (members.isEmpty() || members.size() > maxShapes)
        return std::nullopt;
    bool viewHasField = !!m_program.at(view).fieldNamed(name);
    MethodTargets targets;
    for (LayoutID member : members) {
        FunctionID function;
        if (m_program.at(member).fieldNamed(name)) {
            if (!viewHasField)
                return std::nullopt;
        } else {
            if (m_program.at(member).mayHaveOtherPrototype)
                return std::nullopt;
            for (LayoutID ancestor = member; ancestor && !function; ancestor = m_program.at(ancestor).base) {
                if (m_program.at(ancestor).hasMethodOfUnknownName)
                    return std::nullopt;
                for (auto& method : m_program.at(ancestor).methods) {
                    if (method.name != name)
                        continue;
                    if (method.kind != MethodKind::Method || method.mayBeReplaced)
                        return std::nullopt;
                    function = method.function;
                }
            }
            if (!function || m_program.at(function).environment || m_program.at(function).blocks.isEmpty())
                return std::nullopt;
        }
        size_t at = targets.findIf([&](auto& target) {
            return target.function == function;
        });
        if (at == notFound) {
            if (targets.size() == maxTargets)
                return std::nullopt;
            at = targets.size();
            targets.append({ function, { } });
        }
        targets[at].shapes.append(m_program.at(member).shape);
    }
    return targets;
}

// What is passed for a parameter, held to its type as the general entry of the function holds it: an annotation coerces. So a call does
// the same whichever way it gets to the function.
Lowering::Argument Lowering::argumentFor(const Function& callee, unsigned parameter, ValueID id)
{
    TypeID declared = callee.parameterTypes[parameter];
    if (!declared || callee.reasonToHoldParameter(parameter) != CastReason::As)
        return argument(id);
    Value cast;
    cast.opcode = Opcode::Cast;
    cast.type = declared;
    cast.rep = repOf(m_program.at(declared));
    cast.position = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).position;
    cast.children[0] = id;
    cast.immediates[1] = static_cast<uint32_t>(CastReason::As);
    return { coerce(get(id), typeOf(id), cast), &m_program.at(declared) };
}

Operand Lowering::callMethod(FunctionID function, CellPointer thisObject, const Arguments& arguments, const Type& result)
{
    auto& callee = m_program.at(function);
    auto& undefinedType = m_program.at(m_program.types.undefined);
    Type arrayType = Type::engineClass(m_builtins.arrayClass());
    Vector<Argument, 8> all;
    // The object is of a class that has the method, so it is what the method takes `this` as.
    if (callee.has(Format::FunctionHasThis))
        all.append({ thisObject, &typePassedAs(callee, 0) });
    bool hasRest = callee.has(Format::FunctionHasRestParameter);
    unsigned numberOfPlainParameters = callee.parameterTypes.size() - hasRest;
    for (unsigned i = 0; i < numberOfPlainParameters; ++i)
        all.append(i < arguments.size() ? argumentFor(callee, i, arguments[i]) : Argument { undefined(), &undefinedType });
    if (hasRest) {
        Arguments rest;
        for (unsigned i = numberOfPlainParameters; i < arguments.size(); ++i)
            rest.append(arguments[i]);
        auto [count, list] = argumentBuffer(rest);
        all.append({ AOT_TYPED_OPERATION(NewArray, count, list), &arrayType });
    }
    return callCompiled(function, all, result);
}

// What resolveCalls() has left.
Operand Lowering::lowerCallMethod(ValueID result, const Value& value)
{
    ValueID object = value.child(0);
    StringID name = value.string();
    auto& receiver = typeOf(object);
    auto& type = typeOf(result);
    LayoutID view = receiver.onlyView();
    auto targets = view ? methodTargets(view, name) : std::nullopt;
    if (!targets) {
        auto generally = [&] {
            return ensure(callMethodGenerally(in<Boxed>(object), name, value.arguments), type);
        };
        if (auto builtin = expectedBuiltin(BuiltinKind::Method, object, m_program.builtinName(name), value.arguments))
            return lowerExpectedBuiltin(result, *builtin, std::nullopt, generally);
        return generally();
    }

    CellPointer cell = nonNil(object, name);
    return ifOfMember(cell, object, view, repOf(type), [&] {
        return callMethodOfMember(cell, receiver, *targets, value, type);
    }, [&] {
        return ensure(callMethodGenerally(box(cell), name, value.arguments), type);
    });
}

// The object is of a member of the view the receiver is looked at as.
Operand Lowering::callMethodOfMember(CellPointer cell, const Type& receiver, const MethodTargets& targets, const Value& value, const Type& type)
{
    auto call = [&](const MethodTarget& target) {
        if (target.function)
            return callMethod(target.function, cell, value.arguments, type);
        // A function that is kept in a field is read as the field is, and called as any value is.
        Boxed callee = readField(cell, *fieldFor(receiver, value.string()), m_program.at(m_program.types.anything)).as<Boxed>();
        return ensure(callGenerally(callee, box(cell), value.arguments), type);
    };
    if (targets.size() == 1)
        return call(targets.first());

    // By shape. The object is of a member of the view, so the function most of them have needs no case.
    unsigned usual = 0;
    for (unsigned i = 1; i < targets.size(); ++i) {
        if (targets.at(i).shapes.size() > targets.at(usual).shapes.size())
            usual = i;
    }
    Vector<LBasicBlock, 4> blocks;
    Vector<FTL::SwitchCase> cases;
    for (unsigned i = 0; i < targets.size(); ++i) {
        blocks.append(m_out.newBlock());
        if (i == usual)
            continue;
        for (uint32_t shape : targets.at(i).shapes)
            cases.append(FTL::SwitchCase(m_out.constInt32(static_cast<int32_t>(Typed::shapeWordOf(shape))), blocks[i], FTL::Weight()));
    }
    LBasicBlock continuation = m_out.newBlock();
    m_out.switchInstruction(shapeWordOf(cell).value(), cases, blocks[usual], FTL::Weight());
    Rep rep = repOf(type);
    Vector<ValueFromBlock, 4> results;
    for (unsigned i = 0; i < targets.size(); ++i) {
        m_out.appendTo(blocks[i]);
        Operand yielded = call(targets.at(i));
        RELEASE_ASSERT(yielded.rep == rep);
        results.append(m_out.anchor(yielded.value));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
    return Operand(rep, m_out.phi(b3TypeOf(rep), results));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
