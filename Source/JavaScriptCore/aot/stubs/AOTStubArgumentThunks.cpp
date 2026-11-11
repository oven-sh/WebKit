/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubCalls.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTStubAssembler.h"
#include "AOTStubCatalog.h"
#include "AOTStubDescriptor.h"
#include "AOTStubHelpers.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "Butterfly.h"
#include "JSArrayBufferView.h"
#include "JSObject.h"
#include "JSString.h"
#include <algorithm>
#include <limits>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT {

template<typename Functor>
static void forEachThunkMove(std::span<const uint64_t> moves, const Functor& functor)
{
    RELEASE_ASSERT(!(moves.size() % 2));
    for (size_t i = 0; i < moves.size(); i += 2) {
        RELEASE_ASSERT(moves[i] >> 8 < MacroAssembler::numberOfRegisters());
        functor(static_cast<ArgumentSource>(moves[i] & 0xff), static_cast<GPRReg>(moves[i] >> 8), moves[i + 1]);
    }
}

static bool isOperandOfThunkCallee(StubKey key, const StubSignature& signature, GPRReg reg)
{
    for (unsigned i = 0; i < signature.numberOfOperands; ++i) {
        if (signature.operands[i] == Reg(reg))
            return true;
    }
    if (!signature.usesCallConvention)
        return false;
    if (reg == calleeGPR || reg == thisGPR || reg == countGPR)
        return true;
    for (unsigned i = 0; i < std::min<unsigned>(key.count(), numberOfArgumentGPRs); ++i) {
        if (reg == argumentGPR(i))
            return true;
    }
    return false;
}

static bool areThunkMovesSound(StubKey key, const StubSignature& signature, bool movesImmediate, std::span<const uint64_t> moves)
{
    RegisterSet scratchRegisters = RegisterSet::macroClobberedGPRs();
    RegisterSet destinations;
    bool areSound = true;
    forEachThunkMove(moves, [&](ArgumentSource source, GPRReg destination, uint64_t payload) {
        bool isScratch = scratchRegisters.contains(destination, IgnoreVectors);
        bool isImmediate = destination == signature.immediate;
        if (destinations.contains(destination, IgnoreVectors))
            areSound = false;
        if ((isScratch || isImmediate) && !isOperandOfThunkCallee(key, signature, destination))
            areSound = false;
        if ((isScratch && source != ArgumentSource::Constant) || (isImmediate && movesImmediate))
            areSound = false;
        if (source == ArgumentSource::Register && (payload >= MacroAssembler::numberOfRegisters() || scratchRegisters.contains(static_cast<GPRReg>(payload), IgnoreVectors)))
            areSound = false;
        if (source == ArgumentSource::ThrowWordWithMention || source == ArgumentSource::ConstantBelowMention)
            areSound = false;
        destinations.add(destination, IgnoreVectors);
    });
    return areSound;
}

static void emitRegisterMovesOfThunk(CCallHelpers& jit, std::span<const uint64_t> moves)
{
    Vector<std::pair<GPRReg, GPRReg>, 8> pending;
    forEachThunkMove(moves, [&](ArgumentSource source, GPRReg destination, uint64_t payload) {
        if (source == ArgumentSource::Register && static_cast<GPRReg>(payload) != destination)
            pending.append({ static_cast<GPRReg>(payload), destination });
    });
    while (!pending.isEmpty()) {
        size_t unread = pending.findIf([&](auto& move) {
            return !pending.containsIf([&](auto& other) {
                return other.first == move.second;
            });
        });
        if (unread != notFound) {
            jit.move(pending[unread].first, pending[unread].second);
            pending.removeAt(unread);
            continue;
        }
        auto [source, destination] = pending.takeLast();
        jit.swap(source, destination);
        for (auto& other : pending) {
            if (other.first == destination)
                other.first = source;
            else if (other.first == source)
                other.first = destination;
        }
        pending.removeAllMatching([](auto& move) {
            return move.first == move.second;
        });
    }
}

