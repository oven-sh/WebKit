/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlanOfArrival.h"

#if ENABLE(AOT)

#include "AOTCallSiteCache.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTLeaveRows.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "AOTSavedFrame.h"
#include "AOTState.h"
#include "AOTStubsLeaving.h"
#include "AOTVMState.h"
#include "CCallHelpers.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "JSCJSValueInlines.h"
#include "PureNaN.h"
#include "StackAlignment.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// As ConversionEmitter::box() boxes the form.
static uint64_t boxForArrival(FormInLeaveRow form, uint64_t bits)
{
    switch (form) {
    case FormInLeaveRow::JSValue:
    case FormInLeaveRow::Cell:
        return bits;
    case FormInLeaveRow::Int32:
        return JSValue::encode(jsNumber(static_cast<int32_t>(bits)));
    case FormInLeaveRow::Boolean:
        return JSValue::encode(jsBoolean(static_cast<uint32_t>(bits)));
    case FormInLeaveRow::Double:
        return JSValue::encode(jsNumber(purifyNaN(std::bit_cast<double>(bits))));
    case FormInLeaveRow::Int64:
        return JSValue::encode(jsNumber(static_cast<double>(static_cast<int64_t>(bits))));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PlanOfArrival* layOutSlowFrames(Vector<uint64_t>& memoryOfPlan, const RegistersLaidDownAtLeave& laidDown, const ImageFrame& fastFrame, std::span<const uint32_t> wordsFromRowOn, const void* stackLimit, const MakerOfHeapWords& makeHeapWords)
{
    // What arrives for good takes the place of the fast frame under the same return address, and returns as a frame does that owns nothing above it.
    RELEASE_ASSERT(fastFrame.hasFrameRecord() && !fastFrame.bytesAboveFrameRecord() && !fastFrame.liesBelowSpilledArguments());
    uint64_t* framePointerOfFastBody = laidDown.framePointerOfFastBody;

    constexpr uint32_t noPart = UINT32_MAX;
    struct Part {
        PartOfLeaveRow kind;
        uint32_t words;
        uint32_t firstValue;
        // Of a frame or a call: the part HeapWords in front of it, and its own among the addresses. Of heap words: which cell.
        uint32_t heapWords;
        uint32_t address;
    };
    struct Value {
        uint32_t slot;
        bool isCellOfHeapWords;
        uint64_t word;
    };
    Vector<Part, 8> parts;
    Vector<Value, 32> values;
    uint32_t numberOfAddresses = 0;
    uint32_t numberOfCells = 0;
    LeaveRowReader row(wordsFromRowOn);
    bool comesBack = row.end() == EndOfLeave::ComesBack;
    while (row.partsLeft()) {
        LeaveRowReader::Part part = row.beginPart();
        bool isHeapWords = part.kind == PartOfLeaveRow::HeapWords;
        bool followsHeapWords = !parts.isEmpty() && parts.last().kind == PartOfLeaveRow::HeapWords;
        RELEASE_ASSERT(!isHeapWords || !followsHeapWords);
        parts.append({ part.kind, part.words, static_cast<uint32_t>(values.size()), isHeapWords ? numberOfCells++ : followsHeapWords ? parts.last().heapWords : noPart, isHeapWords ? 0 : numberOfAddresses++ });
        while (row.valuesLeft()) {
            ValueInLeaveRow value = row.next();
            uint64_t bits = 0;
            switch (value.place) {
            case PlaceInLeaveRow::Register:
                RELEASE_ASSERT(value.where <= Reg::maxIndex());
                bits = laidDown.byIndex[value.where];
                break;
            case PlaceInLeaveRow::Stack:
                RELEASE_ASSERT(value.where * sizeof(uint32_t) <= fastFrame.frameSizeInBytes());
                if (has32Bits(value.form))
                    bits = reinterpret_cast<const uint32_t*>(framePointerOfFastBody)[-static_cast<intptr_t>(value.where)];
                else
                    bits = framePointerOfFastBody[-static_cast<intptr_t>(value.where / 2)];
                break;
            case PlaceInLeaveRow::Constant:
                bits = value.where;
                break;
            case PlaceInLeaveRow::HeapWords:
                RELEASE_ASSERT(parts.last().heapWords != noPart && !isHeapWords);
                break;
            case PlaceInLeaveRow::Address:
                RELEASE_ASSERT(!isHeapWords);
                bits = std::bit_cast<uint64_t>(laidDown.addresses[numberOfAddresses++]);
                break;
            }
            values.append({ value.slot, value.place == PlaceInLeaveRow::HeapWords, boxForArrival(value.form, bits) });
        }
    }
    RELEASE_ASSERT(parts.last().kind != PartOfLeaveRow::HeapWords);
    auto valuesOf = [&](size_t part) {
        size_t end = part + 1 < parts.size() ? parts[part + 1].firstValue : values.size();
        return values.span().subspan(parts[part].firstValue, end - parts[part].firstValue);
    };

    // What stays of the stack: for good, all above the record of the fast frame, which the outermost frame takes over. Else the fast frame too, down to where its stack pointer stood in front of the pushes.
    uint64_t* stackPointerOfFastBody = std::bit_cast<uint64_t*>(&laidDown.addresses[isX86_64() ? numberOfAddresses : (numberOfAddresses + 1) & ~1u]);
    uint64_t* top = comesBack ? stackPointerOfFastBody : framePointerOfFastBody;
    size_t wordsBelowTop = 0;
    bool hasFrameAbove = comesBack;
    for (size_t part = parts.size(); part--;) {
        switch (parts[part].kind) {
        case PartOfLeaveRow::HeapWords:
            continue;
        case PartOfLeaveRow::Frame:
            RELEASE_ASSERT(!(parts[part].words % 2));
            wordsBelowTop += parts[part].words + (hasFrameAbove ? 2 : 0);
            break;
        case PartOfLeaveRow::Call:
            // The innermost. On x86-64 a call leaves the address that it returns to on the stack.
            RELEASE_ASSERT(part <= 1 && (!part || parts[0].kind == PartOfLeaveRow::HeapWords));
            wordsBelowTop += isX86_64() && hasFrameAbove;
            break;
        }
        hasFrameAbove = true;
    }
    if (std::bit_cast<uintptr_t>(top) < std::bit_cast<uintptr_t>(stackLimit) || std::bit_cast<uintptr_t>(top) - std::bit_cast<uintptr_t>(stackLimit) < wordsBelowTop * sizeof(uint64_t))
        return nullptr;

    // Whoever calls puts collections off: a cell that is made here is held by nothing yet.
    Vector<HeapWordsOfArrival, 4> cells;
    for (size_t part = 0; part < parts.size(); ++part) {
        if (parts[part].kind != PartOfLeaveRow::HeapWords)
            continue;
        cells.append(makeHeapWords(parts[part].words));
        for (const Value& value : valuesOf(part)) {
            RELEASE_ASSERT(value.slot <= parts[part].words);
            cells.last().words[value.slot - 1] = value.word;
        }
    }

    Vector<StoreOfArrival, 32> stores;
    uint64_t registers[RegistersLaidDownAtLeave::numberOfWords];
    memcpy(registers, laidDown.byIndex, sizeof(registers));
    if (!comesBack) {
        // What its own return would have put back.
        const uint64_t* saved = framePointerOfFastBody - fastFrame.startOfCalleeSaves();
        for (uint64_t mask = ImageFrame::unpackRegisters(fastFrame.calleeSaveRegisters); mask; mask &= mask - 1, ++saved)
            registers[WTF::ctz(mask)] = *saved;
    }

    // From the outermost part down. Each is as if what stands above had called it, but for the outermost of a leave for good, which stands in the place of the fast frame.
    uint64_t* framePointer = framePointerOfFastBody;
    uint64_t* stackPointer = stackPointerOfFastBody;
    uint64_t returnAddress = std::bit_cast<uint64_t>(laidDown.returnAddress);
    uint32_t wordsOfFrameAbove = UINT32_MAX;
    hasFrameAbove = comesBack;
    for (size_t part = parts.size(); part--;) {
        if (parts[part].kind == PartOfLeaveRow::HeapWords)
            continue;
        auto wordOf = [&](const Value& value) {
            return value.isCellOfHeapWords ? cells[parts[part].heapWords].cell : value.word;
        };
        if (parts[part].kind == PartOfLeaveRow::Frame) {
            if (hasFrameAbove) {
                stores.append({ stackPointer - 1, returnAddress });
                stores.append({ stackPointer - 2, std::bit_cast<uint64_t>(framePointer) });
                framePointer = stackPointer - 2;
            }
            for (const Value& value : valuesOf(part)) {
                RELEASE_ASSERT(value.slot <= parts[part].words);
                stores.append({ framePointer - value.slot, wordOf(value) });
            }
            stackPointer = framePointer - parts[part].words;
            wordsOfFrameAbove = parts[part].words;
            returnAddress = std::bit_cast<uint64_t>(laidDown.addresses[parts[part].address]);
            hasFrameAbove = true;
            continue;
        }
        if (!hasFrameAbove) {
            // It takes the place of the call that entered the fast body.
            returnAddress = framePointerOfFastBody[1];
            framePointer = std::bit_cast<uint64_t*>(framePointerOfFastBody[0]);
            stackPointer = framePointerOfFastBody + 2;
        }
        for (const Value& value : valuesOf(part)) {
            if (value.slot < firstSlotOfStackWordInLeaveRow) {
                RELEASE_ASSERT(value.slot - 1 <= Reg::maxIndex());
                registers[value.slot - 1] = wordOf(value);
                continue;
            }
            uint32_t stackWord = value.slot - firstSlotOfStackWordInLeaveRow;
            RELEASE_ASSERT(stackWord < wordsOfFrameAbove);
            stores.append({ stackPointer + stackWord, wordOf(value) });
        }
#if CPU(X86_64)
        --stackPointer;
        if (hasFrameAbove)
            stores.append({ stackPointer, returnAddress });
#else
        registers[Reg(CCallHelpers::linkRegister).index()] = returnAddress;
#endif
    }
    bool entersByCall = parts[parts[0].kind == PartOfLeaveRow::HeapWords].kind == PartOfLeaveRow::Call;
    RELEASE_ASSERT(stackPointer >= top - wordsBelowTop && (stackPointer == top - wordsBelowTop || (entersByCall && !comesBack && numberOfAddresses == 1)));
    RELEASE_ASSERT(std::bit_cast<uintptr_t>(stackPointer) % stackAlignmentBytes() == (isX86_64() && entersByCall ? sizeof(uint64_t) : 0));

    memoryOfPlan.grow(std::max(memoryOfPlan.size(), PlanOfArrival::wordsFor(stores.size())));
    auto* plan = reinterpret_cast<PlanOfArrival*>(memoryOfPlan.mutableSpan().data());
    plan->target = laidDown.addresses[0];
    plan->stackPointer = stackPointer;
    plan->framePointer = framePointer;
    plan->numberOfStores = stores.size();
    memcpy(plan->registers, registers, sizeof(registers));
    memcpy(plan->stores(), stores.span().data(), stores.size() * sizeof(StoreOfArrival));
    return plan;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLayOutSlowFrames, void*, (Instance* instance, uint64_t* registersLaidDown))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTLayOutSlowFrames);
    DeferTerminationForAWhile deferTermination(vm);
    // Several cells may be made in a row, and none is held by anything until the stub has laid the frames.
    DeferGCForAWhile deferGC(vm);
    const auto& laidDown = *reinterpret_cast<const RegistersLaidDownAtLeave*>(registersLaidDown);
    const VMState& state = vm.aotState();
    const CallSiteCache::Entry* site = state.callSiteCache->find(laidDown.returnAddress);
    if (!site)
        site = state.callSiteCache->resolve(*state.image, laidDown.returnAddress);
    RELEASE_ASSERT(site && !site->isStubReturnSite());
    // The entry may be gone once anybody else has asked the cache.
    ImageFrame fastFrame = site->frame;
    std::span<const uint32_t> words = state.image->frameMapWords(site->code.index());
    uint32_t firstWordOfRow = static_cast<uint32_t>(laidDown.byIndex[Reg(rowOfLeaveGPR).index()]);
    RELEASE_ASSERT(firstWordOfRow < words.size());
    auto makeHeapWords = [&](uint32_t numberOfWords) {
        SavedFrame* cell = SavedFrame::createHeapFrame(vm, *instance, numberOfWords, nullptr);
        return HeapWordsOfArrival { std::bit_cast<uint64_t>(cell), reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(cell) + SavedFrame::offsetOfWords()) };
    };
    return layOutSlowFrames(instance->state->memoryOfPlanOfArrival, laidDown, fastFrame, words.subspan(firstWordOfRow), vm.softStackLimit(), makeHeapWords);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
