/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class EnvironmentMaterialization {
public:
    // A closure of the frame in whose place this use is going to take a closure of the heap, which is made in front of it.
    struct ClosureMadeAgain {
        ValueID closure;
        Use use;
    };

    // The closures that are going to be made again are for canRun(). run() finds them made.
    EnvironmentMaterialization(FunctionAnalyses&, ValueID environment, const BitVector& frameClosures, std::span<const ClosureMadeAgain> = { });

    bool canRun();
    void run(IRBuilder&);

private:
    bool requiresHeapEnvironment(ValueID) const;
    bool capturesEnvironment(const Value& closure) const;
    bool isProfitable(std::span<const ProgramPoint> materializationPoints);
    bool isAnyHolderLive(std::span<const ProgramPoint> materializationPoints, const BitVector& holders);
    bool isAnyOtherHolderLiveWhereClosureIsMadeAgain(const BitVector& holders);
    Vector<ProgramPoint, 4> findMaterializationPoints() const;
    std::optional<BitVector> findFrameHolders();

    FunctionAnalyses& m_analyses;
    const Function& m_function;
    ValueID m_environment;
    const BitVector& m_frameClosures;
    std::span<const ClosureMadeAgain> m_closuresMadeAgain;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
