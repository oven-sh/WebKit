/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsPropertyProbes.h"

namespace JSC { namespace AOT {

void loadNameOfKey(StubAssembler&, GPRReg key, GPRReg uid, GPRReg scratch, JumpList& hasNoName);
// In a stub that keeps the registers of its caller, lostAnyway names those it need not keep.
void callLeafLookup(StubAssembler&, Entry, GPRReg second, std::optional<GPRReg> value, GPRReg baseAfterCall, const RegisterSet& lostAnyway = { });
// A slot for reads has said that nothing is found under the key. Its header names the Structure.
void rememberAbsenceOfKey(StubAssembler&, GPRReg key, GPRReg header, GPRReg place, GPRReg scratch);
// The register of the StructureID of the object is overwritten.
void rememberPlaceOfExtraProperty(StubAssembler&, SlotAccess, const CachedSlot&, GPRReg object, GPRReg key, GPRReg place, GPRReg structureID);
// PropertyCacheWords::placesFiledFromSlots.
Jump branchIfEveryPlaceWasFiledFromSlots(StubAssembler&);
// A slot for stores has said that the object gains the name in the storage that it has.
void rememberPlaceOfAddition(StubAssembler&, const CachedSlot&, GPRReg object, GPRReg key, GPRReg place, GPRReg scratch);
// The type of a field has accepted the value, which is stored at the address, in the cell of the object. classOfSlot has the AcceptedKindOfValue::ClassOfSlot. Keeps the object and the value.
void rememberPlaceOfValueInField(StubAssembler&, GPRReg object, GPRReg key, GPRReg value, GPRReg type, GPRReg address, GPRReg classOfSlot, GPRReg place, GPRReg scratch, GPRReg secondScratch);

static_assert(firstStubOperandRegister == StubRegister::R0 && keyCellRegister == StubRegister::A1 && storedValueByValRegister == StubRegister::A2);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
