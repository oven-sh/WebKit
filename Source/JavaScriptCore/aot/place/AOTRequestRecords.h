/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTSlotPlan.h"
#include "IndexingType.h"
#include "JSExportMacros.h"
#include <optional>
#include <span>
#include <wtf/Variant.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct NewClassRecord {
    LayoutID classLayout;

    friend bool operator==(const NewClassRecord&, const NewClassRecord&) = default;
};

struct CloneAsRecord {
    LayoutID layout;
    uint32_t shape { 0 };
    Vector<FieldIndex> fields;

    friend bool operator==(const CloneAsRecord&, const CloneAsRecord&) = default;
};

struct TemplateObjectRecord {
    Vector<TemplateString> parts;

    friend bool operator==(const TemplateObjectRecord&, const TemplateObjectRecord&) = default;
};

struct DirectEvalRecord {
    EvalScopeID scope;
    ModuleID module;

    friend bool operator==(const DirectEvalRecord&, const DirectEvalRecord&) = default;
};

struct DataRecord {
    DataID data;
    std::optional<uint32_t> sharedStorageOffset;
    // Where the node of an array that is its own storage begins in the table of the image.
    std::optional<uint32_t> node;

    friend bool operator==(const DataRecord&, const DataRecord&) = default;
};

// How the elements of an array of constants are stored, in the table of the image and in the array that is made of it.
JS_EXPORT_PRIVATE IndexingType indexingTypeOf(const Program&, const DataNode::Array&);

struct CollectionDataRecord {
    DataID data;
    bool hasValues { false };

    friend bool operator==(const CollectionDataRecord&, const CollectionDataRecord&) = default;
};

using RequestRecord = Variant<NewClassRecord, CloneAsRecord, TemplateObjectRecord, DirectEvalRecord, DataRecord, CollectionDataRecord>;

JS_EXPORT_PRIVATE uint32_t sizeInBytesOf(const Program&, const RequestRecord&);

inline constexpr uint32_t offsetOfFirstRequest = sizeof(uint32_t);

class RequestTable {
public:
    std::span<const RequestRecord> records() const { return m_records.span(); }
    uint32_t offsetOf(unsigned index) const { return m_offsets[index]; }
    uint32_t sizeInBytes() const { return m_sizeInBytes; }
    JS_EXPORT_PRIVATE const RequestRecord* recordAt(Request) const;

    uint32_t numberOfSharedRequests() const { return m_numberOfSharedRequests; }

    Request ensure(const Program&, RequestRecord&&);

private:
    Vector<RequestRecord> m_records;
    Vector<uint32_t> m_offsets;
    HashedIndex m_index;
    uint32_t m_sizeInBytes { offsetOfFirstRequest };
    uint32_t m_numberOfSharedRequests { 0 };
};

struct PlacedPropertyRun {
    Vector<PropertyKeyID, 8> keys;

    friend bool operator==(const PlacedPropertyRun&, const PlacedPropertyRun&) = default;
};

class PlacedPropertyRuns {
public:
    std::span<const PlacedPropertyRun> runs() const { return m_runs.span(); }
    const PlacedPropertyRun& at(Request run) const { return m_runs[run.value()]; }

    Request ensure(PlacedPropertyRun&&);

private:
    Vector<PlacedPropertyRun> m_runs;
    HashedIndex m_index;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