static void emitLoadsOfThunk(CCallHelpers& jit, std::span<const uint64_t> moves)
{
    forEachThunkMove(moves, [&](ArgumentSource source, GPRReg destination, uint64_t payload) {
        auto addressFrom = [&](GPRReg base) {
            return CCallHelpers::Address(base, safeCast<int32_t>(static_cast<int64_t>(payload)));
        };
        switch (source) {
        case ArgumentSource::Register:
        case ArgumentSource::Constant:
            return;
        case ArgumentSource::Stack32:
            jit.load32(addressFrom(GPRInfo::callFrameRegister), destination);
            return;
        case ArgumentSource::Stack64:
            jit.load64(addressFrom(GPRInfo::callFrameRegister), destination);
            return;
        case ArgumentSource::Stack32OffStackPointer:
            jit.load32(addressFrom(CCallHelpers::stackPointerRegister), destination);
            return;
        case ArgumentSource::Stack64OffStackPointer:
            jit.load64(addressFrom(CCallHelpers::stackPointerRegister), destination);
            return;
        case ArgumentSource::ThrowWordWithMention:
        case ArgumentSource::ConstantBelowMention:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static void emitConstantsOfThunk(CCallHelpers& jit, std::span<const uint64_t> moves)
{
    forEachThunkMove(moves, [&](ArgumentSource source, GPRReg destination, uint64_t payload) {
        if (source == ArgumentSource::Constant)
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(payload)), destination);
    });
}

static void returnLengthOfListOrTextInFrontOfMoves(CCallHelpers& jit, const StubSignature& signature, std::span<const uint64_t> moves, std::optional<int32_t> asked, bool checksWalk)
{
    const GPRReg operand = signature.operands[0].gpr();
    const GPRReg scratch = *signature.immediate;
    const GPRReg result = asked ? scratch : signature.firstResult.gpr();
    auto answer = [&](CCallHelpers::Address length) {
        if (!asked) {
            jit.load32(length, result);
            jit.or64(numberTag, result);
            jit.ret();
            return;
        }
#if CPU(X86_64)
        jit.m_assembler.cmpl_im(*asked, length.offset, length.base);
#else
        jit.load32(length, scratch);
        jit.m_assembler.cmp<32>(scratch, UInt12(*asked));
#endif
        jit.ret();
    };
    GPRReg receiver = operand;
    bool liesInRegister = true;
    forEachThunkMove(moves, [&](ArgumentSource source, GPRReg destination, uint64_t payload) {
        if (destination != operand)
            return;
        if (source == ArgumentSource::Register)
            receiver = static_cast<GPRReg>(payload);
        else
            liesInRegister = false;
    });
    if (!liesInRegister || receiver == scratch)
        return;
    CCallHelpers::Jump isNotCell = jit.branchIfNotCell(receiver);
    CCallHelpers::Jump isNoContiguousList = jit.branch8(CCallHelpers::NotEqual, CCallHelpers::Address(receiver, JSCell::indexingTypeAndMiscOffset()), CCallHelpers::TrustedImm32(ArrayWithContiguous));
    CCallHelpers::Label hasPlainStorage = jit.label();
    jit.loadPtr(CCallHelpers::Address(receiver, JSObject::butterflyOffset()), result);
    answer(CCallHelpers::Address(result, Butterfly::offsetOfPublicLength()));

    isNoContiguousList.link(&jit);
    CCallHelpers::Jump isNoText = jit.branch8(CCallHelpers::NotEqual, CCallHelpers::Address(receiver, JSCell::typeInfoTypeOffset()), CCallHelpers::TrustedImm32(StringType));
    jit.loadPtr(CCallHelpers::Address(receiver, JSString::offsetOfValue()), scratch);
    CCallHelpers::Jump isRope = jit.branchIfRopeStringImpl(scratch);
    answer(CCallHelpers::Address(scratch, StringImpl::lengthMemoryOffset()));

    isRope.link(&jit);
    answer(CCallHelpers::Address(receiver, JSRopeString::offsetOfLength()));

    isNoText.link(&jit);
    static_assert(ArrayWithUndecided + 2 == ArrayWithInt32 && ArrayWithUndecided + 4 == ArrayWithDouble && ArrayWithUndecided + 6 == ArrayWithContiguous);
    jit.load8(CCallHelpers::Address(receiver, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.sub32(CCallHelpers::TrustedImm32(ArrayWithUndecided), scratch);
    jit.branchTest32(CCallHelpers::Zero, scratch, CCallHelpers::TrustedImm32(0xff & ~(6 | CopyOnWrite | MayHaveIndexedAccessors))).linkTo(hasPlainStorage, &jit);
    CCallHelpers::JumpList isNoPlainBuffer;
    if (!checksWalk) {
        jit.load32(CCallHelpers::Address(receiver, JSCell::structureIDOffset()), scratch);
        isNoPlainBuffer.append(jit.branch32(CCallHelpers::NotEqual, scratch, CCallHelpers::Address(instanceGPR, Instance::offsetOfReceiverStructureID(Receiver::Buffer))));
        isNoPlainBuffer.append(jit.branchTest8(CCallHelpers::NonZero, CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(builtins.someClassOfViewsWasWrittenAbove))));
        isNoPlainBuffer.append(jit.branchTest8(CCallHelpers::NonZero, CCallHelpers::Address(receiver, JSArrayBufferView::offsetOfMode()), CCallHelpers::TrustedImm32(resizabilityAndAutoLengthMask)));
        jit.load64(CCallHelpers::Address(receiver, JSArrayBufferView::offsetOfLength()), scratch);
        isNoPlainBuffer.append(jit.branch64(CCallHelpers::Above, scratch, CCallHelpers::TrustedImm32(std::numeric_limits<int32_t>::max())));
        answer(CCallHelpers::Address(receiver, JSArrayBufferView::offsetOfLength()));
    }
    isNoPlainBuffer.link(&jit);
    isNotCell.link(&jit);
}

static std::expected<ArgumentThunks, LinkFailure> generateArgumentThunksOfChunk(BuildContext& context, const StubCatalog& catalog, std::span<const KeptArgumentMoves> lists, std::span<const uint32_t> stubKeyIndexes)
{
    ArgumentThunks result;
    CCallHelpers jit;
    RelocationRecorder recorder(jit, context.assemblerPolicy());
    readTagsFromInstance(jit);
    AllowMacroScratchRegisterUsage allowScratch(jit);
    Vector<CCallHelpers::Label> labels;
    labels.reserveInitialCapacity(lists.size());
    for (size_t index = 0; index < lists.size(); ++index) {
        std::span<const uint64_t> words = lists[index].span();
        StubKey key = StubKey::fromBits(words[0]);
        RELEASE_ASSERT(exists(key));
        const StubSignature& signature = catalog.summaryOf(key).signature;
        bool movesImmediate = words[1] & hasImmediate;
        RELEASE_ASSERT(movesImmediate ? signature.immediate && !(words[1] >> 33) : !words[1]);
        std::span<const uint64_t> moves = words.subspan(numberOfHeaderWords);
        RELEASE_ASSERT(areThunkMovesSound(key, signature, movesImmediate, moves));

        labels.append(jit.label());
        if (key.family() == StubFamily::GetLengthAtSite)
            returnLengthOfListOrTextInFrontOfMoves(jit, signature, moves, std::nullopt, catalog.options().checksWalk);
        if (key.family() == StubFamily::LengthIs)
            returnLengthOfListOrTextInFrontOfMoves(jit, signature, moves, static_cast<int32_t>(key.count()), catalog.options().checksWalk);
        emitRegisterMovesOfThunk(jit, moves);
        emitLoadsOfThunk(jit, moves);
        emitConstantsOfThunk(jit, moves);
        if (movesImmediate)
            jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(words[1])), *signature.immediate);
        recorder.jump({ SymbolSpace::Stub, stubKeyIndexes[index] });
    }
    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::Thunk);
    if (buffer.didFailToAllocate())
        return makeUnexpected(LinkFailure::OutOfMemory);
    result.offsets = labels.map([&](CCallHelpers::Label label) {
        return buffer.linkBuffer().offsetOf(label);
    });
    buffer.finalize(false, "AOT argument thunks"_s);
    auto relocations = recorder.link(buffer, &context);
    if (!relocations)
        return makeUnexpected(relocations.error());
    RELEASE_ASSERT(relocations->size() == lists.size());
    result.code = { Vector<uint8_t>(buffer.bytes()), WTF::move(*relocations) };
    return result;
}

