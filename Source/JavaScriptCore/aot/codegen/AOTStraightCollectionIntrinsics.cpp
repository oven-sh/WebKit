/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "JSMap.h"
#include "JSMapIterator.h"
#include "JSSet.h"
#include "JSSetIterator.h"
#include "VM.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(OBJECT_OFFSETOF(JSMap, m_storage) == OBJECT_OFFSETOF(JSSet, m_storage));
static_assert(static_cast<unsigned>(JSMapIterator::Field::Storage) == static_cast<unsigned>(JSSetIterator::Field::Storage));
static_assert(static_cast<unsigned>(SetNextResult::HasEntry) == static_cast<unsigned>(MapNextResult::HasEntry) && static_cast<unsigned>(SetNextResult::Entry) == static_cast<unsigned>(MapNextResult::Entry) && static_cast<unsigned>(SetNextResult::Element) == static_cast<unsigned>(MapNextResult::Key));

OpcodeSet StraightEmitter::opcodesOfCollectionIntrinsics()
{
    return {
        Opcode::CollectionTable, Opcode::CollectionIteratorClosedMark, Opcode::GetCollectionIteratorTable, Opcode::PutCollectionIteratorTable,
        Opcode::IsCollectionIteratorClosedMark, Opcode::MapNext, Opcode::SetNext,
    };
}

static CCallHelpers::Address tableOfCollectionIterator(GPRReg iterator)
{
    return CCallHelpers::Address(iterator, static_cast<int32_t>(JSInternalFieldObjectImpl<>::offsetOfInternalField(static_cast<unsigned>(JSSetIterator::Field::Storage))));
}

static void loadClosedMarkOfCollectionIterators(StraightEmitter& emitter, GPRReg mark)
{
    CCallHelpers& jit = emitter.jit();
    jit.loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), mark);
    jit.loadPtr(CCallHelpers::Address(mark, VM::offsetOfOrderedHashTableSentinel()), mark);
}

static void defineTableOrNull(StraightEmitter& emitter, const BodyValue& value, GPRReg table)
{
    CCallHelpers& jit = emitter.jit();
    CCallHelpers::Jump hasTable = jit.branchTest64(CCallHelpers::NonZero, table);
    jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsNull())), table);
    hasTable.link(&jit);
    emitter.define(value, table);
}

static void generatePutCollectionIteratorTable(StraightEmitter& emitter, const BodyValue& value)
{
    CCallHelpers& jit = emitter.jit();
    GPRReg iterator = StraightEmitter::temporary(1);
    GPRReg table = StraightEmitter::resultGPR;
    emitter.load(value.objectOperand(), iterator);
    emitter.load(value.valueOperand(), table);
    CCallHelpers::Jump isTable = jit.branch64(CCallHelpers::NotEqual, table, CCallHelpers::TrustedImm64(JSValue::encode(jsNull())));
    jit.move(CCallHelpers::TrustedImm32(0), table);
    isTable.link(&jit);
    jit.store64(table, tableOfCollectionIterator(iterator));
    if (!emitter.isConstant(value.valueOperand()))
        emitter.barrierAfterStore(iterator);
    emitter.defineSameAs(value, value.valueOperand());
}

// An integer lies in its slot as an int32 or as a double. The operation reads the low half of the register, which is the int32.
static void loadEntryOfCollectionStep(StraightEmitter& emitter, ValueID operand, GPRReg entry)
{
    CCallHelpers& jit = emitter.jit();
    if (std::optional<JSValue> constant = emitter.constantOf(operand); constant && constant->isAnyInt()) {
        jit.move(CCallHelpers::TrustedImm64(constant->asAnyInt()), entry);
        return;
    }
    emitter.load(operand, entry);
    CCallHelpers::Jump isInt32 = jit.branchIfInt32(entry);
    jit.unboxDoubleWithoutAssertions(entry, entry, StraightEmitter::resultFPR);
    jit.truncateDoubleToInt64(StraightEmitter::resultFPR, entry);
    isInt32.link(&jit);
}

static void generateStepOfCollection(StraightEmitter& emitter, const BodyValue& value, RuntimeOperation operation, ValueID collection)
{
    CCallHelpers& jit = emitter.jit();
    GPRReg entry = operationArgumentGPR(3);
    loadEntryOfCollectionStep(emitter, value.entryOperand(), entry);
    emitter.callOperation(value, operation, { collection, value.tableOperand(), StraightArgument::inRegister(entry), StraightArgument::addressInScratchArea(0) });

    // The library converts one part in front of the next, so the parts do not stand in a row.
    BodyBlock block = emitter.currentBlock();
    for (uint32_t next = value.id().index() + 1; next < block.endOfValues().index(); ++next) {
        BodyValue part = emitter.index().value(ValueID(next));
        if (part.opcode() != Opcode::ResultAt || part.resultsOperand() != value.id())
            continue;
        GPRReg boxed = StraightEmitter::temporary(3);
        switch (static_cast<MapNextResult>(part.index())) {
        case MapNextResult::HasEntry:
            boxed = GPRInfo::returnValueGPR;
            emitter.boxInForm(Representation::Boolean, boxed);
            break;
        case MapNextResult::Entry:
            boxed = GPRInfo::returnValueGPR2;
            emitter.boxInForm(Representation::Int32, boxed);
            break;
        case MapNextResult::Key:
            jit.load64(emitter.wordOfScratchArea(0), boxed);
            break;
        case MapNextResult::Value:
            jit.load64(emitter.wordOfScratchArea(1), boxed);
            break;
        }
        jit.store64(boxed, emitter.slotOf(part.id()));
    }
}

void StraightEmitter::generateCollectionIntrinsic(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::CollectionTable:
        load(value.collectionOperand(), resultGPR);
        jit().loadPtr(CCallHelpers::Address(resultGPR, JSMap::offsetOfStorage()), resultGPR);
        defineTableOrNull(*this, value, resultGPR);
        return;
    case Opcode::CollectionIteratorClosedMark:
        loadClosedMarkOfCollectionIterators(*this, resultGPR);
        define(value);
        return;
    case Opcode::GetCollectionIteratorTable:
        load(value.objectOperand(), resultGPR);
        jit().load64(tableOfCollectionIterator(resultGPR), resultGPR);
        defineTableOrNull(*this, value, resultGPR);
        return;
    case Opcode::PutCollectionIteratorTable:
        generatePutCollectionIteratorTable(*this, value);
        return;
    case Opcode::IsCollectionIteratorClosedMark:
        loadClosedMarkOfCollectionIterators(*this, temporary(1));
        load(value.tableOperand(), resultGPR);
        jit().compare64(CCallHelpers::Equal, resultGPR, temporary(1), resultGPR);
        defineBoolean(value);
        return;
    case Opcode::MapNext:
        generateStepOfCollection(*this, value, RuntimeOperation::MapNext, value.mapOperand());
        return;
    case Opcode::SetNext:
        generateStepOfCollection(*this, value, RuntimeOperation::SetNext, value.setOperand());
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
