/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArgumentsEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTCallEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConvention.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTEntryEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubsCallLists.h"
#include "AirFrameEmission.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "Interpreter.h"
#include <iterator>

namespace JSC { namespace AOT { namespace Typed {

Word32 ArgumentsEmitter::restLength(uint32_t firstArgument)
{
    Word32 count = emitter<EntryEmitter>().argumentCount();
    if (!firstArgument)
        return count;
    Word32 skipped = word32(static_cast<int32_t>(firstArgument));
    Condition hasRest = below(skipped, count);
    return select(hasRest, sub(count, skipped), 0);
}

Pointer ArgumentsEmitter::restList(uint32_t firstArgument)
{
    EntryEmitter& entry = emitter<EntryEmitter>();
    RELEASE_ASSERT(!entry.indexOfFirstArgumentInList());
    return offsetBy(entry.argumentList(), static_cast<ptrdiff_t>(firstArgument) * sizeOfStackWord);
}

void ArgumentsEmitter::generateGetArgument(const ValueContext& value)
{
    uint32_t index = value->argumentIndex();
    Pointer list = restList(0);
    Word32 position = word32(static_cast<int32_t>(index));
    define(value, either<Boxed>(below(position, emitter<EntryEmitter>().argumentCount()), [&] {
        return Boxed(m_out.load64(m_out.address(m_heaps.argumentBuffer, list.value(), static_cast<ptrdiff_t>(index) * sizeOfStackWord)));
    }, [&] {
        return undefined();
    }));
}

void ArgumentsEmitter::generateGetRestLength(const ValueContext& value)
{
    define(value, restLength(value->firstArgument()));
}

void ArgumentsEmitter::generateCreateRest(const ValueContext& value)
{
    uint32_t firstArgument = value->firstArgument();
    CellAllocationEmitter& cells = emitter<CellAllocationEmitter>();
    if (value->storage() == AllocationStorage::Heap) {
        Word32 length = restLength(firstArgument);
        define(value, cells.newArrayFromBuffer(value, length, restList(firstArgument)));
        return;
    }
    Word32 length = restLength(firstArgument);
    define(value, cells.newArrayInFrameFromBuffer(value, length, restList(firstArgument), static_cast<unsigned>(build().limit(LimitID::MaxFrameArrayLength))));
}

Integer ArgumentsEmitter::indexOfBoxedKey(Boxed key, LBasicBlock isNotIndex)
{
    LBasicBlock isNotInt32 = newBlock();
    Join<Integer> join(m_out);
    branchUnless(isInt32(key), isNotInt32);
    join.add(signExtended(unboxInt32(key)));

    m_out.appendTo(isNotInt32);
    branchUnless(isNumber(key), isNotIndex);
    Number number = unboxDouble(key);
    Integer index = emitter<ConversionEmitter>().toInteger(number);
    branchUnless(equal(toNumber(index), number), isNotIndex);
    join.add(index);
    return join.finish();
}

Boxed ArgumentsEmitter::readFromMaterializedList(const ValueContext& value, Boxed key)
{
    uint32_t firstArgument = value->firstArgument();
    CellPointer array = callOperation<CellPointer>(value, RuntimeOperation::NewArray, { restLength(firstArgument), restList(firstArgument) }, { .isOnRarePath = true });
    if (value->readsArgumentsObject())
        return callOperation<Boxed>(value, RuntimeOperation::GetFromArguments, { array, key }, { .isOnRarePath = true });
    return emitter<AccessEmitter>().getByVal(value, boxCell(array), key);
}

void ArgumentsEmitter::generateGetMyArgumentByVal(const ValueContext& value)
{
    uint32_t firstArgument = value->firstArgument();
    ValueID key = value->propertyOperand();
    Representation keyRepresentation = representationOf(key);
    if (!canBeKeyOf(keyRepresentation)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    const Type& keyType = typeOf(key);
    bool keyIsNumber = keyType.isNumber();
    LBasicBlock outOfBounds = newBlock();
    LBasicBlock slowPath = keyIsNumber ? nullptr : newBlock();
    LBasicBlock isNotIndex = keyIsNumber ? outOfBounds : slowPath;
    Join<Boxed> join(m_out);

    Integer index = keyRepresentation == Representation::JSValue
        ? indexOfBoxedKey(in<Boxed>(key), isNotIndex)
        : emitter<ElementAccessEmitter>().indexOrElse(key, keyType.isNumberWithoutFraction(), isNotIndex);
    if (index.value()->hasInt64() && static_cast<uint64_t>(index.value()->asInt64()) > maxArguments)
        m_out.jump(outOfBounds);
    else {
        branchUnless(below(index, zeroExtended(restLength(firstArgument))), outOfBounds);
        join.add(Boxed(m_out.load64(m_out.baseIndex(m_heaps.argumentBuffer, restList(firstArgument).value(), index.value(), FTL::ScaleEight))));
    }

    m_out.appendTo(outOfBounds);
    join.add(undefined());

    if (slowPath) {
        m_out.appendTo(slowPath);
        join.add(readFromMaterializedList(value, in<Boxed>(key)));
    }
    define(value, join.finish());
}

void ArgumentsEmitter::emitTailCallWithList(Boxed callee, Boxed thisValue, Word32 count, Pointer list)
{
    const StubSignature& signature = inputs().stubCatalog.summaryOf(tailCallWithList()).signature;
    LValue operands[] = { callee.value(), thisValue.value(), zeroExtended(count).value(), list.value(), m_out.constInt64(sizeOfSpilledArgumentRegisters) };
    RELEASE_ASSERT(!signature.usesCallConvention && signature.numberOfOperands == std::size(operands));

    emitter<CellAllocationEmitter>().leaveFrameOfCheckedCells();
    B3::PatchpointValue* jump = m_out.patchpoint(B3::Void);
    for (size_t i = 0; i < std::size(operands); ++i)
        jump->append(B3::ConstrainedValue(operands[i], B3::ValueRep::reg(signature.operands[i])));
    jump->clobber(RegisterSet::macroClobberedGPRs());
    jump->effects.terminal = true;
    jump->setNeedsFrameRecord(true);
    jump->setGenerator([records = &m_records](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        RELEASE_ASSERT(params.context().frameIsSetUp);
        B3::Air::emitCalleeSaveRestore(jit, params.code());
        records->stubCalls.tailCall(jit, tailCallWithList());
    });
}

void ArgumentsEmitter::generateCallForwardVarargs(const ValueContext& value)
{
    ValueID calleeOperand = value->calleeOperand();
    Boxed callee = in<Boxed>(calleeOperand);
    Boxed thisValue = in<Boxed>(value->thisValueOperand());
    Word32 count = restLength(value->firstArgument());
    Pointer list = restList(value->firstArgument());
    CallEmitter& calls = emitter<CallEmitter>();
    if (!value.returnCall()) {
        define(value, calls.callWithList(value, callee, thisValue, count, list));
        return;
    }

    if (!typeOf(calleeOperand).isSubsetOf(m_program.at(TypeTable::function), m_program))
        calls.requireCallableBeforeTailCall(value, callee);
    emitTailCallWithList(callee, thisValue, count, list);
    calls.didEmitTailCall();
    m_records.frame.dropsArgumentListFrames = true;
}

void ArgumentsEmitter::generateConstructSuperForwardVarargs(const ValueContext& value)
{
    Boxed parent = in<Boxed>(value->parentOperand());
    Boxed newTarget = in<Boxed>(value->newTargetOperand());
    Word32 count = restLength(value->firstArgument());
    Pointer list = restList(value->firstArgument());
    define(value, emitter<CallEmitter>().superConstructWithList(value, parent, newTarget, count, list));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
