/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTUseLists.h"
#include "AOTVector.h"
#include "AOTBitVector.h"

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class IRBuilder;

void replaceUse(Function&, const Use&, ValueID replacement);
bool acceptsCastOfValue(const Function&, const Use&);
void becomeUpcast(Function&, ValueID, ValueID operand);
void becomeUndefined(Function&, ValueID);

class ValueSubstitution {
public:
    explicit ValueSubstitution(const Function& function)
        : m_numberOfValues(function.values.size())
    {
    }

    void replace(ValueID from, ValueID to);
    ValueID resolve(ValueID) const;
    bool isEmpty() const { return !m_numberOfReplacements; }
    void apply(Function&, FunctionAnalyses*);

private:
    mutable Vector<ValueID> m_replacements;
    size_t m_numberOfValues;
    unsigned m_numberOfReplacements { 0 };
};

void replaceReadWithValue(const Program&, Function&, ValueSubstitution&, ValueID read, ValueID replacement);

void keepReadForValidation(IRBuilder&, ProgramPoint read, ValueID replacement);
void appendKeptReadForValidation(IRBuilder&, BlockValues&, ValueID movedRead);
BitVector findReadsKeptForValidation(const Function&);

bool isTestedByValidation(const Function&, const UseLists&, ValueID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
