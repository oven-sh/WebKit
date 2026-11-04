/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyAccess.h"
#include "TypedArrayType.h"

namespace JSC { namespace AOT { namespace Typed {

std::optional<Access> elementAccessOf(Opcode);
Opcode namedOperationFor(Opcode elementOperation);

struct ElementAccessQuery {
    Access access { Access::Read };
    // The function in whose text the access stands.
    const Function& writtenIn;
    const Type& receiver;
    const Type* receiverExpected { nullptr };
    // Of a receiver that is a copy: what is known of the list that it was made from.
    ArrayStorage storageOfCopiedList { ArrayStorage::Other };
    const Type& key;
    const Type* keyExpected { nullptr };
    const Type* stored { nullptr };
    const Type* storedExpected { nullptr };
};

enum class ElementAccessForm : uint8_t { ByKey, NamedKey, Array, TypedArray, String, ArgumentsElement, Split };

enum class ConstantKeyKind : uint8_t { None, Text, Symbol };

struct ConstantKey {
    ConstantKeyKind kind { ConstantKeyKind::None };
    StringID text;
    PropertyKeyID symbol;

    explicit operator bool() const { return kind != ConstantKeyKind::None; }
};

ConstantKey constantKeyOf(const Program&, const Type& key);
// The same, where an operation that stands in the text of the function may name a property by it. A symbol that a module of the library creates is a name in the text of the library alone.
// Anywhere else it is a key like any other value, and the run time sees each look under it: PropertyCaches::prepareHoldersBeforeLookupsUnder().
ConstantKey constantNameOf(const Program&, const Function& writtenIn, const Type& key);
bool isSymbolCreatedByLibrary(const Program&, PropertyKeyID);
// The run time asks such an object to prepare itself before native code looks at its own properties: PropertyCaches::prepareHoldersBeforeLookupsUnder().
bool hasFieldUnderSymbolCreatedByLibrary(const Program&, const Layout&);
TypeID testedNumberTypeContaining(const Type& expectedNumber);
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
    bool checksNullish { false };
    bool testsKeyIsNumber { false };
    bool testsStoredValue { false };
    bool testsArrayStorage { false };
    bool testsInBounds { false };
    bool isInBounds { false };
    bool testsPresence { false };
    bool isPresent { false };
    bool readsHoleAsNaN { false };
    bool isInsideView { false };
    bool receiverIsNeverPristine { false };
    OptionSet<PristineAspects> requiredAspects;
    UnprovenCause cause { UnprovenCause::Unknown };

    bool isResolved() const { return form == ElementAccessForm::NamedKey || form == ElementAccessForm::Array || form == ElementAccessForm::TypedArray; }
    bool needsNoTest() const { return !testsReceiverClass && !testsReceiverIsString && !checksNullish && !testsKeyIsNumber && !testsStoredValue && !testsArrayStorage && !testsInBounds && !testsPresence && requiredAspects.isEmpty(); }
    bool isCharacterReadAfterTests() const { return form == ElementAccessForm::ByKey && !needsNoTest(); }

    void dump(PrintStream&) const;
};

ElementAccessPlan planElementAccess(const Program&, const ElementAccessQuery&);

std::optional<ElementAccessPlan> planElementAccessOf(const Program&, const Function&, const Value& access);

// The list that slice(), concat(), toReversed(), toSpliced() or with() has made this one from. A copy has the storage of the list that it is made from.
ValueID findCopiedList(const Program&, const Function&, ValueID copy);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
