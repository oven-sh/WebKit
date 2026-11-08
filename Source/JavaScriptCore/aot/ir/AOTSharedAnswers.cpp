/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSharedAnswers.h"

#if ENABLE(AOT)

#include <algorithm>
#include <bit>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t mostListsInShardOfSharedAnswers = 1u << 24;

ASCIILiteral nameOf(SharedQuestion question)
{
    switch (question) {
    case SharedQuestion::PartitionOfReceivers:
        return "PartitionOfReceivers"_s;
    case SharedQuestion::StateOfFieldArm:
        return "StateOfFieldArm"_s;
    case SharedQuestion::NamedObjectsOfIntersection:
        return "NamedObjectsOfIntersection"_s;
    case SharedQuestion::NamedObjectsOfDifference:
        return "NamedObjectsOfDifference"_s;
    case SharedQuestion::NamedObjectsOfUnionPartKind:
        return "NamedObjectsOfUnionPartKind"_s;
    case SharedQuestion::RoundOverSet:
        return "RoundOverSet"_s;
    case SharedQuestion::ContentsOfArrayInData:
        return "ContentsOfArrayInData"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint64_t mixWordOfSharedAnswers(uint64_t hash, uint64_t word)
{
    hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
    return hash ^ (hash >> 29);
}

static uint64_t hashOfSharedList(std::span<const uint32_t> words)
{
    uint64_t hash = mixWordOfSharedAnswers(0xcbf29ce484222325ull, words.size());
    size_t index = 0;
    for (; index + 1 < words.size(); index += 2)
        hash = mixWordOfSharedAnswers(hash, static_cast<uint64_t>(words[index]) << 32 | words[index + 1]);
    if (index < words.size())
        hash = mixWordOfSharedAnswers(hash, words[index]);
    return mixWordOfSharedAnswers(hash, 0);
}

static uint64_t hashOfSharedQuestion(const SharedAnswers::Question& question)
{
    uint64_t hash = mixWordOfSharedAnswers(0xcbf29ce484222325ull, static_cast<uint64_t>(question.kind) << 32 | question.first);
    hash = mixWordOfSharedAnswers(hash, static_cast<uint64_t>(question.second) << 32 | question.third);
    hash = mixWordOfSharedAnswers(hash, static_cast<uint64_t>(question.fourth) << 32 | question.fifth);
    return mixWordOfSharedAnswers(hash, 0);
}

static constexpr unsigned shardOfHashInSharedAnswers(uint64_t hash, unsigned bitsOfShard)
{
    return static_cast<unsigned>(hash >> (64 - bitsOfShard));
}

SharedAnswers::SharedAnswers() = default;
SharedAnswers::~SharedAnswers() = default;

uint32_t SharedAnswers::internList(std::span<const uint32_t> words)
{
    return internListWithHash(words, hashOfSharedList(words));
}

uint32_t SharedAnswers::internListWithHash(std::span<const uint32_t> words, uint64_t hash)
{
    unsigned shardIndex = shardOfHashInSharedAnswers(hash, bitsOfShard);
    Shard& shard = m_shards[shardIndex];
    auto isSameList = [&](const InternedList& interned) {
        return std::ranges::equal(interned.words, words);
    };
    auto numberOf = [&](const InternedList& interned) {
        return interned.position << bitsOfShard | shardIndex;
    };

    if (const InternedList* found = shard.lists.find(hash, isSameList))
        return numberOf(*found);

    Locker locker { shard.lock };
    if (const InternedList* found = shard.lists.find(hash, isSameList))
        return numberOf(*found);
    size_t position = shard.lists.all().size();
    RELEASE_ASSERT(position < mostListsInShardOfSharedAnswers);
    return numberOf(shard.lists.add(makeUniqueWithoutFastMallocCheck<InternedList>(InternedList { hash, static_cast<uint32_t>(position), Vector<uint32_t>(words) })));
}

std::span<const uint32_t> SharedAnswers::listAt(uint32_t number) const
{
    return m_shards[number & (numberOfShards - 1)].lists.at(number >> bitsOfShard).words.span();
}

const SharedAnswers::Answer* SharedAnswers::find(const Question& question) const
{
    uint64_t hash = hashOfSharedQuestion(question);
    const Shard& shard = m_shards[shardOfHashInSharedAnswers(hash, bitsOfShard)];

    CountsOfQuestion& counts = shard.counts[static_cast<unsigned>(question.kind)];
    counts.asked.fetch_add(1, std::memory_order_relaxed);
    const AddedAnswer* added = shard.answers.find(hash, [&](const AddedAnswer& candidate) {
        return candidate.question == question;
    });
    if (!added)
        return nullptr;
    counts.found.fetch_add(1, std::memory_order_relaxed);
    return added->answer.get();
}

const SharedAnswers::Answer& SharedAnswers::add(const Question& question, std::unique_ptr<Answer> answer)
{
    RELEASE_ASSERT(answer);
    uint64_t hash = hashOfSharedQuestion(question);
    Shard& shard = m_shards[shardOfHashInSharedAnswers(hash, bitsOfShard)];

    Locker locker { shard.lock };
    const AddedAnswer* added = shard.answers.find(hash, [&](const AddedAnswer& candidate) {
        return candidate.question == question;
    });
    if (!added)
        added = &shard.answers.add(makeUniqueWithoutFastMallocCheck<AddedAnswer>(AddedAnswer { hash, question, WTF::move(answer) }));
    return *added->answer;
}

size_t SharedAnswers::sizeInBytes() const
{
    size_t size = sizeof(SharedAnswers);
    for (const Shard& shard : m_shards) {
        Locker locker { shard.lock };
        size += shard.lists.sizeInBytesWithoutRecords() + shard.answers.sizeInBytesWithoutRecords();
        for (const std::unique_ptr<InternedList>& interned : shard.lists.all())
            size += sizeof(InternedList) + interned->words.capacity() * sizeof(uint32_t);
        for (const std::unique_ptr<AddedAnswer>& added : shard.answers.all())
            size += sizeof(AddedAnswer) + added->answer->sizeInBytes();
    }
    return size;
}

void SharedAnswers::dumpCounts(PrintStream& out) const
{
    struct SumOfQuestion {
        uint64_t asked { 0 };
        uint64_t found { 0 };
        uint64_t answers { 0 };
        uint64_t bytes { 0 };
    };
    std::array<SumOfQuestion, numberOfSharedQuestions> sums { };
    uint64_t numberOfLists = 0;
    uint64_t numberOfWords = 0;
    for (const Shard& shard : m_shards) {
        Locker locker { shard.lock };
        numberOfLists += shard.lists.all().size();
        for (const std::unique_ptr<InternedList>& interned : shard.lists.all())
            numberOfWords += interned->words.size();
        for (unsigned kind = 0; kind < numberOfSharedQuestions; ++kind) {
            sums[kind].asked += shard.counts[kind].asked.load(std::memory_order_relaxed);
            sums[kind].found += shard.counts[kind].found.load(std::memory_order_relaxed);
        }
        for (const std::unique_ptr<AddedAnswer>& added : shard.answers.all()) {
            SumOfQuestion& sum = sums[static_cast<unsigned>(added->question.kind)];
            ++sum.answers;
            sum.bytes += sizeof(AddedAnswer) + added->answer->sizeInBytes();
        }
    }
    out.print("AOT shared answers | all | bytes ", sizeInBytes(), " | lists ", numberOfLists, " | words of lists ", numberOfWords, "\n");
    for (unsigned kind = 0; kind < numberOfSharedQuestions; ++kind)
        out.print("AOT shared answers | ", nameOf(static_cast<SharedQuestion>(kind)), " | asked ", sums[kind].asked, " | found ", sums[kind].found, " | answers ", sums[kind].answers, " | bytes ", sums[kind].bytes, "\n");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
