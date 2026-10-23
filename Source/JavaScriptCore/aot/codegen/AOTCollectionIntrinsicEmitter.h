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
    explicit CollectionIntrinsicEmitter(CodeGenerator&);

    void generateCollectionTable(const ValueContext&);
    void generateMapNext(const ValueContext&);
    void generateSetNext(const ValueContext&);

private:
    enum class Collection : uint8_t { Map, Set };

    class TableReader;

    void generateNext(Collection, const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
