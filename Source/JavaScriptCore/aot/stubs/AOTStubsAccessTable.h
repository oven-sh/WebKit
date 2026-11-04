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

enum class PartOfAccessTable : uint8_t { Reads, Stores, Calls };

// cell is a cell and name the number of a name, with nothing above its 32 bits. Both are kept. On ARM64 the scratch register of the assembler is written too.
struct AccessTableRegisters {
    GPRReg cell;
    GPRReg name;
    // On ARM64 the whole key. On x86-64 its low half: the halves are compared and stored where they lie.
    GPRReg key;
    GPRReg place;
};

void locatePlaceInAccessTable(StubAssembler&, PartOfAccessTable, const AccessTableRegisters&, unsigned numberOfArguments = 0);
// Takes what locatePlaceInAccessTable() has left. Where the key at the offset is the key, leaves the word behind it in word. scratch is written on ARM64 alone.
void loadWordBehindKeyInAccessTable(StubAssembler&, const AccessTableRegisters&, ptrdiff_t offsetOfKey, GPRReg word, GPRReg scratch, JumpList& hasOtherKey);

struct AccessTableRead {
    AccessTableRegisters registers;
    GPRReg word;
    // Written on ARM64 alone.
    GPRReg scratch;
};

// The whole hit: the place, one comparison, the value. At both lists the receiver and the name are as they were.
// At isAnsweredByFieldTable word holds the form and the operand: FieldTableProbe::hasFormAndOperand.
void returnValueByAccessTable(StubAssembler&, const AccessTableRead&, JumpList& isNotAnswered, JumpList& isAnsweredByFieldTable);
// answer, which is written, has AccessTable::answerHasForm. A value in a butterfly is read into result, which may be answer, and finish, which leaves the stub, goes on with it.
// At findsGetterInOwnStorage answer is as it was.
void readByFormOfAnswerInAccessTable(StubAssembler&, GPRReg cell, GPRReg answer, GPRReg scratch, GPRReg result, JumpList& isAnsweredByFieldTable, JumpList& findsGetterInOwnStorage, const ScopedLambda<void()>& finish);
void loadValueInOwnStorageByAnswer(StubAssembler&, GPRReg cell, GPRReg answer, GPRReg scratch, GPRReg result);
void loadValueInStorageOfHolderByAnswer(StubAssembler&, GPRReg answer, GPRReg scratch, GPRReg result);

// The receiver is a compiled object that has every field in its slot, and the slot of the field of the name is empty. At the list the receiver and the name are as they were.
void returnWhatStandsAboveFieldLeftOut(StubAssembler&, const AccessTableRead&, JumpList& isNotAnswered);

struct AccessTableStore {
    AccessTableRegisters registers;
    GPRReg value;
    GPRReg answer;
    GPRReg scratch;
};

// For a store that is no definition. At both lists the receiver, the name and the value are as they were.
// At isStoredByFieldTable the order word forbids nothing and answer holds the form and the operand: FieldTableProbe::hasFormAndOperand.
void storeByAccessTableAndReturn(StubAssembler&, const AccessTableStore&, JumpList& isNotStored, JumpList& isStoredByFieldTable);

// AccessTable::answerOfFieldTable().
void makeAnswerOfFieldTable(StubAssembler&, GPRReg formAndOperand, GPRReg answer);
// AccessTable::answerInOwnStorage(), of an index with its sign in all 64 bits.
void makeAnswerInOwnStorage(StubAssembler&, GPRReg wordIndexAndAnswer);
// AccessTable::answerOfGetterInOwnStorage(), of such an index.
void makeAnswerOfGetterInOwnStorage(StubAssembler&, GPRReg wordIndexAndAnswer);
// AccessTable::answerInStorageOfHolder(), of a cell of the front table that says so. The two may be one register.
void makeAnswerInStorageOfHolder(StubAssembler&, GPRReg frontCell, GPRReg answer);

// Each of these goes on behind itself, whether or not it has filed, and writes key and place. What it is handed beside them is kept.
// For a compiled object and a field of the form InlineValue.
void fileOwnValueInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg offsetInCell);
// For a compiled object and a field of any other form.
void fileAnswerOfFieldTableInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg answer);
// For what a cell of the front table says of any other Structure. A dictionary changes without becoming another Structure, so nothing is filed for one.
void fileOwnValueOfOrdinaryCellInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg offsetInCell);
void fileOwnValueInStorageInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg answer);
// In these three the receiver may be no cell: then nothing is filed. In the first place holds the holder. The last writes answer.
void fileValueAboveInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg slot);
void fileValueInStorageOfHolderInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg answer);
void fileAbsenceInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg answer);
// For returnWhatStandsAboveFieldLeftOut(), by whoever has a Slot that says so. answer: the address of the word in the cell of the holder, or AccessTable::answerInStorageOfHolder(). The second writes answer.
void fileValueAboveFieldLeftOutInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg holder, GPRReg answer);
void fileAbsenceAboveFieldLeftOutInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg answer);
// answer: AccessTable::Store::answer.
void fileStoreToFieldInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg answer, uint32_t orderWordBitsThatForbid);
void fileStoreToOrdinaryCellInAccessTable(StubAssembler&, const AccessTableRegisters&, GPRReg answer);
// The second half of the Read of the receiver and the name was found a moment ago, it is an address, and the word there holds the callee. closureWord, which is written, is the word of the callee, and says that a call with so many arguments enters it as it is.
void fileCallInAccessTable(StubAssembler&, const AccessTableRegisters&, unsigned numberOfArguments, GPRReg callee, GPRReg closureWord);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
