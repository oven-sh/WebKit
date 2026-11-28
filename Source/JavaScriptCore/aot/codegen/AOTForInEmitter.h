/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

class ForInEmitter final : public OperationEmitter {
public:
    explicit ForInEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateForInOpen(const ValueContext&);
    void generateForInOpenCountingNames(const ValueContext&);
    void generateForInStep(const ValueContext&);

    struct EnumeratorFields {
        Word32 cachedStructureIDForNames;
        Word32 indexedLength;
        Word32 endOwnPropertyIndex;
        Word32 endGenericPropertyIndex;
    };

    EnumeratorFields loadFields(CellPointer enumerator);
    Integer nameIndexOf(const EnumeratorFields& fields, Integer position) { return sub(position, zeroExtended(fields.indexedLength)); }
    Integer nameIndexOf(CellPointer enumerator, Integer position) { return sub(position, zeroExtended(loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_indexLength))); }
    Condition isOwnName(const EnumeratorFields& fields, Integer nameIndex) { return below(nameIndex, zeroExtended(fields.endOwnPropertyIndex)); }
    Condition hasCachedKeys(CellPointer enumerator, const EnumeratorFields&, CellPointer object);
    Condition isInStructurePart(CellPointer enumerator, Integer nameIndex) { return below(nameIndex, zeroExtended(loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_endStructurePropertyIndex))); }
    Condition describesOrdinaryObject(CellPointer enumerator) { return isZero(loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedOrderWordMask)); }
    // EnumeratorGetByVal loads the value of a name of an ordinary object from the slot at the index of the name. The index is the offset only under the enumerator's Structure for names and offsets,
    // which is null or the Structure for names that hasCachedKeys() tests. A compiled object has none and is asked by the name.
    Condition canLoadValues(CellPointer enumerator) { return logicalOr(isNotZero(loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedStructureIDForNamesAndOffsets)), logicalNot(describesOrdinaryObject(enumerator))); }

private:
    std::array<Operand, 2> openEnumerator(const ValueContext&, bool countsCurrentNames);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
