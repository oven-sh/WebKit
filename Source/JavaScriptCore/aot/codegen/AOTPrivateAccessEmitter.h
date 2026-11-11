/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTSlotPlan.h"

namespace JSC { namespace AOT { namespace Typed {

class PrivateAccessEmitter final : public OperationEmitter {
public:
    explicit PrivateAccessEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generatePrivateGet(const ValueContext&);
    void generatePrivateSet(const ValueContext&);
    void generatePrivateDefine(const ValueContext&);
    void generatePrivateHas(const ValueContext&);

private:
    struct SlotsOfProvenReceiver {
        FieldAccess name;
        FieldAccess brand;
    };

    std::optional<FieldAccess> findBoxedSlotInCellOfEveryReceiver(const Type& receiver, PropertyKeyID, bool isStore);
    TypedPointer addressOfSlot(CellPointer object, const FieldAccess&);
    std::optional<SlotsOfProvenReceiver> findSlotsOfProvenReceiver(const ValueContext&, bool isStore);
    TypedPointer slotOfNameIfEvaluationMadeObject(const SlotsOfProvenReceiver&, CellPointer object, CellPointer token, LBasicBlock asksRunTime);
    uint16_t indexInLayoutRecord(const ValueContext&);
    bool isEvaluatedOnce(const ValueContext&);
    bool findsLocationInLine(const ValueContext&);
    TypedPointer locationInKnownStructure(const ValueContext&, Boxed object, CellPointer token, LBasicBlock isUnknown);
    Boxed callPrivateGet(const ValueContext&, Boxed object, CellPointer token);
    void storePrivateField(const ValueContext&, RuntimeOperation, bool isOnRarePath = false);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
