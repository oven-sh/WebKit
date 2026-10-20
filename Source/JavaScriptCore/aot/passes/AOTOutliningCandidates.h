/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

struct OptimizationInputs;

struct OutliningSite {
    bool isRegion() const { return !!exitBlock; }

    FunctionID function;
    BlockID entryBlock;
    uint32_t firstIndex { 0 };
    uint32_t numberOfEntryValues { 0 };
    Vector<BlockID, 2> interiorBlocks;
    BlockID exitBlock;
    std::optional<uint32_t> exitArgument;
    ValueID root;
    bool rootIsUsed { false };
    Vector<ValueID, 6> liveIns;
    TypeID resultType;
    uint32_t codeBytes { 0 };
    unsigned hash { 0 };
};

inline std::span<const ValueID> entryValuesOf(const Function& function, const OutliningSite& site)
{
    return function.at(site.entryBlock).values.subspan(site.firstIndex, site.numberOfEntryValues);
}

Vector<OutliningSite> findOutliningSites(const Program&, const OptimizationInputs&, BuildContext&);

bool areEqualSites(const Program&, const OutliningSite&, const OutliningSite&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
