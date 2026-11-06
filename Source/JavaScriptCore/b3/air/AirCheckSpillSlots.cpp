/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirCheckSpillSlots.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirDominators.h"
#include "AirInstInlines.h"
#include "AirPhaseScope.h"
#include "B3Procedure.h"
#include "B3Value.h"
#include <algorithm>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace B3 { namespace Air {

static constexpr unsigned bytesPerWord = 4;

PlacesOfValues::PlacesOfValues(Code& code, Words words)
    : m_code(code)
    , m_words(words)
    , m_firstWordOfSlot(FillWith { }, code.stackSlots().size(), 0u)
{
    if (words == Words::OfSlots) {
        for (StackSlot* slot : code.stackSlots()) {
            m_firstWordOfSlot[slot->index()] = m_numberOfWords;
            if (slot->kind() == StackSlotKind::Spill)
                m_numberOfWords += (slot->byteSize() + bytesPerWord - 1) / bytesPerWord;
        }
    } else {
        int64_t highest = 0;
        for (StackSlot* slot : code.stackSlots()) {
            if (slot->kind() != StackSlotKind::Spill)
                continue;
            m_lowestByteOfFrame = std::min<int64_t>(m_lowestByteOfFrame, slot->offsetFromFP());
            highest = std::max<int64_t>(highest, slot->offsetFromFP() + static_cast<int64_t>(slot->byteSize()));
        }
        m_lowestByteOfFrame &= ~static_cast<int64_t>(bytesPerWord - 1);
        m_numberOfWords = (highest - m_lowestByteOfFrame + bytesPerWord - 1) / bytesPerWord;
    }
    m_firstEventOfBlock.reserveInitialCapacity(code.size() + 1);
}

void PlacesOfValues::beginBlock(BasicBlock* block)
{
    while (m_firstEventOfBlock.size() <= block->index())
        m_firstEventOfBlock.append(m_events.size());
}

void PlacesOfValues::addWords(StackSlot* slot, int64_t firstByte, unsigned numberOfBytes, unsigned instIndex, Kind kind, Holder holder)
{
    if (slot->kind() != StackSlotKind::Spill || !numberOfBytes)
        return;
    int64_t base = m_words == Words::OfSlots ? static_cast<int64_t>(m_firstWordOfSlot[slot->index()]) * bytesPerWord : slot->offsetFromFP() - m_lowestByteOfFrame;
    int64_t first = (base + firstByte) / bytesPerWord;
    int64_t last = (base + firstByte + numberOfBytes - 1) / bytesPerWord;
    RELEASE_ASSERT(first >= 0 && last < static_cast<int64_t>(m_numberOfWords));
    for (int64_t word = first; word <= last; ++word)
        m_events.append(Event { static_cast<unsigned>(word), instIndex, holder, kind });
}

void PlacesOfValues::addStackArgs(Inst& inst, unsigned instIndex, Kind kind)
{
    bool hasStackArg = false;
    for (Arg& arg : inst.args())
        hasStackArg |= arg.isStack();
    if (!hasStackArg)
        return;
    inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width width) {
        if (!arg.isStack())
            return;
        if (kind == Kind::Read && Arg::isAnyUse(role))
            addWords(arg.stackSlot(), arg.offset(), std::min<int64_t>(bytesForWidth(width), static_cast<int64_t>(arg.stackSlot()->byteSize()) - arg.offset()), instIndex, kind, valueOfSlot);
        // See the header: a write counts for the whole slot.
        if (kind == Kind::Write && Arg::isAnyDef(role))
            addWords(arg.stackSlot(), 0, arg.stackSlot()->byteSize(), instIndex, kind, valueOfSlot);
    });
}

void PlacesOfValues::addChangeOfValue(StackSlot* slot, unsigned instIndex)
{
    addWords(slot, 0, slot->byteSize(), instIndex, Kind::Write, somethingElse);
}

void PlacesOfValues::addRead(Reg reg, Tmp tmp, unsigned instIndex)
{
    m_events.append(Event { placeOf(reg), instIndex, static_cast<Holder>(tmp.internalValue()), Kind::Read });
}

void PlacesOfValues::addWrite(Reg reg, Tmp tmp, unsigned instIndex)
{
    m_events.append(Event { placeOf(reg), instIndex, static_cast<Holder>(tmp.internalValue()), Kind::Write });
}

void PlacesOfValues::addClobber(Reg reg, unsigned instIndex)
{
    m_events.append(Event { placeOf(reg), instIndex, somethingElse, Kind::Write });
}

