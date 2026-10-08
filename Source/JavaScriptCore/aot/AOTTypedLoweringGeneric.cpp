/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltins.h"
#include "AOTOperationsObjects.h"
#include "B3StackmapGenerationParams.h"
#include "B3VariableValue.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

// Values of which nothing is known go through the stubs of code compiled from bytecode. A cache is a slot in a block the function gets once it has missed often enough; until then it uses one all functions share, whose slots stay empty.

// The block as of now. It is looked for on entry and once around every loop, since the function may get its own while it runs.
void Lowering::loadICData()
{
    PatchpointValue* number = m_out.patchpoint(Int64);
    number->effects = Effects::none();
    m_heaps.decoratePatchpointRead(&m_heaps.AOTInstance_mutableFields, number);
    number->setGenerator([indexReferences = &m_context.indexReferences](CCallHelpers& jit, const StackmapGenerationParams& params) {
        indexReferences->load16(jit, instanceGPR, params[0].gpr(), Instance::offsetOfStates(), sizeof(uint32_t));
    });
    LValue data = m_out.loadPtr(TypedPointer(m_heaps.AOTInstance_mutableFields, m_out.add(m_instance, m_out.shl(number, m_out.constInt32(3)))));
    m_out.m_block->appendNew<VariableValue>(m_proc, Set, m_out.origin(), m_data, data);
}

// Nothing: the function has as many as it can have, or the name has no number.
std::optional<unsigned> Lowering::allocateSite(StringID name, unsigned extra)
{
    uint32_t identifier = name.index() < m_program.identifierNumbers.size() ? m_program.identifierNumbers[name.index()] : 0;
    if (!identifier || !Site::fits(identifier, extra) || m_context.sites.size() >= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return std::nullopt;
    Site site { };
    site.identifierAndExtra = identifier | extra << Site::identifierBits;
    m_context.sites.append(site);
    if (!m_context.identifiers.contains(name))
        m_context.identifiers.append(name);
    return m_context.sites.size() - 1;
}

Pointer Lowering::slotAddress(unsigned slot)
{
    LValue data = m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), m_data);
    return Pointer(m_out.add(data, m_out.constIntPtr(AOT::Data::offsetOfSlots() + slot * sizeof(AOT::Slot))));
}

LValue Lowering::callICStub(Stub stub, LType type, const Vector<std::pair<LValue, GPRReg>, 4>& operands, std::optional<std::pair<GPRReg, uint32_t>> immediate, OperationEffects effects)
{
    m_context.emitsCalls = true;
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    applyEffects(patchpoint, effects);
    for (auto& [value, reg] : operands)
        patchpoint->append(ConstrainedValue(value, ValueRep::reg(reg)));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
#if CPU(X86_64)
    for (GPRReg reg : stubTemporaryGPRs)
        patchpoint->clobberLate(RegisterSet { reg });
#endif
    if (type != Void)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, stub, immediate, site](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (immediate)
            jit.move(CCallHelpers::TrustedImm32(immediate->second), immediate->first);
        stubCalls->call(jit, stub, site);
    });
    return patchpoint;
}

