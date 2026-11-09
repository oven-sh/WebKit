/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

template<typename... Results> class ResultsJoin;

class CollectionIntrinsicEmitter final : public OperationEmitter {
public:
    explicit CollectionIntrinsicEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateCollectionTable(const ValueContext&);
    void generateCollectionTableFacts(const ValueContext&);
    void generateMapNext(const ValueContext& value) { generateNext(Collection::Map, value); }
    void generateSetNext(const ValueContext& value) { generateNext(Collection::Set, value); }
    void generateCollectionIteratorClosedMark(const ValueContext&);
    void generateGetCollectionIteratorTable(const ValueContext&);
    void generatePutCollectionIteratorTable(const ValueContext&);
    void generateIsCollectionIteratorClosedMark(const ValueContext&);
    void generateGetValueAtPlaceInMap(const ValueContext&);
    void generateFindKeyInMap(const ValueContext&);
    void generateMovesOfEntriesInCollections(const ValueContext&);
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
    using StepJoin = ResultsJoin<Condition, Word32, Boxed, Boxed, Boxed>;
    void generateNextOnFacts(Collection, const ValueContext&, TableReader&, StepJoin&, LBasicBlock hasNoEntry, LBasicBlock slowPath);
    void stepByOperation(Collection, const ValueContext&, TableReader&, StepJoin&, CellPointer owner, Boxed table, Integer start);
    InlineWidths inlineWidthsOf(Collection, ValueID collection);
    Integer movesOfEntriesInCollections();
    InlineWidths inlineWidthsOf(Collection, const ValueContext&, ValueID collection);

    ValueID m_forwardedTableRead;
    LValue m_forwardedTable { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