void PlacesOfValues::fail(ASCIILiteral where, const ReadFromOutside& read, std::optional<Write> otherWrite, const Vector<unsigned>& path)
{
    bool isRegister = read.place >= m_numberOfWords;
    StringPrintStream out;
    out.print(where, ": ");
    if (isRegister)
        out.print(Reg::fromIndex(read.place - m_numberOfWords), " is read for ", Tmp::tmpForInternalValue(static_cast<int>(read.holder)));
    else if (m_words == Words::OfSlots) {
        for (StackSlot* slot : m_code.stackSlots()) {
            unsigned first = m_firstWordOfSlot[slot->index()];
            if (slot->kind() == StackSlotKind::Spill && first <= read.place && read.place < first + (slot->byteSize() + bytesPerWord - 1) / bytesPerWord)
                out.print(pointerDump(slot), ", bytes ", (read.place - first) * bytesPerWord, " to ", (read.place - first) * bytesPerWord + bytesPerWord - 1, ", is read");
        }
    } else {
        int64_t byte = m_lowestByteOfFrame + static_cast<int64_t>(read.place) * bytesPerWord;
        out.print("the bytes ", byte, " to ", byte + bytesPerWord - 1, " from the frame pointer, are read");
    }
    Inst& reader = m_code[read.blockIndex]->at(read.instIndex);
    out.print(" by `", reader, "` (#", read.blockIndex, ", instruction ", read.instIndex, "), ");
    if (otherWrite) {
        out.print("but `", m_code[otherWrite->blockIndex]->at(otherWrite->instIndex), "` (#", otherWrite->blockIndex, ", instruction ", otherWrite->instIndex, ") ");
        out.print(isRegister ? "has written the register, and nothing brings the value back on the path " : "has changed the value it stands for, and nothing stores it on the path ");
    } else
        out.print("and nothing writes it on the path ");
    for (unsigned i = 0; i < path.size(); ++i)
        out.print(i ? " > #" : "#", path[i]);
    m_code.proc().fail(isRegister ? FailureKind::RegisterLacksItsValue : FailureKind::SpillSlotLacksItsValue, reader.origin ? reader.origin->origin() : Origin(), out.toString());
}

