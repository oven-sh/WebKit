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
// ValueOrNothing: the value, or no value at all where the key is absent.
enum class LookupAnswer : uint8_t { IsPresent, Value, ValueOrNothing };

class HashTableBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit HashTableBuiltinEmitter(CodeGenerator&);

    FOR_EACH_AOT_INLINE_BUILTIN_IN_HASH_TABLE_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)

    void remove(const InlineCall&, HashTableOwner);
    Boxed normalizedKey(const OperationContext&, ValueID);
    Word32 hashOfBits(Boxed normalizedKey);
    Integer sizeOf(CellPointer table, HashTableOwner);
    CellPointer moveTableToHeap(const OperationContext&, CellPointer owner);
    ValueID findGetBehindHas(const InlineCall& has, InlineBuiltin get);
    void didFindValueFor(ValueID get, Boxed valueOrNothing);
    std::optional<Boxed> valueFoundFor(ValueID get);

private:
    Boxed normalizedNumber(const OperationContext&, Number);
    Word32 hashOfBitsByOneMultiplication(Boxed normalizedKey);
    LValue tagsNearTag(LValue eightTags, LValue tag, uint8_t distance);
    void lookUp(const InlineCall&, HashTableOwner, LookupAnswer);
    bool hasReceiverThatLeavesLoop(const InlineCall&);
    bool looksAmongTags(const InlineCall&);
    bool looksIntoHomeSlot(const InlineCall&, KeyComparison);
    Boxed lookUpInHomeSlot(const InlineCall&, CellPointer owner, Boxed key, KeyComparison, HashTableOwner, LookupAnswer, ValueID valueRead);
    Boxed lookUpAmongTags(const InlineCall&, CellPointer owner, CellPointer key, StubKey, LookupAnswer, ValueID valueRead);
    void insert(const InlineCall&, HashTableOwner);
    void generateSize(const InlineCall&, HashTableOwner);

    // What the search of `map.has(key)` found, for the `map.get(key)` behind it.
    Vector<std::pair<ValueID, LValue>, 2> m_valuesFoundByHas;
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
