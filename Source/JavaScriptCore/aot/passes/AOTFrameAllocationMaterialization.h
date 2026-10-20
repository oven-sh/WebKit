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
    EnvironmentMaterialization(FunctionAnalyses&, ValueID environment, const BitVector& frameClosures);

    bool canRun();
    void run(IRBuilder&);

private:
    bool requiresHeapEnvironment(ValueID) const;
    bool capturesEnvironment(const Value& closure) const;
    bool isProfitable(std::span<const ProgramPoint> materializationPoints);
    bool isAnyHolderLive(std::span<const ProgramPoint> materializationPoints, const BitVector& holders);
    Vector<ProgramPoint, 4> findMaterializationPoints() const;
    std::optional<BitVector> findFrameHolders();

    FunctionAnalyses& m_analyses;
    const Function& m_function;
    ValueID m_environment;
    const BitVector& m_frameClosures;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
