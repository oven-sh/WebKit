/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTSiteWeight.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <compare>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class FunctionAnalyses;
class Heat;
class Placement;

class SiteCaches {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SiteCaches);
public:
    SiteCaches() = default;
    SiteCaches(uint32_t count, uint32_t numberOfSitesWithoutCache)
        : m_count(count)
        , m_numberOfSitesWithoutCache(numberOfSitesWithoutCache)
    {
    }

    uint32_t count() const { return m_count; }
    uint32_t numberOfSitesWithoutCache() const { return m_numberOfSitesWithoutCache; }

    JS_EXPORT_PRIVATE void dump(PrintStream&, const Program&) const;

private:
    uint32_t m_count { 0 };
    uint32_t m_numberOfSitesWithoutCache { 0 };
};

// For a store by name whose receiver is proven to have a layout with a plain field of the name, at one place in the record of every layout that it may have: how far behind the data of the layout the record of the field lies.
enum class NameHasColumn : bool { No, Yes };
JS_EXPORT_PRIVATE std::optional<uint32_t> findRecordOfStoredField(const Program&, const Placement&, const Function&, const Value&, NameHasColumn);
// Of a store that has such a record: in every layout of the receiver the slot lies in the cell and holds any value as it is, and the type of the field is that of any value.
bool storedFieldTakesAnyValue(const Program&, const Placement&, const Function&, const Value&);
JS_EXPORT_PRIVATE unsigned numberOfSiteCachesUsedBy(const Program&, const Placement&, const Function&, const Value&);
// The same of an operation that is no run of stores, or is one that has a request.
unsigned numberOfSiteCachesUsedByOperationWithRequest(const Program&, const Placement&, const Function&, const Value&);
// Nothing reads what a site that runs at most once would remember, so it has no cache.
JS_EXPORT_PRIVATE bool mayReadSiteCachesAgain(FunctionAnalyses&, BlockID);
// Caches and their states are numbered in the order of the program's text: module, function as written, the form of it that holds the site.
struct SiteCachePosition {
    uint32_t module { 0 };
    uint32_t sourceFunction { 0 };
    uint32_t function { 0 };
    uint32_t value { 0 };

    friend auto operator<=>(const SiteCachePosition&, const SiteCachePosition&) = default;
};
JS_EXPORT_PRIVATE SiteCachePosition siteCachePositionOf(const Program&, FunctionID, ValueID);
struct SiteCacheRequest {
    SiteWeight weight { 0 };
    FunctionID function;
    ValueID value;
    bool runsAtMostOnce { false };
};
enum class RequestsOfStoreRuns : bool { AreWritten, AreNotWrittenYet };
// The requests of one function, in front of the step that writes the requests of runs of stores.
void collectSiteCacheRequests(const Program&, const Placement&, const FlowResults&, const Heat&, BuildContext&, FunctionID, bool needsWeights, Vector<SiteCacheRequest>&);
// With requests by function: what collectSiteCacheRequests() gave for each function with a body. No value has got another number since.
JS_EXPORT_PRIVATE SiteCaches placeSiteCaches(ProgramEditor&, const Placement&, const FlowResults&, const Heat&, BuildContext&, std::span<const Vector<SiteCacheRequest>> requestsOfFunctions = { });

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
