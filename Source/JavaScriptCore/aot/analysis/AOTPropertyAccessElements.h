/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyAccess.h"
#include "TypedArrayType.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

std::optional<Access> elementAccessOf(Opcode);
Opcode namedOperationFor(Opcode elementOperation);

struct ElementAccessQuery {
    Access access { Access::Read };
    const Type& receiver;
    const Type* receiverPrediction { nullptr };
    const Type& key;
    const Type* keyPrediction { nullptr };
    const Type* stored { nullptr };
    const Type* storedPrediction { nullptr };
};

enum class ElementAccessForm : uint8_t {
    ByKey,
    NamedKey,
    Array,
    TypedArray,
    String,
    Split,
};

enum class ConstantKeyKind : uint8_t { None, Text, Symbol };

struct ConstantKey {
    ConstantKeyKind kind { ConstantKeyKind::None };
    StringID text;
    PropertyKeyID symbol;

    explicit operator bool() const { return kind != ConstantKeyKind::None; }
};

ConstantKey constantKeyOf(const Program&, const Type& key);
// The test of a value that is predicted to be a number asks for int32 or for any number. A narrower range in a prediction is what flow happened to see.
TypeID testedNumberTypeContaining(const Type& predictedNumber);
PropertyKeyID internKey(TypeArena&, const ConstantKey&);
void becomeNamedAccess(Value& elementAccess, PropertyKeyID);

struct ElementAccessPlan {
    ElementAccessForm form { ElementAccessForm::ByKey };
    KnowledgeLevel level { KnowledgeLevel::Unknown };
    Opcode operation { Opcode::Undefined };
    ConstantKey constantKey;
    BuiltinClassID builtinClass;
    ArrayStorage arrayStorage { ArrayStorage::Other };
    TypedArrayType typedArrayType { NotTypedArray };
    bool keyIsIntegral { false };
    bool testsReceiverClass { false };
    bool testsReceiverIsString { false };
    bool testsKeyIsNumber { false };
    bool testsStoredValue { false };
    bool testsArrayStorage { false };
    bool testsInBounds { false };
    bool isInBounds { false };
    OptionSet<PristineAspects> requiredAspects;
    UnprovenCause cause { UnprovenCause::Unknown };

    bool isResolved() const { return form != ElementAccessForm::ByKey && form != ElementAccessForm::Split && form != ElementAccessForm::String; }
    bool needsNoTest() const { return !testsReceiverClass && !testsReceiverIsString && !testsKeyIsNumber && !testsStoredValue && !testsArrayStorage && !testsInBounds && requiredAspects.isEmpty(); }
    // The form String says that the types prove a character read. This access has that form once its tests have narrowed its operands.
    bool isCharacterReadAfterTests() const { return form == ElementAccessForm::ByKey && !needsNoTest(); }

    void dump(PrintStream&) const;
};

ElementAccessPlan planElementAccess(const Program&, const ElementAccessQuery&);

std::optional<ElementAccessPlan> planElementAccessOf(const Program&, const Function&, const Value& access);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
