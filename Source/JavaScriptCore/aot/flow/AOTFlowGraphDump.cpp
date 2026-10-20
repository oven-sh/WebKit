/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowGraph.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTFlowNames.h"
#include "AOTIRPrinter.h"
#include <algorithm>
#include <initializer_list>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!FlowInstanceID(FlowNode::noOwner));

namespace {

enum class LocalRole : uint8_t {
    EntryParameter,
    Result,
    Thrown,
    Value,
    Temporary,
};

struct LocalDescription {
    LocalRole role { LocalRole::Temporary };
    uint32_t index { 0 };
    ValueID value;
};

} // anonymous namespace

static LocalDescription describeLocal(const FlowSummary& summary, FlowLocalID local)
{
    uint32_t index = local.index();
    if (index < summary.numberOfEntryParameters)
        return { LocalRole::EntryParameter, index, { } };
    uint32_t resultIndex = index - summary.numberOfEntryParameters;
    if (resultIndex < summary.numberOfResults)
        return { LocalRole::Result, resultIndex, { } };
    if (local == summary.thrown())
        return { LocalRole::Thrown, 0, { } };
    if (ValueID value = summary.originOf(local))
        return { LocalRole::Value, 0, value };
    return { LocalRole::Temporary, index, { } };
}

static TypeID typeOfLocal(const Function& function, const FlowSummary& summary, FlowLocalID local)
{
    LocalDescription description = describeLocal(summary, local);
    switch (description.role) {
    case LocalRole::EntryParameter:
        if (const DeclaredType* declared = function.declaredEntryParameter(description.index))
            return declared->type;
        return function.at(function.at(entryBlock).parameters[description.index]).type;
    case LocalRole::Result:
        return function.isUnsplitCoroutine() ? TypeID() : function.results[description.index].type;
    case LocalRole::Value:
        return function.at(description.value).type;
    case LocalRole::Thrown:
    case LocalRole::Temporary:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID typeOfDeclaredPart(const Program& program, const FlowObject& object, uint32_t index)
{
    switch (object.kind) {
    case FlowObjectKind::Allocation:
        return fieldAt(program, { object.layout, index }).declared.type;
    case FlowObjectKind::Closure:
        return program.at(object.function).captures[index].type;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static PropertyKeyID keyOfNode(std::span<const FlowProperty> properties, FlowNodeID FlowProperty::* member, FlowNodeID node)
{
    auto found = std::ranges::find(properties, node, member);
    RELEASE_ASSERT(found != properties.end());
    return found->key;
}

static void dumpLocal(IRPrinter& printer, const FlowSummary& summary, FlowLocalID local)
{
    PrintStream& out = printer.out();
    LocalDescription description = describeLocal(summary, local);
    switch (description.role) {
    case LocalRole::EntryParameter:
        out.print("parameter "_s, description.index);
        return;
    case LocalRole::Result:
        out.print("result "_s, description.index);
        return;
    case LocalRole::Thrown:
        out.print("thrown"_s);
        return;
    case LocalRole::Value:
        printer.print(description.value);
        return;
    case LocalRole::Temporary:
        out.print("temporary "_s, description.index);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpInstance(IRPrinter& printer, const FlowGraph& graph, FlowInstanceID instance)
{
    printer.out().print("i"_s, instance.index(), ", "_s);
    if (graph.isHost(instance))
        printer.out().print("host"_s);
    else
        printer.print(graph.at(instance).function);
}

static void dumpDetails(IRPrinter& printer, const FlowGraph& graph, const FlowNodeDescription& description)
{
    PrintStream& out = printer.out();
    switch (description.kind) {
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
        return;
    case FlowNodeKind::Variable:
        printer.print(description.variable);
        return;
    case FlowNodeKind::Heritage:
        printer.print(description.layout);
        return;
    case FlowNodeKind::DeclaredPart:
    case FlowNodeKind::Position:
        graph.objects().dump(out, description.object);
        out.print(", "_s, description.index);
        return;
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
        graph.objects().dump(out, description.object);
        return;
    case FlowNodeKind::Property:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
        graph.objects().dump(out, description.object);
        out.print(", "_s);
        printer.print(description.key);
        return;
    case FlowNodeKind::Temporary:
        if (description.instance)
            dumpInstance(printer, graph, description.instance);
        return;
    case FlowNodeKind::Local:
        dumpInstance(printer, graph, description.instance);
        out.print(", "_s);
        dumpLocal(printer, graph.summaryOf(description.instance), description.local);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeID FlowGraph::typeAsRead(FlowNodeID node) const
{
    switch (kindOf(node)) {
    case FlowNodeKind::Variable:
        return m_program.at(VariableID(at(node).owner)).declared.type;
    case FlowNodeKind::DeclaredPart: {
        FlowNodeDescription description = describe(node);
        return typeOfDeclaredPart(m_program, m_objects.at(description.object), description.index);
    }
    case FlowNodeKind::Local: {
        FlowInstanceID instance(at(node).owner);
        if (isHost(instance))
            return { };
        return typeOfLocal(m_program.at(at(instance).function), summaryOf(instance), localOf(node));
    }
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::Position:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
    case FlowNodeKind::Property:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
    case FlowNodeKind::Temporary:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FunctionID FlowGraph::functionChargedFor(FlowNodeID node) const
{
    uint32_t owner = at(node).owner;
    switch (kindOf(node)) {
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
        return { };
    case FlowNodeKind::Variable:
        return m_program.at(m_program.at(VariableID(owner)).module).body;
    case FlowNodeKind::Heritage:
        return m_program.at(LayoutID(owner)).constructor;
    case FlowNodeKind::DeclaredPart:
    case FlowNodeKind::Position:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
    case FlowNodeKind::Property:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
        return m_objects.allocatingFunctionOf(FlowObjectID(owner));
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local: {
        FlowInstanceID instance(owner);
        return instance ? at(instance).function : FunctionID();
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowNodeDescription FlowGraph::describe(FlowNodeID node) const
{
    FlowNodeDescription description;
    description.kind = kindOf(node);
    uint32_t owner = at(node).owner;

    switch (description.kind) {
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
        break;
    case FlowNodeKind::Variable:
        description.variable = VariableID(owner);
        break;
    case FlowNodeKind::Heritage:
        description.layout = LayoutID(owner);
        break;
    case FlowNodeKind::DeclaredPart:
        description.object = FlowObjectID(owner);
        description.index = node.index() - m_objectNodes[owner].firstPart.index();
        break;
    case FlowNodeKind::Position:
        description.object = FlowObjectID(owner);
        description.index = node.index() - m_objectNodes[owner].firstPart.index() - m_objects.at(description.object).numberOfDeclaredParts;
        break;
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
        description.object = FlowObjectID(owner);
        break;
    case FlowNodeKind::Property:
        description.object = FlowObjectID(owner);
        description.key = keyOfNode(propertiesOf(description.object), &FlowProperty::value, node);
        break;
    case FlowNodeKind::Getter:
        description.object = FlowObjectID(owner);
        description.key = keyOfNode(propertiesOf(description.object), &FlowProperty::getter, node);
        break;
    case FlowNodeKind::Setter:
        description.object = FlowObjectID(owner);
        description.key = keyOfNode(propertiesOf(description.object), &FlowProperty::setter, node);
        break;
    case FlowNodeKind::Temporary:
        description.instance = FlowInstanceID(owner);
        break;
    case FlowNodeKind::Local:
        description.instance = FlowInstanceID(owner);
        description.local = localOf(node);
        break;
    }
    return description;
}

Witness FlowGraph::witnessOf(FlowInstanceID instance, ValueID origin) const
{
    if (!instance || isHost(instance))
        return noWitness;
    FunctionID id = at(instance).function;
    const Function& function = m_program.at(id);
    if (!origin)
        return { id.index(), Wire::none, function.declarationLocation() };
    return { id.index(), origin.index(), function.locationOf(origin) };
}

void FlowGraph::dumpNameOf(PrintStream& out, FlowNodeID node) const
{
    IRPrinter printer(out, m_program);
    out.print("n"_s, node.index(), " "_s, nameOf(kindOf(node)), "("_s);
    dumpDetails(printer, *this, describe(node));
    out.print(")"_s);
}

// One line for each value of the function, in the order of the blocks, whether or not it shares its local with others.
static void dumpLocals(PrintStream& out, const FlowGraph& graph, FlowInstanceID instance)
{
    const FlowSummary& summary = graph.summaryOf(instance);
    IRPrinter printer(out, graph.program());
    auto dumpLine = [&](FlowLocalID local, ValueID value) {
        FlowNodeID node = graph.nodeOf(instance, local);
        if (graph.valueOf(node).isEmpty())
            return;
        out.print("n"_s, node.index(), " "_s, nameOf(FlowNodeKind::Local), "("_s);
        dumpInstance(printer, graph, instance);
        out.print(", "_s);
        if (value)
            printer.print(value);
        else
            dumpLocal(printer, summary, local);
        out.print(") = "_s);
        graph.values().dump(out, graph.valueOf(node));
        out.print("\n"_s);
    };
    auto dumpLocalsWithRole = [&](std::initializer_list<LocalRole> roles) {
        for (uint32_t index = 0; index < summary.numberOfLocals; ++index) {
            if (std::ranges::contains(roles, describeLocal(summary, FlowLocalID(index)).role))
                dumpLine(FlowLocalID(index), ValueID());
        }
    };

    dumpLocalsWithRole({ LocalRole::EntryParameter, LocalRole::Result, LocalRole::Thrown });
    if (!graph.isHost(instance)) {
        const Function& function = graph.program().at(summary.function);
        auto dumpValue = [&](ValueID value) {
            if (FlowLocalID local = summary.localOfValue[value.index()])
                dumpLine(local, value);
        };
        for (BlockID id : CFG(function).reversePostOrder()) {
            const Block& block = function.at(id);
            if (id != entryBlock)
                std::ranges::for_each(block.parameters, dumpValue);
            std::ranges::for_each(block.values, dumpValue);
        }
    }
    dumpLocalsWithRole({ LocalRole::Temporary });
}

void FlowGraph::dump(PrintStream& out) const
{
    for (uint32_t index = 0; index < numberOfNodes(); ++index) {
        FlowNodeID node(index);
        if (kindOf(node) == FlowNodeKind::Local) {
            FlowInstanceID instance = instanceOf(node);
            dumpLocals(out, *this, instance);
            index += summaryOf(instance).numberOfLocals - 1;
            continue;
        }

        const FlowValue& value = valueOf(node);
        if (value.isEmpty())
            continue;
        dumpNameOf(out, node);
        out.print(" = "_s);
        m_values.dump(out, value);
        out.print("\n"_s);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
