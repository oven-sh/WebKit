/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTStubsCollections.h"

namespace JSC { namespace AOT { namespace Typed {

enum class HashTableOwner : uint8_t { Map, Set };

class HashTableBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit HashTableBuiltinEmitter(CodeGenerator&);

    FOR_EACH_AOT_INLINE_BUILTIN_IN_HASH_TABLE_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)

    void remove(const InlineCall&, HashTableOwner);
    Boxed normalizedKey(const OperationContext&, ValueID);
    Word32 hashOfBits(Boxed normalizedKey);
    Integer sizeOf(CellPointer table, HashTableOwner);

private:
    Boxed normalizedNumber(const OperationContext&, Number);
    void lookUp(const InlineCall&, HashTableOwner, bool returnsValue);
    bool looksAmongTags(const InlineCall&);
    Boxed lookUpAmongTags(const InlineCall&, CellPointer owner, CellPointer key, StubKey, bool returnsValue);
    void insert(const InlineCall&, HashTableOwner);
    void generateSize(const InlineCall&, HashTableOwner);
};

class KeyPartsLookupEmitter final : public BuiltinFamilyEmitter {
public:
    explicit KeyPartsLookupEmitter(CodeGenerator&);

    void generateMapGetByKeyParts(const ValueContext&);
    void generateHasByKeyParts(const ValueContext&);

private:
    Pointer partsBuffer(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
