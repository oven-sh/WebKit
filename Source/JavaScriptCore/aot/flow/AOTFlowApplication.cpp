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

void remapTypes(FlowPositionUpdate& position, const TypeRemap& remap)
{
    remap.apply(position.type);
    remap.apply(position.predicted);
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
    for (TypeID& prediction : signature.predictionsOfGenericVersion)
        remap.apply(prediction);
    for (FlowPositionUpdate& result : signature.results)
        remapTypes(result, remap);
}

void remapTypes(FlowBodyUpdate& body, const TypeRemap& remap)
{
    if (remap.isIdentity())
        return;
    for (FlowValueProof& proof : body.proofs) {
        remap.apply(proof.type);
        remap.apply(proof.predicted);
    }
    for (FlowAccessStamp& access : body.accesses)
        remap.apply(access.receivers);
    for (FlowCallStamp& call : body.calls)
        remap.apply(call.callees);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
