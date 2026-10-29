/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApplication.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowStamps.h"
#include "AOTThreads.h"

namespace JSC { namespace AOT { namespace Typed {

void FlowApplicationDeleter::operator()(FlowApplication* application) const
{
    delete application;
}

LayoutID layoutOfPlainLiteral(const Program& program, const Value& value)
{
    if (value.opcode == Opcode::NewObject)
        return value.fields().size() == value.variadicOperands().size() && !value.prototypeOperand() ? value.layout() : LayoutID();
    if (value.opcode != Opcode::Data)
        return { };
    const auto* object = program.at(value.data()).root.tryAs<DataNode::Object>();
    return object && object->members.isEmpty() ? object->layout : LayoutID();
}

TypeID typeBehindDictionaries(std::span<const std::pair<TypeID, TypeID>> ascendingTypes, TypeID type)
{
    auto found = std::ranges::lower_bound(ascendingTypes, type, { }, &std::pair<TypeID, TypeID>::first);
    return found != ascendingTypes.end() && found->first == type ? found->second : type;
}

void remapTypes(FlowPositionUpdate& position, const TypeRemap& remap)
{
    remap.apply(position.type);
}

void remapTypes(FlowLayoutUpdate& layout, const TypeRemap& remap)
{
    if (remap.isIdentity())
        return;
    for (FlowFieldUpdate& field : layout.fields)
        remapTypes(field.position, remap);
    for (FlowAppendedField& field : layout.appendedFields)
        remapTypes(field.position, remap);
}

void remapTypes(FlowSignatureUpdate& signature, const TypeRemap& remap)
{
    if (remap.isIdentity())
        return;
    for (FlowPositionUpdate& parameter : signature.entryParameters)
        remapTypes(parameter, remap);
    for (FlowPositionUpdate& result : signature.results)
        remapTypes(result, remap);
}

void remapTypes(FlowBodyUpdate& body, const TypeRemap& remap)
{
    if (remap.isIdentity())
        return;
    for (FlowValueProof& proof : body.proofs)
        remap.apply(proof.type);
    for (FlowValueExpectation& expectation : body.expectations)
        remap.apply(expectation.type);
    for (FlowClaimUpdate& claim : body.claims)
        remap.apply(claim.arriving);
    for (FlowAccessStamp& access : body.accesses)
        remap.apply(access.receivers);
    for (FlowCallStamp& call : body.calls)
        remap.apply(call.callees);
    for (FlowAllocationStamp& allocation : body.allocations)
        remap.apply(allocation.elements);
}

FlowAccessSiteNumbering numberAccessSites(const Program& program, BuildContext& build)
{
    unsigned count = program.functions.size();
    FlowAccessSiteNumbering numbering;
    numbering.firstSiteOfFunction = Vector<uint32_t>(FillWith { }, count + 1, 0u);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned index) {
        uint32_t numberOfSites = 0;
        forEachAccessSiteValue(program.functions[index], [&](ValueID, uint32_t) {
            ++numberOfSites;
        });
        numbering.firstSiteOfFunction[index + 1] = numberOfSites;
    });
    for (unsigned index = 0; index < count; ++index)
        numbering.firstSiteOfFunction[index + 1] += numbering.firstSiteOfFunction[index];
    return numbering;
}

Vector<AccessSiteID> accessSitesByValue(const Function& function, const FlowAccessSiteNumbering& numbering, FunctionID functionAsRead)
{
    Vector<AccessSiteID> sites(function.values.size());
    forEachAccessSiteValue(function, [&](ValueID value, uint32_t ordinal) {
        sites[value.index()] = numbering.siteOf(functionAsRead, ordinal);
    });
    return sites;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
