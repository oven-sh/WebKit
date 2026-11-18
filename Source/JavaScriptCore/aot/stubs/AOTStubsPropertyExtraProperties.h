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

struct ExtraPropertyAdditionExits {
    JumpList isNotExtensible;
    JumpList growsStorage;
    JumpList cannot;
};

using StoreFinish = ScopedLambda<void(GPRReg owner)>;

void loadExtraProperties(StubAssembler&, GPRReg object, GPRReg hiddenWordOffset, GPRReg result, JumpList& hasNone);
void loadExtraPropertiesByStructure(StubAssembler&, GPRReg object, GPRReg structure, GPRReg scratch, GPRReg result, JumpList& hasNone);
void locateExtraProperty(StubAssembler&, const CachedSlot&, const ExtraPropertyRegisters&, JumpList& miss);
void readExtraPropertyAndReturn(StubAssembler&, const CachedSlot&, const ExtraPropertyRegisters&, GPRReg result, JumpList& miss);
void loadExtraPropertyByFrontCell(StubAssembler&, GPRReg cell, GPRReg object, GPRReg storage, GPRReg wordIndex, GPRReg result, JumpList& hasNone);
#if CPU(X86_64)
void loadExtraPropertyByFrontCell(StubAssembler&, const BaseIndex& cell, bool isOutOfLine, GPRReg object, GPRReg word, GPRReg result, JumpList& hasNone);
#endif
void storeExtraProperty(StubAssembler&, const CachedSlot&, const ExtraPropertyRegisters&, GPRReg value, const StoreFinish&, JumpList& miss);
void addExtraProperty(StubAssembler&, const CachedSlot&, const ExtraPropertyRegisters&, GPRReg value, ExtraPropertyAdditionExits&);
void addExtraPropertyInNewStorage(StubAssembler&, const CachedSlot&, const ExtraPropertyGrowthRegisters&, GPRReg value, JumpList& cannot);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
