/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSourcePlans.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTCellFormat.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include <algorithm>

namespace JSC { namespace AOT {

SourcePlans::SourcePlans(State& state)
{
    state.caches->add(CacheKind::WeakByKey, "source plans"_s, *this);
}

SourcePlans::~SourcePlans() = default;

SourcePlans::Words SourcePlans::createUnusable(uint32_t numberOfMembers)
{
    size_t size = SourcePlan::numberOfWords(numberOfMembers);
    Words words = makeUniqueArray<uint32_t>(size);
    std::fill_n(words.get(), size, static_cast<uint32_t>(SourcePlan::absent));
    words[SourcePlan::isUsableWord] = 0;
    words[SourcePlan::orderWord] = unorderedOrderWord;
    return words;
}

const uint32_t* SourcePlans::findInTable(Structure* structure, uint32_t request)
{
    auto found = m_plans.find({ structure, request });
    if (found == m_plans.end())
        return nullptr;
    m_lastStructure = structure;
    m_lastRequest = request;
    m_lastPlan = found->value.get();
    return m_lastPlan;
}

const uint32_t* SourcePlans::planThatNeverApplies(uint32_t numberOfMembers)
{
    if (m_plansThatNeverApply.isEmpty() || numberOfMembers > m_membersOfPlanThatNeverApplies) {
        m_membersOfPlanThatNeverApplies = std::max(numberOfMembers, 2 * m_membersOfPlanThatNeverApplies);
        m_plansThatNeverApply.append(createUnusable(m_membersOfPlanThatNeverApplies));
    }
    return m_plansThatNeverApply.last().get();
}

const uint32_t* SourcePlans::add(Structure* structure, uint32_t request, uint32_t numberOfMembers, Words&& plan)
{
    if (m_plans.size() >= maxNumberOfPlans)
        return planThatNeverApplies(numberOfMembers);
    return m_plans.add({ structure, request }, WTF::move(plan)).iterator->value.get();
}

size_t SourcePlans::removeDeadEntries(VM& vm, CollectionScope)
{
    m_lastStructure = nullptr;
    m_lastPlan = nullptr;
    size_t sizeBefore = m_plans.size();
    m_plans.removeIf([&](auto& entry) {
        return !vm.heap.isMarked(entry.key.first);
    });
    return sizeBefore - m_plans.size();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
