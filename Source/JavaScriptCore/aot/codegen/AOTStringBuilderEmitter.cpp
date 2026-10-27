/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringBuilderStorage.h"
#include "AOTStubsStrings.h"
#include "AOTVector.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3VariableValue.h"
#include "CCallHelpers.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

using Storage = StringBuilderStorage;

static void storeBytes(Output& out, unsigned width, LValue value, TypedPointer pointer)
{
    switch (width) {
    case 8:
        out.store64(value, pointer);
        return;
    case 4:
        out.store32(value, pointer);
        return;
    case 2:
        out.store32As16(value, pointer);
        return;
    case 1:
        out.store32As8(value, pointer);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr OpcodeSet stackBuilderOperations {
    Opcode::StringBuilderAppendConstant, Opcode::StringBuilderAppendInteger, Opcode::StringBuilderAppendNumber, Opcode::StringBuilderAppendBoolean, Opcode::StringBuilderAppendString,
    Opcode::StringBuilderAppendStringRange, Opcode::StringBuilderAppendStringOrNullish, Opcode::StringBuilderAppendValue, Opcode::StringBuilderLength, Opcode::StringBuilderFinish, Opcode::StringBuilderDiscard,
};

void StringBuilderEmitter::findStackBuilders()
{
    m_hasFoundStackBuilders = true;
    if (m_function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) || !m_function.hasOrdinaryFrame())
        return;

    const CFG& cfg = analyses().cfg();
    const UseLists& useLists = analyses().useLists();
    struct Web {
        bool isOnStack { true };
        bool isOnlyInCompactCode { true };
        bool readsLength { false };
        bool appendsOnlyStrings { true };
        bool beginsWithString { false };
        uint32_t slot { 0 };
        BitVector websOpenAtOnce;
    };
    Vector<Web, 2> webs;
    Vector<uint32_t> webOf;
    webOf.fill(noStackBuilder, m_function.values.size());
    BitVector isMember(m_function.values.size());
    Vector<ValueID, 8> worklist;
    auto visit = [&](ValueID member) {
        if (webOf[member.index()] != noStackBuilder)
            return;
        webOf[member.index()] = webs.size() - 1;
        isMember.set(member.index());
        worklist.append(member);
    };
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID begin : m_function.at(block).values) {
            if (m_function.at(begin).opcode != Opcode::StringBuilderBegin || webOf[begin.index()] != noStackBuilder)
                continue;
            webs.append(Web { });
            visit(begin);
            while (!worklist.isEmpty()) {
                ValueID member = worklist.takeLast();
                Web& web = webs.last();
                ValueLocation location = useLists.locationOf(member);
                if (location.placement == ValuePlacement::Parameter) {
                    web.isOnStack &= location.block != cfg.root();
                    for (Edge edge : cfg.incomingEdges(location.block)) {
                        IncomingValue incoming = incomingValueOf(m_function, edge, location.index);
                        if (edge.isHandlerEdge() || incoming.kind != IncomingValue::Kind::Argument)
                            web.isOnStack = false;
                        else
                            visit(incoming.argument);
                    }
                } else
                    web.isOnStack &= m_function.at(member).opcode == Opcode::StringBuilderBegin;
                for (const Use& use : useLists.usesOf(member)) {
                    if (use.kind == Use::Kind::EdgeArgument) {
                        visit(m_function.at(targetOf(m_function, use.edge()).block).parameters[use.operandIndex]);
                        continue;
                    }
                    if (use.kind != Use::Kind::Operand) {
                        web.isOnStack = false;
                        continue;
                    }
                    const Value& user = m_function.at(use.user);
                    web.isOnStack &= stackBuilderOperations.contains(user.opcode) && user.builderOperand() == member;
                    web.readsLength |= user.opcode == Opcode::StringBuilderLength;
                    // What yields a value appends nothing. A wrong guess costs a comparison that never holds.
                    web.appendsOnlyStrings &= resultRuleOf(user.opcode) != ResultRule::None || user.opcode == Opcode::StringBuilderAppendString || user.opcode == Opcode::StringBuilderDiscard;
                    web.isOnlyInCompactCode &= m_function.at(use.block).shape == CodeShape::Compact;
                }
            }
            // There the calls of the other form are shorter.
            webs.last().isOnStack &= !webs.last().isOnlyInCompactCode;
        }
    }
    if (!std::ranges::any_of(webs, &Web::isOnStack))
        return;

    // One record serves a web, so no two of its builders may be open at once, and an exception must end it: the cursor that a handler would go on with may be stale.
    Liveness liveness(m_function, cfg, useLists, { .trackedValues = &isMember });
    for (BlockID block : cfg.reversePostOrder()) {
        if (m_function.at(block).isHandlerBlock()) {
            for (ValueID live : liveness.liveAtHead(block))
                webs[webOf[live.index()]].isOnStack = false;
        }
        const BlockValues& values = m_function.at(block).values;
        bool hasBegin = std::ranges::any_of(values, [&](ValueID id) {
            return m_function.at(id).opcode == Opcode::StringBuilderBegin;
        });
        if (!hasBegin)
            continue;
        Liveness::LocalCalc liveBefore(liveness, block);
        for (uint32_t index = values.size(); index--;) {
            liveBefore.execute(index);
            if (m_function.at(values[index]).opcode != Opcode::StringBuilderBegin)
                continue;
            uint32_t web = webOf[values[index].index()];
            liveBefore.forEachLive([&](ValueID live) {
                uint32_t other = webOf[live.index()];
                webs[web].isOnStack &= other != web;
                webs[web].websOpenAtOnce.set(other);
                webs[other].websOpenAtOnce.set(web);
            });
        }
    }

    for (BlockID block : cfg.reversePostOrder()) {
        ValueID begin;
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (value.opcode == Opcode::StringBuilderBegin) {
                begin = id;
                continue;
            }
            if (!begin || !stackBuilderOperations.contains(value.opcode) || value.builderOperand() != begin || value.opcode == Opcode::StringBuilderLength)
                continue;
            if (value.opcode == Opcode::StringBuilderAppendString) {
                m_firstAppends.set(id.index());
                webs[webOf[begin.index()]].beginsWithString = true;
            }
            begin = ValueID();
        }
    }

    Vector<B3::Air::StackSlot*, 2> slots;
    m_stackBuilders.grow(webs.size());
    for (uint32_t index = 0; index < webs.size(); ++index) {
        Web& web = webs[index];
        if (!web.isOnStack)
            continue;
        BitVector taken;
        for (uint32_t other : web.websOpenAtOnce) {
            if (other < index && webs[other].isOnStack)
                taken.set(webs[other].slot);
        }
        web.slot = taken.findBit(0, false);
        if (web.slot == slots.size())
            slots.append(m_proc.addStackSlot(sizeof(StackStringBuilder)));
        StackBuilder& builder = m_stackBuilders[index];
        builder = StackBuilder { slots[web.slot], m_proc.addVariable(B3::Int64), m_proc.addVariable(B3::Int64), web.readsLength };
        if (web.appendsOnlyStrings && web.beginsWithString) {
            builder.firstPart = m_proc.addVariable(B3::Int64);
            builder.endOfFirstPart = m_proc.addVariable(B3::Int64);
        }
    }
    m_stackBuilderOf = WTF::move(webOf);
}

