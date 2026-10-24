/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRelocation.h"
#include "B3RelocatableValue.h"
#include "B3RelocationLowering.h"
#include "GPRInfo.h"

namespace JSC { namespace AOT {

class JS_EXPORT_PRIVATE ImageRelocationLowering final : public B3::RelocationLowering {
    WTF_MAKE_NONCOPYABLE(ImageRelocationLowering);
public:
    struct PointerTable {
        RelocationSpace space;
        GPRReg base;
        int32_t offset;
        bool offsetHoldsTablePointer;
        std::optional<uint32_t> (*runtimeFunctionSlot)(B3::RuntimeFunction) { nullptr };
    };

    static PointerTable instanceRuntimeTable();

    ImageRelocationLowering(PointerTable, RelocationRecorder&);

    B3::Value* lower(B3::Procedure&, B3::InsertionSet&, size_t index, const B3::RelocatableValue&) final;

private:
    B3::Value* loadSlot(B3::InsertionSet&, size_t index, B3::Origin, uint32_t slot) const;
    B3::Value* insertAddressOf(B3::InsertionSet&, size_t index, B3::Origin, CodeSymbol);

    PointerTable m_table;
    RelocationRecorder& m_relocations;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
