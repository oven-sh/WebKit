/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class CollectionIntrinsicEmitter final : public OperationEmitter {
public:
    explicit CollectionIntrinsicEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateCollectionTable(const ValueContext&);
    void generateMapNext(const ValueContext& value) { generateNext(Collection::Map, value); }
    void generateSetNext(const ValueContext& value) { generateNext(Collection::Set, value); }
    void generateCollectionIteratorClosedMark(const ValueContext&);
    void generateGetCollectionIteratorTable(const ValueContext&);
    void generatePutCollectionIteratorTable(const ValueContext&);
    void generateIsCollectionIteratorClosedMark(const ValueContext&);
    void generateGetValueAtPlaceInMap(const ValueContext&);
    void generateFindKeyInMap(const ValueContext&);
    void generateMovesOfEntriesInCollections(const ValueContext&);
    void generateCollectionColumns(const ValueContext&);
    void generateGetMapValueByTag(const ValueContext&);
    void generateHasKeyByTag(const ValueContext&);
    void generateGetMapValueByStringTag(const ValueContext&);
    void generateHasStringKeyByTag(const ValueContext&);
    enum class EntryAnswer : uint8_t { IsPresent, Value, Place };
    Operand findNumberAtHomeSlot(CellPointer owner, Boxed key, EntryAnswer, const ScopedLambda<Operand()>& lookUpByHash);
    bool isCounterOrSmallNumber(ValueID key) const;
    void generateTryPutValueAtPlaceInMap(const ValueContext&);

private:
    enum class Collection : uint8_t { Map, Set };

    class TableReader;

    struct InlineWidths {
        bool keysAreWide { false };
        bool readsKeysOfBothWidths { false };
        std::optional<bool> valuesAreWide;
    };

    void generateNext(Collection, const ValueContext&);
    InlineWidths inlineWidthsOf(Collection, const ValueContext& step, ValueID collection);
    Integer movesOfEntriesInCollections();
    struct CollectionColumns {
        LValue cache { nullptr };
        std::optional<CellPointer> collection;
        LValue tags { nullptr };
        LValue mask { nullptr };
        LValue keys { nullptr };
        LValue values { nullptr };
        bool keysAreCells { false };
        bool keyIsString { false };
        bool keyMayBeNullish { false };
        bool keyMayBeAtItsPlace { false };
        bool readsValues { false };
        bool keepsEntry { false };
        std::optional<EdgeID> edgeIfAbsent;
    };
    static constexpr int64_t entryOfAbsentKey = -1;
    static constexpr int64_t unknownEntry = -2;
    enum class KeyOfLookup : uint8_t { Number, Cell, String };
    bool mayBeNullish(ValueID key) const;
    CollectionColumns columnsOfLookup(const ValueContext&, ValueID collection, KeyOfLookup, bool readsValues);
    std::array<Operand, 4> readColumns(LValue table, KeyOfLookup, bool readsValues);
    void cacheColumns(LValue cache, LValue table, KeyOfLookup, bool readsValues);
    TypedPointer cachedWord(LValue cache, unsigned index);
    void getMapValue(const ValueContext&, CollectionColumns);
    void testForKey(const ValueContext&, const CollectionColumns&);
    Operand lookUpByTag(CollectionColumns, Boxed key, const ScopedLambda<Operand()>& lookUpByHash);
    Boxed valueOfEntry(const CollectionColumns&, LValue entry);
    Operand valueAtTestedEntry(const ValueContext&, const CollectionColumns&, LValue entry, const ScopedLambda<Operand()>& lookUpByHash);
    LValue entryOfTest(ValueID test);

    bool takesTableReadBeforeStep(const ValueContext& step, ValueID collection);
    bool mayReadValue(const ValueContext& step);
    uint32_t forEachResultRead(const ValueContext& step, const ScopedLambda<void(ValueID)>&);
    ValueID findTableReadAfterStep(const ValueContext& step, ValueID collection);
    Vector<std::pair<ValueID, LValue>> m_entriesOfTests;
    LValue m_entryOfTest { nullptr };
    ValueID m_forwardedTableRead;
    LValue m_forwardedTable { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
