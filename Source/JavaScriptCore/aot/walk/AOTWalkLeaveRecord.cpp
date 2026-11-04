/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkLeaveRecord.h"

#if ENABLE(AOT)

#include "AOTSlowFrames.h"

namespace JSC { namespace AOT { namespace Typed {

// Null: the value has no form that crosses, and the walk has failed.
Reference Walker::inFormThatCrosses(Reference value, Origin origin)
{
    // Its boxing hangs on the type, or allocates: cold code in the block that leaves.
    if (value && !crossesInItsForm(representationOf(value)))
        value = convert(value, Representation::JSValue, origin);
    if (!value || isUnreachable() || !crossesInItsForm(representationOf(value))) {
        fail("a value that a fast body is left with has no form that crosses"_s);
        return { };
    }
    return value;
}

bool Walker::endWithLeave(std::span<const uint32_t> words, std::span<const Reference> operands, Origin origin)
{
    Draft leave(Opcode::Leave, origin);
    leave.setFrames(words);
    leave.setValuesOperands(operands);
    chooseConservativeFields(leave);
    build(WTF::move(leave));
    if (!isUnreachable())
        endAsUnreachable(origin);
    return !hasFailed();
}

bool Walker::buildLeave(std::span<const LeaveFrame> frames, Origin origin)
{
    RELEASE_ASSERT(!frames.empty());
    origin.isTailCall = false;
    Vector<uint32_t, 32> words;
    Vector<Reference, 16> operands;
    for (const LeaveFrame& frame : frames) {
        RELEASE_ASSERT(frame.function ? frame.liveList.size() == frame.values.size() : frame.liveList.empty() && frame.values.size() == 1 && &frame != &frames.front() && &frame != &frames.back());
        words.append(frame.function ? frame.function.index() : functionOfFrameThatSelectsResult);
        words.append(frame.number);
        words.append(frame.entry);
        words.append(static_cast<uint32_t>(frame.values.size()));
        for (size_t index = 0; index < frame.values.size(); ++index) {
            RELEASE_ASSERT(index < 1 || frame.liveList[index - 1] < frame.liveList[index]);
            words.append(frame.function ? frame.liveList[index].index() : 0);
            Reference value = inFormThatCrosses(frame.values[index], origin);
            if (!value)
                return false;
            operands.append(value);
        }
    }
    return endWithLeave(words.span(), operands.span(), origin);
}

bool Walker::buildLeaveAtHead(const LeaveAtHead& leave, Origin origin)
{
    LeaveAtHeadInChain only { leave, std::nullopt, { } };
    buildLeaveOfChain(singleElementSpan(only), false, origin);
    return !hasFailed();
}

Reference Walker::buildLeaveOfChain(std::span<const LeaveAtHeadInChain> frames, bool comesBack, Origin origin)
{
    RELEASE_ASSERT(!frames.empty());
    origin.isTailCall = false;
    Vector<uint32_t, 32> words;
    Vector<Reference, 16> operands;
    for (const LeaveAtHeadInChain& frame : frames) {
        bool isInnermost = &frame == &frames.front();
        RELEASE_ASSERT(frame.function && frame.number <= static_cast<uint32_t>(INT32_MAX) && frame.indexesOfWords.size() == frame.words.size());
        RELEASE_ASSERT(!isInnermost || (!frame.wordOfOperation && !frame.thisOfConstruction));
        RELEASE_ASSERT(frames.size() == 1 || frame.numberOfWords);
        RELEASE_ASSERT(!frame.wordOfOperation || *frame.wordOfOperation < frame.numberOfWords);
        size_t head = words.size();
        size_t firstOperand = operands.size();
        words.append(frame.function.index());
        words.append(frame.number);
        words.append(frameOfLeaveArrivesAtHead);
        words.append(0);
        words.append(frame.numberOfWords);
        words.append(frame.wordOfOperation ? *frame.wordOfOperation + 1 : 0);
        auto take = [&](uint32_t word, Reference given) {
            Reference value = inFormThatCrosses(given, origin);
            if (!value)
                return false;
            words.append(word);
            operands.append(value);
            return true;
        };
        for (size_t parameter = 0; parameter < frame.arguments.size(); ++parameter) {
            if (frame.arguments[parameter] && !take(isArgumentOfLeaveAtHead | static_cast<uint32_t>(parameter), frame.arguments[parameter]))
                return { };
        }
        for (size_t place = 0; place < frame.words.size(); ++place) {
            RELEASE_ASSERT(frame.indexesOfWords[place] < frame.numberOfWords && (!place || frame.indexesOfWords[place - 1] < frame.indexesOfWords[place]));
            if (!take(frame.indexesOfWords[place], frame.words[place]))
                return { };
        }
        if (frame.thisOfConstruction && !take(isThisOfConstructionInLeaveAtHead, frame.thisOfConstruction))
            return { };
        words[head + 3] = static_cast<uint32_t>(operands.size() - firstOperand);
    }
    if (!comesBack) {
        endWithLeave(words.span(), operands.span(), origin);
        return { };
    }

    // Typed as a direct call of the slow body is.
    Draft leave(Opcode::LeaveCopies, origin);
    leave.setFunction(frames.back().function);
    leave.setRole(CodeRole::SlowBody);
    leave.setFrames(words.span());
    leave.setValuesOperands(operands.span());
    chooseConservativeFields(leave);
    return build(WTF::move(leave));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
