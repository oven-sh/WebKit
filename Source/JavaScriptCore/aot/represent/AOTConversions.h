/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTRepresentationRules.h"
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class UseLists;

struct ConversionStep {
    Opcode opcode { Opcode::Box };
    Representation to { Representation::JSValue };

    friend bool operator==(const ConversionStep&, const ConversionStep&) = default;
};

using ConversionPath = Vector<ConversionStep, 2>;

std::optional<ConversionPath> findConversion(const Type&, Representation from, Representation to);

class ConversionInserter {
    WTF_MAKE_NONCOPYABLE(ConversionInserter);
public:
    ConversionInserter(FunctionPassContext&, IRBuilder&);

    void convertOperand(ValueID user, uint32_t operandIndex, Representation wanted, ProgramPoint pointOfUser);
    void convertTerminatorOperand(BlockID, uint32_t operandIndex, Representation wanted);
    void convertEdgeArgument(Edge, uint32_t argumentIndex, Representation wanted);
    void convertEdgeResult(Edge, uint32_t parameterIndex, Representation passed, Representation wanted);

    uint32_t numberOfConversions() const { return m_numberOfConversions; }
    void finish();

private:
    enum class SharingScope : uint8_t { Block, Function };

    struct SharedConversion {
        Representation to { Representation::Unchosen };
        ValueID converted;
        std::optional<uint32_t> previous;
    };

    struct SharedConversionLists {
        BlockID block;
        std::optional<uint32_t> lastInBlock;
        std::optional<uint32_t> lastAfterDefinition;
    };

    struct EdgeConversion {
        uint32_t parameterIndex { 0 };
        std::optional<Representation> passed;
        Representation wanted { Representation::Unchosen };
    };

    struct SplitEdge {
        Edge edge;
        Vector<EdgeConversion, 2> conversions;
        BlockID added;
    };

    ValueID convert(ValueID source, Representation wanted, ProgramPoint before, SharingScope);
    ValueID findSharedConversion(SharingScope, BlockID, ValueID source, Representation wanted) const;
    ValueID findConversionInList(std::optional<uint32_t> last, Representation wanted) const;
    void shareConversion(SharingScope, BlockID, ValueID source, Representation to, ValueID converted);

    void convertJumpArgument(BlockID, uint32_t argumentIndex, Representation wanted);
    void convertHandlerArgument(BlockID covered, uint32_t argumentIndex, Representation wanted);
    void convertOnSplitEdge(Edge, const EdgeConversion&);
    BlockID splitEdge(const SplitEdge&);

    void reportUnsupportedConversion(ValueID source, BlockID);

    FunctionPassContext& m_pass;
    IRBuilder& m_builder;
    const UseLists& m_useLists;
    Vector<SharedConversion> m_sharedConversions;
    Vector<SharedConversionLists> m_sharedConversionsOf;
    Vector<SplitEdge> m_splitEdges;
    uint32_t m_numberOfConversions { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
