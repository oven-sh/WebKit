/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// The row of a point at which a fast body leaves for good: where each value of the live list lies in the fast body, in which form, and into which slot of which slow frame it goes.
// Bytes, packed into the words of the unit that are written while it is encoded: CodeRecords::frameMaps.words, which Image::frameMapWords() hands back. A row begins at a word.
//
//     row    = varint (numberOfParts << 1 | comesBack) . part, the innermost first
//     part   = varint (words << 2 | kind) . varint numberOfValues . value, ascending by slot
//     value  = byte (place & 3 | form << 2 | slotFollowsLast << 5 | place >> 2 << 6) . varint (slot - last slot - 1) unless slotFollowsLast . what the place takes
enum class EndOfLeave : uint8_t {
    // The fast frame is taken down, and what arrives stands in its place, under its return address.
    ForGood,
    // The fast frame stays, and what arrives stands below it as if the fast body had called it: the outermost part returns to where the stub was called from, with the stack pointer as it was in front of the pushes.
    // What the outermost part takes on the stack lies in the lowest words of the fast frame, as at any call.
    ComesBack,
};

enum class PartOfLeaveRow : uint8_t {
    // A frame that the routine builds: of a slow body that is entered in the middle or returned into, or of a stub. `words`: below the frame record, a multiple of 2.
    // A slot: words below the frame pointer of the frame, first word 1.
    Frame,
    // A unit is entered at its head, as a call enters it, and sets up its frame itself. In front of every frame. `words` is 0.
    // A slot: Reg::index() + 1 of the register that takes the value, or firstSlotOfStackWordInLeaveRow and the number of the stack word that does: the words right above the address that the unit returns to.
    // Under no frame it takes the place of the call that entered the fast body, as where a guard jumps on to a body: the callers of all units of a function leave the same room. Under a frame the stack words are the lowest of that frame.
    Call,
    // A cell of `words` heap words, which the routine makes as NewHeapWords does. It stands in front of the frame or the call that it belongs to, and no address is pushed for it. A slot: the word + 1.
    HeapWords,
};

enum class PlaceInLeaveRow : uint8_t {
    // A byte: Reg::index().
    Register,
    // A varint: units of four bytes below the frame pointer of the fast frame: a value of 32 bits may lie in half a word.
    Stack,
    // A varint: the 64 bits as the form has them, signs folded into the lowest bit.
    Constant,
    // Nothing: the cell of the part HeapWords that stands in front of this part. Its form is Cell.
    HeapWords,
    // Nothing: the next address that the leaving code has pushed. Its form is JSValue: it is handed on bit for bit.
    // THE ADDRESSES OF A ROW, in the order in which its reader meets them, which is the reverse of the order in which they are pushed: for each part that is a frame or a call one of its own
    // (of the innermost: where it is entered; of any other: where the part below it returns to), and behind it one for each value of the part that has this place.
    Address,
};

// The forms that are boxed by arithmetic alone. Whatever has another form is boxed in the block that leaves.
enum class FormInLeaveRow : uint8_t { JSValue, Cell, Int32, Boolean, Double, Int64 };
constexpr bool has32Bits(FormInLeaveRow form) { return form == FormInLeaveRow::Int32 || form == FormInLeaveRow::Boolean; }

inline constexpr unsigned maxFramesInLeaveRow = 255;
inline constexpr uint32_t maxWordsInLeaveRow = 1u << 28;
inline constexpr uint32_t firstSlotOfStackWordInLeaveRow = 257;

struct ValueInLeaveRow {
    uint32_t slot;
    FormInLeaveRow form;
    PlaceInLeaveRow place;
    // Reg::index(), units of four bytes below the frame pointer, or the bits.
    uint64_t where;
};

class LeaveRowWriter {
public:
    LeaveRowWriter(Vector<uint32_t>& words, unsigned numberOfParts, EndOfLeave end = EndOfLeave::ForGood)
        : m_words(words)
        , m_framesLeft(numberOfParts)
    {
        RELEASE_ASSERT(numberOfParts && numberOfParts <= maxFramesInLeaveRow);
        appendVarint(numberOfParts << 1 | static_cast<unsigned>(end));
    }

