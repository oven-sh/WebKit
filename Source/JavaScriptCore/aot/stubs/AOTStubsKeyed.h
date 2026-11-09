/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsPropertyProbes.h"

namespace JSC { namespace AOT {

void loadNameOfKey(StubAssembler&, GPRReg key, GPRReg uid, GPRReg scratch, JumpList& hasNoName);
void callLeafLookup(StubAssembler&, Entry, GPRReg second, std::optional<GPRReg> value, GPRReg baseAfterCall, const RegisterSet& lostAnyway = { });
void rememberAbsenceOfKey(StubAssembler&, GPRReg key, GPRReg header, GPRReg place, GPRReg scratch);
void rememberOwnGetterOfKey(StubAssembler&, GPRReg key, GPRReg header, GPRReg wordIndex, GPRReg place, GPRReg scratch);
void rememberGetterAboveOfKey(StubAssembler&, GPRReg key, GPRReg header, GPRReg addressOfWord, GPRReg place, GPRReg scratch);
void rememberPlaceOfExtraProperty(StubAssembler&, SlotAccess, const CachedSlot&, GPRReg object, GPRReg key, GPRReg place, GPRReg structureID);
void rememberPlaceOfAddition(StubAssembler&, const CachedSlot&, GPRReg object, GPRReg key, GPRReg place, GPRReg scratch);
void branchUnlessKindOfValueIsAccepted(StubAssembler&, GPRReg value, GPRReg type, GPRReg kind, GPRReg entry, JumpList& isNotKnownToBeAccepted);
void rememberAcceptedKindOfValue(StubAssembler&, GPRReg value, GPRReg type, GPRReg filedInPlaceOfType, GPRReg place, GPRReg scratch, GPRReg secondScratch);
void rememberPlaceOfValueInField(StubAssembler&, GPRReg object, GPRReg key, GPRReg value, GPRReg type, GPRReg address, GPRReg classOfSlot, GPRReg place, GPRReg scratch, GPRReg secondScratch);
void rememberPlaceOfFieldOfAnyValue(StubAssembler&, GPRReg object, GPRReg key, GPRReg offsetOfSlot, GPRReg place, GPRReg scratch);

static_assert(firstStubOperandRegister == StubRegister::R0 && keyCellRegister == StubRegister::A1 && storedValueByValRegister == StubRegister::A2);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
