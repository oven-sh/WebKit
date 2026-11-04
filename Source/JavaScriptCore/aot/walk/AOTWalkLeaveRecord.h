/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// One frame that a fast body leaves. The unit's own function, or a function that is copied in line in it.
struct LeaveFrame {
    // Null: no frame of a function. `new F()` stood as a copy in the fast body: between the frame of F and that of its caller lies a frame of two words that holds the `this` which the copy was entered with.
    // F returns into a stub that hands on what F has returned if that is an object, else `this`. liveList is empty, values holds `this` alone, and number and entry say nothing.
    FunctionID function;
    // In front of position p: 2 * p, and one more where p is the terminator's. The return into the call c, of a frame that is not the innermost: 0x80000000 | c.
    uint32_t number { 0 };
    // Of the slow unit: what stands for (function, number) there.
    uint16_t entry { 0 };
    // The live list of the point: input values of the function, ascending. And what stands for each where the walk stands.
    std::span<const ValueID> liveList;
    std::span<const Reference> values;
};

// The innermost first. It ends the block: nothing is built behind it.
// Whoever calls has made sure that the heap is as the written program has it at the point: that each value can cross. What waits for a call of unknown code is laid down by build(), as in front of any such call.
// A value goes in the form that it has. A form that the row of a point does not take is boxed here. False: a value has no form that crosses, nothing is built, and the walk has failed.
inline bool buildLeave(Walker& walker, std::span<const LeaveFrame> frames, Origin origin) { return walker.buildLeave(frames, origin); }

// The slow body of the unit's own function is entered AT ITS HEAD, as a guard jumps on to a body: it takes the entry parameters of the function, then the number of the point and a cell of heap words, and switches on the number.
// The routine makes the cell and fills it, boxed, takes the fast frame down, and the head returns to whoever called the fast body. In the place of NewHeapWords, a PutHeapFrameWord for each word, the boxing, CallDirect and Return.
struct LeaveAtHead {
    FunctionID function;
    uint32_t number { 0 };
    // Of the cell. The words are the frame of the slow body: each input value has its own, and only those of the live list are filled.
    uint32_t numberOfWords { 0 };
    // One for each entry parameter of the function, in their order. Null: the way of the point never reads the parameter, which holds garbage.
    std::span<const Reference> arguments;
    // Which words are filled, ascending, and with what.
    std::span<const uint32_t> indexesOfWords;
    std::span<const Reference> words;
};
// As buildLeave(): it ends the block, and each value goes in the form that it has.
inline bool buildLeaveAtHead(Walker& walker, const LeaveAtHead& leave, Origin origin) { return walker.buildLeaveAtHead(leave, origin); }

// One frame of a chain of copies that is left by words. Each frame has a cell of its own, which is made and filled at the leave, and each is entered at the head of its slow body:
// the innermost at once, any other when the slow body of the copy that stood in it has returned.
struct LeaveAtHeadInChain : LeaveAtHead {
    // Of a frame that is not the innermost: the word of the operation in whose place the copy stood: what the slow body of the copy returns goes there. Nothing: nobody reads the operation, or it is a store.
    std::optional<uint32_t> wordOfOperation;
    // Of a frame that is not the innermost: null unless the copy stood for a construction: the value of the operation is what the copy returns if that is an object, else this.
    Reference thisOfConstruction;
};
// The innermost first.
// For good: the outermost frame is the unit's own. It ends the block, as buildLeaveAtHead() does, and gives null.
// It comes back: the outermost frame is a copy's, and the fast body goes on behind that copy. The value of the record is what the slow body of that copy returns, typed as a direct call of it: a call of unknown code like any other.
// Null there: the walk has failed.
inline Reference buildLeaveOfChain(Walker& walker, std::span<const LeaveAtHeadInChain> frames, bool comesBack, Origin origin) { return walker.buildLeaveOfChain(frames, comesBack, origin); }

// The words of Operation::frames(), frame by frame.
inline constexpr uint32_t functionOfFrameThatSelectsResult = UINT32_MAX;
// In the third word, beside the entry: the slow body is entered at its head. So are all frames of the record or none. Two more words stand in front of the list of such a frame:
// the number of heap words of its cell, and one more than the word of the operation in whose place the copy above stood (0: none).
// Its list: the arguments, each isArgumentOfLeaveAtHead | the entry parameter, ascending, then the heap words that are filled, ascending, then isThisOfConstructionInLeaveAtHead where the copy stood for a construction.
inline constexpr uint32_t frameOfLeaveArrivesAtHead = 1u << 16;
inline constexpr uint32_t isArgumentOfLeaveAtHead = 1u << 31;
inline constexpr uint32_t isThisOfConstructionInLeaveAtHead = 1u << 30;
struct FrameOfLeave {
    // Null: the frame that selects the result of a construction. liveList has one word, which says nothing.
    FunctionID function;
    uint32_t number;
    uint16_t entry;
    std::span<const uint32_t> liveList;
    bool arrivesAtHead { false };
    uint32_t numberOfHeapWords { 0 };
    std::optional<uint32_t> wordOfOperation { };
};
template<typename Functor>
void forEachFrameOfLeave(std::span<const uint32_t> words, const Functor& functor)
{
    while (!words.empty()) {
        RELEASE_ASSERT(words.size() >= 4);
        bool arrivesAtHead = words[2] & frameOfLeaveArrivesAtHead;
        size_t head = arrivesAtHead ? 6 : 4;
        RELEASE_ASSERT(words.size() >= head && words.size() - head >= words[3]);
        functor(FrameOfLeave { words[0] == functionOfFrameThatSelectsResult ? FunctionID() : FunctionID(words[0]), words[1], static_cast<uint16_t>(words[2]), words.subspan(head, words[3]), arrivesAtHead, arrivesAtHead ? words[4] : 0, arrivesAtHead && words[5] ? std::optional<uint32_t>(words[5] - 1) : std::nullopt });
        words = words.subspan(head + words[3]);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