    void beginFrame(uint32_t wordsOfFrame, uint32_t numberOfValues)
    {
        RELEASE_ASSERT(!(wordsOfFrame % 2));
        m_hasFrame = true;
        m_cellIsAtHand = std::exchange(m_lastIsHeapWords, false);
        beginPart(PartOfLeaveRow::Frame, wordsOfFrame, wordsOfFrame, numberOfValues);
    }

    void beginCall(uint32_t numberOfValues)
    {
        RELEASE_ASSERT(!m_hasCall && !m_hasFrame);
        m_hasCall = true;
        m_cellIsAtHand = std::exchange(m_lastIsHeapWords, false);
        beginPart(PartOfLeaveRow::Call, 0, firstSlotOfStackWordInLeaveRow + maxWordsInLeaveRow, numberOfValues);
    }

    void beginHeapWords(uint32_t numberOfWords, uint32_t numberOfValues)
    {
        RELEASE_ASSERT(numberOfWords && !m_lastIsHeapWords);
        m_lastIsHeapWords = true;
        m_cellIsAtHand = false;
        beginPart(PartOfLeaveRow::HeapWords, numberOfWords, numberOfWords, numberOfValues);
    }

    void add(const ValueInLeaveRow& value)
    {
        RELEASE_ASSERT(m_valuesLeft && value.slot > m_lastSlot && value.slot <= m_wordsOfFrame);
        --m_valuesLeft;
        bool slotFollowsLast = value.slot == m_lastSlot + 1;
        unsigned place = static_cast<unsigned>(value.place);
        m_bytes.append(static_cast<uint8_t>((place & 3) | static_cast<unsigned>(value.form) << 2 | static_cast<unsigned>(slotFollowsLast) << 5 | place >> 2 << 6));
        if (!slotFollowsLast)
            appendVarint(value.slot - m_lastSlot - 1);
        m_lastSlot = value.slot;
        switch (value.place) {
        case PlaceInLeaveRow::Register:
            RELEASE_ASSERT(value.where <= UINT8_MAX);
            m_bytes.append(static_cast<uint8_t>(value.where));
            return;
        case PlaceInLeaveRow::Stack:
            RELEASE_ASSERT(value.where && value.where <= 2 * maxWordsInLeaveRow && (has32Bits(value.form) || !(value.where % 2)));
            appendVarint(value.where);
            return;
        case PlaceInLeaveRow::Constant:
            appendVarint(value.where << 1 ^ static_cast<uint64_t>(static_cast<int64_t>(value.where) >> 63));
            return;
        case PlaceInLeaveRow::HeapWords:
            RELEASE_ASSERT(m_cellIsAtHand && value.form == FormInLeaveRow::Cell);
            return;
        case PlaceInLeaveRow::Address:
            RELEASE_ASSERT(!m_lastIsHeapWords && value.form == FormInLeaveRow::JSValue);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The index of the first word of the row among the words of the unit.
    uint32_t finish()
    {
        RELEASE_ASSERT(!m_framesLeft && !m_valuesLeft && (m_hasCall || m_hasFrame) && !m_lastIsHeapWords);
        uint32_t first = static_cast<uint32_t>(m_words.size());
        while (m_bytes.size() % sizeof(uint32_t))
            m_bytes.append(0);
        m_words.grow(first + m_bytes.size() / sizeof(uint32_t));
        memcpy(m_words.mutableSpan().subspan(first).data(), m_bytes.span().data(), m_bytes.size());
        return first;
    }

private:
    void beginPart(PartOfLeaveRow kind, uint32_t words, uint32_t lastSlot, uint32_t numberOfValues)
    {
        RELEASE_ASSERT(m_framesLeft && !m_valuesLeft && words <= maxWordsInLeaveRow && numberOfValues <= lastSlot);
        --m_framesLeft;
        m_valuesLeft = numberOfValues;
        m_wordsOfFrame = lastSlot;
        m_lastSlot = 0;
        appendVarint(static_cast<uint64_t>(words) << 2 | static_cast<uint64_t>(kind));
        appendVarint(numberOfValues);
    }

    void appendVarint(uint64_t value)
    {
        for (; value >= 0x80; value >>= 7)
            m_bytes.append(static_cast<uint8_t>(value | 0x80));
        m_bytes.append(static_cast<uint8_t>(value));
    }

    Vector<uint32_t>& m_words;
    Vector<uint8_t, 64> m_bytes;
    unsigned m_framesLeft;
    uint32_t m_valuesLeft { 0 };
    uint32_t m_wordsOfFrame { 0 };
    uint32_t m_lastSlot { 0 };
    bool m_lastIsHeapWords { false };
    bool m_cellIsAtHand { false };
    bool m_hasCall { false };
    bool m_hasFrame { false };
};

class LeaveRowReader {
public:
    // From the first word of the row to the end of the words of the unit.
    explicit LeaveRowReader(std::span<const uint32_t> words)
        : m_bytes(asBytes(words))
    {
        uint64_t head = readVarint();
        m_end = static_cast<EndOfLeave>(head & 1);
        m_framesLeft = static_cast<unsigned>(head >> 1);
        RELEASE_ASSERT(m_framesLeft && m_framesLeft <= maxFramesInLeaveRow);
    }

    EndOfLeave end() const { return m_end; }

    unsigned partsLeft() const { return m_framesLeft; }
    uint32_t valuesLeft() const { return m_valuesLeft; }

    struct Part {
        PartOfLeaveRow kind;
        uint32_t words;
    };

    Part beginPart()
    {
        RELEASE_ASSERT(m_framesLeft && !m_valuesLeft);
        --m_framesLeft;
        uint64_t head = readVarint();
        uint64_t words = head >> 2;
        uint64_t numberOfValues = readVarint();
        RELEASE_ASSERT((head & 3) <= static_cast<uint64_t>(PartOfLeaveRow::HeapWords) && words <= maxWordsInLeaveRow && numberOfValues <= firstSlotOfStackWordInLeaveRow + maxWordsInLeaveRow);
        m_valuesLeft = static_cast<uint32_t>(numberOfValues);
        m_lastSlot = 0;
        return { static_cast<PartOfLeaveRow>(head & 3), static_cast<uint32_t>(words) };
    }

    ValueInLeaveRow next()
    {
        RELEASE_ASSERT(m_valuesLeft);
        --m_valuesLeft;
        uint8_t head = readByte();
        ValueInLeaveRow value;
        value.place = static_cast<PlaceInLeaveRow>((head & 3) | (head >> 6 & 1) << 2);
        value.form = static_cast<FormInLeaveRow>(head >> 2 & 7);
        RELEASE_ASSERT(value.place <= PlaceInLeaveRow::Address && value.form <= FormInLeaveRow::Int64 && !(head >> 7));
        m_lastSlot += 1 + (head >> 5 & 1 ? 0 : static_cast<uint32_t>(readVarint()));
        value.slot = m_lastSlot;
        switch (value.place) {
        case PlaceInLeaveRow::Register:
            value.where = readByte();
            break;
        case PlaceInLeaveRow::Stack:
            value.where = readVarint();
            break;
        case PlaceInLeaveRow::Constant: {
            uint64_t folded = readVarint();
            value.where = folded >> 1 ^ (0 - (folded & 1));
            break;
        }
        case PlaceInLeaveRow::HeapWords:
        case PlaceInLeaveRow::Address:
            value.where = 0;
            break;
        }
        return value;
    }

private:
    uint8_t readByte()
    {
        RELEASE_ASSERT(!m_bytes.empty());
        uint8_t byte = m_bytes.front();
        m_bytes = m_bytes.subspan(1);
        return byte;
    }

    uint64_t readVarint()
    {
        uint64_t value = 0;
        for (unsigned shift = 0;; shift += 7) {
            RELEASE_ASSERT(shift < 64);
            uint8_t byte = readByte();
            value |= static_cast<uint64_t>(byte & 0x7f) << shift;
            if (!(byte & 0x80))
                return value;
        }
    }

    std::span<const uint8_t> m_bytes;
    EndOfLeave m_end { EndOfLeave::ForGood };
    unsigned m_framesLeft { 0 };
    uint32_t m_valuesLeft { 0 };
    uint32_t m_lastSlot { 0 };
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
