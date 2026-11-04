/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTResultRules.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class BodyIndex;
class BodyIndexTable;

// The effects of one operation with the operands it has. The schema declares a bound for each opcode and names the rule that may say less (EffectRule):
//   Declared                         the bound, at the places that the operation's own words name
//   Operator                         by the operands' types: arithmetic on numbers does nothing, on strings it makes strings, and an object among the operands runs its conversions
//   Access, Call, Builtin, Inline    the bound, unless the caller hands in what it found out:
//       an access      nobody hands anything in: an access that is proven to run no code is built as an operation of slots or elements, whose own row says so
//       a call         Link's effects of the one callee: LinkedFunction::effectsOnReturn and effectsOnThrow
//       a built-in     the effects of its row on the operands at hand, worked out in walk/AOTWalkBuiltins.cpp and handed to Walker::build()
// Under every rule an operand that the row calls or runs a protocol on adds the code that this may run, given its type: effectsOfProtocolsOn(). The declared bound does not hold that part.
// Every answer lies inside the declared bound united with that part: what is handed in is cut by the two together. Less is said only for a reason that holds in every run.
// WalkState is ended by these, the sweep drops by these, and checking builds hold every load and store of the emitted code to them: AOTEffectsCheck.h.

// `foundByCaller` null: nobody knows more than the schema.
// It may be handed in under every rule. Under Declared and Operator it stands in place of what the protocols of the operands add:
// a ResolvePromise or a ReturnAsyncResult whose value is proven to be no thenable is handed its declaredEffectsOfOperation().
// What no caller sees stays: the row's own writes of MemberGuards, and one more where such an operand may be a function, which gets its prototype property when first asked for it.
// An operation that suspends ignores it: any code may run before it goes on.
// Of an operation that testsItsOwnGuess(): the bound of genericOperationOf() its opcode, which the failing side does, united with its own.
// Of a proven PutSlot or PutSlotOrAdd: a write of NamedProperties that is handed in is added to the bound, and is the one answer that lies outside it. The store may set the mark of the cell.
template<typename Operation> Effects effectsOf(const TypingContext&, const Operation&, OperandTypes, TypeID result, const Effects* foundByCaller = nullptr);
template<typename Terminator> Effects effectsOfTerminator(const TypingContext&, const Terminator&, OperandTypes, const Effects* foundByCaller = nullptr);

Effects effectsOfOperator(const TypingContext&, Opcode, std::span<const TypeID> operandTypes, TypeID result);
// What the protocols of one operand may run, given its type: nothing for a primitive that the protocol does not convert, anything for an object.
// Whoever has resolved the members that a protocol calls on the objects of the type says less by itself.
Effects effectsOfProtocolsOn(const TypingContext&, OptionSet<Protocol>, TypeID operand);
// By the row alone, whatever the operands are: the operation calls an operand or runs a protocol on one. The declared bound does not hold that part.
bool operandsMayRunCode(Opcode);
// Until the operation is over nothing runs but the operation itself, whatever its operands are: it always completes, it throws nothing, and no operand of it runs code.
bool letsNothingElseRun(Opcode);
// `class Pool { static made = 0; static limit = 8; static all = load(); static last = 0 }`: two. `definer` holds the NewClass.
// The first so many of Layout::staticFields are plain properties of the class object before anything but the definition can look at it:
// from the NewClass to the definition of each stand only operations that let nothing else run, so none of them throws on the way either.
uint32_t numberOfStaticFieldsDefinedBeforeOtherCodeRuns(const Program&, const BodyIndexTable&, const BodyIndex& definer, ValueID newClass);

// The one question in front of dropping an operation whose result nobody uses: it neither throws by a rule of the language nor runs code, and writes nothing that anybody sees.
inline bool neitherThrowsNorRunsCode(const Effects& effects) { return effects.isRemovableWhenUnused(); }
inline bool neitherThrowsNorRunsCode(EffectTraits traits) { return traits.isRemovableWhenUnused(); }

// Texts that are joined: whether the result surely fits in a string, so that the operation throws only for lack of memory.
bool hasTextByItself(const Type&);
uint64_t longestTextOf(TypeReader, const Type&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
