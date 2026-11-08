/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubsAccessTableWords.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

enum class PartOfAccessTable : uint8_t { Reads, Calls };

struct AccessTableRegisters {
    GPRReg cell;
    GPRReg name;
    GPRReg key;
    GPRReg place;
};

void locatePlaceInAccessTable(StubAssembler&, PartOfAccessTable, const AccessTableRegisters&, unsigned numberOfArguments = 0);
void loadWordBehindKeyInAccessTable(StubAssembler&, const AccessTableRegisters&, ptrdiff_t offsetOfKey, GPRReg word, GPRReg scratch, JumpList& hasOtherKey);

struct AccessTableRead {
    AccessTableRegisters registers;
    GPRReg word;
    GPRReg scratch;
    bool waysOnwardAreCalls { false };
};

void returnValueByAccessTable(StubAssembler&, const AccessTableRead&, JumpList& isNotAnswered);
void readByFormOfAnswerInAccessTable(StubAssembler&, GPRReg cell, GPRReg answer, GPRReg scratch, GPRReg result, JumpList& findsGetterInOwnStorage, const ScopedLambda<void()>& finish);
void loadValueInOwnStorageByAnswer(StubAssembler&, GPRReg cell, GPRReg answer, GPRReg scratch, GPRReg result);
void loadValueInStorageOfHolderByAnswer(StubAssembler&, GPRReg answer, GPRReg scratch, GPRReg result);

void returnWhatStandsAboveFieldLeftOut(StubAssembler&, const AccessTableRead&, JumpList& isNotAnswered);

struct AccessTableStore {
    AccessTableRegisters registers;
    GPRReg value;
    GPRReg answer;
    GPRReg scratch;
};

void makeAnswerOfFieldTable(StubAssembler&, GPRReg formAndOperand, GPRReg answer);
void makeAnswerInOwnStorage(StubAssembler&, GPRReg wordIndexAndAnswer);
void makeAnswerOfGetterInOwnStorage(StubAssembler&, GPRReg wordIndexAndAnswer);
void makeAnswerInStorageOfHolder(StubAssembler&, GPRReg frontCell, GPRReg answer);

void fileOwnValueInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg offsetInCell);
void fileOwnValueOfOrdinaryCellInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg offsetInCell);
void fileOwnValueInStorageInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg answer);
void fileValueAboveInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg slot);
void fileValueInStorageOfHolderInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg answer);
void fileAbsenceInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg answer);
void fileValueAboveFieldLeftOutInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg holder, GPRReg answer);
void fileAbsenceAboveFieldLeftOutInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg answer);
void fileCallInAccessTable(StubAssembler&, const AccessTableRegisters&, unsigned numberOfArguments, GPRReg callee, GPRReg closureWord);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
