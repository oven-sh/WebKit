/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTLeaveRows.h"
#include "AOTMachine.h"
#include "AOTRepresentation.h"
#include "AOTSavedFrame.h"
#include "AOTStubsLeaving.h"

namespace JSC { namespace AOT { namespace Typed {

// Nothing: the block that leaves boxes the value itself (ConversionEmitter::box()), or the point is no point.
constexpr std::optional<FormInLeaveRow> formInLeaveRowOf(Representation representation)
{
    switch (representation) {
    case Representation::JSValue:
        return FormInLeaveRow::JSValue;
    case Representation::Cell:
        return FormInLeaveRow::Cell;
    case Representation::Int32:
        return FormInLeaveRow::Int32;
    case Representation::Boolean:
        return FormInLeaveRow::Boolean;
    case Representation::Double:
        return FormInLeaveRow::Double;
    case Representation::Int64:
        return FormInLeaveRow::Int64;
    default:
        return std::nullopt;
    }
}

// In the generator of the Patch that leaves: `place` is PatchParameters[child] of an operand that was appended as ColdAny.
inline ValueInLeaveRow valueInLeaveRow(uint32_t slot, Representation representation, const Machine::ValueRep& place)
{
    std::optional<FormInLeaveRow> form = formInLeaveRowOf(representation);
    RELEASE_ASSERT(form);
    if (place.isReg())
        return { slot, *form, PlaceInLeaveRow::Register, place.reg().index() };
    if (place.isStack()) {
        intptr_t offset = place.offsetFromFP();
        RELEASE_ASSERT(offset < 0 && !(offset % static_cast<intptr_t>(sizeof(uint32_t))));
        return { slot, *form, PlaceInLeaveRow::Stack, static_cast<uint64_t>(-offset) / sizeof(uint32_t) };
    }
    RELEASE_ASSERT(place.isConstant());
    return { slot, *form, PlaceInLeaveRow::Constant, static_cast<uint64_t>(place.value()) };
}

// The part Call of a row: a unit is entered at its head. One address is pushed for it: the primary entry of the unit.
// `arguments`: for each parameter of `locations` what it takes, whose slot says nothing here. Nothing: the way that the unit goes never reads the parameter, which holds garbage.
inline void addCallOfHead(LeaveRowWriter& row, const ArgumentLocations& locations, std::span<const std::optional<ValueInLeaveRow>> arguments)
{
    RELEASE_ASSERT(locations.convention == EntryConvention::Direct && arguments.size() == locations.parameters.size());
    Vector<ValueInLeaveRow, 8> values;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (!arguments[index])
            continue;
        const ArgumentLocation& location = locations.parameters[index];
        ValueInLeaveRow value = *arguments[index];
        // The routine boxes whatever it hands over.
        RELEASE_ASSERT(location.representation == Representation::JSValue || (location.representation == Representation::Cell && value.form == FormInLeaveRow::Cell));
        switch (location.kind) {
        case ArgumentLocation::Kind::GPR:
            value.slot = location.reg.index() + 1;
            break;
        case ArgumentLocation::Kind::Stack:
            RELEASE_ASSERT(location.stackIndex < locations.numberOfStackArguments);
            value.slot = firstSlotOfStackWordInLeaveRow + location.stackIndex;
            break;
        case ArgumentLocation::Kind::None:
        case ArgumentLocation::Kind::FPR:
            RELEASE_ASSERT_NOT_REACHED();
        }
        values.append(value);
    }
    std::ranges::sort(values, [](const ValueInLeaveRow& left, const ValueInLeaveRow& right) {
        return left.slot < right.slot;
    });
    row.beginCall(static_cast<uint32_t>(values.size()));
    for (const ValueInLeaveRow& value : values)
        row.add(value);
}

// A caller in a chain of copies that is left by words: what its frame of the row holds until the slow body of the callee has returned (FrameThatGoesOn of stubs/AOTStubsLeaving.h).
struct CallerThatGoesOn {
    // Of the caller's slow body, and what each of its parameters takes, as for addCallOfHead().
    const ArgumentLocations& locations;
    std::span<const std::optional<ValueInLeaveRow>> arguments;
    // Of the operation in whose place the copy stood, in the caller's heap words. Nothing: what the callee returns is dropped: nobody reads the operation, or it is a store.
    std::optional<uint32_t> wordOfOperation;
    // The copy stood for a construction: the value of the operation is what the callee returns if that is an object, else this. Its slot says nothing here.
    std::optional<ValueInLeaveRow> thisOfConstruction;
    // Of the part below, the callee's slow body: formInLeaveRowOf(conventions.resultRepresentation(unit)): a form that the row does not take makes the chain no point. It has one result.
    // It has no default: whoever leaves it out stops the build, and stores no raw bits into a word.
    std::optional<FormInLeaveRow> formOfResultOfPartBelow;
    // And its ArgumentLocations::numberOfStackArguments.
    uint32_t stackWordsOfPartBelow { 0 };
};

// The part Frame of such a caller. The part HeapWords of its cell stands right in front.
// TWO ADDRESSES ARE PUSHED FOR IT, in this order: the primary entry of the caller's slow body, then goOnInSlowBodyOfCaller() and bytesInFrontOfReturnIntoStubOfLeaving more.
inline void addFrameThatGoesOn(LeaveRowWriter& row, const CallerThatGoesOn& caller)
{
    const ArgumentLocations& locations = caller.locations;
    RELEASE_ASSERT(locations.convention == EntryConvention::Direct && caller.arguments.size() == locations.parameters.size());
    auto raw = [](unsigned slot, uint64_t bits) {
        return ValueInLeaveRow { slot, FormInLeaveRow::JSValue, PlaceInLeaveRow::Constant, bits };
    };
    Vector<ValueInLeaveRow, 16> values;
    values.append({ FrameThatGoesOn::slotOfHead, FormInLeaveRow::JSValue, PlaceInLeaveRow::Address, 0 });
    values.append({ FrameThatGoesOn::slotOfCell, FormInLeaveRow::Cell, PlaceInLeaveRow::HeapWords, 0 });
    values.append(raw(FrameThatGoesOn::slotOfOffsetOfWord, caller.wordOfOperation ? SavedFrame::offsetOfWords() + *caller.wordOfOperation * sizeof(uint64_t) : 0));
    values.append(caller.thisOfConstruction.value_or(raw(0, 0)));
    RELEASE_ASSERT(values.last().form == FormInLeaveRow::JSValue || values.last().form == FormInLeaveRow::Cell);
    values.last().slot = FrameThatGoesOn::slotOfThis;
    RELEASE_ASSERT(caller.formOfResultOfPartBelow);
    values.append(raw(FrameThatGoesOn::slotOfFormOfResult, static_cast<uint64_t>(*caller.formOfResultOfPartBelow)));
    values.append(raw(FrameThatGoesOn::slotOfNumberOfStackWords, locations.numberOfStackArguments));
    for (size_t index = 0; index < caller.arguments.size(); ++index) {
        if (!caller.arguments[index])
            continue;
        const ArgumentLocation& location = locations.parameters[index];
        ValueInLeaveRow value = *caller.arguments[index];
        RELEASE_ASSERT(location.representation == Representation::JSValue || (location.representation == Representation::Cell && value.form == FormInLeaveRow::Cell));
        if (location.kind == ArgumentLocation::Kind::Stack) {
            RELEASE_ASSERT(location.stackIndex < locations.numberOfStackArguments);
            value.slot = FrameThatGoesOn::firstSlotOfStackWords + location.stackIndex;
        } else {
            RELEASE_ASSERT(location.kind == ArgumentLocation::Kind::GPR);
            unsigned place = 0;
            while (place < numberOfDirectArgumentGPRs && directArgumentGPR(place) != location.reg.gpr())
                ++place;
            RELEASE_ASSERT(place < numberOfDirectArgumentGPRs);
            value.slot = FrameThatGoesOn::firstSlotOfRegisters + place;
        }
        values.append(value);
    }
    std::ranges::sort(values, [](const ValueInLeaveRow& left, const ValueInLeaveRow& right) {
        return left.slot < right.slot;
    });
    row.beginFrame(FrameThatGoesOn::wordsFor(locations.numberOfStackArguments, caller.stackWordsOfPartBelow), static_cast<uint32_t>(values.size()));
    for (const ValueInLeaveRow& value : values)
        row.add(value);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
