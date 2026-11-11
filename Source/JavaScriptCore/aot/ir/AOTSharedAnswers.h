/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "JSExportMacros.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <span>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

enum class SharedQuestion : uint32_t {
    PartitionOfReceivers,
    StateOfFieldArm,
    NamedObjectsOfIntersection,
    NamedObjectsOfDifference,
    NamedObjectsOfUnionPartKind,
    RoundOverSet,
    ContentsOfArrayInData,
};
inline constexpr unsigned numberOfSharedQuestions = static_cast<unsigned>(SharedQuestion::ContentsOfArrayInData) + 1;
JS_EXPORT_PRIVATE ASCIILiteral nameOf(SharedQuestion);

class SharedAnswers {
    WTF_MAKE_NONCOPYABLE(SharedAnswers);
public:
#if ASSERT_ENABLED || ASAN_ENABLED
    static constexpr bool worksFoundAnswersOutAgain = true;
#else
    static constexpr bool worksFoundAnswersOutAgain = false;
#endif

    class Answer {
        WTF_MAKE_NONCOPYABLE(Answer);
    public:
        Answer() = default;
        virtual ~Answer() = default;
        virtual size_t sizeInBytes() const = 0;
    };

    struct Question {
        SharedQuestion kind { SharedQuestion::PartitionOfReceivers };
        uint32_t first { 0 };
        uint32_t second { 0 };
        uint32_t third { 0 };
        uint32_t fourth { 0 };
        uint32_t fifth { 0 };

        friend bool operator==(const Question&, const Question&) = default;
    };

    JS_EXPORT_PRIVATE SharedAnswers();
    JS_EXPORT_PRIVATE ~SharedAnswers();

    JS_EXPORT_PRIVATE uint32_t internList(std::span<const uint32_t>);
    JS_EXPORT_PRIVATE uint32_t internListWithHash(std::span<const uint32_t>, uint64_t hash);
    JS_EXPORT_PRIVATE std::span<const uint32_t> listAt(uint32_t number) const;

    JS_EXPORT_PRIVATE const Answer* find(const Question&) const;
    JS_EXPORT_PRIVATE const Answer& add(const Question&, std::unique_ptr<Answer>);

    template<typename AnswerType>
    const AnswerType& findOrAdd(const Question& question, NOESCAPE const auto& workOut)
    {
        if (const Answer* found = find(question)) {
            const auto& answer = static_cast<const AnswerType&>(*found);
            if constexpr (worksFoundAnswersOutAgain)
                RELEASE_ASSERT(*workOut() == answer);
            return answer;
        }
        return static_cast<const AnswerType&>(add(question, workOut()));
    }

    JS_EXPORT_PRIVATE size_t sizeInBytes() const;
    JS_EXPORT_PRIVATE void dumpCounts(PrintStream&) const;

private:
    static constexpr unsigned bitsOfShard = 6;
    static constexpr unsigned numberOfShards = 1u << bitsOfShard;

    struct InternedList {
        uint64_t hash { 0 };
        uint32_t position { 0 };
        Vector<uint32_t> words;
    };
    struct AddedAnswer {
        uint64_t hash { 0 };
        Question question;
        std::unique_ptr<Answer> answer;
    };
    template<typename Record>
    class Records {
    public:
        const Record* find(uint64_t hash, NOESCAPE const Invocable<bool(const Record&)> auto& isWanted) const
        {
            const Index* index = m_index.load(std::memory_order_acquire);
            if (!index)
                return nullptr;
            for (size_t slot = hash & index->mask;; slot = (slot + 1) & index->mask) {
                const Record* record = index->slots[slot].load(std::memory_order_acquire);
                if (!record)
                    return nullptr;
                if (record->hash == hash && isWanted(*record))
                    return record;
            }
        }

        const Record& at(size_t position) const
        {
            PlaceInSegments place = placeInSegments(position);
            return *m_segments[place.segment].load(std::memory_order_acquire)[place.offset].load(std::memory_order_acquire);
        }

        const Record& add(std::unique_ptr<Record> record)
        {
            PlaceInSegments place = placeInSegments(m_records.size());
            std::atomic<const Record*>* segment = m_segments[place.segment].load(std::memory_order_relaxed);
            if (!segment) {
                m_memoryOfSegments.append(std::unique_ptr<std::atomic<const Record*>[]>(new std::atomic<const Record*>[recordsInFirstSegment << place.segment]()));
                segment = m_memoryOfSegments.last().get();
                m_segments[place.segment].store(segment, std::memory_order_release);
            }
            segment[place.offset].store(record.get(), std::memory_order_release);

            Index* index = m_index.load(std::memory_order_relaxed);
            if (!index || 2 * (m_records.size() + 1) > index->mask + 1) {
                m_indexes.append(makeUniqueWithoutFastMallocCheck<Index>(std::max<size_t>(64, std::bit_ceil(4 * (m_records.size() + 1)))));
                index = m_indexes.last().get();
                for (const std::unique_ptr<Record>& earlier : m_records)
                    index->place(*earlier);
                m_index.store(index, std::memory_order_release);
            }
            index->place(*record);
            m_records.append(WTF::move(record));
            return *m_records.last();
        }
        std::span<const std::unique_ptr<Record>> all() const LIFETIME_BOUND { return m_records.span(); }
        size_t sizeInBytesWithoutRecords() const
        {
            size_t size = m_records.capacity() * sizeof(std::unique_ptr<Record>) + m_indexes.capacity() * sizeof(std::unique_ptr<Index>);
            for (size_t segment = 0; segment < m_memoryOfSegments.size(); ++segment)
                size += (recordsInFirstSegment << segment) * sizeof(std::atomic<const Record*>);
            for (const std::unique_ptr<Index>& index : m_indexes)
                size += sizeof(Index) + (index->mask + 1) * sizeof(std::atomic<const Record*>);
            return size;
        }

    private:
        struct Index {
            explicit Index(size_t numberOfSlots)
                : mask(numberOfSlots - 1)
                , slots(new std::atomic<const Record*>[numberOfSlots]())
            {
            }
            void place(const Record& record)
            {
                size_t slot = record.hash & mask;
                while (slots[slot].load(std::memory_order_relaxed))
                    slot = (slot + 1) & mask;
                slots[slot].store(&record, std::memory_order_release);
            }

            size_t mask;
            std::unique_ptr<std::atomic<const Record*>[]> slots;
        };

        static constexpr size_t recordsInFirstSegment = 64;
        struct PlaceInSegments {
            unsigned segment;
            size_t offset;
        };
        static PlaceInSegments placeInSegments(size_t position)
        {
            unsigned segment = std::bit_width(position / recordsInFirstSegment + 1) - 1;
            return { segment, position - ((static_cast<size_t>(1) << segment) - 1) * recordsInFirstSegment };
        }

        std::array<std::atomic<std::atomic<const Record*>*>, 32> m_segments { };
        Vector<std::unique_ptr<std::atomic<const Record*>[]>> m_memoryOfSegments;
        std::atomic<Index*> m_index { nullptr };
        Vector<std::unique_ptr<Index>> m_indexes;
        Vector<std::unique_ptr<Record>> m_records;
    };
    struct CountsOfQuestion {
        std::atomic<uint64_t> asked { 0 };
        std::atomic<uint64_t> found { 0 };
    };
    struct Shard {
        mutable Lock lock;
        Records<InternedList> lists;
        Records<AddedAnswer> answers;
        mutable std::array<CountsOfQuestion, numberOfSharedQuestions> counts { };
        uint8_t padding[64];
    };

    std::array<Shard, numberOfShards> m_shards;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
