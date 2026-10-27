/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTVector.h"
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

enum class StoredKind : uint8_t {
    Int32 = 1 << 0,
    Undefined = 1 << 1,
    Null = 1 << 2,
    Other = 1 << 3,
    NoInt32 = 1 << 4,
};

// It follows the stores that the expected types of receivers and callees let it see: what it says of a field or a parameter is a guess.
class VisibleStores {
public:
    using FunctionFilter = ScopedLambda<bool(FunctionID, unsigned worker)>;

    VisibleStores() = default;
    VisibleStores(const Program&, const ParallelLoops&, NOESCAPE const FunctionFilter&);

    Expected<OptionSet<StoredKind>> kindsStoredIn(LayoutID layout, uint32_t field) const { return m_kinds.isEmpty() ? OptionSet<StoredKind> { } : m_kinds[layout.index()][field]; }
    Expected<OptionSet<StoredKind>> kindsPassedTo(FunctionID function, unsigned parameter) const { return m_parameterKinds.isEmpty() ? OptionSet<StoredKind> { } : m_parameterKinds[function.index()][parameter]; }
    Expected<bool> typesPassedLieInInt32(FunctionID function, unsigned parameter) const { return !m_parametersPassedOtherTypes.isEmpty() && !m_parametersPassedOtherTypes[function.index()].get(parameter); }
    Expected<bool> isUsedAsInteger(LayoutID layout, uint32_t field) const { return !m_fieldsUsedAsIntegers.isEmpty() && m_fieldsUsedAsIntegers[layout.index()].get(field); }
    // What the field holds is an operand of `&`, `|`, `^` or a shift, which take 32 bits of it. `| 0` is a conversion.
    Expected<bool> isUsedAsBits(LayoutID layout, uint32_t field) const { return !m_fieldsUsedAsBits.isEmpty() && m_fieldsUsedAsBits[layout.index()].get(field); }
    // What the field holds goes into a sum or a difference that is stored back into it.
    Expected<bool> isAddedTo(LayoutID layout, uint32_t field) const { return !m_fieldsAddedTo.isEmpty() && m_fieldsAddedTo[layout.index()].get(field); }
    Expected<bool> typesOfInitialValuesFitInt32Slot(LayoutID layout, uint32_t field) const { return m_fieldsStartedWithOtherTypes.isEmpty() || !m_fieldsStartedWithOtherTypes[layout.index()].get(field); }

private:
    Vector<Vector<OptionSet<StoredKind>>> m_kinds;
    Vector<Vector<OptionSet<StoredKind>>> m_parameterKinds;
    Vector<BitVector> m_parametersPassedOtherTypes;
    Vector<BitVector> m_fieldsUsedAsIntegers;
    Vector<BitVector> m_fieldsUsedAsBits;
    Vector<BitVector> m_fieldsAddedTo;
    Vector<BitVector> m_fieldsStartedWithOtherTypes;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
