/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTOpcode.h"
#include "AOTResultRules.h"
#include "AOTType.h"
#include "AOTVector.h"
#include "TypedArrayType.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class Facts;
class Program;

// The types that the built-ins' table writes, in the one type domain. Pure functions of their arguments: the Facts rules, the result rules and the walk ask the same ones.

struct BuiltinTypeContext {
    const Program& program;
    TypeArena& types;
    // For what no type says: whether the objects of a type are pristine. Null in the result rules, where nothing is.
    const Facts* facts { nullptr };
    // What the contents of an operand are read through, as in the result rules. Null: PositionTypes::knowingNothing().
    const PositionTypes* positions { nullptr };
    // Expected: the operands' types are guesses, and what is written of the contents of a class counts.
    TypeLevel level { TypeLevel::Proven };
};

// An empty id: the operand was not passed.
struct BuiltinOperandTypes {
    TypeID receiver;
    Vector<TypeID, 4> arguments;
    // What holds of an object among the operands where the call stands, which no type says. Nothing wherever nobody has looked: it only ever claims less.
    PristineAspectSet aspectsOfReceiver;
    // By argument. It may be shorter than `arguments`.
    Vector<PristineAspectSet, 4> aspectsOfArguments;

    TypeID at(BuiltinOperand operand) const
    {
        if (operand.isReceiver())
            return receiver;
        RELEASE_ASSERT(operand.kind() == BuiltinOperand::Kind::Argument);
        return operand.argumentIndex() < arguments.size() ? arguments[operand.argumentIndex()] : TypeID();
    }

    PristineAspectSet aspectsOfArgument(size_t index) const { return index < aspectsOfArguments.size() ? aspectsOfArguments[index] : PristineAspectSet(); }
};

struct PristineRequirement {
    BuiltinClassID builtinClass;
    PristineAspectSet aspects;

    explicit operator bool() const { return !aspects.isEmpty(); }

    friend bool operator==(const PristineRequirement&, const PristineRequirement&) = default;
};

enum class TypeFit : uint8_t { Unknown, Outside, Inside };

struct OperandFit {
    TypeFit fit { TypeFit::Unknown };
    // Inside but for this, which nothing proves.
    PristineRequirement unprovenPristine;
    // Inside only because Facts says that the objects are pristine. Whoever builds on it leans on FactKind::PristineInstances first.
    bool restsOnPristineInstances { false };
};

// AOTBuiltinTypes.cpp
// `operands`: what a type that is relative to the operands of a call is relative to. Without them such a part is any value of its kind.
TypeID typeOfBuiltinType(const BuiltinTypeContext&, BuiltinTypeID, const BuiltinOperandTypes* operands = nullptr);
TypeID typeOfBuiltinLayoutField(const BuiltinTypeContext&, const BuiltinLayoutField&);
TypeID typeOfBuiltinConstant(const BuiltinTypeContext&, BuiltinID);
TypeID parameterTypeOfOverload(const BuiltinTypeContext&, BuiltinOverloadID, BuiltinOperand);
// Of the function that is the library's own text of a built-in, for `this` or a positional parameter of which nothing is written: as `this` what the first overload that names the function takes,
// as an argument what all such overloads for that receiver take there. `Array.prototype.map`: `this` is an Array and the callback a function. Null: nothing is expected.
// It stands in the place of a written type. typeTestedAtEntry() of facts/AOTSummary.h, computePositionTypes() and Walker::inlineCall() go by it.
TypeID typeExpectedByLibraryFunction(const Program&, TypeArena&, FunctionID, uint32_t entryParameter);
uint16_t primitivesOfBuiltinType(const BuiltinType&);
// Of a type that has Primitive::Integer among those.
Range integerRangeWrittenBy(const BuiltinType&);
TypedArrayType typedArrayTypeOf(const BuiltinTypeContext&, TypeID);
// A type says nothing of it. Storage, Names, NamesKeptBelow and Prototype where Facts::everyObjectHas(ObjectFact::IsPristine) of the type, else none.
// Never Dense or AsCreated by that fact alone: it speaks of members and of prototype links, not of holes or of what was stored since the object was made.
// Dense only where every object has ObjectFact::HasNoHoles as well, AsCreated only where every object has ObjectFact::IsAsCreated as well.
// What a test has proven since is Known's: pristineAspectsKnownOf() of walk/AOTWalkLibrary.h.
PristineAspectSet pristineAspectsProvenBy(const BuiltinTypeContext&, TypeID, BuiltinClassID);

// AOTBuiltinTypeFit.cpp
OperandFit fitOf(const BuiltinTypeContext&, TypeID operandOrNotPassed, BuiltinTypeID tested);
bool fitsIfPristine(const BuiltinTypeContext&, TypeID operandOrNotPassed, BuiltinTypeID tested);
// What fitOf() goes by for one primitive of the operand, for whoever has the values one by one: the solve. `integers`: those of the operand, read for Integer alone. Never LiteralString, whose texts are asked one by one.
TypeFit fitOfPrimitiveInBuiltinType(const BuiltinType& tested, PrimitiveSet, Range integers);
// Of an operand that the call leaves out.
TypeFit fitOfOmittedOperandInBuiltinType(const BuiltinType& tested);

// AOTBuiltinResultTypes.cpp
struct BuiltinResultQuery {
    BuiltinOverloadID overload;
    BuiltinCallForm form { BuiltinCallForm::Whole };
    const BuiltinOperandTypes& operands;
    // Of the operand lengthOfReceiver. Null: the call names none.
    TypeID lengthOfReceiver { };
};

Range computeResultRange(BuiltinRangeRule, std::span<const Range> operandRanges);
// The result rule of CallBuiltin, CallInline and TryCallInline.
TypeID resultTypeOfBuiltinCall(const BuiltinTypeContext&, const BuiltinResultQuery&);
// Of a call whose overload nobody has matched: the union over the overloads that these operands can reach.
TypeID resultTypeOfReachableOverloads(const BuiltinTypeContext&, BuiltinID, BuiltinCallMode, const BuiltinOperandTypes&);
// Those overloads, in the order of the table. None where the built-in has no overload of the mode or the call passes more arguments than the table has words for.
Vector<BuiltinOverloadID, 4> overloadsReachedBy(const BuiltinTypeContext&, BuiltinID, BuiltinCallMode, const BuiltinOperandTypes&);
// The rows of a function with BuiltinFlag::RequiresHolderAsThis speak of a call on its holder, or on undefined where it has none: `Promise.resolve.call(Other, x)` makes what Other makes.
// An empty id: no `this` is passed.
bool rowsSpeakOfCallOnReceiver(const BuiltinTypeContext&, BuiltinID, TypeID receiverOrNotPassed);
bool overloadTakesOperands(const BuiltinTypeContext&, BuiltinOverloadID, BuiltinCallForm, const BuiltinOperandTypes&);
bool overloadHasForm(const BuiltinTable&, BuiltinOverloadID, BuiltinCallForm);
bool distinguishesOmittedFromUndefined(const BuiltinTable&, const BuiltinParameter&);
// A member of %CompilerHelpers% or of Bun.bundle: no function stands behind it at run time, so a call that the walk leaves is an internal error of the build.
bool isCallThatTheWalkMustLower(const BuiltinTable&, BuiltinID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