auto StringBuilderEmitter::stackBuilderFor(ValueID builder) -> const StackBuilder*
{
    if (!m_hasFoundStackBuilders)
        findStackBuilders();
    if (m_stackBuilderOf.isEmpty())
        return nullptr;
    uint32_t index = m_stackBuilderOf[builder.index()];
    return index != noStackBuilder && m_stackBuilders[index].slot ? &m_stackBuilders[index] : nullptr;
}

Pointer StringBuilderEmitter::addressOf(const StackBuilder& builder)
{
    return Pointer(m_out.m_block->appendNew<B3::SlotBaseValue>(m_proc, m_out.origin(), builder.slot));
}

LValue StringBuilderEmitter::get(B3::Variable* variable)
{
    return m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Get, m_out.origin(), variable);
}

void StringBuilderEmitter::set(B3::Variable* variable, LValue value)
{
    m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), variable, value);
}

Pointer StringBuilderEmitter::cursorOf(const StackBuilder& builder)
{
    return Pointer(get(builder.cursor));
}

void StringBuilderEmitter::setCursor(const StackBuilder& builder, Pointer cursor)
{
    set(builder.cursor, cursor.value());
}

void StringBuilderEmitter::didAppendString(const StackBuilder& builder, ValueID append, CellPointer string)
{
    if (!m_firstAppends.get(append.index()))
        return;
    set(builder.firstPart, string.value());
    set(builder.endOfFirstPart, cursorOf(builder).value());
}

