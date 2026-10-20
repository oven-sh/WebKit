/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONSerializerBuilder.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestSite.h"
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<PristineAspects> serializedArrayAspects { PristineAspects::Storage, PristineAspects::Names };

static bool canHoldElements(const Program& program, ArrayStorage storage, const Type& elements)
{
    return typeOfElementRead(storage).withoutNullish().overlaps(elements, program);
}

static IRBuilder::ProvenBranch emitBranchOnNextIndex(IRBuilder& builder, ValueID index, ValueID length)
{
    ValueID next = builder.append(Opcode::Add, { index, builder.appendInteger(1) });
    return builder.emitBranchWithProof(builder.append(Opcode::Less, { next, length }));
}

void JSONSerializerBuilder::appendArray(ValueID array, const ValuePlan& plan, const Position& position)
{
    IRBuilder& builder = this->builder();
    MergePoint done = createMergePoint();
    MergePoint written = createMergePoint();
    MergePoint closing = createMergePoint();
    BlockID generic = builder.addBlock(BlockFrequency::Rare);
    Cursor start = m_cursor;

    ValueID pristine = emitProvenBranch(builder.append(makePristineTest(array, BuiltinClasses::Array, serializedArrayAspects)), generic);
    ValueID length = builder.append(Opcode::GetArrayLength, { array, pristine });
    ValueID isEmpty = builder.append(Opcode::StrictEq, { length, builder.appendInteger(0) });
    BlockID hasElements = builder.emitBranch(isEmpty).notTaken;
    appendKeyOf(position);
    appendToken(JSONToken::EmptyArray);
    jumpTo(written);

    builder.moveToEndOf(hasElements);
    ElementCode elementCode = ElementCode::RunsNoUnknownCode;
    for (const ArrayStorageForm& form : arrayStorageForms) {
        if (!canHoldElements(program(), form.kind, program().at(plan.element)))
            continue;
        IRBuilder::ProvenBranch hasStorage = builder.emitBranchWithProof(builder.append(makeArrayStorageTest(array, form.kind)));
        ValueID guards[] { pristine, hasStorage.provenOnTaken };
        m_cursor = start;
        appendKeyOf(position);
        appendToken(JSONToken::OpenArray);
        if (appendArrayLoop({ array, builder.appendAllGuards(guards), length, plan.element, closing }, form.kind) == ElementCode::MayRunUnknownCode)
            elementCode = ElementCode::MayRunUnknownCode;
        builder.moveToEndOf(hasStorage.notTaken);
    }
    builder.emitJump(generic);

    if (closing.block) {
        continueAt(closing);
        appendToken(JSONToken::CloseArray);
        if (elementCode == ElementCode::MayRunUnknownCode && m_afterUnknownCode) {
            if (position.endsWithComma())
                appendToken(JSONToken::Comma);
            leaveAfterUnknownCode();
        }
        jumpTo(written);
    }
    continueAt(written);
    if (position.endsWithComma())
        appendToken(JSONToken::Comma);
    jumpTo(done);

    builder.moveToEndOf(generic);
    m_cursor = start;
    appendGeneric(array, position);
    jumpTo(done);
    continueAt(done);
}

