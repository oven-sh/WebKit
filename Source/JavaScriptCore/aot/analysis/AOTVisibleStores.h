/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

enum class StoredKind : uint8_t {
    Int32 = 1 << 0,
    Undefined = 1 << 1,
    Null = 1 << 2,
    Other = 1 << 3,
    // A number that the program shows to be no int32: a quotient, a power, a constant with a fraction or outside the range. It is one of the other values too.
    NoInt32 = 1 << 4,
};

// What the stores that the program shows put into each field, followed through variables, parameters, results and other fields.
class VisibleStores {
public:
    // Called for each function with a body, on the thread that is about to read it. A function for which it answers false is not read.
    using FunctionFilter = ScopedLambda<bool(FunctionID, unsigned worker)>;

    VisibleStores() = default;
    VisibleStores(const Program&, const ParallelLoops&);
    VisibleStores(const Program&, const ParallelLoops&, NOESCAPE const FunctionFilter&);

    OptionSet<StoredKind> kindsStoredIn(LayoutID layout, uint32_t field) const { return m_kinds.isEmpty() ? OptionSet<StoredKind> { } : m_kinds[layout.index()][field]; }
    // What the calls that the program shows pass for a positional parameter. Nothing where it shows no call.
    OptionSet<StoredKind> kindsPassedTo(FunctionID function, unsigned parameter) const { return m_parameterKinds.isEmpty() ? OptionSet<StoredKind> { } : m_parameterKinds[function.index()][parameter]; }
    // Whether each of those calls passes, for a parameter of type number, a value whose own type lies in int32. A sum of two int32s is of the kind and not of the type.
    bool typesPassedLieInInt32(FunctionID function, unsigned parameter) const { return !m_parametersPassedOtherTypes.isEmpty() && !m_parametersPassedOtherTypes[function.index()].get(parameter); }
    // Whether something that the program reads from the field goes where only an integer makes sense.
    bool isUsedAsInteger(LayoutID layout, uint32_t field) const { return !m_fieldsUsedAsIntegers.isEmpty() && m_fieldsUsedAsIntegers[layout.index()].get(field); }
    // Whether each allocation that the program shows starts the field with a value whose own type fits a slot of four bytes, undefined and null aside.
    bool typesOfInitialValuesFitInt32Slot(LayoutID layout, uint32_t field) const { return m_fieldsStartedWithOtherTypes.isEmpty() || !m_fieldsStartedWithOtherTypes[layout.index()].get(field); }

private:
    Vector<Vector<OptionSet<StoredKind>>> m_kinds;
    Vector<Vector<OptionSet<StoredKind>>> m_parameterKinds;
    Vector<BitVector> m_parametersPassedOtherTypes;
    Vector<BitVector> m_fieldsUsedAsIntegers;
    Vector<BitVector> m_fieldsStartedWithOtherTypes;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
