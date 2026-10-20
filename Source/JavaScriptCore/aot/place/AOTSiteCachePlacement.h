/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "JSExportMacros.h"
#include <compare>
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

JS_EXPORT_PRIVATE unsigned numberOfSiteCachesUsedBy(const Program&, const Placement&, const Value&);
// Nothing reads what a site that runs at most once would remember, so it has no cache.
JS_EXPORT_PRIVATE bool mayReadSiteCachesAgain(FunctionAnalyses&, BlockID);
// Caches and their states are numbered in the order of the program's text: module, function as written, the form of it that holds the site. Most modules and functions of a large
// program never run, and what belongs to them then shares no page with what does.
struct SiteCachePosition {
    uint32_t module { 0 };
    uint32_t sourceFunction { 0 };
    uint32_t function { 0 };
    uint32_t value { 0 };

    friend auto operator<=>(const SiteCachePosition&, const SiteCachePosition&) = default;
};
JS_EXPORT_PRIVATE SiteCachePosition siteCachePositionOf(const Program&, FunctionID, ValueID);
JS_EXPORT_PRIVATE SiteCaches placeSiteCaches(ProgramEditor&, const Placement&, const FlowResults&, const Heat&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