// The length of an array, a string or a typed array is no property that a slot could say the place of.
Boxed Lowering::emitGetLength(Boxed base)
{
    if (m_context.sites.size() >= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return AOT_TYPED_OPERATION(GetByName, base, word32(m_length.index()));
    Site site { };
    site.identifierAndExtra = static_cast<uint32_t>(WellKnownIdentifier::Length);
    m_context.sites.append(site);
    Pointer address = slotAddress(m_context.sites.size() - 1);
    return Boxed(callICStub(Stub::GetLength, Int64, { { base.value(), firstStubOperandGPR }, { address.value(), GPRInfo::argumentGPR1 } }));
}

Boxed Lowering::emitGetById(Boxed base, StringID name)
{
    if (name == m_length)
        return emitGetLength(base);
    auto slot = allocateSite(name, 0);
    if (!slot)
        return AOT_TYPED_OPERATION(GetByName, base, word32(name.index()));
    Pointer address = slotAddress(*slot);
    auto throughStub = [&] {
        return Boxed(callICStub(Stub::GetById, Int64, { { base.value(), firstStubOperandGPR }, { address.value(), GPRInfo::argumentGPR1 } }));
    };
    if (!m_isInLoop)
        return throughStub();

    // In a loop the usual hit is tested for in place: the object has the structure the slot was filled for, and the property is in the object itself.
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(base).value(), usually(isCellCase), rarely(miss));
    m_out.appendTo(isCellCase);
    CellPointer cell = asCell(base);
    Integer word(m_out.load64(TypedPointer(m_heaps.AOTData_slotWords.atAnyIndex(), address.value())));
    Word32 hasStructure = equal(low32(word), loadField<Word32>(cell, m_heaps.JSCell_structureID));
    Word32 isInObject = isZero(Integer(m_out.bitAnd(word.value(), m_out.constInt64(static_cast<int64_t>(AOT::Slot::isIndirect) << 32))));
    m_out.branch(logicalAnd(hasStructure, isInObject).value(), usually(hit), rarely(miss));
    m_out.appendTo(hit);
    // It may be a field of an object of the program, so it is a read of any memory.
    LValue location = m_out.bitAnd(m_out.lShr(word.value(), m_out.constInt32(32)), m_out.constInt64(AOT::Slot::directLocationMask));
    ValueFromBlock fast = m_out.anchor(m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.shl(location, m_out.constInt32(3))))));
    m_out.jump(continuation);
    ValueFromBlock slow;
    inColdBlock(miss, [&] {
        slow = m_out.anchor(throughStub().value());
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
    return Boxed(m_out.phi(Int64, fast, slow));
}

void Lowering::emitPutById(Boxed base, StringID name, Boxed value)
{
    constexpr unsigned isStrict = 2;
    auto slot = allocateSite(name, isStrict);
    if (!slot) {
        AOT_TYPED_OPERATION(SetByName, base, word32(name.index()), value);
        return;
    }
    Pointer address = slotAddress(*slot);
    callICStub(Stub::PutById, Void, { { base.value(), firstStubOperandGPR }, { value.value(), GPRInfo::argumentGPR1 }, { address.value(), GPRInfo::argumentGPR2 } });
}

Boxed Lowering::emitGetByVal(Boxed base, Boxed key)
{
    return Boxed(callICStub(Stub::GetByVal, Int64, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 } }));
}

void Lowering::emitPutByVal(Boxed base, Boxed key, Boxed value)
{
    constexpr uint32_t isStrict = 1;
    callICStub(Stub::PutByVal, Void, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 }, { value.value(), GPRInfo::argumentGPR2 } }, std::pair { GPRInfo::argumentGPR3, isStrict });
}

// The stubs do numbers themselves and call the engine for the rest.
Boxed Lowering::emitGenericArithmetic(Stub stub, Boxed a, Boxed b, OperationEffects effects)
{
    return Boxed(callICStub(stub, Int64, { { a.value(), firstStubOperandGPR }, { b.value(), GPRInfo::argumentGPR1 } }, std::nullopt, effects));
}

Word32 Lowering::emitGenericCompare(Stub stub, Boxed a, Boxed b, OperationEffects effects)
{
    return Word32(callICStub(stub, Int32, { { a.value(), firstStubOperandGPR }, { b.value(), GPRInfo::argumentGPR1 } }, std::nullopt, effects));
}

// Predictions.
//
// In code written in JavaScript every value is anything, and the checker says what it expects it to be: Value::prediction. That may be
// wrong, so nothing is taken from it but the order in which things are tried. An operation that would be carried out the general way
// looks at what is expected of its own operands and tests for that. Where the tests pass the operands ARE of those types, and the
// operation is lowered the way it is for operands whose types say so. Where one fails, the general way is taken. Then both go on together.
//
// What is done in place is what the language says, in every case that gets there. The lowering for typed operands has cases where it
// goes by a rule of its own (a hole in an array, an accessor among the elements, a field that is absent, a method that is not looked up
// in the receiver). None of those is reached from here: each such function takes a block to go to instead, which is the general way.
//
// To the summaries and to whoever asks what may throw, the operation is what it is without predictions. What is emitted in place
// changes no more than the general way may, and tells the back end what it changes itself.

