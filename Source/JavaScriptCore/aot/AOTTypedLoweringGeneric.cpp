/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationsObjects.h"
#include "B3StackmapGenerationParams.h"
#include "B3VariableValue.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

// Values of which nothing is known go through the stubs of code compiled from bytecode. A cache is a slot in a block the function gets once it has missed often enough; until then it uses one all functions share, whose slots stay empty.

// The block as of now: the function may get its own while it runs. One that was found earlier stays valid.
LValue Lowering::loadICData()
{
    PatchpointValue* number = m_out.patchpoint(Int64);
    number->effects = Effects::none();
    // Any operation may give the function slots of its own.
    m_heaps.decoratePatchpointRead(&m_heaps.AOTTyped_collectorState, number);
    number->setGenerator([indexReferences = &m_context.indexReferences](CCallHelpers& jit, const StackmapGenerationParams& params) {
        indexReferences->load16(jit, instanceGPR, params[0].gpr(), Instance::offsetOfStates(), sizeof(uint16_t));
    });
    return m_out.loadPtr(TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(m_instance, m_out.shl(number, m_out.constInt32(3)))));
}

// Nothing: the function has as many as it can have, the name has no number, or the code runs once.
std::optional<unsigned> Lowering::allocateSite(StringID name, unsigned extra)
{
    if (m_program.runsOnce(m_id) && !m_isInLoop)
        return std::nullopt;
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

// In the prologue and at every LoopHint: an activation that stays in a loop sees the block the function gets while it runs an iteration later. Until then it misses, which is all that comes of an old address.
void Lowering::findICData()
{
    if (m_icDataAsOfLastHint)
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), m_icDataAsOfLastHint, loadICData());
}

// A hit addresses the slot from the block. The sum is for a stub: where that is called off the usual path the sum is made there, of what the back end does not know to be the same address.
Pointer Lowering::slotAddress(unsigned slot, SlotAddressIsFor use)
{
    LValue data;
    if (m_icDataAsOfLastHint)
        data = m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), m_icDataAsOfLastHint);
    else {
        if (m_blockOfICData != m_out.m_block) {
            m_icData = loadICData();
            m_blockOfICData = m_out.m_block;
        }
        data = m_icData;
    }
    if (use == SlotAddressIsFor::ColdBlock)
        data = m_out.opaque(data);
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

Boxed Lowering::emitGetById(Boxed base, StringID name, bool isCalled)
{
    if (name == m_length)
        return emitGetLength(base);
    if (BuiltinID method = m_builtins.callOrApply(m_program.builtinName(name)))
        return getCallOrApply(base, name, method);
    if (BuiltinID method = bindIfNamed(name))
        return getCallOrApply(base, name, method);
    if (auto row = bufferAccessorNamed(name))
        return getBufferAccessor(base, name, *row, isCalled);
    if (isNamedLastIndex(name))
        return getLastIndex(base, name, isCalled);
    return emitCachedGetById(base, name, isCalled);
}

