/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCallSiteTable.h"
#include "AOTImageFormat.h"
#include "AOTPrefixCode.h"
#include "AOTVector.h"

namespace JSC { namespace AOT {

using PositionCodes = PrefixCodes<PositionIndex::numberOfCodes>;

class PositionEncoder {
public:
    void encode(const SiteDescription&, OwnPosition ofFunctionInFront);
    const PositionCodes::Counts& counts() const { return m_counts; }
    void appendNextCoded(Vector<uint8_t>& stream, const PositionCodes&);

private:
    Vector<uint8_t> m_heads;
    Vector<PrefixSymbol> m_symbols;
    Vector<PrefixSymbol> m_roomForSymbols;
    Vector<uint8_t> m_roomForBytes;
    PositionCodes::Counts m_counts { };
    size_t m_nextHead { 0 };
    size_t m_nextSymbol { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
