/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIR.h"
#include "AOTRawWordCheck.h"
#include "AOTVector.h"
#include <compare>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

struct RawWordProduction {
    ValueID producer;
    uint32_t resultIndex { 0 };

    friend bool operator==(const RawWordProduction&, const RawWordProduction&) = default;
    friend auto operator<=>(const RawWordProduction&, const RawWordProduction&) = default;
};

struct EndedRawPointer {
    ProgramPoint end;
    ValueID pointer;
};

enum class ProductionReach : uint8_t { Unknown, OnlyProductions };
enum class ProductionIdentity : uint8_t { Unknown, SameProduction };

class RawWordWebs {
    WTF_MAKE_NONCOPYABLE(RawWordWebs);
public:
    RawWordWebs(const Program&, const Function&, FunctionAnalyses&);

    bool isEmpty() const { return m_rawWords.isEmpty() && m_trackedValues.isEmpty(); }
    const BitVector& rawWords() const LIFETIME_BOUND { return m_rawWords; }
    std::optional<OpaqueKind> rawKindOf(ValueID) const;

    std::optional<RawWordProduction> productionOf(ValueID) const;
    ProductionReach collectProductions(ValueID word, Vector<RawWordProduction, 2>&) const;
    ProductionIdentity compareProductions(ValueID, ValueID) const;

    ValueID canonicalBase(ValueID) const;
    ValueID baseOf(const RawWordProduction&) const;
    // A pointer and its base may merge side by side in the parameters of one block.
    bool isBaseOnEveryPath(ValueID base, ValueID pointer) const;

    Vector<EndedRawPointer> findEndedPointers() const;

private:
    const Program& m_program;
    const Function& m_function;
    FunctionAnalyses& m_analyses;
    BitVector m_rawWords;
    BitVector m_trackedValues;
    BitVector m_blocksDefiningTrackedValues;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
