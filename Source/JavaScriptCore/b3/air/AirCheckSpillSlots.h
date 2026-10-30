/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirArg.h"
#include "AirBasicBlock.h"
#include "AirStackSlot.h"
#include "AirTmp.h"
#include "Reg.h"
#include <wtf/Vector.h>

namespace JSC { namespace B3 { namespace Air {

class Code;

// What happens to the places that register allocation keeps values in, in the order of the program. A place is a word (four bytes) of a spill slot, or a register.
//
// Every read of a place has to see the value that it is read for, on every path from the entry. So, going back from the read along each path, the first thing met has to
// be a write of that value. It is an error to meet the entry (nothing has written the place) or a write of something else.
// - A spill slot is read for the value that it stands for. Something else is in it once that value has been computed anew into a register and not stored.
// - A register is read for a Tmp. Something else is in it once another Tmp that has the same register has been defined, or an instruction has clobbered it.
// Only the register allocator can say which registers hold the value of which slot, and which Tmp has which register.
//
// A write through a slot counts for the whole slot, however narrow it is, and a read may be wider than any write and still be right:
// - A 32-bit write that extends with zeros (ZDef) into a slot of eight bytes does write all eight: lowerStackArgs stores a zero into the upper four behind it. The upper four are
//   looked at. A flag is stored by Move32, copied from slot to slot by a 64-bit Move and made a boolean by Or64, the Move32 that extended it having been coalesced away.
// - A double is stored by MoveDouble into the sixteen bytes that a procedure with vectors gives every slot of its bank, and read by MoveVector. What reads the register is
//   scalar, and the upper lanes are as undefined in a register.
// So a store that is too narrow for what is made of the value later is not found here.
class PlacesOfValues {
public:
    // Behind stack allocation slots share bytes, and a write through one slot is a write for the other.
    enum class Words : bool { OfSlots, OfFrame };

    JS_EXPORT_PRIVATE PlacesOfValues(Code&, Words);

    // The blocks are given in the order of their indexes, and what happens in a block in the order in which it happens.
    JS_EXPORT_PRIVATE void beginBlock(BasicBlock*);

    // The reads, and the writes, of the stack arguments of an instruction.
    void addReadsOfSlots(Inst& inst, unsigned instIndex) { addStackArgs(inst, instIndex, Kind::Read); }
    void addWritesOfSlots(Inst& inst, unsigned instIndex) { addStackArgs(inst, instIndex, Kind::Write); }
    // The instruction computes the value that the slot stands for anew, into a register.
    JS_EXPORT_PRIVATE void addChangeOfValue(StackSlot*, unsigned instIndex);

    JS_EXPORT_PRIVATE void addRead(Reg, Tmp, unsigned instIndex);
    JS_EXPORT_PRIVATE void addWrite(Reg, Tmp, unsigned instIndex);
    JS_EXPORT_PRIVATE void addClobber(Reg, unsigned instIndex);

    // False, and the procedure has failed with FailureKind::SpillSlotLacksItsValue or RegisterLacksItsValue, if some read does not see its value. The failure's detail names the
    // place, the instruction that reads it, the instruction that wrote something else, and a path.
    JS_EXPORT_PRIVATE bool check(ASCIILiteral where);

private:
    enum class Kind : uint8_t { Read, Write };
    // What a place holds: the value of the slot, a Tmp by its internal value (never zero), or something that nobody reads it for.
    using Holder = unsigned;
    static constexpr Holder valueOfSlot = 0;
    static constexpr Holder somethingElse = std::numeric_limits<unsigned>::max();

    struct Event {
        unsigned place;
        unsigned instIndex;
        Holder holder;
        Kind kind;
    };
    struct Write {
        unsigned blockIndex;
        unsigned instIndex;
        Holder holder;
    };
    struct ReadFromOutside {
        unsigned place;
        Holder holder;
        unsigned blockIndex;
        unsigned instIndex;
    };

    JS_EXPORT_PRIVATE void addStackArgs(Inst&, unsigned instIndex, Kind);
    void addWords(StackSlot*, int64_t firstByte, unsigned numberOfBytes, unsigned instIndex, Kind, Holder);
    unsigned placeOf(Reg reg) const { return m_numberOfWords + reg.index(); }
    void fail(ASCIILiteral where, const ReadFromOutside&, std::optional<Write> otherWrite, const Vector<unsigned>& path);

    Code& m_code;
    Words m_words;
    int64_t m_lowestByteOfFrame { 0 };
    unsigned m_numberOfWords { 0 };
    Vector<unsigned> m_firstWordOfSlot;
    Vector<Event> m_events;
    Vector<unsigned> m_firstEventOfBlock;
};

// By the stack arguments alone: every word of a spill slot that is read has been written on every path from the entry.
JS_EXPORT_PRIVATE bool checkSpillSlotsAreWritten(Code&, PlacesOfValues::Words, ASCIILiteral where);

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
