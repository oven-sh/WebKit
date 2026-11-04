/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "JSType.h"
#include "PropertyOffset.h"
#include "StructureID.h"

namespace JSC { namespace AOT {

struct Instance;

// In BuiltinWords::ownSlotStructureIDs, for an own slot whose Structure was looked for and does not hold the property where compiled code reads it. No cell has this number.
inline constexpr uint32_t structureIDOfNoInstance = StructureID::nukedStructureIDBit;

// The table counts the properties that the constructor defines. Only a plain object has room for properties inside its cell.
constexpr PropertyOffset propertyOffsetOfOwnSlot(JSType typeOfInstances, uint8_t positionAmongOwnSlots)
{
    return typeOfInstances == FinalObjectType ? positionAmongOwnSlots : firstOutOfLineOffset + positionAmongOwnSlots;
}

// Finds the Structure that the constructor leaves an instance with once it has defined the properties up to the own slot, and gives compiled code its number.
void publishStructureOfOwnSlot(Instance&, BuiltinOverloadID);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
