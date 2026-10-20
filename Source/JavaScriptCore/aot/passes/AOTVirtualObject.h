/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTLimits.h"
#include <optional>
#include <span>
#include <wtf/Forward.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

class FlowResults;
class FunctionAnalyses;
class TypeArena;

using VirtualObjectID = ID<struct VirtualObjectIDTag>;

constexpr OpcodeSet opcodesInterpretedAs(Interpretation interpretation)
{
    OpcodeSet result;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (interpretedOf(opcode) == interpretation)
            result.add(opcode);
    }
    return result;
}

// The schema answers by opcode. What a built-in call does to its receiver depends on the built-in.
Interpretation interpretationOf(const Program&, const Value&);

enum class VirtualObjectKind : uint8_t {
    Opaque,
    Object,
    Environment,
    Closure,
    Array,
    ArgumentList,
    Arguments,
    StringRange,
    BuiltinFields,
    BuiltinPrefix,
};

enum class VirtualFieldKind : uint8_t {
    Named,
    LayoutField,
    Element,
    Capture,
    Length,
    Capacity,
    Prototype,
    Callee,
    ArgumentCount,
    RestList,
    StringBase,
    RangeStart,
    RangeEnd,
    Operand,
};

struct VirtualField {
    VirtualFieldKind kind { VirtualFieldKind::Element };
    PropertyKeyID key;
    LayoutID layout;
    uint32_t index { 0 };

    static VirtualField named(PropertyKeyID key) { return { VirtualFieldKind::Named, key, { }, 0 }; }
    static VirtualField layoutField(LayoutID layout, uint32_t field) { return { VirtualFieldKind::LayoutField, { }, layout, field }; }
    static VirtualField element(uint32_t index) { return { VirtualFieldKind::Element, { }, { }, index }; }
    static VirtualField capture(uint32_t index) { return { VirtualFieldKind::Capture, { }, { }, index }; }
    static VirtualField operand(uint32_t index) { return { VirtualFieldKind::Operand, { }, { }, index }; }
    static VirtualField part(VirtualFieldKind kind) { return { kind, { }, { }, 0 }; }

    bool isProperty() const { return kind == VirtualFieldKind::Named || kind == VirtualFieldKind::LayoutField || kind == VirtualFieldKind::Element; }

    void dump(PrintStream&) const;

    friend bool operator==(const VirtualField&, const VirtualField&) = default;
};

enum class VirtualConstant : uint8_t { Undefined, Null, False, True };

struct VirtualObjectDescription {
    VirtualObjectKind kind { VirtualObjectKind::Opaque };
    ValueID allocation;
    TypeID type;
    LayoutID layout;
    FunctionID function;
    TypeID elementType;
    uint32_t firstArgument { 0 };
    uint32_t numberOfParameters { 0 };
    std::optional<VirtualConstant> absentResult;
    bool isMapped { false };
    bool restListStaysAsCreated { true };
    bool hasKeyOrder { false };
    bool hasIdentity { true };
    bool canBeBuilt { true };
    bool staysInPlace { false };
    bool isDead { false };
};

enum class VirtualObjectSupport : uint8_t {
    Unknown,
    NotAnAllocation,
    NotFresh,
    HasEffectsAtAllocation,
    HasChosenStorage,
    HasNoPartsForm,
    ExceedsLimit,
    Supported,
};

struct DescribedObject {
    VirtualObjectSupport support { VirtualObjectSupport::Unknown };
    LimitID limit { LimitID::None };
    VirtualObjectDescription description;
};

struct VirtualObjectContext {
    const Program& program;
    const Function& function;
    const FlowResults* flow { nullptr };
};

DescribedObject describeVirtualObject(FunctionAnalyses&, ValueID allocation);

using InitialFieldFunctor = ScopedLambda<void(const VirtualField&, ValueID)>;
void forEachInitialField(const VirtualObjectContext&, const VirtualObjectDescription&, NOESCAPE const InitialFieldFunctor&);
uint32_t initialLengthOf(const Function&, const VirtualObjectDescription&);

std::optional<uint32_t> layoutFieldOf(const Program&, const VirtualObjectDescription&, const VirtualField&);
std::optional<uint32_t> prefixPropertyOf(const VirtualObjectContext&, const VirtualObjectDescription&, PropertyKeyID);
VirtualField fieldOfLayoutField(const Program&, LayoutID, uint32_t field);
TypeID typeOfField(const VirtualObjectContext&, const VirtualObjectDescription&, const VirtualField&);
bool isEnumerable(const Program&, const VirtualObjectDescription&, const VirtualField&);

enum class FieldLookup : uint8_t { Unknown, Field, DynamicElement, AbsentEverywhere };
enum class AbsentField : uint8_t { Unknown, FindsNothing, Throws };
enum class PresentField : uint8_t { Unknown, IsOwnData, Throws };

struct VirtualFieldAccess {
    FieldLookup lookup { FieldLookup::Unknown };
    AbsentField whenAbsent { AbsentField::Unknown };
    PresentField whenPresent { PresentField::Unknown };
    VirtualField field;
    ValueID dynamicIndex;
};

VirtualFieldAccess lookUpField(const VirtualObjectContext&, const VirtualObjectDescription&, const Value& access);
VirtualFieldAccess lookUpNamedField(const VirtualObjectContext&, const VirtualObjectDescription&, const Value& access);
VirtualFieldAccess lookUpElement(const VirtualObjectContext&, const VirtualObjectDescription&, const Value& access, ValueID position);

std::optional<VirtualConstant> virtualConstantOf(const Program&, const Function&, ValueID);
TypeID typeOf(VirtualConstant);
Value makeConstant(VirtualConstant);

struct FieldValue {
    VirtualField field;
    ValueID value;
};

struct AllocationRequest {
    const VirtualObjectDescription& description;
    const Value& prototype;
    std::span<const FieldValue> properties;
    std::span<const FieldValue> parts;
    uint32_t length { 0 };
    // The allocation itself produces its absentResult when its operands say so, and both outcomes are still possible here.
    bool mayBeAbsent { false };
};

Value makeAllocation(const Program&, const AllocationRequest&);
Value makeFieldRead(const Program&, const VirtualObjectDescription&, const VirtualField&, ValueID cell, ValueID guard);
Value makeFieldStore(const Program&, const VirtualObjectDescription&, const FieldValue&, ValueID cell, ValueID guard);
void setExactReceivers(Value& slotOperation, LayoutID, TypeArena&);

enum class SpeculativeLoad : uint8_t { Unknown, Safe };
SpeculativeLoad canLoadSpeculatively(const VirtualObjectContext&, const VirtualObjectDescription&, const VirtualField&);

enum class LateStore : uint8_t { Unknown, NeedsNoPlaceholder, NeedsUndefinedPlaceholder, CreatesProperty };
LateStore canStoreLate(const VirtualObjectContext&, const VirtualObjectDescription&, const VirtualField&);

ASCIILiteral nameOf(VirtualObjectKind);
ASCIILiteral nameOf(VirtualFieldKind);
ASCIILiteral nameOf(VirtualObjectSupport);
ASCIILiteral nameOf(VirtualConstant);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
