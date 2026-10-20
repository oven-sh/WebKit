/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsPropertyProbes.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

struct ExtraPropertyRegisters {
    GPRReg object;
    GPRReg extraProperties;
    GPRReg wordIndex;
    GPRReg scratch;
};

struct ExtraPropertyGrowthRegisters {
    ExtraPropertyRegisters access;
    GPRReg oldCapacity;
    GPRReg storage;
    GPRReg scratch;
};

// Nothing is written before a jump to any list. At growsStorage and at cannot the addition is still right, and operationAOTAddPropertyBySlot does it.
// At growsStorage extraProperties has the cell, scratch the StructureID that the cell gets, wordIndex the word index of the property.
struct ExtraPropertyAdditionExits {
    JumpList isNotExtensible;
    JumpList growsStorage;
    JumpList cannot;
};

// Ends the stub, after a write barrier on owner if valueMayNeedBarrier.
using StoreFinish = ScopedLambda<void(GPRReg owner, bool valueMayNeedBarrier)>;

void loadExtraProperties(StubAssembler&, GPRReg object, GPRReg hiddenWordOffset, GPRReg result, JumpList& hasNone);
// The property is at BaseIndex(extraProperties, wordIndex, TimesEight): extraProperties has the cell or its butterfly.
void locateExtraProperty(StubAssembler&, const CachedSlot&, const ExtraPropertyRegisters&, JumpList& miss);
void readExtraPropertyAndReturn(StubAssembler&, const CachedSlot&, const ExtraPropertyRegisters&, GPRReg result, JumpList& miss);
void storeExtraProperty(StubAssembler&, const CachedSlot&, const ExtraPropertyRegisters&, GPRReg value, const StoreFinish&, JumpList& miss);
void addExtraProperty(StubAssembler&, const CachedSlot&, const ExtraPropertyRegisters&, GPRReg value, const StoreFinish&, ExtraPropertyAdditionExits&);
// Goes on from growsStorage. Reads the header of the slot before it writes oldCapacity, storage or scratch, so one of them may hold the header.
void addExtraPropertyInNewStorage(StubAssembler&, const CachedSlot&, const ExtraPropertyGrowthRegisters&, GPRReg value, const StoreFinish&, JumpList& cannot);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
