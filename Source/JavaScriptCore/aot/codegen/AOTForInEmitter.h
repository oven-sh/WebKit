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
    explicit ForInEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateForInOpen(const ValueContext&);
    void generateForInStep(const ValueContext&);

    struct EnumeratorFields {
        Word32 cachedStructureID;
        Word32 indexedLength;
        Word32 endStructurePropertyIndex;
        Word32 endOwnPropertyIndex;
        Word32 endGenericPropertyIndex;
    };

    EnumeratorFields loadFields(CellPointer enumerator);
    Integer nameIndexOf(const EnumeratorFields& fields, Integer position) { return sub(position, zeroExtended(fields.indexedLength)); }
    Condition isInStructurePart(const EnumeratorFields& fields, Integer nameIndex) { return below(nameIndex, zeroExtended(fields.endStructurePropertyIndex)); }
    Condition isOwnName(const EnumeratorFields& fields, Integer nameIndex) { return below(nameIndex, zeroExtended(fields.endOwnPropertyIndex)); }
    Condition hasCachedKeys(CellPointer enumerator, const EnumeratorFields&, CellPointer object);
    Condition describesOrdinaryObject(CellPointer enumerator) { return isZero(loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedOrderWordMask)); }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
