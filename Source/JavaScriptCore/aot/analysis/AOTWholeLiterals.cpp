/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWholeLiterals.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool isListLiteral(const Program& program, const Value& value)
{
    if (value.opcode == Opcode::Data)
        return program.at(value.data()).root.is<DataNode::Array>();
    return value.opcode == Opcode::NewArray && !value.capacityOperand();
}

BitVector collectSitesOfWholeLiterals(const Program& program, const FlowResults& flow, BuildContext& build)
{
    struct LiteralSite {
        AllocationSiteID site;
        bool isWhole;
    };
    Vector<LiteralSite> literals = collectInParallel<LiteralSite>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<LiteralSite>& found) {
        const Function& function = program.functions[index];
        Vector<ValueID, 8> literalsOfFunction;
        forEachListedValue(program, FunctionID(index), { Opcode::Data, Opcode::NewArray }, [&](BlockID, uint32_t, ValueID id, const Value& value) {
            if (isListLiteral(program, value) && value.allocationSite() && flow.objectsNeverChange(value.allocationSite()))
                literalsOfFunction.append(id);
        });
        if (literalsOfFunction.isEmpty())
            return;

        BitVector isWritten(function.values.size());
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    if (operandUsesOf(value.opcode, operandIndex).containsAny(operandWritingUses))
                        isWritten.quickSet(sameObjectThroughCasts(function, operand).index());
                });
            }
        }
        for (ValueID literal : literalsOfFunction)
            found.append({ function.at(literal).allocationSite(), !isWritten.quickGet(literal.index()) });
    });
    BitVector sites(flow.numberOfAllocationSites());
    for (const LiteralSite& literal : literals)
        sites.quickSet(literal.site.index());
    for (const LiteralSite& literal : literals) {
        if (!literal.isWhole)
            sites.quickClear(literal.site.index());
    }
    return sites;
}

std::optional<Vector<KnownValue, 8>> findConstantsOfWholeLiteral(const Program& program, const FlowResults& flow, const Function& function, ValueID value)
{
    const Value& literal = function.at(sameObjectThroughCasts(function, value));
    if (!isListLiteral(program, literal) || !flow.isSiteOfWholeLiterals(literal.allocationSite()))
        return std::nullopt;

    Vector<KnownValue, 8> constants;
    if (literal.opcode == Opcode::Data) {
        for (const DataNode& element : program.at(literal.data()).root.as<DataNode::Array>().elements)
            constants.append(knownValueOf(element));
    } else {
        if (!literal.holes().empty())
            return std::nullopt;
        for (ValueID element : literal.elementsOperands())
            constants.append(knownValueOf(program, function, element));
    }
    bool holdsPrimitives = std::ranges::all_of(constants, [](KnownValue constant) {
        return constant.isKnown() && !constant.isObject();
    });
    if (!holdsPrimitives)
        return std::nullopt;
    return constants;
}

std::optional<Vector<KnownValue, 8>> findConstantsOfWholeList(const Program& program, const FlowResults& flow, const Function& function, ValueID value)
{
    const Value& definition = function.at(sameObjectThroughCasts(function, value));
    if (definition.opcode != Opcode::GetModuleVariable)
        return findConstantsOfWholeLiteral(program, flow, function, value);
    const Vector<KnownValue>* constants = flow.constantListIn(definition.variable());
    if (!constants)
        return std::nullopt;
    return Vector<KnownValue, 8>(constants->span());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