auto JSONSerializerBuilder::appendArrayLoop(const ArrayLoop& loop, ArrayStorage storage) -> ElementCode
{
    IRBuilder& builder = this->builder();
    TypeArena& types = inputs().types;

    BlockID body = builder.addBlock(builder.function().at(builder.currentBlock()).frequency);
    ValueID index = builder.addParameter(body, types.intern(Type::integer({ 0, typeOf(loop.length).integers.maximum - 1 })));
    ValueID cursor = builder.addParameter(body, types.intern(Type::forOpaque(OpaqueKind::BuilderCursor)));
    ValueID guard = builder.addParameter(body, TypeTable::guard);
    ValueID firstIndex = builder.appendInteger(0);
    ensureCursor(0);
    builder.emitJump(body, { firstIndex, m_cursor.value, loop.guard });

    builder.moveToEndOf(body);
    m_cursor = { cursor, 0 };
    builder.append(Opcode::LoopHint, { });
    Value read = IRBuilder::make(Opcode::GetArrayElement, { loop.array, index, guard });
    read.setArrayStorage(storage);
    read.setKeyIsIntegral(true);

    MergePoint afterUnknownCode { .frequency = BlockFrequency::Rare };
    MergePoint* outer = std::exchange(m_afterUnknownCode, &afterUnknownCode);
    appendArrayElement(loop, builder.append(WTF::move(read)), index);
    m_afterUnknownCode = outer;

    if (isInBlock()) {
        IRBuilder::ProvenBranch hasNext = emitBranchOnNextIndex(builder, index, loop.length);
        Cursor afterElement = m_cursor;
        appendToken(JSONToken::Comma);
        builder.emitJump(body, { hasNext.provenOnTaken, m_cursor.value, guard });

        builder.moveToEndOf(hasNext.notTaken);
        m_cursor = afterElement;
        jumpTo(loop.closing);
    }
    if (!afterUnknownCode.block)
        return ElementCode::RunsNoUnknownCode;

    continueAt(afterUnknownCode);
    IRBuilder::ProvenBranch hasNext = emitBranchOnNextIndex(builder, index, loop.length);
    Cursor afterElement = m_cursor;
    BlockID stale = builder.addBlock(BlockFrequency::Rare);
    ValueID renewed[] {
        emitProvenBranch(builder.append(makePristineTest(loop.array, BuiltinClasses::Array, serializedArrayAspects)), stale),
        emitProvenBranch(builder.append(makeArrayStorageTest(loop.array, storage)), stale),
    };
    ValueID renewedGuard = builder.appendAllGuards(renewed);
    ValueID currentLength = builder.append(Opcode::GetArrayLength, { loop.array, renewedGuard });
    emitJumpUnless(builder.append(Opcode::Less, { hasNext.provenOnTaken, currentLength }), stale);
    appendToken(JSONToken::Comma);
    builder.emitJump(body, { hasNext.provenOnTaken, m_cursor.value, renewedGuard });

    builder.moveToEndOf(hasNext.notTaken);
    m_cursor = afterElement;
    jumpTo(loop.closing);

    builder.moveToEndOf(stale);
    m_cursor = afterElement;
    appendRemainingElements(loop, hasNext.provenOnTaken);
    return ElementCode::MayRunUnknownCode;
}

void JSONSerializerBuilder::appendArrayElement(const ArrayLoop& loop, ValueID element, ValueID index)
{
    IRBuilder& builder = this->builder();
    MergePoint done = createMergePoint();
    if (typeOf(element).primitives & Primitive::Absent) {
        Cursor start = m_cursor;
        IRBuilder::ProvenBranch isHole = builder.emitBranchWithProof(builder.appendTypeTest(element, TypeTable::absent), testedOperandIndex, BlockFrequency::Unlikely, BlockFrequency::Normal);
        appendToken(JSONToken::Null);
        jumpTo(done);

        builder.moveToEndOf(isHole.notTaken);
        m_cursor = start;
        element = isHole.provenOnNotTaken;
    }

    appendValue(element, planType(typeOf(element), &program().at(loop.elementType)), { .kind = JSONPosition::Element, .index = index });
    if (!done.block)
        return;
    jumpTo(done);
    continueAt(done);
}

void JSONSerializerBuilder::appendRemainingElements(const ArrayLoop& loop, ValueID firstIndex)
{
    IRBuilder& builder = this->builder();
    BlockID body = builder.addBlock(BlockFrequency::Rare);
    ValueID index = builder.addParameter(body, builder.function().at(firstIndex).type);
    commit();
    builder.emitJump(body, { firstIndex });

    builder.moveToEndOf(body);
    builder.append(Opcode::LoopHint, { });
    Value read = IRBuilder::make(Opcode::ElemGet, { loop.array, index });
    read.setKeyIsIntegral(true);
    ValueID element = builder.append(WTF::move(read));
    appendToken(JSONToken::Comma);
    appendByHost(element, { .kind = JSONPosition::Element, .index = index });
    commit();

    IRBuilder::ProvenBranch hasNext = emitBranchOnNextIndex(builder, index, loop.length);
    builder.emitJump(body, { hasNext.provenOnTaken });

    builder.moveToEndOf(hasNext.notTaken);
    jumpTo(loop.closing);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