void StringBuilderEmitter::setCursorBehindCall(const StackBuilder& builder, Pointer cursor)
{
    setCursor(builder, cursor);
    set(builder.limit, m_out.loadPtr(stackBuilderField(builder, OBJECT_OFFSETOF(StackStringBuilder, limit))));
}

Pointer StringBuilderEmitter::limitOf(const StackBuilder& builder)
{
    return Pointer(get(builder.limit));
}

void StringBuilderEmitter::advance(const OperationContext& context, const StackBuilder& builder, Pointer cursor)
{
    ifElse(belowOrEqual(cursor, limitOf(builder)), [&] {
        setCursor(builder, cursor);
    }, [&] {
        setCursorBehindCall(builder, callOperation<Pointer>(context, RuntimeOperation::StackStringBuilderGrow, { addressOf(builder), cursor }, { .isOnRarePath = true }));
    });
}

void StringBuilderEmitter::generateStringBuilderBegin(const ValueContext& value)
{
    if (const StackBuilder* builder = stackBuilderFor(value.id())) {
        Pointer buffer = bufferOf(*builder);
        Pointer limit = limitOnStack(*builder);
        m_out.storePtr(limit.value(), stackBuilderField(*builder, OBJECT_OFFSETOF(StackStringBuilder, limit)));
        set(builder->limit, limit.value());
        if (builder->readsLength)
            m_out.storePtr(buffer.value(), stackBuilderField(*builder, OBJECT_OFFSETOF(StackStringBuilder, lengthOrigin)));
        setCursor(*builder, buffer);
        if (builder->firstPart) {
            set(builder->firstPart, m_out.constInt64(0));
            set(builder->endOfFirstPart, m_out.constInt64(0));
        }
        define(value, CellPointer(addressOf(*builder).value()));
        return;
    }
    // Every collection clears the word.
    TypedPointer pool = instanceField(m_heaps.collectorState, AOT_INSTANCE_OFFSET(stringBuilder.freeStorage));
    Integer head(m_out.load64(pool));
    define(value, either<CellPointer>(isNotZero(head), [&] {
        CellPointer builder(head.value());
        TypedPointer nextFree = storageField(builder, Storage::offsetOfNextFree());
        m_out.storePtr(m_out.loadPtr(nextFree), pool);
        m_out.storePtr(m_out.constIntPtr(0), nextFree);
        return builder;
    }, [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::StringBuilderBegin, { }, { .isOnRarePath = true });
    }));
}

void StringBuilderEmitter::generateStringBuilderAppendJSON(const ValueContext& value)
{
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::StringBuilderAppendJSON, { in<CellPointer>(value->builderOperand()), in<Boxed>(value->valueOperand()), in<Boxed>(value->propertyOperand()), in<Boxed>(value->holderOperand()) }));
}

void StringBuilderEmitter::generateStringBuilderAppendJSONMembers(const ValueContext& value)
{
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::StringBuilderAppendJSONMembers, { in<CellPointer>(value->builderOperand()), in<Boxed>(value->objectOperand()), in<Boxed>(value->namesOperand()), emitter<StringEmitter>().word32Of(value->startOperand()), in<Boxed>(value->holderOperand()) }));
}

void StringBuilderEmitter::generateStringBuilderLength(const ValueContext& value)
{
    if (const StackBuilder* builder = stackBuilderFor(value->builderOperand())) {
        LValue lengthOrigin = m_out.loadPtr(stackBuilderField(*builder, OBJECT_OFFSETOF(StackStringBuilder, lengthOrigin)));
        define(value, Word32(m_out.castToInt32(m_out.sub(cursorOf(*builder).value(), lengthOrigin))));
        return;
    }
    define(value, lengthOf(in<CellPointer>(value->builderOperand())));
}

// The text that was made last is the text until something is appended: StringBuilderStorage::validLastString().
void StringBuilderEmitter::generateStringBuilderToString(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Integer lastString(m_out.load64(storageField(builder, StringBuilderStorage::offsetOfLastString())));
    Word32 lengthOfLastString(m_out.load32(storageField(builder, StringBuilderStorage::offsetOfLengthOfLastString())));
    Condition hasLastString = isNotZero(lastString);
    define(value, either<CellPointer>(logicalAnd(hasLastString, equal(lengthOfLastString, lengthOf(builder))), [&] {
        return CellPointer(lastString.value());
    }, [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::StringBuilderToString, { builder, word32(0) });
    }));
}

