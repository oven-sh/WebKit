/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"
#include "AOTSchemaEnums.h"
#include "AOTType.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

using RepresentationSet = EnumeratorBitSet<Representation, numberOfEnumerators<Representation>>;

Representation defaultRepresentationOf(const Type&);
Representation representationOf(OpaqueKind);
Representation representationOf(SlotRepresentation);
Representation loadedRepresentationOf(SlotRepresentation, const Type& loaded);
bool isLegalRepresentation(const Type&, Representation);
RepresentationSet legalRepresentationsOf(const Type&);
SlotRepresentation defaultSlotRepresentationOf(const Type&);
Type widestTypeOf(Representation);

// A NaN that is kept as Double has no payload. Kept as DoubleWithPayload it has the sign that arithmetic gave it and any payload. Kept as ImpureDouble it has the bits of some bytes.
constexpr bool holdsAnyNaN(Representation representation)
{
    return representation == Representation::DoubleWithPayload || representation == Representation::ImpureDouble;
}
static_assert(Representation::Double < Representation::DoubleWithPayload && Representation::DoubleWithPayload < Representation::ImpureDouble);

constexpr bool holdsCells(Representation representation) { return holdsCellsOf(representation); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