Boxed Lowering::emitCachedGetById(Boxed base, StringID name, bool isCalled)
{
    auto slot = allocateSite(name, 0);
    if (!slot)
        return AOT_TYPED_OPERATION(GetByName, base, word32(name.index()));
    auto throughStub = [&](SlotAddressIsFor use) {
        return Boxed(callICStub(Stub::GetById, Int64, { { base.value(), firstStubOperandGPR }, { slotAddress(*slot, use).value(), GPRInfo::argumentGPR1 } }));
    };
    // What is called is on the prototype as a rule. In TypeScript such a site is rare, and outside a loop not worth the code.
    if (isCalled || (!m_isInLoop && m_function.isTyped()))
        return throughStub(SlotAddressIsFor::UsualPath);
    Pointer address = slotAddress(*slot);

    // Where the text does not say hot: the stub, which knows both ways: a few instructions at the site.
    if (!m_isInLoop && !m_program.isHotByText(m_id))
        return throughStub(SlotAddressIsFor::UsualPath);
    // The usual hit is tested for in place, as readByName() tests for it: the structure has the number of the name at the place in the object that the slot was filled with, be it the structure the slot was filled
    // for or another. Whatever else the slot says (indirect, a getter, attempts) lies above the number, and no number is equal to that.
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock isNamedCase = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = newColdBlock();
    LBasicBlock hasNoNumber = m_isInLoop ? newColdBlock() : miss;
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(base).value(), usually(isCellCase), rarely(miss));
    m_out.appendTo(isCellCase);
    CellPointer cell = asCell(base);
    Integer word(m_out.load64(TypedPointer(m_heaps.AOTData_slotWords.atAnyIndex(), address.value())));
    LValue number = m_out.lShr(word.value(), m_out.constInt32(32 + AOT::Slot::nameIDShift));
    m_out.branch(m_out.notZero64(number), usually(isNamedCase), rarely(hasNoNumber));
    // Some slots stay without a number: the property lies past the places that structures keep numbers for, the object is a prototype or a dictionary. In a loop: the structure the slot was filled for. Elsewhere the stub tests for it.
    std::optional<ValueFromBlock> fastWithoutNumber;
    if (m_isInLoop) {
        LBasicBlock hitWithoutNumber = newColdBlock();
        m_out.appendTo(hasNoNumber);
        Word32 hasStructure = equal(low32(word), loadField<Word32>(cell, m_heaps.JSCell_structureID));
        Word32 isInObject = isZero(Integer(m_out.bitAnd(word.value(), m_out.constInt64(static_cast<int64_t>(AOT::Slot::isIndirect) << 32))));
        m_out.branch(logicalAnd(hasStructure, isInObject).value(), unsure(hitWithoutNumber), unsure(miss));
        m_out.appendTo(hitWithoutNumber);
        LValue locationWithoutNumber = m_out.bitAnd(m_out.lShr(word.value(), m_out.constInt32(32)), m_out.constInt64(AOT::Slot::directLocationMask));
        fastWithoutNumber = m_out.anchor(m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.shl(locationWithoutNumber, m_out.constInt32(3))))));
        m_out.jump(continuation);
    }
    m_out.appendTo(isNamedCase);
    LValue location = m_out.bitAnd(m_out.lShr(word.value(), m_out.constInt32(32)), m_out.constInt64(AOT::Slot::directLocationMask));
    LValue numbers = m_out.add(fixedPointer(Instance::offsetOfFieldIDAtDirectLocationBase()), m_out.zeroExtPtr(loadField<Word32>(cell, m_heaps.JSCell_structureID).value()));
    LValue found = m_out.load16ZeroExt32(TypedPointer(m_heaps.root, m_out.add(numbers, m_out.shl(location, m_out.constInt32(1)))));
    m_out.branch(m_out.equal(m_out.zeroExtPtr(found), number), usually(hit), rarely(miss));
    m_out.appendTo(hit);
    // It may be a field of an object of the program, so it is a read of any memory.
    ValueFromBlock fast = m_out.anchor(m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.shl(location, m_out.constInt32(3))))));
    m_out.jump(continuation);
    ValueFromBlock slow;
    inColdBlock(miss, [&] {
        slow = m_out.anchor(throughStub(SlotAddressIsFor::ColdBlock).value());
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
    if (fastWithoutNumber)
        return Boxed(m_out.phi(Int64, fast, *fastWithoutNumber, slow));
    return Boxed(m_out.phi(Int64, fast, slow));
}

void Lowering::emitPutById(Boxed base, StringID name, Boxed value)
{
    if (isNamedLastIndex(name))
        return putLastIndex(base, name, value);
    emitCachedPutById(base, name, value);
}

void Lowering::emitCachedPutById(Boxed base, StringID name, Boxed value)
{
    constexpr unsigned isStrict = 2;
    auto slot = allocateSite(name, isStrict);
    if (!slot) {
        AOT_TYPED_OPERATION(SetByName, base, word32(name.index()), value);
        return;
    }
    auto throughStub = [&](SlotAddressIsFor use) {
        callICStub(Stub::PutById, Void, { { base.value(), firstStubOperandGPR }, { value.value(), GPRInfo::argumentGPR1 }, { slotAddress(*slot, use).value(), GPRInfo::argumentGPR2 } });
    };
    if (!m_isInLoop && m_function.isTyped()) {
        throughStub(SlotAddressIsFor::UsualPath);
        return;
    }
    Pointer address = slotAddress(*slot);

    // The usual hit is tested for in place, as generatePutById() tests for it: the object has the structure the slot was filled for and keeps it, nothing is asked of what is
    // stored, and the property is in the object itself.
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(base).value(), usually(isCellCase), rarely(miss));
    m_out.appendTo(isCellCase);
    CellPointer cell = asCell(base);
    // The slot and the structure ID are read as any memory: whatever is written, by whatever call, they are read again behind it. Only Writes::Heap is said to reach their own heaps, and a collection
    // clears slots, a coercion gives an object another structure.
    Integer word(m_out.load64(TypedPointer(m_heaps.root, address.value())));
    static_assert(OBJECT_OFFSETOF(AOT::Slot, newStructureID) == sizeof(uint64_t) && OBJECT_OFFSETOF(AOT::Slot, fieldType) == sizeof(uint64_t) + sizeof(uint32_t));
    Integer newStructureAndFieldType(m_out.load64(TypedPointer(m_heaps.root, m_out.add(address.value(), m_out.constIntPtr(sizeof(uint64_t))))));
    Word32 hasStructure = equal(low32(word), Word32(m_out.load32(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(JSCell::structureIDOffset()))))));
    constexpr uint64_t offsetIsNegative = 1ull << (32 + AOT::Slot::offsetBits - 1);
    Word32 isInObject = isZero(Integer(m_out.bitAnd(word.value(), m_out.constInt64(static_cast<int64_t>(offsetIsNegative)))));
    m_out.branch(logicalAnd(logicalAnd(hasStructure, isZero(newStructureAndFieldType)), isInObject).value(), usually(hit), rarely(miss));
    m_out.appendTo(hit);
    LValue location = m_out.bitAnd(m_out.lShr(word.value(), m_out.constInt32(32)), m_out.constInt64(AOT::Slot::offsetMask));
    m_out.store64(value.value(), TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.shl(location, m_out.constInt32(3)))));
    LBasicBlock storesCell = m_out.newBlock();
    m_out.branch(isCell(value).value(), unsure(storesCell), unsure(continuation));
    m_out.appendTo(storesCell);
    barrier(cell);
    m_out.jump(continuation);
    inColdBlock(miss, [&] {
        throughStub(SlotAddressIsFor::ColdBlock);
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
}

// Nothing: the function has as many sites as it can have.
bool Lowering::emitDeleteByVal(Boxed base, Boxed key)
{
    if (m_context.sites.size() + 2 > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return false;
    m_context.sites.append(Site { });
    m_context.sites.append(Site { });
    callICStub(Stub::DeleteByValCached, Void, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 }, { slotAddress(m_context.sites.size() - 2).value(), GPRInfo::argumentGPR2 } });
    return true;
}

