/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOpenSites.h"

#if ENABLE(AOT)

#include "AOTBindGlobalNames.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInt32Conversions.h"
#include "AOTProgramPasses.h"
#include "AOTResolveAccesses.h"
#include "AOTResolveBuiltinsInternal.h"
#include "AOTSelectTypedResultsInternal.h"
#include "AOTSimplifyBuiltinCallsInternal.h"
#include <bit>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet opcodesOf(SiteList list)
{
    switch (list) {
    case SiteList::BuiltinUses:
        return builtinUseOpcodes;
    case SiteList::Calls:
        return { Opcode::Call };
    case SiteList::CallsWithList:
        return callsWithList;
    case SiteList::InstanceOfTests:
        return { Opcode::InstanceOf };
    case SiteList::IteratorResults:
        return { Opcode::CreateIteratorResult };
    case SiteList::ArgumentsObjects:
        return { Opcode::CreateUnmappedArguments };
    case SiteList::ConstructionsAndBuiltinCalls:
        return { Opcode::Construct, Opcode::CallBuiltin };
    case SiteList::BuiltinCalls:
        return { Opcode::CallBuiltin };
    case SiteList::SimplifiedIntrinsics:
        return simplifiedIntrinsics;
    case SiteList::Int32Conversions:
        return int32ConvertingOperations;
    case SiteList::ConstantListSpreads:
        return constantListSpreadOpcodes;
    case SiteList::ConstantListCalls:
        return constantListCallOpcodes;
    case SiteList::GlobalNames:
        return bindableAccesses;
    case SiteList::CallSites:
        return callSiteOpcodes;
    case SiteList::Accesses:
        return accessesToResolve;
    case SiteList::TypedResultCalls:
        return typedResultCallOpcodes;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr uint32_t isConstantListSource = 1u << numberOfSiteLists;
static constexpr uint32_t allLists = isConstantListSource - 1;

static_assert(numberOfSiteLists < 32);

static constexpr std::array<uint32_t, numberOfEnumerators<Opcode>> createBitsByOpcode()
{
    std::array<uint32_t, numberOfEnumerators<Opcode>> result { };
    for (Opcode opcode : allMembersOf<Opcode>) {
        uint32_t& bits = result[static_cast<unsigned>(opcode)];
        for (unsigned list = 0; list < numberOfSiteLists; ++list) {
            if (opcodesOf(static_cast<SiteList>(list)).contains(opcode))
                bits |= 1u << list;
        }
        if (constantListSourceOpcodes.contains(opcode))
            bits |= isConstantListSource;
    }
    return result;
}

static constexpr std::array<uint32_t, numberOfEnumerators<Opcode>> bitsByOpcode = createBitsByOpcode();

auto OpenSites::fill(Lists& lists, FunctionAnalyses& analyses, const Function& function) -> Traits
{
    for (auto& list : lists)
        list.shrink(0);
    Traits traits;
    uint32_t seen = 0;
    for (BlockID id : analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        std::span<const ValueID> values = block.values.span();
        for (uint32_t index = 0; index < values.size(); ++index) {
            uint32_t bits = bitsByOpcode[static_cast<unsigned>(function.at(values[index]).opcode)];
            seen |= bits;
            for (uint32_t wanting = bits & allLists; wanting; wanting &= wanting - 1)
                lists[std::countr_zero(wanting)].append(ValueSite { id, index, values[index] });
        }
        traits.hasRequiredTailCall |= block.terminator.kind == TerminatorKind::ReturnCall && block.terminator.requirement() == TailCallRequirement::Required;
    }
    traits.hasConstantListSource = seen & isConstantListSource;
    return traits;
}

void OpenSites::bringUpToDate(const ChangeLog& log, FunctionAnalyses& analyses, const Function& function)
{
    if (m_logIsWritten && !log.hasChangesSince(m_seen)) {
        if (m_verifiesLists) [[unlikely]] {
            Lists lists;
            RELEASE_ASSERT(fill(lists, analyses, function) == m_traits);
            RELEASE_ASSERT(lists == m_lists);
        }
        return;
    }
    m_traits = fill(m_lists, analyses, function);
    m_seen = log.end();
}

std::span<const ValueSite> OpenSites::sitesIn(SiteList list, const ChangeLog& log, FunctionAnalyses& analyses, const Function& function)
{
    bringUpToDate(log, analyses, function);
    return m_lists[static_cast<unsigned>(list)].span();
}

bool OpenSites::hasRequiredTailCall(const ChangeLog& log, FunctionAnalyses& analyses, const Function& function)
{
    bringUpToDate(log, analyses, function);
    return m_traits.hasRequiredTailCall;
}

bool OpenSites::hasConstantListSource(const ChangeLog& log, FunctionAnalyses& analyses, const Function& function)
{
    bringUpToDate(log, analyses, function);
    return m_traits.hasConstantListSource;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
