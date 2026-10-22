/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

struct FieldAccess;
struct StateOfNewObject;

class SlotRunEmitter final : public OperationEmitter {
public:
    explicit SlotRunEmitter(CodeGenerator&);

    void generatePutSlots(const ValueContext&);

private:
    struct Entry;
    struct Run;

    void storeRun(const ValueContext&, CellPointer, const Run&);
    Condition isMissing(CellPointer, const Entry&);
    void listMissingFields(const ValueContext&, CellPointer, const Run&, const std::optional<StateOfNewObject>&);
    bool listFieldsInRuntime(const ValueContext&, CellPointer, const Run&, std::optional<uint32_t> placePlusOne);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
