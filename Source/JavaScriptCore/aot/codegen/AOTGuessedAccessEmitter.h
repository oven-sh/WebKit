/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class GuessedAccessEmitter final : public OperationEmitter {
public:
    explicit GuessedAccessEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    template<typename Context> static bool testsForItself(const Context& access) { return access->level() == KnowledgeLevel::Expected; }

    void generateSlotRead(const ValueContext&);
    void generateSlotStore(const ValueContext&);
    void generateElementRead(const ValueContext&);
    void generateElementStore(const ValueContext&);
    void generateElementTest(const ValueContext&);
    bool tryReadNumberOfProvenList(const ValueContext&);

private:
    CellPointer receiverOrElse(const ValueContext&, Boxed object, LayoutSetID receivers, LBasicBlock failure, const FieldAccess* readBehindTest = nullptr, std::optional<CellPointer>* baseOfSlot = nullptr);
    CellPointer elementsOwnerOrElse(const ValueContext&, Boxed object, LBasicBlock failure);
    Word32 indexOrElse(Boxed position, LBasicBlock failure);
    bool isTestedForPresence(const ValueContext& read);
    void readElementOfProvenList(const ValueContext&);
    Condition hasElementOfList(const ValueContext&);
    void readNumberOfProvenList(const ValueContext&);
    void storeInProvenListUnderAnyKey(const ValueContext&);
    bool unitReckonsWithElement(const ValueContext& read);

    bool m_reckonersAreKnown { false };
    BitVector m_isReckonedWith;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
