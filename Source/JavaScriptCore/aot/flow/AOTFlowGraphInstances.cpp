/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowGraph.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static std::tuple<FunctionID, FunctionID, ValueID, FlowInstanceID, uint8_t> callSiteInstanceKey(FunctionID function, const FlowCallSiteKey& key)
{
    return { function, key.caller, key.call, key.outer, static_cast<uint8_t>(key.context) };
}

FlowInstanceID FlowGraph::findInstance(FunctionID function, FlowContext context) const
{
    return m_instanceTable.byFunction[function.index()][static_cast<size_t>(context)];
}

FlowInstanceID FlowGraph::appendInstance(FunctionID function, FlowContext context, std::optional<FlowCallSiteKey> callSite)
{
    const FlowSummary& summary = summaryOf(function);
    RELEASE_ASSERT(summary.hasBody());

    FlowInstanceID instance(m_instanceTable.instances.size());
    FlowNodeID firstNode = appendNodes(FlowNodeKind::Local, summary.numberOfLocals, instance.index());
    m_instanceTable.instances.append(InstanceData { FlowInstance { function, context, FlowInstanceState::Created, firstNode, callSite }, { }, { }, { } });
    return instance;
}

FlowInstanceID FlowGraph::createInstance(FunctionID function, FlowContext context)
{
    FlowInstanceID& slot = m_instanceTable.byFunction[function.index()][static_cast<size_t>(context)];
    RELEASE_ASSERT(!slot);
    slot = appendInstance(function, context, std::nullopt);
    return slot;
}

FlowInstanceID FlowGraph::findCallSiteInstance(FunctionID function, const FlowCallSiteKey& key) const
{
    return m_instanceTable.byCallSite.get(callSiteInstanceKey(function, key));
}

FlowInstanceID FlowGraph::createCallSiteInstance(FunctionID function, const FlowCallSiteKey& key)
{
    RELEASE_ASSERT(hasTrackedOperands(key.context));
    FlowInstanceID instance = appendInstance(function, key.context, key);
    bool isNew = m_instanceTable.byCallSite.add(callSiteInstanceKey(function, key), instance).isNewEntry;
    RELEASE_ASSERT(isNew);
    m_instanceTable.callSiteInstancesByFunction[function.index()].append(instance);
    ++m_instanceTable.numberOfCallSiteInstances;
    return instance;
}

std::span<const FlowInstanceID> FlowGraph::callSiteInstancesOf(FunctionID function) const
{
    return m_instanceTable.callSiteInstancesByFunction[function.index()].span();
}

void FlowGraph::setObjectsOf(FlowInstanceID instance, Vector<FlowObjectID>&& objectsBySite)
{
    const FlowSummary& summary = summaryOf(instance);
    InstanceData& data = m_instanceTable.instances[instance.index()];
    RELEASE_ASSERT(objectsBySite.size() == summary.sites.size());
    RELEASE_ASSERT(data.objects.isEmpty() && data.externals.isEmpty());

    data.objects = WTF::move(objectsBySite);
    data.externals = summary.externals.map([&](const FlowExternalNode& external) {
        return resolve(external, data.objects.span());
    });
}

FlowNodeID FlowGraph::resolve(const FlowExternalNode& external, std::span<const FlowObjectID> objectsBySite)
{
    FlowObjectID object = external.isPartOfSite() ? objectsBySite[external.site()] : FlowObjectID();
    if (object == unknownFlowObject)
        return undeclaredPart(unknownFlowObject);

    switch (external.kind()) {
    case FlowExternalKind::Variable:
        return variable(external.variable());
    case FlowExternalKind::Heritage:
        return heritage(external.layout());
    case FlowExternalKind::NativeCode:
        return nativeCode();
    case FlowExternalKind::UnknownJavaScript:
        return unknownJavaScript();
    case FlowExternalKind::SiteField:
    case FlowExternalKind::SiteCapture:
        return declaredPart(object, external.index());
    case FlowExternalKind::SiteSlot: {
        FlowNodeID node = slot(object, external.slot());
        RELEASE_ASSERT(node);
        return node;
    }
    case FlowExternalKind::SitePosition:
        return position(object, external.index());
    case FlowExternalKind::SiteUndeclaredPart: {
        FlowNodeID node = undeclaredPart(object);
        RELEASE_ASSERT(node);
        return node;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowObjectID FlowGraph::objectOf(FlowInstanceID instance, uint32_t siteInSummary) const
{
    return m_instanceTable.instances[instance.index()].objects[siteInSummary];
}

uint16_t& FlowGraph::rangeGrowth(FlowInstanceID instance, uint32_t naturalOperationIndex)
{
    InstanceData& data = m_instanceTable.instances[instance.index()];
    if (data.rangeGrowth.isEmpty())
        data.rangeGrowth.fill(0, summaryOf(instance).statements.naturalOperations.size());
    return data.rangeGrowth[naturalOperationIndex];
}

FlowNodeID FlowGraph::nodeOf(FlowInstanceID instance, FlowLocalID local) const
{
    if (!instance || !local)
        return { };
    const InstanceData& data = m_instanceTable.instances[instance.index()];
    const FlowSummary& summary = summaryOf(data.instance.function);
    if (!summary.isExternal(local))
        return FlowNodeID(data.instance.firstNode.index() + local.index());
    RELEASE_ASSERT(!data.externals.isEmpty() || summary.externals.isEmpty());
    return data.externals[local.index() - summary.numberOfLocals];
}

FlowNodeID FlowGraph::nodeOf(FlowInstanceID instance, ValueID value) const
{
    if (!instance)
        return { };
    const FlowSummary& summary = summaryOf(instance);
    if (value.index() >= summary.localOfValue.size())
        return { };
    return nodeOf(instance, summary.localOfValue[value.index()]);
}

FlowLocalID FlowGraph::localOf(FlowNodeID node) const
{
    RELEASE_ASSERT(kindOf(node) == FlowNodeKind::Local);
    return FlowLocalID(node.index() - at(FlowInstanceID(at(node).owner)).firstNode.index());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
