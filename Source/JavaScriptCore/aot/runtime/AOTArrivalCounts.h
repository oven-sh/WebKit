/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "JSCJSValue.h"
#include "JSExportMacros.h"
#include <utility>
#include <wtf/Forward.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class Structure;
struct ClassInfo;

namespace AOT {

enum class Entry : uint16_t;

enum class ArrivalKind : uint8_t {
    // A compiled object. The id is the number of its layout.
    Layout,
    // The same, for an object that has extra properties, another prototype than its layout gives it, or is not extensible.
    LayoutLeftShape,
    // An object without a class of its own that no compiled code created. The id is a number of ArrivalCounts for the names it has.
    OrdinaryObject,
    // Any other object. The id is a number of ArrivalCounts for the name of its class.
    BuiltinClass,
    // The id is the number of the function of a compiled closure, or ArrivalKey::nativeFunction.
    Function,
    // The id is a PrimitiveArrival.
    Primitive,
};

enum class PrimitiveArrival : uint32_t { Undefined, Null, Boolean, Number, String, Symbol, BigInt };

struct ArrivalKey {
    static constexpr uint32_t nativeFunction = UINT32_MAX;

    uint32_t site { 0 };
    ArrivalKind kind { ArrivalKind::Layout };
    uint32_t id { 0 };

    friend bool operator==(const ArrivalKey&, const ArrivalKey&) = default;
};

struct ArrivalKeyHash {
    static unsigned hash(const ArrivalKey& key) { return computeHash(key.site, key.kind, key.id); }
    static bool equal(const ArrivalKey& a, const ArrivalKey& b) { return a == b; }
    static constexpr bool safeToCompareToEmptyOrDeleted = true;
};

// The compiler gives no site one of the two largest numbers.
struct ArrivalKeyHashTraits : WTF::GenericHashTraits<ArrivalKey> {
    static constexpr bool emptyValueIsZero = false;
    static ArrivalKey emptyValue() { return { UINT32_MAX, ArrivalKind::Layout, 0 }; }
    static void constructDeletedValue(ArrivalKey& slot) { slot = { UINT32_MAX - 1, ArrivalKind::Layout, 0 }; }
    static bool isDeletedValue(const ArrivalKey& key) { return key.site == UINT32_MAX - 1; }
};

// What arrived at the accesses that go by name, in an executable built with --aot-count-arrivals. One per instance, created by the first arrival.
class ArrivalCounts {
    WTF_MAKE_TZONE_ALLOCATED(ArrivalCounts);
    WTF_MAKE_NONCOPYABLE(ArrivalCounts);
public:
    JS_EXPORT_PRIVATE ArrivalCounts();
    JS_EXPORT_PRIVATE ~ArrivalCounts();

    // Allocates no cell and runs no JavaScript.
    JS_EXPORT_PRIVATE void record(JSValue receiver, uint32_t site);

    // An access by name whose stub finds its answer calls nothing.
    JS_EXPORT_PRIVATE void didCallOperation(Entry);

    // "<count> TAB arrival:<site> TAB <what arrived>", by site and then with the largest count first, and behind them "<count> TAB left:<site> TAB <operation>" in the same order. src/aot/report/tools/arrivals.ts in Bun reads the lines.
    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

private:
    struct Arrival {
        ArrivalKind kind;
        uint32_t id;
    };
    // A Structure may die and another one may take its address, and a dictionary changes its names and ceases to be one in place. The hash of the names tells most of that apart.
    using StructureKey = std::pair<const Structure*, uint64_t>;

    Arrival classify(JSValue);
    uint32_t numberOfNamesOf(Structure*);
    uint32_t numberOfClass(const ClassInfo*);
    uint32_t numberOfText(const String&);
    String describe(const ArrivalKey&) const;

    using SiteAndOperation = std::pair<uint32_t, uint16_t>;

    UncheckedKeyHashMap<ArrivalKey, uint64_t, ArrivalKeyHash, ArrivalKeyHashTraits> m_counts;
    UncheckedKeyHashMap<SiteAndOperation, uint64_t, DefaultHash<SiteAndOperation>, PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint16_t>>> m_operationsBehindArrivals;
    uint32_t m_lastSite { 0 };
    bool m_hasCalledNothingSinceArrival { false };
    UncheckedKeyHashMap<StructureKey, uint32_t> m_namesOfStructures;
    UncheckedKeyHashMap<const ClassInfo*, uint32_t> m_classes;
    UncheckedKeyHashMap<String, uint32_t> m_numbersOfTexts;
    Vector<String> m_texts;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
