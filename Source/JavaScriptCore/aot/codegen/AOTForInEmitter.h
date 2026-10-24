/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ForInEmitter final : public OperationEmitter {
public:
    explicit ForInEmitter(CodeGenerator&);

    void generateForInOpen(const ValueContext&);
    void generateForInStep(const ValueContext&);

    struct EnumeratorFields {
        Word32 cachedStructureID;
        Word32 indexedLength;
        Word32 endStructurePropertyIndex;
        Word32 endGenericPropertyIndex;
    };

    EnumeratorFields loadFields(CellPointer enumerator);
    Integer nameIndexOf(const EnumeratorFields&, Integer position);
    Condition isInStructurePart(const EnumeratorFields&, Integer nameIndex);
    Condition hasCachedKeys(CellPointer enumerator, const EnumeratorFields&, CellPointer object);
    Condition describesOrdinaryObject(CellPointer enumerator);
    CellPointer nameAt(CellPointer enumerator, Integer nameIndex);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
