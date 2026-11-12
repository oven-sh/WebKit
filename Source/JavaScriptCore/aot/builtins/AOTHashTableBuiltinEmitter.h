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
enum class LookupAnswer : uint8_t { IsPresent, Value };

class HashTableBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit HashTableBuiltinEmitter(CodeGenerator&);

    FOR_EACH_AOT_INLINE_BUILTIN_IN_HASH_TABLE_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)

    void remove(const InlineCall&, HashTableOwner);
    Boxed normalizedKey(const OperationContext&, ValueID);
    Word32 hashOfBits(Boxed normalizedKey);
    Integer sizeOf(CellPointer table, HashTableOwner);
    Integer findPlace(const OperationContext&, ValueID map, ValueID key);

private:
    bool isSoughtAtItsPlace(const OperationContext&, ValueID key);
    Boxed normalizedNumber(Number);
    void lookUp(const InlineCall&, HashTableOwner, LookupAnswer);
    Operand search(const OperationContext&, ValueID collection, ValueID key, HashTableOwner, LookupAnswer);
    Operand lookUpByStub(const OperationContext&, CellPointer owner, Boxed key, StubKey, LookupAnswer);
    void insert(const InlineCall&, HashTableOwner);
    void generateSize(const InlineCall&, HashTableOwner);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
