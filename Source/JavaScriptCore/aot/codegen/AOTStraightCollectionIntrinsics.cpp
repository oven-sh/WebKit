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
    emitter.load(value.objectOperand(), iterator);
    GPRReg table = emitter.loadIntoResultRegister(value.valueOperand());
    CCallHelpers::Jump isTable = jit.branch64(CCallHelpers::NotEqual, table, CCallHelpers::TrustedImm64(JSValue::encode(jsNull())));
    jit.move(CCallHelpers::TrustedImm32(0), table);
    isTable.link(&jit);
    jit.store64(table, tableOfCollectionIterator(iterator));
    if (!emitter.isConstant(value.valueOperand()))
        emitter.barrierAfterStore(iterator);
    emitter.defineSameAs(value, value.valueOperand());
}

static void loadEntryOfCollectionStep(StraightEmitter& emitter, ValueID operand, GPRReg entry)
{
    CCallHelpers& jit = emitter.jit();
    if (std::optional<JSValue> constant = emitter.constantOf(operand); constant && constant->isAnyInt()) {
        jit.move(CCallHelpers::TrustedImm64(constant->asAnyInt()), entry);
        return;
    }
    emitter.load(operand, entry);
    CCallHelpers::Jump isInt32 = jit.branchIfInt32(entry);
    FPRReg number = emitter.takeResultFPR();
    jit.unboxDoubleWithoutAssertions(entry, entry, number);
    jit.truncateDoubleToInt64(number, entry);
    isInt32.link(&jit);
}

static void generateStepOfCollection(StraightEmitter& emitter, const BodyValue& value, RuntimeOperation operation, ValueID collection)
{
    CCallHelpers& jit = emitter.jit();
    GPRReg entry = operationArgumentGPR(3);
    loadEntryOfCollectionStep(emitter, value.entryOperand(), entry);
    emitter.callOperation(value, operation, { collection, value.tableOperand(), StraightArgument::inRegister(entry), StraightArgument::addressInScratchArea(0) });

    BodyBlock block = emitter.currentBlock();
    for (uint32_t next = value.id().index() + 1; next < block.endOfValues().index(); ++next) {
        BodyValue part = emitter.index().value(ValueID(next));
        if (part.opcode() != Opcode::ResultAt || part.resultsOperand() != value.id())
            continue;
        GPRReg boxed = StraightEmitter::temporary(3);
        switch (static_cast<MapNextResult>(part.index())) {
        case MapNextResult::HasEntry:
            boxed = emitter.resultOfCall();
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
    case Opcode::CollectionTable: {
        GPRReg result = loadIntoResultRegister(value.collectionOperand());
        jit().loadPtr(CCallHelpers::Address(result, JSMap::offsetOfStorage()), result);
        defineTableOrNull(*this, value, result);
        return;
    }
    case Opcode::CollectionIteratorClosedMark:
        loadClosedMarkOfCollectionIterators(*this, takeResultRegister());
        define(value);
        return;
    case Opcode::GetCollectionIteratorTable: {
        GPRReg result = loadIntoResultRegister(value.objectOperand());
        jit().load64(tableOfCollectionIterator(result), result);
        defineTableOrNull(*this, value, result);
        return;
    }
    case Opcode::PutCollectionIteratorTable:
        generatePutCollectionIteratorTable(*this, value);
        return;
    case Opcode::IsCollectionIteratorClosedMark: {
        loadClosedMarkOfCollectionIterators(*this, temporary(1));
        GPRReg result = loadIntoResultRegister(value.tableOperand());
        jit().compare64(CCallHelpers::Equal, result, temporary(1), result);
        defineBoolean(value, result);
        return;
    }
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
