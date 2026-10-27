/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"

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

// The slot of a field whose type is only expected is boxed.
TypeID slotTypeOf(const Program&, const Field&);
// FieldData::type: what the run time tests a value against that it stores into the slot.
inline TypeID recordedTypeOf(const Program& program, const Field& field) { return TypeTable::valuesOf(slotTypeOf(program, field)); }

// The slot of a number or a boolean keeps the markers for undefined and null whatever the type of its field says, until flow proves that nothing outside the type is stored.
// The types of reads and stores, the plan of the slots and the check of the plan all ask here.
bool slotHoldsUndefinedAndNull(const Program&, FieldReference);
// A number or a boolean that every constructor stores before the object leaves its hands.
bool holdsRawValueFromBirth(const Program&, FieldReference);

bool canBeStoredBehindAllocation(const Field&);
// Something that fits the slot and stands in it until the store behind the allocation. There is none for a slot that holds only objects, functions, symbols or BigInts.
std::optional<Value> tryCreateFiller(const Program&, const Type&);
bool canStoreInitialValueBehindAllocation(const Program&, const Value& allocation, uint32_t valueIndex);
void storeUnprovenInitialValues(const Program&, FunctionID, Function&, TypeArena&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
