/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowRecords.h"
#include "AOTLedger.h"
#include <algorithm>
#include <initializer_list>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// What flow says of a field of a layout because of who writes it. Each claim is a function of the writers; where one of them has not accounted for the field nothing is said.

inline bool isWrittenAs(std::span<const Write> writes, std::initializer_list<WriteForm> forms)
{
    return std::ranges::any_of(writes, [&](const Write& write) {
        return std::ranges::contains(forms, write.form);
    });
}

struct ClaimsOfFieldsEnabled {
    bool storeCounts { false };
    bool holderMutations { false };
    bool initialization { false };
};

inline void claimWhatNeverHappens(FieldFlow& flow, const Writers& writers, bool everyCreationFillsIt, ClaimsOfFieldsEnabled enabled)
{
    auto all = writers.all();
    if (!all)
        return;
    std::span<const Write> writes = *all;
    if (enabled.storeCounts)
        flow.location.writers = WriterState::AllKnown;
    if (enabled.holderMutations) {
        if (!isWrittenAs(writes, { WriteForm::Delete }))
            flow.stability.add(FieldStability::IsNeverDeleted);
        if (!isWrittenAs(writes, { WriteForm::Define, WriteForm::DefineAccessor }))
            flow.stability.add(FieldStability::IsNeverRedefined);
    }
    if (everyCreationFillsIt && flow.location.writers == WriterState::AllKnown && enabled.initialization)
        flow.location.initialization = InitializationState::SetAtCreation;
}

inline void claimStoreCount(FieldFlow& flow, const Writers& writers)
{
    auto all = writers.all();
    if (!all || flow.location.writers != WriterState::AllKnown)
        return;
    if (isWrittenAs(*all, { WriteForm::Store, WriteForm::Define, WriteForm::DefineAccessor, WriteForm::Delete }))
        return;
    flow.location.stores = StoreCount::AtMostOnce;
    flow.location.isUnchangedDuringActivation = true;
}

// A read of the contents of a container finds what was stored there. A definition may have put an accessor in its place, and another object above may answer for a hole.
inline bool claimContentsAreWhatWasStored(const Writers& writers)
{
    auto all = writers.all();
    if (!all)
        return false;
    return !isWrittenAs(*all, { WriteForm::Define, WriteForm::DefineAccessor, WriteForm::Link });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
