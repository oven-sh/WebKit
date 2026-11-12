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
    InlineWidths inlineWidthsOf(Collection, ValueID collection);
    Integer movesOfEntriesInCollections();

    bool takesTableReadBeforeStep(const ValueContext& step, ValueID collection);
    bool mayReadValue(const ValueContext& step);
    ValueID findTableReadAfterStep(const ValueContext& step, ValueID collection);
    ValueID m_forwardedTableRead;
    LValue m_forwardedTable { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
