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

std::optional<Vector<ConstantMember, 8>> findMembersOfObjectLiteral(const Program& program, const Function& function, const Value& literal)
{
    if (literal.opcode == Opcode::Data) {
        const DataNode::Object* object = program.at(literal.data()).root.tryAs<DataNode::Object>();
        if (!object)
            return std::nullopt;
        Vector<ConstantMember, 8> members;
        for (const DataMember& member : object->members) {
            KnownValue value = knownValueOf(member.value);
            if (program.at(member.key).kind != PropertyKeyKind::Text || program.nameClassOf(member.key) == NameClass::Proto || !value.isKnown() || value.isObject())
                return std::nullopt;
            members.append({ member.key, value });
        }
        return members;
    }
    if (literal.opcode != Opcode::NewObject || literal.prototypeOperand())
        return std::nullopt;
    auto fields = literal.fields();
    auto values = literal.valuesOperands();
    if (fields.size() != values.size())
        return std::nullopt;
    const Layout& layout = program.at(literal.layout());
    Vector<ConstantMember, 8> members;
    for (size_t index = 0; index < fields.size(); ++index) {
        PropertyKeyID key = layout.fields[fields[index]].key;
        KnownValue value = knownValueOf(program, function, values[index]);
        if (!key || program.at(key).kind != PropertyKeyKind::Text || !value.isKnown() || value.isObject())
            return std::nullopt;
        members.append({ key, value });
    }
    return members;
}

std::optional<Vector<ConstantMember, 8>> findMembersOfWholeObject(const Program& program, const FlowResults& flow, const Function& function, ValueID value)
{
    const Value& literal = function.at(sameObjectThroughCasts(function, value));
    if ((literal.opcode != Opcode::NewObject && literal.opcode != Opcode::Data) || !literal.allocationSite() || !flow.isSiteOfWholeLiterals(literal.allocationSite()))
        return std::nullopt;
    return findMembersOfObjectLiteral(program, function, literal);
}

// Flow follows an object with a layout by its layout: every literal of the same names counts.
static bool objectsOfLayoutNeverChange(const FlowResults& flow, const Value& literal)
{
    static constexpr OptionSet<ObjectMutation> mutationsOfContents { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::AddProperty, ObjectMutation::ChangePrototype, ObjectMutation::StoreAfterCreation };
    return flow.at(literal.allocationSite()).allocationClass && flow.classOf(literal.allocationSite()).impossibleMutations.containsAll(mutationsOfContents);
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
        forEachListedValue(program, FunctionID(index), { Opcode::Data, Opcode::NewArray, Opcode::NewObject }, [&](BlockID, uint32_t, ValueID id, const Value& value) {
            if (!value.allocationSite())
                return;
            bool neverChanges = flow.objectsNeverChange(value.allocationSite());
            if (isListLiteral(program, value) ? neverChanges : (neverChanges || objectsOfLayoutNeverChange(flow, value)) && findMembersOfObjectLiteral(program, function, value))
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
                        isWritten.quickSet(sameObjectThroughCasts(function, operand).valueInFrontOfFacts().index());
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
