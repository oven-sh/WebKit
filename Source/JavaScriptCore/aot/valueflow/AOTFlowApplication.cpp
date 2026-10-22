/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApplication.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

void FlowApplicationDeleter::operator()(FlowApplication* application) const
{
    delete application;
}

LayoutID layoutOfEmptyLiteral(const Program& program, const Value& value)
{
    if (value.opcode == Opcode::NewObject)
        return value.fields().empty() && !value.prototypeOperand() ? value.layout() : LayoutID();
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
    for (FlowClaimUpdate& claim : body.claims)
        remap.apply(claim.arriving);
    for (FlowAccessStamp& access : body.accesses)
        remap.apply(access.receivers);
    for (FlowCallStamp& call : body.calls)
        remap.apply(call.callees);
    for (FlowAllocationStamp& allocation : body.allocations)
        remap.apply(allocation.elements);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
