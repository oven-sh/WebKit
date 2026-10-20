/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

struct FieldAccess;

class SlotRunEmitter final : public OperationEmitter {
public:
    explicit SlotRunEmitter(CodeGenerator&);

    void generatePutSlots(const ValueContext&);

private:
    struct Entry;
    struct Run;

    void storeRun(const ValueContext&, CellPointer, const Run&);
    Condition isMissing(CellPointer, const Entry&);
    void listMissingFields(const ValueContext&, CellPointer, const Run&);
    void listFieldsInRuntime(const ValueContext&, CellPointer, const Run&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
