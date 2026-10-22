/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include <optional>
#include <span>
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class ArrivalSiteNumbers;
class Program;
struct Function;
struct PropertyAccessContext;
struct Value;

// An access that goes by name or by key. A counting build records what its receiver is, each time it runs.
struct ArrivalSite {
    BlockID block;
    ValueID value;
    // 1 for the second read of a PropGetPair.
    uint32_t part { 0 };
};

uint32_t numberOfArrivalSitesOf(const Value&);
Vector<ArrivalSite> findArrivalSites(const Function&);

JS_EXPORT_PRIVATE ArrivalSiteNumbers numberArrivalSites(const Program&, BuildContext&);

class ArrivalSiteNumbers {
public:
    // The run time keeps the two numbers above for the empty and the deleted key of its table.
    static constexpr uint32_t maxNumberOfSites = UINT32_MAX - 1;

    ArrivalSiteNumbers() = default;

    bool isCounting() const { return m_isCounting; }
    uint32_t firstOf(FunctionID) const;
    uint32_t numberFor(FunctionID, uint32_t ordinal) const;
    // Nothing for a value that is no arrival site, such as an operation that reads by key only on its slow path.
    std::optional<uint32_t> numberOf(FunctionID, ValueID, uint32_t part) const;
    // In block and value order: the index is the ordinal.
    std::span<const ArrivalSite> sitesOf(FunctionID) const LIFETIME_BOUND;
    JS_EXPORT_PRIVATE void dump(PrintStream&, const Program&) const;

private:
    friend ArrivalSiteNumbers numberArrivalSites(const Program&, BuildContext&);

    struct SitesOfFunction {
        uint32_t first { 0 };
        Vector<ArrivalSite> inOrder;
        // The ordinal of the first site of each value, sorted by value.
        Vector<uint32_t> firstOrdinalsByValue;
    };

    Vector<SitesOfFunction> m_functions;
    bool m_isCounting { false };
};

// What the counts of a run are joined to, by number.
struct ArrivalSiteReport {
    // Ascending by number.
    Vector<Wire::ArrivalSiteRecord> sites;
    // The numbers of the expected layouts. Each site names a range of this list.
    Vector<uint32_t> siteLayouts;
    // Every layout whose objects can arrive, expected anywhere or not, ascending by number.
    Vector<Wire::ArrivalLayoutRecord> layouts;
    // The own data properties with a text key, in field order. Each layout names a range of this list.
    Vector<Wire::ArrivalLayoutFieldRecord> layoutFields;
    // What the text ranges of the records are ranges of.
    Vector<uint8_t> text;
};

JS_EXPORT_PRIVATE ArrivalSiteReport describeArrivalSites(const PropertyAccessContext&, const ArrivalSiteNumbers&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
