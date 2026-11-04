/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSavedFrameEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTLeaveRowsOfCode.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSlowFrames.h"
#include "AOTStubsLeaving.h"
#include "AOTWalkLeaveRecord.h"
#include "AllowMacroScratchRegisterUsage.h"

namespace JSC { namespace AOT { namespace Typed {

// The target of the jump of a test that has failed. No value is moved and none is boxed: the row says where each lies and in which form, and the routine lays them into the frames of the slow bodies.
//     the addresses, pushed, the outermost frame's first: where the innermost is entered, and where each frame below another returns to . the index of the row . the call.
// One register is written, in which no value ever lies, so every operand may lie anywhere.
// A call and no jump: by the return address the run time finds the unit, its words and the frame that holds here.
void SavedFrameEmitter::generateLeave(const ValueContext& value)
{
    leaveForSlowBodies(value, false);
}

// The same, but the fast frame stays: the frames are laid below it, and the outermost slow body returns to here as to a call of it.
void SavedFrameEmitter::generateLeaveCopies(const ValueContext& value)
{
    leaveForSlowBodies(value, true);
}

void SavedFrameEmitter::leaveForSlowBodies(const ValueContext& value, bool comesBack)
{
    struct Frame {
        CodeUnit slowBody;
        uint16_t entry;
        uint32_t wordsOfFrame;
        uint32_t numberOfValues;
        // Of a frame that arrives at the head.
        uint32_t number;
        uint32_t numberOfHeapWords;
        std::optional<uint32_t> wordOfOperation;
    };
    Vector<Frame, 2> frames;
    Vector<uint32_t, 16> slots;
    bool arrivesAtHead = false;
    forEachFrameOfLeave(value->frames(), [&](const FrameOfLeave& frame) {
        // The slots of a frame that arrives at the head are the words of its list as they are.
        if (frame.arrivesAtHead) {
            arrivesAtHead = true;
            frames.append({ { frame.function, CodeRole::SlowBody }, primaryEntry, 0, static_cast<uint32_t>(frame.liveList.size()), frame.number, frame.numberOfHeapWords, frame.wordOfOperation });
            slots.appendRange(frame.liveList.begin(), frame.liveList.end());
            return;
        }
        RELEASE_ASSERT(!arrivesAtHead);
        if (!frame.function) {
            frames.append({ { }, 0, wordsOfFrameThatSelectsResultOfConstruction, 1, 0, 0, std::nullopt });
            slots.append(slotOfThisInFrameThatSelectsResultOfConstruction);
            return;
        }
        const Function& function = m_program.at(frame.function);
        frames.append({ { frame.function, CodeRole::SlowBody }, frame.entry, wordsOfSlowFrame(function), static_cast<uint32_t>(frame.liveList.size()), 0, 0, std::nullopt });
        for (uint32_t input : frame.liveList)
            slots.append(slotInSlowFrame(function, ValueID(input)));
    });
    std::span<const ValueID> values = value->valuesOperands();
    RELEASE_ASSERT(!frames.isEmpty() && slots.size() == values.size() && (arrivesAtHead || !comesBack));

    // In front of the Patch: a constant is made where it is first asked for.
    Vector<LValue, 16> operands;
    Vector<Representation, 16> forms;
    for (ValueID id : values) {
        Operand operand = get(id);
        // The walk has boxed every other form.
        RELEASE_ASSERT(formInLeaveRowOf(operand.representation) && crossesInItsForm(operand.representation));
        forms.append(operand.representation);
        operands.append(operand.value);
    }

    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    StubKey key = comesBack ? enterSlowBodiesAndComeBack() : leaveForSlowBody();
    // Where the stack has no room for the slow frames the routine throws at this site, as a call would.
    CallPatchpointRequest request {
        .resultType = Machine::Void,
        .effects = inputs().stubCatalog.effectsOf(key),
        .neverReturns = !comesBack,
        .holdsUniqueRecord = true,
    };
    // It returns as a direct call of the outermost slow body does.
    std::optional<ArgumentLocation> result;
    if (comesBack) {
        ArgumentLocations locations = conventions().locationsOf(m_program, frames.last().slowBody);
        RELEASE_ASSERT(locations.results.size() == 1);
        result = locations.results[0];
        RELEASE_ASSERT(result->kind == ArgumentLocation::Kind::GPR || result->kind == ArgumentLocation::Kind::FPR);
        request.resultType = machineTypeOf(result->representation);
        request.earlyClobbered = RegisterSet::macroClobberedGPRs();
        request.lateClobbered = calls.registersClobberedByCalls();
        request.lateClobbered.merge(inputs().stubCatalog.summaryOf(key).clobbered);
        request.effects = Effects::unknownCode();
        // What that unit takes on the stack lies in the lowest words of this frame. The room is what every caller of a unit of the function leaves: one unit may hand its arguments on to another.
        if (uint32_t stackWordsToReserve = std::max(conventions().stackWordsReservedByCallersOf(m_program, frames.last().slowBody), locations.numberOfStackWords()))
            m_proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(stackWordsToReserve * sizeOfStackWord));
    }
    Machine::PatchOperation* patch = calls.createCallPatchpoint(value, request);
    patch->setNeedsFrameRecord(true);
    // PatchParameters: the results, then the operands.
    unsigned firstValue = patch->numChildren() + (result ? 1 : 0);
    for (LValue operand : operands)
        patch->append(operand, Machine::ValueRep::ColdAny);
    if (result)
        patch->resultConstraints = { Machine::ValueRep::reg(result->reg) };