std::expected<ArgumentThunks, LinkFailure> generateArgumentThunks(BuildContext& context, const StubCatalog& catalog, const Vector<KeptArgumentMoves>& all)
{
    constexpr size_t listsInChunk = 1024;
    ArgumentThunks result;
    if (all.isEmpty())
        return result;
    ASSERT(std::ranges::is_sorted(all, [](KeptArgumentMoves a, KeptArgumentMoves b) {
        return std::ranges::lexicographical_compare(a.span(), b.span());
    }));

    Vector<uint32_t> stubKeyIndexes;
    stubKeyIndexes.reserveInitialCapacity(all.size());
    for (KeptArgumentMoves list : all) {
        StubKey key = calleeOfArgumentMoves(list.span());
        if (result.stubKeys.isEmpty() || result.stubKeys.last() != key)
            result.stubKeys.append(key);
        stubKeyIndexes.append(safeCast<uint32_t>(result.stubKeys.size() - 1));
    }

    size_t numberOfChunks = (all.size() + listsInChunk - 1) / listsInChunk;
    Vector<std::expected<ArgumentThunks, LinkFailure>> chunks(numberOfChunks);
    forEachIndexInParallel(context.parallelLoops(), static_cast<unsigned>(numberOfChunks), [&](unsigned chunk) {
        size_t first = chunk * listsInChunk;
        size_t count = std::min(listsInChunk, all.size() - first);
        chunks[chunk] = generateArgumentThunksOfChunk(context, catalog, all.span().subspan(first, count), stubKeyIndexes.span().subspan(first, count));
    });

    CheckedSize size = 0;
    for (const auto& chunk : chunks) {
        if (!chunk)
            return makeUnexpected(chunk.error());
        size += chunk->code.bytes.size();
    }
    if (size.hasOverflowed() || size.value() > std::numeric_limits<uint32_t>::max())
        return makeUnexpected(LinkFailure::OutOfMemory);
    result.code.bytes.reserveInitialCapacity(size.value());
    result.code.relocations.reserveInitialCapacity(all.size());
    result.offsets.reserveInitialCapacity(all.size());
    for (const auto& chunk : chunks) {
        uint32_t start = static_cast<uint32_t>(result.code.bytes.size());
        result.code.bytes.appendVector(chunk->code.bytes);
        for (uint32_t offset : chunk->offsets)
            result.offsets.append(start + offset);
        for (Relocation relocation : chunk->code.relocations) {
            relocation.offset += start;
            result.code.relocations.append(relocation);
        }
    }
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
