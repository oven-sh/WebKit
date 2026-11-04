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

// In which form a value of the type is kept when it is built. A pure function of the proven type. An expected type never chooses a form:
//   never, or one of undefined, null, absent alone    Void
//   booleans                                           Boolean
//   integers                                           Int32 inside Range::int32(), else Int64
//   numbers                                            Double
//   numbers beside undefined, null, absent             OptionalInt64 without doubles, else OptionalDouble
//   literal strings, the same markers beside them      LiteralString
//   strings, symbols, BigInts and objects alone        Cell
//   an environment, an opaque value                    by its kind; JSValue for a cell beside a marker
//   everything else                                    JSValue
// Unspecified takes the form of the rest of the type. Cell32 is never the answer, and RawWord only for an opaque value whose kind is a raw word: otherwise a slot or an operation asks for them.
Representation representationOf(const Type&);
// Without a table for a type of primitives alone.
Representation representationOf(TypeReader, TypeID);
Representation representationOf(OpaqueKind);
Representation representationOf(SlotRepresentation);
Representation loadedRepresentationOf(SlotRepresentation, const Type& loaded);
bool isLegalRepresentation(const Type&, Representation);
// Without a table for a type of primitives alone.
bool isLegalRepresentation(TypeReader, TypeID, Representation);
// A join's parameter: the form its united type allows, unless every argument already has one other form that is legal for that type.
Representation representationOfJoin(const Type& united, std::span<const Representation> arguments);
SlotRepresentation defaultSlotRepresentationOf(const Type&);
// Without seeds. One of TypeTable's constants or a type of primitives alone, so it needs no arena. Never for Unchosen, Void and RawWord.
TypeID widestTypeOf(Representation);
// The representation in which an operand of the kind is handed to its operation: wantsOf(kind). Integer: Int32 where the operand's type fits, else Int64. A kind that names an opaque kind: representationOf() of that opaque kind.
// Declared and Convertible, which the schema leaves open: the operand's own representation. Where Plan gave the position one, representationStoredInSlot(), representationOfArgument() and the like in walk/AOTRepresentationRules.h say it.
// An operand whose type is opaque keeps the representation of its kind, unless it is a cell under Declared, Convertible or EvalOperand.
Representation representationWantedBy(OperandKind, const Type& operand);

// A Double holds one NaN. Whoever reads a double out of raw bytes makes it that one: there is no form for any other.
// At the read, or for an element of a view in front of the first reader that could tell: CodeGenerator::settleNaNOf().
constexpr bool holdsCells(Representation representation) { return holdsCellsOf(representation); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
