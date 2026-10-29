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
    void generateStepMapIterator(const ValueContext& value) { generateIteratorStep(Collection::Map, value); }
    void generateStepSetIterator(const ValueContext& value) { generateIteratorStep(Collection::Set, value); }
    void generateIsCollectionTableFull(const ValueContext&);
    void generateReserveCollectionCapacity(const ValueContext&);

private:
    enum class Collection : uint8_t { Map, Set };

    class TableReader;

    std::optional<bool> isTakenForNumberAfter(const ValueContext& step, uint32_t result);
    void generateNext(Collection, const ValueContext&);
    void generateIteratorStep(Collection, const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