void StringBuilderEmitter::generateStringBuilderFinish(const ValueContext& value)
{
    if (const StackBuilder* builder = stackBuilderFor(value->builderOperand())) {
        auto finishByCall = [&] {
            return callOperation<CellPointer>(value, RuntimeOperation::StackStringBuilderFinish, { addressOf(*builder), cursorOf(*builder), word32(value->isLikelyKey()) });
        };
        if (!builder->firstPart || value->isLikelyKey()) {
            define(value, finishByCall());
            return;
        }
        // A flat first part behind which nothing has characters is the text. A builder that has left the frame takes long strings without moving its cursor.
        StringEmitter& strings = emitter<StringEmitter>();
        LBasicBlock firstMayBeAll = newBlock();
        LBasicBlock byCall = newBlock();
        Join<CellPointer> text(m_out);
        m_out.branch(equal(cursorOf(*builder), Pointer(get(builder->endOfFirstPart))).value(), unlikelyEdge(firstMayBeAll), normalEdge(byCall));

        m_out.appendTo(byCall);
        text.add(finishByCall());

        m_out.appendTo(firstMayBeAll);
        CellPointer firstPart(get(builder->firstPart));
        Pointer limit = limitOf(*builder);
        strings.continueIf(equal(limit, limitOnStack(*builder)), rareEdge(byCall));
        strings.continueIf(hasNoBits(loadField<Pointer>(firstPart, m_heaps.JSString_value), JSString::isRopeInPointer), unlikelyEdge(byCall));
        text.add(firstPart);
        define(value, text.finish());
        return;
    }
    define(value, callOperation<CellPointer>(value, RuntimeOperation::StringBuilderFinish, { in<CellPointer>(value->builderOperand()), word32(value->isLikelyKey()) }));
}

void StringBuilderEmitter::generateStringBuilderDiscard(const ValueContext& value)
{
    if (const StackBuilder* builder = stackBuilderFor(value->builderOperand())) {
        Pointer limit = limitOf(*builder);
        ifElse(equal(limit, limitOnStack(*builder)), [] { }, [&] {
            callVoidOperation(value, RuntimeOperation::StackStringBuilderDiscard, { addressOf(*builder) }, { .isOnRarePath = true, .accessesCollectorState = true });
        });
        return;
    }
    // The builder goes back to the pool, whose word is collector state, and nothing collects.
    callStub(value, stringBuilderDiscard(), { in<CellPointer>(value->builderOperand()).value() }, { .accessesCollectorState = true });
}

void StringBuilderEmitter::generateStringBuilderCheckpoint(const ValueContext& value)
{
    define(value, lengthOf(in<CellPointer>(value->builderOperand())));
}

void StringBuilderEmitter::generateStringBuilderRollBack(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::StringBuilderRollBack, { in<CellPointer>(value->builderOperand()), emitter<StringEmitter>().word32Of(value->checkpointOperand()) });
}

Word32 StringBuilderEmitter::lengthOf(CellPointer builder)
{
    Word32 position(m_out.load32(storageField(builder, Storage::offsetOfPosition())));
    Word32 lengthOutsideBuffer(m_out.load32(storageField(builder, Storage::offsetOfLengthOutsideBuffer())));
    return add(position, lengthOutsideBuffer);
}

Pointer StringBuilderEmitter::tryReserve(CellPointer builder, Word32 size, LBasicBlock whenThereIsNoRoom)
{
    Integer position = zeroExtended(Word32(m_out.load32(storageField(builder, Storage::offsetOfPosition()))));
    Integer limit = zeroExtended(Word32(m_out.load32(storageField(builder, Storage::offsetOfLimit()))));
    branchUnless(belowOrEqual(add(position, zeroExtended(size)), limit), whenThereIsNoRoom);
    return offsetBy(Pointer(m_out.loadPtr(storageField(builder, Storage::offsetOfCharacters()))), position, 0);
}

Pointer StringBuilderEmitter::reserve(const OperationContext& context, CellPointer builder, uint32_t length)
{
    LBasicBlock slow = newBlock();
    Join<Pointer> join(m_out);
    join.add(tryReserve(builder, word32(static_cast<int32_t>(roundUpToMultipleOf<sizeof(uint64_t)>(length))), slow));

    m_out.appendTo(slow);
    join.add(callOperation<Pointer>(context, RuntimeOperation::StringBuilderReserve, { builder, integer(length) }, { .isOnRarePath = true }));
    return join.finish();
}

