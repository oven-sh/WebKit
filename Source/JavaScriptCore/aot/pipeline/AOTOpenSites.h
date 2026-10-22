/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTChangeLog.h"
#include "AOTIR.h"
#include <array>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

struct ValueSite {
    BlockID block;
    uint32_t index { 0 };
    ValueID value;

    ProgramPoint point() const { return { block, index }; }

    friend bool operator==(const ValueSite&, const ValueSite&) = default;
};

// One list for each set of opcodes that a resolving pass starts from. The sets stand in AOTOpenSites.cpp, beside the headers of the passes that name them.
enum class SiteList : uint8_t {
    BuiltinUses,
    Calls,
    CallsWithList,
    InstanceOfTests,
    IteratorResults,
    ArgumentsObjects,
    ConstructionsAndBuiltinCalls,
    BuiltinCalls,
    SimplifiedIntrinsics,
    Int32Conversions,
    ConstantListSpreads,
    ConstantListCalls,
    GlobalNames,
    CallSites,
    Accesses,
    TypedResultCalls,
};

inline constexpr unsigned numberOfSiteLists = static_cast<unsigned>(SiteList::TypedResultCalls) + 1;

// The operations of a function that a resolving pass may have something to do at, by the set of opcodes that the pass starts from, in the order of the blocks in reverse post order and of the values in their blocks.
// One walk fills all lists. They hold until the log says that the function has changed.
class OpenSites {
    WTF_MAKE_NONCOPYABLE(OpenSites);
public:
    OpenSites() = default;

    // Good until the function is edited.
    std::span<const ValueSite> sitesIn(SiteList, const ChangeLog&, FunctionAnalyses&, const Function&) LIFETIME_BOUND;
    // A reachable block ends in a tail call that has to stay one.
    bool hasRequiredTailCall(const ChangeLog&, FunctionAnalyses&, const Function&);
    // A reachable block lists an operation that may give a list of constants.
    bool hasConstantListSource(const ChangeLog&, FunctionAnalyses&, const Function&);

    // Until somebody says so, nobody writes the log, and every question is answered by a walk.
    void setLogIsWritten(bool isWritten) { m_logIsWritten = isWritten; }
    void setVerifiesLists(bool verifies) { m_verifiesLists = verifies; }

private:
    using Lists = std::array<Vector<ValueSite, 4>, numberOfSiteLists>;

    struct Traits {
        bool hasRequiredTailCall { false };
        bool hasConstantListSource { false };

        friend bool operator==(const Traits&, const Traits&) = default;
    };

    void bringUpToDate(const ChangeLog&, FunctionAnalyses&, const Function&);
    static Traits fill(Lists&, FunctionAnalyses&, const Function&);

    Lists m_lists;
    ChangeLog::Cursor m_seen;
    Traits m_traits;
    bool m_logIsWritten { false };
    bool m_verifiesLists { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
