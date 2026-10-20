/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTUseLists.h"
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class IRBuilder;

void replaceUse(Function&, const Use&, ValueID replacement);
bool acceptsCastOfValue(const Function&, const Use&);
void becomeUpcast(Function&, ValueID, ValueID operand);
void becomeUndefined(Function&, ValueID);

class ValueSubstitution {
public:
    explicit ValueSubstitution(const Function&);

    void replace(ValueID from, ValueID to);
    ValueID resolve(ValueID) const;
    bool isEmpty() const { return !m_numberOfReplacements; }
    void apply(Function&, FunctionAnalyses*);

private:
    void ensureSize(size_t);

    mutable Vector<ValueID> m_replacements;
    unsigned m_numberOfReplacements { 0 };
};

void replaceReadWithValue(const Program&, Function&, ValueSubstitution&, ValueID read, ValueID replacement);

// In a validating build a copy of a read that a pass replaces or moves stays where the read was, and ValidateSameValue compares it with what took the read's place.
// No pass replaces or moves the copy. The insertions are left for the caller to apply.
void keepReadForValidation(IRBuilder&, ProgramPoint read, ValueID replacement);
void appendKeptReadForValidation(IRBuilder&, BlockValues&, ValueID movedRead);
BitVector findReadsKeptForValidation(const Function&);

// A test of a validating build says something only about a value that the program really looked up: a read that a pass answers from the results under test would agree with them.
bool isTestedByValidation(const Function&, const UseLists&, ValueID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
