/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTSlotPlan.h"
#include "AOTVector.h"
#include "IndexingType.h"
#include "JSExportMacros.h"
#include <optional>
#include <span>
#include <wtf/Hasher.h>
#include <wtf/Variant.h>

namespace JSC { namespace AOT { namespace Typed {

inline void add(Hasher& hasher, const TemplateString& string) { add(hasher, string.cooked, string.raw); }

struct NewClassRecord {
    LayoutID classLayout;

    friend bool operator==(const NewClassRecord&, const NewClassRecord&) = default;
    friend void add(Hasher& hasher, const NewClassRecord& record) { add(hasher, record.classLayout); }
};

struct TemplateObjectRecord {
    Vector<TemplateString> parts;

    friend bool operator==(const TemplateObjectRecord&, const TemplateObjectRecord&) = default;
    friend void add(Hasher& hasher, const TemplateObjectRecord& record) { add(hasher, record.parts); }
};

struct DirectEvalRecord {
    EvalScopeID scope;
    ModuleID module;

    friend bool operator==(const DirectEvalRecord&, const DirectEvalRecord&) = default;
    friend void add(Hasher& hasher, const DirectEvalRecord& record) { add(hasher, record.scope, record.module); }
};

struct DataRecord {
    DataID data;
    std::optional<uint32_t> sharedStorageOffset;
    // Where the node of an array that is its own storage begins in the table of the image.
    std::optional<uint32_t> node;

    friend bool operator==(const DataRecord&, const DataRecord&) = default;
    friend void add(Hasher& hasher, const DataRecord& record) { add(hasher, record.data, record.sharedStorageOffset, record.node); }
};

// How the elements of an array of constants are stored, in the table of the image and in the array that is made of it.
JS_EXPORT_PRIVATE IndexingType indexingTypeOf(const Program&, const DataNode::Array&);

struct CollectionDataRecord {
    DataID data;
    bool hasValues { false };

    friend bool operator==(const CollectionDataRecord&, const CollectionDataRecord&) = default;
    friend void add(Hasher& hasher, const CollectionDataRecord& record) { add(hasher, record.data, record.hasValues); }
};

using RequestRecord = Variant<NewClassRecord, TemplateObjectRecord, DirectEvalRecord, DataRecord, CollectionDataRecord>;

JS_EXPORT_PRIVATE uint32_t sizeInBytesOf(const Program&, const RequestRecord&);

// What the row of a binding holds as numbers of identifiers: the name that evaluated text writes, and the key of the field of an instance that a private name stands for.
struct EvalBindingNames {
    StringID name;
    PropertyKeyID privateKey;
};
EvalBindingNames namesInRowOf(const Program&, const EvalBinding&);

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