Pointer StringBuilderEmitter::reserveAtMost(const OperationContext& context, CellPointer builder, uint32_t size)
{
    LBasicBlock slow = newBlock();
    Join<Pointer> join(m_out);
    join.add(tryReserve(builder, word32(static_cast<int32_t>(size)), slow));

    m_out.appendTo(slow);
    join.add(callOperation<Pointer>(context, RuntimeOperation::StringBuilderReserve, { builder, integer(encodeReservationOfAtMost(size)) }, { .isOnRarePath = true }));
    return join.finish();
}

void StringBuilderEmitter::commit(CellPointer builder, Pointer cursor)
{
    LValue characters = m_out.loadPtr(storageField(builder, Storage::offsetOfCharacters()));
    LValue position = m_out.castToInt32(m_out.sub(cursor.value(), characters));
    m_out.store32(position, storageField(builder, Storage::offsetOfPosition()));
}

Pointer StringBuilderEmitter::writeBytes(Pointer cursor, std::span<const Latin1Character> bytes)
{
    for (size_t offset = 0; offset < bytes.size();) {
        size_t remaining = bytes.size() - offset;
        unsigned width = remaining > 4 ? 8 : remaining > 2 ? 4 : remaining;
        uint64_t bits = 0;
        for (unsigned i = 0; i < width && i < remaining; ++i)
            bits |= static_cast<uint64_t>(bytes[offset + i].value) << (8 * i);
        LValue constant = width == 8 ? m_out.constInt64(static_cast<int64_t>(bits)) : m_out.constInt32(static_cast<int32_t>(bits));
        storeBytes(m_out, width, constant, bytesAt(cursor, offset));
        offset += width;
    }
    return offsetBy(cursor, static_cast<ptrdiff_t>(bytes.size()));
}

Pointer StringBuilderEmitter::writeDigits(Pointer cursor, Word32 value)
{
    static_assert(maxInlineDigits == sizeof(uint64_t));
    static_assert(firstIntegerWrittenByCall == 100'000'000);
    constexpr uint64_t asciiZeros = 0x3030303030303030ull;
    LBasicBlock oneDigit = newBlock();
    LBasicBlock severalDigits = newBlock();
    LBasicBlock store = newBlock();
    auto constant = [&](uint64_t bits) {
        return m_out.constInt64(static_cast<int64_t>(bits));
    };
    LValue number = zeroExtended(value).value();
    m_out.branch(m_out.below(number, constant(10)), normalEdge(oneDigit), normalEdge(severalDigits));

    m_out.appendTo(oneDigit);
    ValueFromBlock oneDigitText = m_out.anchor(m_out.add(number, constant('0')));
    ValueFromBlock oneDigitCount = m_out.anchor(constant(1));
    m_out.jump(store);

    m_out.appendTo(severalDigits);
    LValue upperHalf = m_out.lShr(m_out.mul(number, constant(0xd1b71759ull)), 45);
    LValue lowerHalf = m_out.sub(number, m_out.mul(upperHalf, constant(10000)));
    LValue halves = m_out.bitOr(upperHalf, m_out.shl(lowerHalf, m_out.constInt32(32)));
    LValue hundredthsOfHalves = m_out.lShr(m_out.mul(halves, constant(10486)), 20);
    LValue upperPairs = m_out.bitAnd(hundredthsOfHalves, constant(0x7full << 32 | 0x7f));
    LValue lowerPairs = m_out.sub(halves, m_out.mul(upperPairs, constant(100)));
    LValue pairs = m_out.add(m_out.shl(lowerPairs, m_out.constInt32(16)), upperPairs);
    LValue tenthsOfPairs = m_out.lShr(m_out.mul(pairs, constant(103)), 10);
    LValue tens = m_out.bitAnd(tenthsOfPairs, constant(0x000f000f000f000full));
    LValue digits = m_out.add(tens, m_out.shl(m_out.sub(pairs, m_out.mul(tens, constant(10))), 8));
    B3::PatchpointValue* trailingZeros = m_out.patchpoint(B3::Int64);
    trailingZeros->appendSomeRegister(digits);
    trailingZeros->effects = B3::Effects::none();
    trailingZeros->setRunsWithoutFrame(true);
    trailingZeros->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        jit.countTrailingZeros64WithoutNullCheck(params[1].gpr(), params[0].gpr());
    });
    LValue leadingZeroBits = m_out.bitAnd(trailingZeros, constant(0x38));
    LValue textBehindZeros = m_out.add(digits, constant(asciiZeros));
    ValueFromBlock severalDigitsText = m_out.anchor(m_out.lShr(textBehindZeros, m_out.castToInt32(leadingZeroBits)));
    LValue mostDigits = constant(maxInlineDigits);
    ValueFromBlock severalDigitsCount = m_out.anchor(m_out.sub(mostDigits, m_out.lShr(leadingZeroBits, 3)));
    m_out.jump(store);

    m_out.appendTo(store);
    LValue text = m_out.phi(B3::Int64, oneDigitText, severalDigitsText);
    Integer count(m_out.phi(B3::Int64, oneDigitCount, severalDigitsCount));
    m_out.store64(text, bytesAt(cursor));
    return offsetBy(cursor, count, 0);
}

