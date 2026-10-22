/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRequestRecords.h"

#if ENABLE(AOT)

#include "AOTOperationData.h"
#include "AOTPlacementSteps.h"
#include <algorithm>
#include <wtf/Hasher.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename... Records>
static constexpr bool areWordMultiples = (!(sizeof(Records) % sizeof(uint32_t)) && ...);

static_assert(areWordMultiples<NewClassRequest, CloneAsRequest, TemplateObjectRequest, TemplateObjectRequest::Part, DirectEvalRequest, EvalBindingData, DataRequest, CollectionDataRequest>);
static_assert(!(offsetOfFirstRequest % sizeof(uint32_t)));

uint32_t sizeInBytesOf(const Program& program, const RequestRecord& request)
{
    size_t size = WTF::switchOn(request,
        [](const NewClassRecord&) -> size_t {
            return sizeof(NewClassRequest);
        },
        [](const CloneAsRecord& record) -> size_t {
            return sizeof(CloneAsRequest) + sizeof(uint32_t) * record.fields.size();
        },
        [](const TemplateObjectRecord& record) -> size_t {
            return sizeof(TemplateObjectRequest) + sizeof(TemplateObjectRequest::Part) * record.parts.size();
        },
        [&](const DirectEvalRecord& record) -> size_t {
            return sizeof(DirectEvalRequest) + (sizeof(EvalBindingData) + sizeof(uint32_t)) * program.at(record.scope).bindings.size();
        },
        [](const DataRecord&) -> size_t {
            return sizeof(DataRequest);
        },
        [](const CollectionDataRecord&) -> size_t {
            return sizeof(CollectionDataRequest);
        });
    RELEASE_ASSERT(size <= UINT32_MAX);
    return static_cast<uint32_t>(size);
}

static void add(Hasher& hasher, const TemplateString& string)
{
    add(hasher, string.cooked, string.raw);
}

static unsigned hashOf(const RequestRecord& request)
{
    return WTF::switchOn(request,
        [&](const NewClassRecord& record) {
            return computeHash(request.index(), record.classLayout);
        },
        [&](const CloneAsRecord& record) {
            return computeHash(request.index(), record.layout, record.shape, record.fields);
        },
        [&](const TemplateObjectRecord& record) {
            return computeHash(request.index(), record.parts);
        },
        [&](const DirectEvalRecord& record) {
            return computeHash(request.index(), record.scope, record.module);
        },
        [&](const DataRecord& record) {
            return computeHash(request.index(), record.data, record.sharedStorageOffset, record.node);
        },
        [&](const CollectionDataRecord& record) {
            return computeHash(request.index(), record.data, record.hasValues);
        });
}

Request RequestTable::ensure(const Program& program, RequestRecord&& request)
{
    unsigned hash = hashOf(request);
    std::optional<uint32_t> equalRequest = m_index.find(hash, [&](uint32_t position) {
        return m_records[position] == request;
    });
    if (equalRequest) {
        ++m_numberOfSharedRequests;
        return Request(m_offsets[*equalRequest]);
    }

    uint32_t offset = m_sizeInBytes;
    uint64_t end = static_cast<uint64_t>(offset) + sizeInBytesOf(program, request);
    RELEASE_ASSERT(end <= UINT32_MAX);
    m_index.add(hash, static_cast<uint32_t>(m_records.size()));
    m_records.append(WTF::move(request));
    m_offsets.append(offset);
    m_sizeInBytes = static_cast<uint32_t>(end);
    return Request(offset);
}

const RequestRecord* RequestTable::recordAt(Request request) const
{
    auto found = std::ranges::lower_bound(m_offsets, request.value());
    if (found == m_offsets.end() || *found != request.value())
        return nullptr;
    return &m_records[found - m_offsets.begin()];
}

Request PlacedPropertyRuns::ensure(PlacedPropertyRun&& run)
{
    unsigned hash = computeHash(run.keys);
    std::optional<uint32_t> equalRun = m_index.find(hash, [&](uint32_t position) {
        return m_runs[position] == run;
    });
    if (equalRun)
        return Request(*equalRun);

    uint32_t position = static_cast<uint32_t>(m_runs.size());
    m_index.add(hash, position);
    m_runs.append(WTF::move(run));
    return Request(position);
}

void collectStringConstantsOf(const Program& program, const RequestRecord& request, BitVector& stringConstants)
{
    WTF::switchOn(request,
        [](const NewClassRecord&) { },
        [](const CloneAsRecord&) { },
        [&](const TemplateObjectRecord& record) {
            for (const TemplateString& part : record.parts) {
                stringConstants.set(part.raw.index());
                if (part.cooked)
                    stringConstants.set(part.cooked.index());
            }
        },
        [](const DirectEvalRecord&) { },
        [&](const DataRecord& record) {
            collectStringConstantsOf(program, program.at(record.data).root, stringConstants);
        },
        [&](const CollectionDataRecord& record) {
            collectStringConstantsOf(program, program.at(record.data).root, stringConstants);
        });
}

void collectStringConstantsOf(const Program&, const DataNode& root, BitVector& stringConstants)
{
    Vector<const DataNode*, 16> nodesToVisit { &root };
    while (!nodesToVisit.isEmpty()) {
        const DataNode& node = *nodesToVisit.takeLast();
        switch (node.kind()) {
        case DataNode::Kind::Undefined:
        case DataNode::Kind::Null:
        case DataNode::Kind::Hole:
        case DataNode::Kind::Boolean:
        case DataNode::Kind::Number:
        case DataNode::Kind::BigInt:
            break;
        case DataNode::Kind::String:
            stringConstants.set(node.as<DataNode::String>().value.index());
            break;
        case DataNode::Kind::Array:
            for (const DataNode& element : node.as<DataNode::Array>().elements)
                nodesToVisit.append(&element);
            break;
        case DataNode::Kind::Object:
            for (const DataMember& member : node.as<DataNode::Object>().members)
                nodesToVisit.append(&member.value);
            break;
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
