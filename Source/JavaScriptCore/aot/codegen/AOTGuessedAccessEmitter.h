/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

// An access of KnowledgeLevel::Expected: a guess that is tested at its use.
// One operation with the effects of the generic one. The passing side does what the operation does at KnowledgeLevel::Proven. The failing side does the generic operation on a rare path, and both go on together.
// The receiver, the position, the stored value and the result are JSValues: nothing is proven of any of them, and the type of the result is only expected.
// But for readTakesIndexOfProvenList(): the step of an iteration has tested for a list and counts the index itself.
// The emitter of each such operation asks testsForItself() first and hands the operation over.
class GuessedAccessEmitter final : public OperationEmitter {
public:
    explicit GuessedAccessEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    template<typename Context> static bool testsForItself(const Context& access) { return access->level() == KnowledgeLevel::Expected; }

    // GetSlot, GetSlotOrUndefined, GetDeferredClosureSlot, GetDeferredClosureSlotOrUndefined, or else PropGet.
    void generateSlotRead(const ValueContext&);
    void generateSlotStore(const ValueContext&);
    // GetArrayElement, GetTypedArrayElement, or else ElemGet.
    void generateElementRead(const ValueContext&);
    // PutArrayElement, PutTypedArrayElement, or else ElemSet.
    void generateElementStore(const ValueContext&);
    // HasArrayElement, or else ElemHas.
    void generateElementTest(const ValueContext&);
    // A GetArrayElement at either level that names no kind of storage and gives an unboxed number, of a proven list under an integer. False: it is no such read, and nothing was emitted.
    bool tryReadNumberOfProvenList(const ValueContext&);

private:
    // A cell of one of the guessed layouts.
    CellPointer receiverOrElse(const ValueContext&, Boxed object, LayoutSetID receivers, LBasicBlock failure);
    // An array in the guessed storage, or a view of the guessed type, and a position that is an int32. Of an access that has code for several kinds of storage: an array.
    CellPointer elementsOwnerOrElse(const ValueContext&, Boxed object, LBasicBlock failure);
    Word32 indexOrElse(Boxed position, LBasicBlock failure);
    void readElementOfProvenList(const ValueContext&);
    void readNumberOfProvenList(const ValueContext&);
    void storeInProvenListUnderAnyKey(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
