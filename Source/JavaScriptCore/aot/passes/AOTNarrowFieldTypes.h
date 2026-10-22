/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

void markEnforcedFields(Program&, BuildContext&);
struct Int32ParameterCandidate {
    FunctionID function;
    uint32_t entryParameterIndex { 0 };
};

Vector<Int32ParameterCandidate> narrowFieldTypes(Program&, BuildContext&);
void expectInt32ParametersWithRemainingChecks(Program&, std::span<const Int32ParameterCandidate>, BuildContext&);

// The slot of a field whose type is only expected is boxed. The rule of TypingContext::typeIDOf() at TypeLevel::Proven.
// The slot of an accessor holds its GetterSetter: Field::slotType is the type of the getter.
TypeID slotTypeOf(const Program&, const Field&);

// The slot of a number or a boolean keeps the markers for undefined and null whatever the type of its field says, until flow proves that nothing outside the type is stored.
// Either stays in the slot: it is no value of another type. A read that has not tested the slot finds them.
bool slotHoldsUndefinedAndNull(const Program&, FieldReference);
// A number or a boolean that every constructor stores before the object leaves its hands. Whoever puts a marker into such a slot sets the mark of the cell,
// so while the mark is off the slot holds the raw value.
bool holdsRawValueFromBirth(const Program&, FieldReference);

bool canBeStoredBehindAllocation(const Field&);
// Something that fits the slot and stands in it until the store behind the allocation. There is none for a slot that holds only objects, functions, symbols or BigInts.
std::optional<Value> tryCreateFiller(const Program&, const Type&);
bool canStoreInitialValueBehindAllocation(const Program&, const Value& allocation, uint32_t valueIndex);
void storeUnprovenInitialValues(const Program&, FunctionID, Function&, TypeArena&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