bool PlacesOfValues::check(ASCIILiteral where)
{
    while (m_firstEventOfBlock.size() <= m_code.size())
        m_firstEventOfBlock.append(m_events.size());
    unsigned numberOfPlaces = m_numberOfWords + Reg::maxIndex() + 1;

    // What can be said inside a block, and what each block leaves behind for those that follow.
    enum class Local : uint8_t { Nothing, ReadFromOutside, Written };
    Vector<Local> local(FillWith { }, numberOfPlaces, Local::Nothing);
    Vector<Holder> holderOfLocal(FillWith { }, numberOfPlaces, valueOfSlot);
    Vector<unsigned> instOfLocal(FillWith { }, numberOfPlaces, 0u);
    Vector<unsigned, 32> touched;
    Vector<ReadFromOutside> readsFromOutside;
    Vector<std::pair<unsigned, Write>> lastWrites;
    for (BasicBlock* block : m_code) {
        unsigned blockIndex = block->index();
        for (unsigned i = m_firstEventOfBlock[blockIndex]; i < m_firstEventOfBlock[blockIndex + 1]; ++i) {
            const Event& event = m_events[i];
            Local& state = local[event.place];
            if (state == Local::Nothing)
                touched.append(event.place);
            if (event.kind == Kind::Write) {
                state = Local::Written;
                holderOfLocal[event.place] = event.holder;
                instOfLocal[event.place] = event.instIndex;
                continue;
            }
            if (state == Local::Written) {
                if (holderOfLocal[event.place] == event.holder)
                    continue;
                fail(where, { event.place, event.holder, blockIndex, event.instIndex }, Write { blockIndex, instOfLocal[event.place], holderOfLocal[event.place] }, { blockIndex });
                return false;
            }
            if (state == Local::ReadFromOutside && holderOfLocal[event.place] == event.holder)
                continue;
            readsFromOutside.append({ event.place, event.holder, blockIndex, event.instIndex });
            state = Local::ReadFromOutside;
            holderOfLocal[event.place] = event.holder;
        }
        for (unsigned place : touched) {
            if (local[place] == Local::Written)
                lastWrites.append({ place, Write { blockIndex, instOfLocal[place], holderOfLocal[place] } });
            local[place] = Local::Nothing;
        }
        touched.shrink(0);
    }
    if (readsFromOutside.isEmpty())
        return true;

    std::ranges::stable_sort(readsFromOutside, [](const ReadFromOutside& a, const ReadFromOutside& b) {
        return a.place != b.place ? a.place < b.place : a.holder < b.holder;
    });
    std::ranges::stable_sort(lastWrites, { }, [](const auto& entry) { return entry.first; });

    // For each place and each value that some block reads it for from outside: back from those blocks, through blocks that do not write the place. The time is that of the blocks in
    // which the value is live.
    Vector<unsigned> stampOfLastWrite(FillWith { }, m_code.size(), 0u);
    Vector<Write> lastWriteOf(m_code.size());
    Vector<unsigned> stampOfVisit(FillWith { }, m_code.size(), 0u);
    Vector<unsigned> follower(FillWith { }, m_code.size(), 0u);
    Vector<unsigned> instOfRead(FillWith { }, m_code.size(), 0u);
    Vector<unsigned, 32> worklist;
    unsigned stampOfPlace = 0;
    unsigned stamp = 0;
    size_t nextLastWrite = 0;
    std::optional<unsigned> placeOfStamp;
    size_t firstLastWriteOfPlace = 0;
    bool isEnteredAtOnePlace = m_code.numEntrypoints() <= 1 && m_code.isEntrypoint(m_code[0]);
    std::unique_ptr<Dominators> dominators;
    for (size_t first = 0; first < readsFromOutside.size();) {
        unsigned place = readsFromOutside[first].place;
        Holder holder = readsFromOutside[first].holder;
        if (placeOfStamp != place) {
            placeOfStamp = place;
            ++stampOfPlace;
            while (nextLastWrite < lastWrites.size() && lastWrites[nextLastWrite].first < place)
                ++nextLastWrite;
            firstLastWriteOfPlace = nextLastWrite;
            for (; nextLastWrite < lastWrites.size() && lastWrites[nextLastWrite].first == place; ++nextLastWrite) {
                stampOfLastWrite[lastWrites[nextLastWrite].second.blockIndex] = stampOfPlace;
                lastWriteOf[lastWrites[nextLastWrite].second.blockIndex] = lastWrites[nextLastWrite].second;
            }
        }
        // A place that one block alone writes, as a value that is computed and stored right behind is. Where that block dominates the block of a read and is another block, every path from the
        // entry to the read runs it whole, in whichever turn of a loop, and nothing else writes the place: going back, its last write is met first. This only ever says yes: the rest is walked.
        if (nextLastWrite - firstLastWriteOfPlace == 1 && isEnteredAtOnePlace && lastWrites[firstLastWriteOfPlace].second.holder == holder) {
            if (!dominators)
                dominators = makeUnique<Dominators>(m_code);
            BasicBlock* blockOfWrite = m_code[lastWrites[firstLastWriteOfPlace].second.blockIndex];
            size_t end = first;
            while (end < readsFromOutside.size() && readsFromOutside[end].place == place && readsFromOutside[end].holder == holder && dominators->strictlyDominates(blockOfWrite, m_code[readsFromOutside[end].blockIndex]))
                ++end;
            if (end == readsFromOutside.size() || readsFromOutside[end].place != place || readsFromOutside[end].holder != holder) {
                first = end;
                continue;
            }
        }
        ++stamp;
        worklist.shrink(0);
        for (; first < readsFromOutside.size() && readsFromOutside[first].place == place && readsFromOutside[first].holder == holder; ++first) {
            unsigned blockIndex = readsFromOutside[first].blockIndex;
            stampOfVisit[blockIndex] = stamp;
            follower[blockIndex] = blockIndex;
            instOfRead[blockIndex] = readsFromOutside[first].instIndex;
            worklist.append(blockIndex);
        }
        auto pathFrom = [&](unsigned blockIndex) {
            Vector<unsigned> path { blockIndex };
            while (follower[blockIndex] != blockIndex) {
                blockIndex = follower[blockIndex];
                path.append(blockIndex);
            }
            return path;
        };
        while (!worklist.isEmpty()) {
            unsigned blockIndex = worklist.takeLast();
            BasicBlock* block = m_code[blockIndex];
            if (block == m_code[0] || m_code.isEntrypoint(block)) {
                Vector<unsigned> path = pathFrom(blockIndex);
                fail(where, { place, holder, path.last(), instOfRead[path.last()] }, std::nullopt, path);
                return false;
            }
            for (BasicBlock* predecessor : block->predecessors()) {
                unsigned predecessorIndex = predecessor->index();
                if (stampOfLastWrite[predecessorIndex] == stampOfPlace) {
                    if (lastWriteOf[predecessorIndex].holder == holder)
                        continue;
                    Vector<unsigned> path = pathFrom(blockIndex);
                    unsigned blockOfRead = path.last();
                    path.insert(0, predecessorIndex);
                    fail(where, { place, holder, blockOfRead, instOfRead[blockOfRead] }, lastWriteOf[predecessorIndex], path);
                    return false;
                }
                if (stampOfVisit[predecessorIndex] == stamp)
                    continue;
                stampOfVisit[predecessorIndex] = stamp;
                follower[predecessorIndex] = blockIndex;
                worklist.append(predecessorIndex);
            }
        }
    }
    return true;
}

bool checkSpillSlotsAreWritten(Code& code, PlacesOfValues::Words words, ASCIILiteral where)
{
    PhaseScope phaseScope(code, "checkSpillSlotsAreWritten"_s);
    PlacesOfValues places(code, words);
    for (BasicBlock* block : code) {
        places.beginBlock(block);
        for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
            places.addReadsOfSlots(block->at(instIndex), instIndex);
            places.addWritesOfSlots(block->at(instIndex), instIndex);
        }
    }
    return places.check(where);
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
