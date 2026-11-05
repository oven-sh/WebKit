/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;
struct InternalError;

namespace Typed {

class BodyIndexTable;
class Program;
struct Function;

// The rules of the wire IR, checked on the words themselves, in every build: DESIGN.md section 4, rule 12.
//
// The stages run in this order, and a stage runs only where the one in front of it found nothing:
//   words       every count stays inside the body, every enumerator, flag and boolean is one the wire may hold, every id is in its unit's table and names a record,
//               every operand is a value of the body, required operands are there, the blocks end at the last word and the values are as many as the record says.
//               It reads no word it has not checked, so it is safe on any bytes. Nothing else in the engine may look at a body that has not passed it or come from our own front end.
//   flow        over a BodyIndex of its own: block order, reachability, definitions in front of and dominating uses, arguments against parameters, proofs, handlers, roles, loop marks,
//               entry parameters. It also computes places, use counts, dominators and loops a second, plain way and compares them with the index: that checks the engine and not the wire,
//               so it is done wherever a test verifies a program, in a checking build and under CheckSet::Validate, and not in each build of a product.
//               What BodyIndexTable::build() itself leans on is checked before the index is built: the comment above findErrorsInWords() lists it.
//   operations what single operations require of their fields, their neighbors and the function's record.

// The words, without the program's tables: ids are checked against the unit's tables only. And of the flow what BodyIndexTable::build() leans on:
// every block but the entry is the target of an earlier block, and the operands of a terminator stand in front of it. Whoever finds nothing here may build an index.
Vector<InternalError> findErrorsInWords(FunctionID, const Body&);

// Every function, in parallel. The errors are reported in the order of the functions. Where there is none: verifyModulesNamedInBodies() of AOTProgramVerifier.h.
bool verifyBodies(const Program&, BuildContext&);
// The index that the flow stage goes over is built in the table and stays there, so that Facts does not build it a second time. The table has a place for each function and as many workers as the loops are wide.
// Null: each index dies with the look at its function, which is how the tests ask: each index is compared with the plain computation.
bool verifyBodies(const Program&, BuildContext&, BodyIndexTable*);

// The product cannot print a body. Whoever can, says so here, and a report shows the function it is about.
extern void (*printerOfBodiesForVerifier)(const Program&, FunctionID, ASCIILiteral heading);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
