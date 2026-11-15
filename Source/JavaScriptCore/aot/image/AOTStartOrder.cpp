/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStartOrder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompiledCode.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

void sortKeys(std::span<uint64_t> keys)
{
    AOT::sort(keys);
}

static Vector<uint32_t> indicesByRank(std::span<const uint32_t> ranks)
{
    Vector<uint64_t> keys(ranks.size(), [&](size_t index) {
        return static_cast<uint64_t>(ranks[index]) << 32 | index;
    });
    sortKeys(keys.mutableSpan());
    return keys.map([](uint64_t key) {
        return static_cast<uint32_t>(key);
    });
}

Vector<uint32_t> rankFunctionsByListedUnits(std::span<const uint32_t> positionsOfListedUnits, const LinkedFunctions& functions, size_t numberOfFunctions)
{
    Vector<uint32_t> ranks(FillWith { }, numberOfFunctions, noStartRank);
    uint32_t nextRank = 0;
    for (uint32_t position : positionsOfListedUnits) {
        uint32_t& rank = ranks[functions[position].unit.function.index()];
        if (rank == noStartRank)
            rank = nextRank++;
    }
    return ranks;
}

Vector<uint32_t> listedNumbersFirst(std::span<const uint32_t> listed, size_t count)
{
    Vector<uint32_t> ranks(FillWith { }, count, noStartRank);
    uint32_t nextRank = 0;
    for (uint32_t number : listed) {
        if (number < count)
            ranks[number] = std::min(ranks[number], nextRank++);
    }
    return indicesByRank(ranks.span());
}

using MentionVisitor = ScopedLambda<void(FunctionID, const Mention&, std::span<const uint32_t> key)>;

static void forEachMentionOfListedUnits(const NumberingInputs& inputs, std::span<const uint32_t> startRanks, NOESCAPE const MentionVisitor& visit)
{
    Vector<uint32_t> ranks(inputs.functions.size(), [&](size_t position) {
        return startRanks[inputs.functions[position].unit.function.index()];
    });
    for (uint32_t position : indicesByRank(ranks.span())) {
        if (ranks[position] == noStartRank)
            break;
        const CompiledCode& code = inputs.functions[position];
        std::span<const uint32_t> words = code.body.mentionWords.span();
        for (const Mention& mention : code.body.mentions)
            visit(code.unit.function, mention, keyOf(mention, words));
    }
}

static std::optional<DataID> findDataNamedBy(const Mention& mention, std::span<const uint32_t> key)
{
    if (mention.space == NumberSpace::DataStorage || mention.space == NumberSpace::DataElements)
        return DataID(key[0]);
    if (mention.space == NumberSpace::Request) {
        auto kind = static_cast<RequestKind>(key[0]);
        if (kind == RequestKind::Data || kind == RequestKind::CollectionData)
            return DataID(key[1]);
    }
    return std::nullopt;
}

static void forEachNodeOfListedData(const NumberingInputs& inputs, const Mention& mention, std::span<const uint32_t> key, BitVector& isVisited, NOESCAPE const ScopedLambda<void(const Typed::DataNode&)>& visit)
{
    std::optional<DataID> data = findDataNamedBy(mention, key);
    if (!data || isVisited.quickSet(data->index()))
        return;
    Vector<const Typed::DataNode*> nodesToVisit { &inputs.program.data[data->index()].root };
    while (!nodesToVisit.isEmpty()) {
        const Typed::DataNode& node = *nodesToVisit.takeLast();
        visit(node);
        if (node.kind() == Typed::DataNode::Kind::Array) {
            for (const Typed::DataNode& element : node.as<Typed::DataNode::Array>().elements)
                nodesToVisit.append(&element);
        } else if (node.kind() == Typed::DataNode::Kind::Object) {
            for (const Typed::DataMember& member : node.as<Typed::DataNode::Object>().members)
                nodesToVisit.append(&member.value);
        }
    }
}

void forEachTextOfListedData(const NumberingInputs& inputs, std::span<const uint32_t> startRanks, NOESCAPE const ScopedLambda<void(StringID, FunctionID)>& visit)
{
    BitVector isVisited(inputs.program.data.size());
    forEachMentionOfListedUnits(inputs, startRanks, scopedLambda<void(FunctionID, const Mention&, std::span<const uint32_t>)>([&](FunctionID function, const Mention& mention, std::span<const uint32_t> key) {
        forEachNodeOfListedData(inputs, mention, key, isVisited, scopedLambda<void(const Typed::DataNode&)>([&](const Typed::DataNode& node) {
            if (node.kind() == Typed::DataNode::Kind::String)
                visit(node.as<Typed::DataNode::String>().value, function);
        }));
    }));
}