// Without the primitives: `undefined` and `null` pass for a reference in every description, and what is tested for is the rest.
// Nothing: nothing is expected, a test has settled it where this code runs, or the type says as much.
std::optional<Type> Lowering::predictionOf(ValueID id, uint16_t without) const
{
    for (auto& assumption : m_assumptions) {
        if (assumption.value == id)
            return std::nullopt;
    }
    TypeID prediction = m_function.at(id).prediction;
    if (!prediction)
        return std::nullopt;
    Type expected = m_program.at(prediction).withoutPrimitives(without);
    // Which integers are expected is more than there is to test for: whether the value is boxed as an int32, or is a number at all.
    if (expected.primitives & Primitive::Int && !(expected.primitives & Primitive::Double)) {
        if (!expected.integers.isWithin(Range::int32()))
            expected.primitives |= Primitive::Double;
        expected.integers = expected.primitives & Primitive::Double ? Range::full() : Range::int32();
    }
    auto& type = typeOf(id);
    if (expected.isNever() || !expected.isWithin(type, m_program) || type.isWithin(expected, m_program))
        return std::nullopt;
    return expected;
}

// What a way to carry out an operation is chosen by.
Type Lowering::expectedTypeOf(ValueID id) const
{
    if (auto prediction = predictionOf(id))
        return *prediction;
    return typeOf(id);
}

bool Lowering::canTellWhetherAsMade(const EngineClass& engineClass)
{
    return engineClass.type == ArrayType || engineClass.type == JSMapType || engineClass.type == JSSetType;
}

// Whether the cell is an instance of the class with the structure the engine gives them: it has no property of its own that the others
// lack, and the prototype of the class, whose properties cannot be replaced. So under a name of the class it finds what the class has.
Word32 Lowering::isAsMade(CellPointer cell, const EngineClass& engineClass)
{
    if (engineClass.type == ArrayType)
        return Word32(isOriginalArray(cell.value()));
    RELEASE_ASSERT(canTellWhetherAsMade(engineClass));
    Receiver receiver = engineClass.type == JSMapType ? Receiver::Map : Receiver::Set;
    return equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), Word32(fixed32(Instance::offsetOfReceiverStructureIDs() + static_cast<unsigned>(receiver) * sizeof(uint32_t))));
}

// Goes on where the value is what is expected, and yields it in the representation of that type.
Operand Lowering::expectOrElse(const Expectation& expectation, LBasicBlock otherwise)
{
    Operand operand = get(expectation.value);
    auto& type = *expectation.type;
    if (operand.rep == Rep::JSValue) {
        Boxed boxed = operand.as<Boxed>();
        // The engine boxes an int32 as one. An integral number that it keeps as a double is for whoever takes numbers.
        if (type.isInteger() && type.integers == Range::int32()) {
            orElse(isInt32(boxed), otherwise);
            return signExtended(low32(boxed));
        }
        // Only an instance has that structure.
        if (expectation.isAsMade && type.onlyEngineClass()) {
            orElse(isCell(boxed), otherwise);
            orElse(isAsMade(asCell(boxed), *type.onlyEngineClass()), otherwise);
            return asCell(boxed);
        }
    }
    Operand fitted = fitOrElse(operand, typeOf(expectation.value), type, otherwise);
    if (expectation.isAsMade && type.onlyEngineClass())
        orElse(isAsMade(fitted.as<CellPointer>(), *type.onlyEngineClass()), otherwise);
    return fitted;
}

// Whether the operation is only carried out the general way because of operands that are expected to be numbers.
bool Lowering::areExpectedToBeNumbers(std::initializer_list<ValueID> operands) const
{
    bool isExpected = false;
    for (ValueID id : operands) {
        if (isNumericOperand(typeOf(id)))
            continue;
        auto prediction = predictionOf(id);
        if (!prediction || !prediction->isNumber())
            return false;
        isExpected = true;
    }
    return isExpected;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