    uint32_t site = calls.addSite(value);
    std::span<const Frame> framesOfRow = m_proc.arena().copySpan<Frame>(frames.span());
    std::span<const uint32_t> slotsOfRow = m_proc.arena().copySpan<uint32_t>(slots.span());
    std::span<const Representation> formsOfRow = m_proc.arena().copySpan<Representation>(forms.span());
    patch->setGenerator([records = &m_records, conventions = &conventions(), program = &m_program, key, site, firstValue, framesOfRow, slotsOfRow, formsOfRow, arrivesAtHead, comesBack](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        auto valueAt = [&](uint32_t slot, size_t place) {
            return valueInLeaveRow(slot, formsOfRow[place], params[firstValue + place]);
        };
        auto writeRowOfFrames = [&] {
            LeaveRowWriter row(records->frameMaps.words, static_cast<unsigned>(framesOfRow.size()));
            size_t place = 0;
            for (const Frame& frame : framesOfRow) {
                row.beginFrame(frame.wordsOfFrame, frame.numberOfValues);
                for (uint32_t index = 0; index < frame.numberOfValues; ++index, ++place)
                    row.add(valueAt(slotsOfRow[place], place));
            }
            return row.finish();
        };
        // For each frame its cell of heap words, which the routine makes and fills, and then what enters the head: the entry parameters of the function, the number of the point, the cell.
        // The innermost is called. Any other waits in a frame of a stub until the slow body below it has returned.
        auto writeRowOfHeads = [&] {
            unsigned numberOfParts = 0;
            for (const Frame& frame : framesOfRow)
                numberOfParts += frame.numberOfHeapWords ? 2 : 1;
            LeaveRowWriter row(records->frameMaps.words, numberOfParts, comesBack ? EndOfLeave::ComesBack : EndOfLeave::ForGood);
            size_t place = 0;
            uint32_t stackWordsOfPartBelow = 0;
            for (const Frame& frame : framesOfRow) {
                bool isInnermost = &frame == &framesOfRow.front();
                ArgumentLocations locations = conventions->locationsOf(*program, frame.slowBody);
                size_t numberOfParameters = locations.parameters.size();
                RELEASE_ASSERT(numberOfParameters >= 2);
                Vector<std::optional<ValueInLeaveRow>, 8> arguments(numberOfParameters);
                size_t end = place + frame.numberOfValues;
                size_t firstWord = place;
                while (firstWord < end && (slotsOfRow[firstWord] & isArgumentOfLeaveAtHead))
                    ++firstWord;
                size_t endOfWords = end;
                std::optional<ValueInLeaveRow> thisOfConstruction;
                if (endOfWords > firstWord && slotsOfRow[endOfWords - 1] == isThisOfConstructionInLeaveAtHead)
                    thisOfConstruction = valueAt(0, --endOfWords);
                if (frame.numberOfHeapWords) {
                    row.beginHeapWords(frame.numberOfHeapWords, static_cast<uint32_t>(endOfWords - firstWord));
                    for (size_t word = firstWord; word < endOfWords; ++word)
                        row.add(valueAt(slotsOfRow[word] + 1, word));
                    arguments[numberOfParameters - 1] = ValueInLeaveRow { 0, FormInLeaveRow::Cell, PlaceInLeaveRow::HeapWords, 0 };
                } else
                    arguments[numberOfParameters - 1] = ValueInLeaveRow { 0, FormInLeaveRow::JSValue, PlaceInLeaveRow::Constant, static_cast<uint64_t>(JSValue::encode(jsUndefined())) };
                for (; place < firstWord; ++place) {
                    uint32_t parameter = slotsOfRow[place] & ~isArgumentOfLeaveAtHead;
                    RELEASE_ASSERT(parameter + 2 < numberOfParameters);
                    // What the convention does not pass the head does not read.
                    if (locations.parameters[parameter].kind != ArgumentLocation::Kind::None)
                        arguments[parameter] = valueAt(0, place);
                }
                arguments[numberOfParameters - 2] = ValueInLeaveRow { 0, FormInLeaveRow::JSValue, PlaceInLeaveRow::Constant, static_cast<uint64_t>(JSValue::encode(jsNumber(static_cast<int32_t>(frame.number)))) };
                if (isInnermost)
                    addCallOfHead(row, locations, arguments.span());
                else {
                    addFrameThatGoesOn(row, CallerThatGoesOn {
                        .locations = locations,
                        .arguments = arguments.span(),
                        .wordOfOperation = frame.wordOfOperation,
                        .thisOfConstruction = thisOfConstruction,
                        .formOfResultOfPartBelow = formInLeaveRowOf(conventions->resultRepresentation((&frame)[-1].slowBody)),
                        .stackWordsOfPartBelow = stackWordsOfPartBelow,
                    });
                }
                // As every caller of a unit of the function leaves it.
                stackWordsOfPartBelow = std::max(conventions->stackWordsReservedByCallersOf(*program, frame.slowBody), locations.numberOfStackWords());
                place = end;
            }
            return row.finish();
        };
        uint32_t indexOfRow = arrivesAtHead ? writeRowOfHeads() : writeRowOfFrames();

        // In the order of the pushes: the outermost frame's first.
        auto forEachAddress = [&](const Invocable<void()> auto& hasMoved) {
            auto moveAddressOfStubBehind = [&](StubKey stub, int32_t bytesInFront) {
                records->stubCalls.moveAddressOfStub(jit, stub, std::nullopt, rowOfLeaveGPR);
                jit.addPtr(CCallHelpers::TrustedImm32(bytesInFront), rowOfLeaveGPR);
            };
            for (size_t index = framesOfRow.size(); index--;) {
                const Frame& frame = framesOfRow[index];
                if (frame.slowBody.function)
                    records->stubCalls.moveAddressOfFunction(frame.slowBody, frame.entry, rowOfLeaveGPR);
                else
                    moveAddressOfStubBehind(selectResultOfConstruction(), bytesInFrontOfReturnIntoSelectResultOfConstruction);
                hasMoved();
                // Where the slow body below such a frame returns to.
                if (arrivesAtHead && index) {
                    moveAddressOfStubBehind(goOnInSlowBodyOfCaller(), bytesInFrontOfReturnIntoStubOfLeaving);
                    hasMoved();
                }
            }
        };
        // Eight bytes an address, the last one's lowest.
#if CPU(X86_64)
        forEachAddress([&] {
            jit.push(rowOfLeaveGPR);
        });
#else
        size_t numberOfAddresses = arrivesAtHead ? 2 * framesOfRow.size() - 1 : framesOfRow.size();
        jit.subPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(numberOfAddresses * sizeof(void*)))), CCallHelpers::stackPointerRegister);
        forEachAddress([&] {
            jit.storePtr(rowOfLeaveGPR, CCallHelpers::Address(CCallHelpers::stackPointerRegister, static_cast<int32_t>(--numberOfAddresses * sizeof(void*))));
        });
#endif
        jit.move(CCallHelpers::TrustedImm32(indexOfRow), rowOfLeaveGPR);
        records->stubCalls.call(jit, key, OptionalIndex { site });
        // A rare block lies at the end of its unit. Without a byte behind the call the return address, by which the run time finds the unit, would be the first of the next one.
        if (!comesBack)
            jit.breakpoint();
    });
    calls.finishCallPatchpoint(value, patch, site, request);
    // A value that its type alone says has no register.
    if (result && hasRegister(value.representation()))
        define(value, emitter<ConversionEmitter>().inRepresentation(value, Operand(result->representation, patch), typeOf(value.id()), value.representation()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