Vector<uint32_t> listSitesInStartOrder(const NumberingInputs& inputs, std::span<const uint32_t> startRanks, const NumberedKeys& sites)
{
    if (startRanks.empty())
        return { };
    Vector<uint32_t> listed;
    forEachMentionOfListedUnits(inputs, startRanks, scopedLambda<void(FunctionID, const Mention&, std::span<const uint32_t>)>([&](FunctionID, const Mention& mention, std::span<const uint32_t> key) {
        if (mention.space != NumberSpace::SiteCache)
            return;
        if (std::optional<uint32_t> place = sites.find(key))
            listed.append(*place);
    }));
    return listedNumbersFirst(listed.span(), sites.size());
}

Vector<LayoutRecordToPlace> sortLayoutRecords(Vector<LayoutRecordToPlace>&& records, const ImageNumbers& numbers, size_t numberOfShapes, size_t numberOfLayouts)
{
    Vector<uint32_t> ranksOfShapes(FillWith { }, numberOfShapes + 1, noStartRank);
    Vector<uint32_t> ranksOfClasses(FillWith { }, numberOfLayouts, noStartRank);
    uint32_t nextRank = 0;
    for (uint32_t shape : numbers.shapesInStartOrder())
        ranksOfShapes[shape] = std::min(ranksOfShapes[shape], nextRank++);
    for (uint32_t layout : numbers.classesInStartOrder())
        ranksOfClasses[layout] = std::min(ranksOfClasses[layout], nextRank++);
    Vector<uint32_t> ranks(records.size(), [&](size_t index) {
        return records[index].shape ? ranksOfShapes[records[index].shape] : ranksOfClasses[records[index].id.index()];
    });
    return indicesByRank(ranks.span()).map([&](uint32_t index) {
        return records[index];
    });
}

void ImageNumbersBuilder::placeListedRowsFirst(std::span<const uint32_t> startRanks)
{
    constexpr uint32_t smallestCountWithHashIndex = 64;
    NumberedKeys& allocations = numbers.m_allocations;
    Vector<uint32_t> rows;
    auto listShapesOf = [&](LayoutID layout) {
        Typed::ShapeRange range = inputs.plan.shapes().shapesOf(layout);
        for (uint32_t shape = range.first; shape < range.end(); ++shape)
            numbers.m_shapesInStartOrder.append(shape);
    };
    BitVector isVisited(inputs.program.data.size());
    forEachMentionOfListedUnits(inputs, startRanks, scopedLambda<void(FunctionID, const Mention&, std::span<const uint32_t>)>([&](FunctionID, const Mention& mention, std::span<const uint32_t> key) {
        if (mention.space == NumberSpace::Allocation) {
            numbers.m_shapesInStartOrder.append(decodeAllocationKey(key).allocation.shape);
            if (std::optional<uint32_t> number = allocations.find(key))
                rows.append(*number);
            return;
        }
        if (mention.space == NumberSpace::Request && static_cast<RequestKind>(key[0]) == RequestKind::NewClass) {
            numbers.m_classesInStartOrder.append(key[1]);
            listShapesOf(LayoutID(key[1]));
            return;
        }
        forEachNodeOfListedData(inputs, mention, key, isVisited, scopedLambda<void(const Typed::DataNode&)>([&](const Typed::DataNode& node) {
            if (node.kind() == Typed::DataNode::Kind::Object)
                listShapesOf(node.as<Typed::DataNode::Object>().layout);
        }));
    }));
    if (allocations.size() < smallestCountWithHashIndex)
        return;
    NumberedKeys reordered;
    reordered.firstWord.append(0);
    for (uint32_t number : listedNumbersFirst(rows.span(), allocations.size())) {
        reordered.words.append(allocations.keyAt(number));
        reordered.firstWord.append(static_cast<uint32_t>(reordered.words.size()));
    }
    reordered.buildHashIndex(context.parallelLoops());
    allocations = WTF::move(reordered);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
