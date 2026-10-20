/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStringEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class StringRangeReadEmitter final : public OperationEmitter {
public:
    explicit StringRangeReadEmitter(CodeGenerator&);

    void generateStringRangeCodeUnitOrNaN(const ValueContext&);
    void generateStringRangeCodeUnitOrMinusOne(const ValueContext&);
    void generateStringRangeCodePointOrUndefined(const ValueContext&);
    void generateStringRangeCharacterOrEmpty(const ValueContext&);
    void generateStringRangeCharacterOrUndefined(const ValueContext&);
    void generateStringRangeEqualsLiteral(const ValueContext&);
    void generateStringRangeToNumber(const ValueContext&);
    void generateStringRangeParseInt(const ValueContext&);
    void generateStringRangeSubstringOrUndefined(const ValueContext&);

    Number toNumber(const CharacterView&);
    Number parseInt(const CharacterView&, Word32 radix);

private:
    struct IndexedRead {
        CharacterView view;
        Word32 index;
        Condition isInRange;
    };

    CharacterView viewOfRange(const ValueContext&);
    IndexedRead openIndexedRead(const ValueContext&);
    void defineCharacter(const ValueContext&, const IndexedRead&, Boxed whenOutOfRange);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
