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

// Who asks SharedAnswers. Whoever takes a number writes it here, and its name in nameOf().
enum class SharedQuestion : uint32_t {
    // walk/AOTAccessPlan.cpp
    PartitionOfReceivers,
    PlanOfNamedAccess,
    // walk/AOTAccessEffects.cpp, in the arena alone: TypeArena::keepAnswerUntilReset().
    EffectsOfNamedAccess,
    // walk/AOTAccessPlanOfFields.cpp
    StateOfFieldArm,
    TestsThatFieldArmInheritsNothing,
    // plan/AOTSlotPlan.cpp
    FieldAccessOfReceivers,
    // walk/AOTMemberPartition.cpp
    PartitionOfMembers,
    MethodOfEachWithoutTests,
    // ir/AOTTypeIntersection.cpp
    NamedObjectsOfIntersection,
    NamedObjectsOfDifference,
    // walk/AOTReceivers.cpp
    NamedObjectsOfUnionPartKind,
    // walk/AOTCalleeCandidates.cpp
    CalleeListOfObjects,
    FactsOfCalleeList,
    // walk/AOTLoopScanEffects.cpp
    EffectsOfTypedBodyAtCall,
    // ir/AOTTypeArena.cpp: TypeArena::rememberRound().
    RoundOverSet,
};
inline constexpr unsigned numberOfSharedQuestions = static_cast<unsigned>(SharedQuestion::RoundOverSet) + 1;
JS_EXPORT_PRIVATE ASCIILiteral nameOf(SharedQuestion);

// What the threads of Compile work out from the program, Facts and Plan alone. Each answer is worked out once for the build, by whichever thread asks first, however many functions ask.
// TypeTable::freeze() makes it, behind Plan: from there on none of the three changes. It dies with the table, and nothing of it outlives the build.
// Nothing in it names a record of a thread's arena, so an answer holds for every thread.
// A number that it hands out follows the arrival of threads: it may be compared for equality and hashed, and decides nothing.
//
// An answer is right only while whoever works it out reads nothing but the three and the words of its question. Debug and ASAN builds work every answer out again when it is found, and stop where the two differ.
class SharedAnswers {
    WTF_MAKE_NONCOPYABLE(SharedAnswers);
public:
#if ASSERT_ENABLED || ASAN_ENABLED
    static constexpr bool worksFoundAnswersOutAgain = true;
#else
    static constexpr bool worksFoundAnswersOutAgain = false;
#endif

    // Whoever asks derives from it. Once added it never changes.
    class Answer {
        WTF_MAKE_NONCOPYABLE(Answer);
    public:
        Answer() = default;
        virtual ~Answer() = default;
        // With all that hangs on it.
        virtual size_t sizeInBytes() const = 0;
    };

    // The words are numbers of lists, ids of the program and plain content.
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

    // Equal lists have one number, which is less than 1 << 30: as a word of TypeArena::rememberAnswer() it lies in no thread.
    JS_EXPORT_PRIVATE uint32_t internList(std::span<const uint32_t>);
    // internList() hands in the hash of the words. A test hands in one hash for two lists.
    JS_EXPORT_PRIVATE uint32_t internListWithHash(std::span<const uint32_t>, uint64_t hash);
    // It stays where it is for as long as the answers live.
    JS_EXPORT_PRIVATE std::span<const uint32_t> listAt(uint32_t number) const;

    // Null: nobody has answered yet.
    JS_EXPORT_PRIVATE const Answer* find(const Question&) const;
    // The answer that stands. Of two threads that have worked out one answer at the same time the first is believed, and both have worked out the same.
    JS_EXPORT_PRIVATE const Answer& add(const Question&, std::unique_ptr<Answer>);

    // workOut() gives a std::unique_ptr<AnswerType>, and two answers of the type compare with ==.
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
    // A line for the lists and a line for each asker: how often it asked, how often an answer was there, how many answers stand, and their bytes.
    JS_EXPORT_PRIVATE void dumpCounts(PrintStream&) const;

private:
    static constexpr unsigned bitsOfShard = 6;
    static constexpr unsigned numberOfShards = 1u << bitsOfShard;

    // A record neither changes nor moves once it is added, so whoever has found it reads it without a lock.
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
    // Nearly every question is about something that somebody has asked before, and what many functions ask about at once is one question, which has one shard: so whoever only looks takes no lock.
    // Open addressing over the records. A slot goes from null to a record once. An index that is outgrown stays as it is for whoever still reads it, and is freed with the answers:
    // a look into it may miss what was added since, and whoever then adds is handed the record that stands.
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

        // Without a lock, for whoever was handed the position.
        const Record& at(size_t position) const
        {
            PlaceInSegments place = placeInSegments(position);
            return *m_segments[place.segment].load(std::memory_order_acquire)[place.offset].load(std::memory_order_acquire);
        }

        // The three below: under the lock of the shard.
        const Record& add(std::unique_ptr<Record> record)
        {
            // In front of the index: whoever finds the record there may ask for it by its position.
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

        // By position. A segment never moves, and each holds twice what the one in front of it holds.
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
    // A hash picks the shard, so threads that add different records seldom meet at one lock.
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