bool Lowering::emitDefineByName(Boxed object, StringID name, Boxed value)
{
    auto text = m_program.string(name);
    bool mayBeIndex = !text.empty() && std::ranges::all_of(text, [](uint8_t character) {
        return isASCIIDigit(character);
    });
    constexpr unsigned isDirectAndStrict = 3;
    auto slot = mayBeIndex ? std::nullopt : allocateSite(name, isDirectAndStrict);
    if (!slot)
        return false;
    callICStub(Stub::PutById, Void, { { object.value(), firstStubOperandGPR }, { value.value(), GPRInfo::argumentGPR1 }, { slotAddress(*slot).value(), GPRInfo::argumentGPR2 } });
    return true;
}

Boxed Lowering::emitGetByVal(Boxed base, Boxed key, bool mayBeName)
{
    return Boxed(callICStub(mayBeName ? Stub::GetByValCached : Stub::GetByVal, Int64, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 } }));
}

void Lowering::emitPutByVal(Boxed base, Boxed key, Boxed value, bool mayBeName)
{
    if (mayBeName) {
        callICStub(Stub::PutByValCached, Void, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 }, { value.value(), GPRInfo::argumentGPR2 } });
        return;
    }
    constexpr uint32_t isStrict = 1;
    callICStub(Stub::PutByVal, Void, { { base.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 }, { value.value(), GPRInfo::argumentGPR2 } }, std::pair { GPRInfo::argumentGPR3, isStrict });
}

static Stub behindInt32Test(Stub stub)
{
    switch (stub) {
    case Stub::Add:
        return Stub::AddBehindInt32Test;
    case Stub::Sub:
        return Stub::SubBehindInt32Test;
    case Stub::Mul:
        return Stub::MulBehindInt32Test;
    case Stub::Div:
        return Stub::DivBehindInt32Test;
    case Stub::BitAnd:
        return Stub::BitAndBehindInt32Test;
    case Stub::BitOr:
        return Stub::BitOrBehindInt32Test;
    case Stub::BitXor:
        return Stub::BitXorBehindInt32Test;
    case Stub::LShift:
        return Stub::LShiftBehindInt32Test;
    case Stub::RShift:
        return Stub::RShiftBehindInt32Test;
    case Stub::URShift:
        return Stub::URShiftBehindInt32Test;
    case Stub::Less:
        return Stub::LessBehindInt32Test;
    case Stub::LessEq:
        return Stub::LessEqBehindInt32Test;
    case Stub::Greater:
        return Stub::GreaterBehindInt32Test;
    case Stub::GreaterEq:
        return Stub::GreaterEqBehindInt32Test;
    default:
        return stub;
    }
}

// The stubs do numbers themselves and call the engine for the rest.
Boxed Lowering::emitGenericArithmetic(Stub stub, Boxed a, Boxed b, OperationEffects effects)
{
    if (m_hasFailedInt32Test)
        stub = behindInt32Test(stub);
    return Boxed(callICStub(stub, Int64, { { a.value(), firstStubOperandGPR }, { b.value(), GPRInfo::argumentGPR1 } }, std::nullopt, effects));
}

Word32 Lowering::emitGenericCompare(Stub stub, Boxed a, Boxed b, OperationEffects effects)
{
    if (m_hasFailedInt32Test)
        stub = behindInt32Test(stub);
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
        if (expectation.hasOriginalStructure && type.onlyEngineClass()) {
            orElse(isCell(boxed), otherwise);
            orElse(hasOriginalStructure(asCell(boxed), *type.onlyEngineClass()), otherwise);
            return asCell(boxed);
        }
    }
    Operand fitted = fitOrElse(operand, typeOf(expectation.value), type, otherwise);
    if (expectation.hasOriginalStructure && type.onlyEngineClass())
        orElse(hasOriginalStructure(fitted.as<CellPointer>(), *type.onlyEngineClass()), otherwise);
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
