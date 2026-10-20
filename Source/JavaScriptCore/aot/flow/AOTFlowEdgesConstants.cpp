/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowEdges.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowKeys.h"
#include "AOTFlowModules.h"
#include "AOTFlowSiteData.h"
#include "AOTValueIdentity.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool standsForObject(const BuiltinTable& builtins, BuiltinID builtin)
{
    if (isObjectKind(builtins.at(builtin).kind))
        return true;
    return std::ranges::any_of(builtins.overloadsOf(builtin), [&](const BuiltinOverload& overload) {
        return overload.mode == BuiltinCallMode::Get && builtins.at(overload.result.type).mayBeObject();
    });
}

static bool hasHole(const Vector<DataNode>& elements)
{
    return elements.containsIf([](const DataNode& element) {
        return element.is<DataNode::Hole>();
    });
}

static bool isStoredOperand(const Value& user, const OperandInfo& operand)
{
    if (!operand.uses.contains(OperandUse::Retains) || operand.uses.containsAny({ OperandUse::Returns, OperandUse::Calls }))
        return false;
    switch (operand.retainedIn) {
    case RetainedIn::UnknownMemory:
        return false;
    case RetainedIn::Result:
        return resultRuleOf(user.opcode) == ResultRule::Allocation;
    case RetainedIn::Operand:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowClosureCapture captureOfStoredClosure(const Function& function, const Value& closure, const Value& user, const OperandInfo& operand)
{
    if (closure.numberOfOperands() != 1 || operand.retainedIn != RetainedIn::Operand)
        return FlowClosureCapture::Other;
    ValueID captured = sameObjectThroughCasts(function, closure.operand(0));
    ValueID retainer = sameObjectThroughCasts(function, user.operand(*operand.retainingOperand));
    return captured == retainer ? FlowClosureCapture::StoredIntoObject : FlowClosureCapture::Other;
}

void FlowSummaryBuilder::addConstant(ValueID id, const Value& value)
{
    FlowLocalID local = localOf(id);
    switch (value.opcode) {
    case Opcode::Undefined:
        seedPrimitives(local, Primitive::Undefined);
        return;
    case Opcode::Null:
        seedPrimitives(local, Primitive::Null);
        return;
    case Opcode::Absent:
        seedPrimitives(local, Primitive::Absent);
        return;
    case Opcode::Boolean:
        seedPrimitives(local, value.boolean() ? Primitive::True : Primitive::False);
        return;
    case Opcode::Integer:
        if (Range::full().contains(value.integer()))
            seedInteger(local, value.integer());
        else
            seedTypeAsRead(id);
        return;
    case Opcode::Double:
        if (isTrackedInteger(value.number()))
            seedInteger(local, static_cast<int64_t>(value.number()));
        else
            seedPrimitives(local, static_cast<uint16_t>(primitiveOfNumber(value.number())));
        return;
    case Opcode::String:
        m_assembler.seed(FlowSeed::string(local, value.string()));
        return;
    case Opcode::BigInt:
        seedPrimitives(local, Primitive::BigInt);
        return;
    case Opcode::KeyConstant:
        if (classOf(m_program, value.key()) == FlowKeyClass::Symbol)
            m_assembler.seed(FlowSeed::symbol(local, value.key()));
        else
            seedTypeAsRead(id);
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void FlowSummaryBuilder::addBuiltin(ValueID id, const Value& value)
{
    const BuiltinTable& builtins = m_program.builtins();
    BuiltinID builtin = builtins.canonicalID(value.builtin());
    if (RuntimeModuleID runtimeModule = findCompiledModuleWithObject(m_program, builtin)) {
        // The operation names the object by the built-in and so does its type, so the value holds both. A lookup goes to what the body makes and passes the built-in by.
        m_assembler.add(FlowModuleOperation { .kind = FlowModuleOperationKind::RequireBuiltinModule, .origin = id, .runtimeModule = runtimeModule, .result = localOf(id) });
    }
    if (isObjectKind(builtins.at(builtin).kind))
        m_assembler.seed(FlowSeed::builtin(localOf(id), builtin));
    else
        seedTypeAsRead(id);
}

void FlowSummaryBuilder::addModuleBuiltin(ValueID id, const Value& value)
{
    const BuiltinTable& builtins = m_program.builtins();
    BuiltinID builtin = builtins.canonicalID(value.builtin());
    if (standsForObject(builtins, builtin))
        m_assembler.seed(FlowSeed::moduleBuiltin(localOf(id), builtin, value.module()));
    else
        seedTypeAsRead(id);
}

void FlowSummaryBuilder::addData(ValueID id, const Value& value)
{
    // Own keys other than array indexes are listed in creation order, so the allocation fills only the fields in front of the first such key that is no field.
    auto addMemberTarget = [&](const FlowNewSite& object, const Layout& layout, PropertyKeyID key, bool& isFilledAtAllocation) {
        std::optional<uint32_t> field = layout.findField(key);
        if (field && isFilledAtAllocation)
            return m_assembler.external(FlowExternalNode::siteField(object.site, *field));
        if (classOf(m_program, key) != FlowKeyClass::Index)
            isFilledAtAllocation = false;

        FlowLocalID memberValue = m_assembler.addTemporary();
        OptionSet<FlowAccessFlag> flags;
        flags.set(FlowAccessFlag::IsInCycle, isInCycle());
        m_assembler.add(FlowPropertyAccess {
            .kind = FlowAccessKind::Define,
            .flags = flags,
            .origin = id,
            .receiver = object.local,
            .key = key,
            .value = memberValue,
            .thrown = thrownTarget(),
        });
        return memberValue;
    };

    Vector<FlowLocalID, 32> targets { localOf(id) };
    forEachDataNodeInPreorder(m_program.at(value.data()).root, [&](uint32_t ordinal, const DataNode& node) {
        FlowNewSite created = addDataNode(id, node, ordinal);
        m_assembler.copy(created.local, targets.takeLast());

        size_t firstChildTarget = targets.size();
        switch (node.kind()) {
        case DataNode::Kind::Array: {
            size_t numberOfElements = node.as<DataNode::Array>().elements.size();
            size_t numberOfPositions = m_assembler.siteAt(created.site).numberOfPositions;
            FlowLocalID rest;
            if (numberOfElements > numberOfPositions)
                rest = m_assembler.external(FlowExternalNode::siteSlot(created.site, BuiltinSlots::ArrayElement));
            targets.appendUsingFunctor(numberOfElements, [&](size_t index) {
                return index < numberOfPositions ? m_assembler.external(FlowExternalNode::sitePosition(created.site, static_cast<uint32_t>(index))) : rest;
            });
            break;
        }
        case DataNode::Kind::Object: {
            const DataNode::Object& object = node.as<DataNode::Object>();
            const Layout& layout = m_program.at(object.layout);
            bool isFilledAtAllocation = true;
            for (const DataMember& member : object.members)
                targets.append(addMemberTarget(created, layout, member.key, isFilledAtAllocation));
            break;
        }
        case DataNode::Kind::Undefined:
        case DataNode::Kind::Null:
        case DataNode::Kind::Hole:
        case DataNode::Kind::Boolean:
        case DataNode::Kind::Number:
        case DataNode::Kind::String:
        case DataNode::Kind::BigInt:
            break;
        }
        std::ranges::reverse(targets.mutableSpan().subspan(firstChildTarget));
    });
    RELEASE_ASSERT(targets.isEmpty());
}

FlowNewSite FlowSummaryBuilder::addDataNode(ValueID origin, const DataNode& node, uint32_t ordinal)
{
    FlowNewSite created;
    created.local = m_assembler.addTemporary();
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Hole:
        seedPrimitives(created.local, Primitive::Undefined);
        return created;
    case DataNode::Kind::Null:
        seedPrimitives(created.local, Primitive::Null);
        return created;
    case DataNode::Kind::Boolean:
        seedPrimitives(created.local, node.as<DataNode::Boolean>().value ? Primitive::True : Primitive::False);
        return created;
    case DataNode::Kind::Number: {
        double number = node.as<DataNode::Number>().value;
        if (isTrackedInteger(number))
            seedInteger(created.local, static_cast<int64_t>(number));
        else
            seedPrimitives(created.local, static_cast<uint16_t>(primitiveOfNumber(number)));
        return created;
    }
    case DataNode::Kind::String:
        m_assembler.seed(FlowSeed::string(created.local, node.as<DataNode::String>().value));
        return created;
    case DataNode::Kind::BigInt:
        seedPrimitives(created.local, Primitive::BigInt);
        return created;
    case DataNode::Kind::Array: {
        const Vector<DataNode>& elements = node.as<DataNode::Array>().elements;
        uint16_t numberOfPositions = 0;
        if (!hasHole(elements))
            numberOfPositions = numberOfPositionsFor(elements.size());
        created.site = m_assembler.addSite({
            .kind = FlowSiteKind::Instance,
            .origin = FlowSiteOrigin::DataNode,
            .numberOfPositions = numberOfPositions,
            .isCreatedDense = !hasHole(elements),
            .value = origin,
            .builtinClass = BuiltinClasses::Array,
            .ordinal = ordinal,
        });
        break;
    }
    case DataNode::Kind::Object:
        created.site = m_assembler.addSite({
            .kind = FlowSiteKind::Layout,
            .origin = FlowSiteOrigin::DataNode,
            .value = origin,
            .layout = node.as<DataNode::Object>().layout,
            .ordinal = ordinal,
        });
        break;
    }
    m_assembler.seed(FlowSeed::site(created.local, created.site));
    m_assembler.add(FlowAllocation { origin, created.site, FlowLocalID(), isInCycle() });
    return created;
}

void FlowSummaryBuilder::addNewClosure(ValueID id, const Value& value)
{
    FlowNewSite created = addSiteFor(id, {
        .kind = FlowSiteKind::Closure,
        .origin = FlowSiteOrigin::Operation,
        .closureFunction = value.function(),
    });
    std::span<const ValueID> captures = value.variadicOperands();
    for (uint32_t index = 0; index < captures.size(); ++index)
        storeIntoSite(id, FlowExternalNode::siteCapture(created.site, index), localOf(captures[index]));
    m_assembler.addClosureCreation({ created.local, id, value.function(), created.site, closureUseOf(id), closureCaptureOf(id, value) });
}

FlowClosureUse FlowSummaryBuilder::closureUseOf(ValueID id) const
{
    std::optional<Use> use = m_uses.singleUse(id);
    if (!use || use->kind != Use::Kind::Operand)
        return FlowClosureUse::Other;
    bool runsOncePerCreation = use->block == m_uses.blockOf(id) || !m_cycles.isInCycle(use->block);
    if (!runsOncePerCreation)
        return FlowClosureUse::Other;
    const Value& user = m_function.at(use->user);
    return isStoredOperand(user, operandInfoOf(user.opcode, use->operandIndex)) ? FlowClosureUse::StoredOnce : FlowClosureUse::Other;
}

FlowClosureCapture FlowSummaryBuilder::closureCaptureOf(ValueID id, const Value& value) const
{
    if (!value.numberOfOperands())
        return FlowClosureCapture::Nothing;
    if (closureUseOf(id) != FlowClosureUse::StoredOnce)
        return FlowClosureCapture::Other;
    Use use = *m_uses.singleUse(id);
    const Value& user = m_function.at(use.user);
    return captureOfStoredClosure(m_function, value, user, operandInfoOf(user.opcode, use.operandIndex));
}

void FlowSummaryBuilder::addParameter(ValueID, const Value&)
{
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSummaryBuilder::addModuleVariableAccess(ValueID id, const Value& value)
{
    FlowLocalID location = m_assembler.external(FlowExternalNode::variable(value.variable()));
    switch (value.opcode) {
    case Opcode::GetModuleVariable: {
        FlowLocalID result = localOf(id);
        m_assembler.copy(location, result);
        m_assembler.add(FlowLocationAccess { FlowAccessKind::Load, readUseOf(id), isInCycle(), id, location, result });
        return;
    }
    case Opcode::PutModuleVariable: {
        FlowLocalID stored = localOf(value.valueOperand());
        m_assembler.copy(stored, location);
        m_assembler.add(FlowLocationAccess { FlowAccessKind::Store, FlowReadUse::Value, isInCycle(), id, location, stored });
        return;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void FlowSummaryBuilder::addEnvironmentFieldAccess(ValueID id, const Value& value)
{
    FlowFieldAccess access {
        .isInCycle = isInCycle(),
        .origin = id,
        .receiver = localOf(value.environmentOperand()),
        .layout = value.layout(),
        .field = value.field(),
    };
    switch (value.opcode) {
    case Opcode::GetEnvironmentField:
        access.kind = FlowAccessKind::Load;
        access.use = readUseOf(id);
        access.value = localOf(id);
        break;
    case Opcode::PutEnvironmentField:
        access.kind = FlowAccessKind::Store;
        access.value = localOf(value.valueOperand());
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    m_assembler.add(access);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
