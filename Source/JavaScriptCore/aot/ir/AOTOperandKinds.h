/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTOpcodeSets.h"
#include "AOTResultRules.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// Every opcode declares the kinds of its operands, and an operation that takes one kind of value has no form for any other.
// Walker::build() asks findIllTypedOperand() of every operation it appends, in every build. An answer stops the compile with an internal error that names the function and the operation.
// Only proven types are looked at. An operand that has its kind by an expectation alone is ill-typed.
// build() refuses each of opcodesThatStateTypes with InternalErrorKind::OpcodeNotInForm.

// The type that holds every value of the kind, for the kinds that one subset test decides:
//   Boolean, Integer, Number    the primitives
//   String                      strings and literal strings. A FlatString passes as well: hasKind()
//   PropertyKey                 strings and symbols, literal or not
//   Object                      TypeTable::object
//   Callable                    TypeTable::function
//   Array                       the class Array, with nothing written of its contents
// Null for every other kind.
TypeID widestTypeOf(TypeArena&, OperandKind);

// Unspecified is left out of `proven` first: a word that may be anything is of the kind that it has once it is filled. That it is only handed on is handsOperandOn().
//   Any                         a value of JavaScript
//   Layout                      objects alone. Where `layoutOfOperation` is an environment: that environment
//   Environment                 some environment
//   EvalOperand                 a value of JavaScript, an environment or a ClassToken
//   Convertible                 a value of JavaScript, an environment or an opaque value that is a cell
//   Declared, Results           yes: findIllTypedOperand() checks them by the operation
//   a kind that names an opaque kind    that opaque value alone
bool hasKind(TypeArena&, TypeID proven, OperandKind, LayoutID layoutOfOperation = { });

struct IllTypedOperand {
    // OperandTypeMismatch, StoredTypeMismatch (the kind Declared), InvalidEnvironmentUse, UnspecifiedValueInspected or ResultIndexOutOfRange.
    InternalErrorKind error { InternalErrorKind::OperandTypeMismatch };
    uint32_t operandIndex { 0 };
    TypeID proven;
    // Null where no type says what was asked for.
    TypeID required;
};

// `proven` are the operands' types at the level Proven, and so is the context. The first operand that does not fit, by operand index.
// An operation that testsItsOwnGuess() takes every operand as Any.
// A never among the operands fits everything: the operation is not reached.
// A present operand whose type is null gives no value: OperandTypeMismatch. Under the kind Results it passes, except at ResultAt, where it is ResultIndexOutOfRange: the caller found no result at that index.
// Among the terminators only Return takes an operand whose type is null.
// Only types are seen, never the producer. Whether the index of a ResultAt is in range, and whether a producer of several results is used as one value, is for whoever holds the producer and can ask Plan.
template<typename Operation> std::optional<IllTypedOperand> findIllTypedOperand(const TypingContext&, const Operation&, OperandTypes proven);
template<typename Terminator> std::optional<IllTypedOperand> findIllTypedOperandOfTerminator(const TypingContext&, const Terminator&, OperandTypes proven, FunctionID returningFunction);

// The operation neither looks at the operand nor puts it where somebody may: it passes the word on to a position that is declared to take a word that may be anything.
template<typename Operation> bool handsOperandOn(const TypingContext&, const Operation&, uint32_t operandIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