Pointer StringBuilderEmitter::copyLatin1Characters(Pointer cursor, Pointer source, Word32 length)
{
    constexpr uint64_t bytesPerMove = 2 * sizeof(uint64_t);
    // The short copy covers two words at most, and the last move of the loop reaches one whole move back from the end.
    uint64_t maxLengthOfShortCopy = std::clamp<uint64_t>(build().limit(LimitID::MaxLengthCopiedInline), bytesPerMove - 1, bytesPerMove);
    LBasicBlock shortCopy = newBlock();
    LBasicBlock loop = newBlock();
    LBasicBlock lastMove = newBlock();
    Join<Pointer> end(m_out);
    Integer size = zeroExtended(length);
    auto moveAt = [&](Integer offset) {
        const B3::AbstractHeap& characters = m_heaps.characters8.atAnyIndex();
        LValue from = offsetBy(source, offset, 0).value();
        Pointer to = offsetBy(cursor, offset, 0);
        LValue firstWord = m_out.load64(TypedPointer(characters, from));
        LValue secondWord = m_out.load64(TypedPointer(characters, from, sizeof(uint64_t)));
        m_out.store64(firstWord, bytesAt(to));
        m_out.store64(secondWord, bytesAt(to, sizeof(uint64_t)));
    };
    ValueFromBlock firstOffset = m_out.anchor(m_out.constInt64(0));
    m_out.branch(belowOrEqual(length, word32(static_cast<int32_t>(maxLengthOfShortCopy))).value(), normalEdge(shortCopy), normalEdge(loop));

    m_out.appendTo(shortCopy);
    end.add(copyShortLatin1Characters(cursor, source, length));

    m_out.appendTo(loop);
    Integer offset(m_out.phi(B3::Int64, firstOffset));
    moveAt(offset);
    Integer nextOffset = add(offset, integer(bytesPerMove));
    m_out.addIncomingToPhi(offset.value(), m_out.anchor(nextOffset.value()));
    m_out.branch(belowOrEqual(add(nextOffset, integer(bytesPerMove)), size).value(), normalEdge(loop), normalEdge(lastMove));

    m_out.appendTo(lastMove);
    moveAt(sub(size, integer(bytesPerMove)));
    end.add(offsetBy(cursor, size, 0));
    return end.finish();
}

Pointer StringBuilderEmitter::copyShortLatin1Characters(Pointer cursor, Pointer source, Word32 length)
{
    const B3::AbstractHeap& characters = m_heaps.characters8.atAnyIndex();
    LBasicBlock done = newBlock();
    Integer size = zeroExtended(length);
    for (unsigned width : { 8u, 4u, 2u }) {
        LBasicBlock copy = newBlock();
        LBasicBlock shorter = newBlock();
        m_out.branch(aboveOrEqual(length, word32(width)).value(), normalEdge(copy), normalEdge(shorter));

        m_out.appendTo(copy);
        Integer lastOffset = sub(size, integer(width));
        LValue first = loadBytes(m_out, width, TypedPointer(characters, source.value()));
        LValue last = loadBytes(m_out, width, TypedPointer(characters, offsetBy(source, lastOffset, 0).value()));
        storeBytes(m_out, width, first, bytesAt(cursor));
        storeBytes(m_out, width, last, bytesAt(offsetBy(cursor, lastOffset, 0)));
        m_out.jump(done);

        m_out.appendTo(shorter);
    }
    LBasicBlock oneCharacter = newBlock();
    m_out.branch(isNotZero(length).value(), normalEdge(oneCharacter), normalEdge(done));

    m_out.appendTo(oneCharacter);
    LValue character = loadBytes(m_out, 1, TypedPointer(characters, source.value()));
    storeBytes(m_out, 1, character, bytesAt(cursor));
    m_out.jump(done);

    m_out.appendTo(done);
    return offsetBy(cursor, size, 0);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
