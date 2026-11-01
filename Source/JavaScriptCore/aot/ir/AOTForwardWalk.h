/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRFunction.h"

namespace JSC { namespace AOT { namespace Typed {

// Reads a body once and builds it anew while it reads. In the order of the walk a block stands behind the blocks that dominate it.
class ForwardWalk {
    WTF_MAKE_NONCOPYABLE(ForwardWalk);
public:
    explicit ForwardWalk(Function&);

    Renumbering run(std::span<const BlockID> order);

private:
    // Only a body that nobody has verified yet holds one.
    struct UseInFrontOfDefinition {
        bool isOutsideValues;
        uint32_t indexOfHolder;
        uint32_t indexOfChild;
    };

    void walkBlock(BlockID);
    ValueID emit(ValueID input);
    void mapToOutput(ValueID&, UseInFrontOfDefinition);
    void mapUsesInFrontOfDefinitions();

    Function& m_function;
    Function::Blocks::Storage m_inputBlocks;
    Function::Values::Storage m_inputValues;
    Vector<BlockID> m_outputBlockOf;
    Vector<ValueID> m_outputOf;
    Vector<UseInFrontOfDefinition> m_usesInFrontOfDefinitions;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
