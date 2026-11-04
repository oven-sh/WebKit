/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations: a key that is one name is the name (`{ get [KEY]() { .. } }`), and a field that constructions alone write keeps what was read out of it once.
// At a suspension: neither rests on anything that other code can change, so both hold on.
// At a throw: a definition by name throws where the definition by key would. A read is noted behind its record.
// Withheld: the definition stays under its computed key, and every read of a field is built where it is written.

#include "config.h"
#include "AOTWalkKnown.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

// ---- K17

// A text that Compile has made is the text of no key, an index is an element, and `__proto__` has a meaning of its own where it is written.
static PropertyKeyID nameOfTextInKnownKeys(const Walker& walker, StringID text)
{
    if (!text || liesInThread(text))
        return { };
    NameClass nameClass = walker.program().nameClass(text);
    if (mayBeElementKey(nameClass) || nameClass == NameClass::Proto)
        return { };
    PropertyKeyID key = walker.types().textKey(text);
    return key && !liesInThread(key) ? key : PropertyKeyID();
}

PropertyKeyID nameThatKeyIs(Walker& walker, Reference key)
{
    // A string is its own key, and the conversion may be typed wider than what it converts.
    const Code& code = walker.code();
    if (key.isValue() && code.at(key.id()).opcode == Opcode::ToPropertyKey) {
        ValueID converted = code.at(key.id()).operand(0);
        key = code.isConstant(converted) ? Reference::constant(converted) : Reference::value(converted);
    }
    TypeID id = key ? walker.typeOf(key) : TypeID();
    if (!id)
        return { };
    Type type = walker.types().at(id);
    if (!type.isJavaScriptValue() || type.hasObjects() || type.primitives != Primitive::LiteralString)
        return { };
    return nameOfTextInKnownKeys(walker, singleStringLiteralOf(type));
}

PropertyKeyID nameWrittenAsKey(const Walker& walker, ValueID key)
{
    const BodyIndex& body = walker.currentBodyIndex();
    ValueID written = key ? body.valueBehindCasts(key) : ValueID();
    // A string is its own key.
    if (written && !body.isParameter(written) && body.opcodeOf(written) == Opcode::ToPropertyKey)
        written = body.valueBehindCasts(body.value(written).operand(0));
    if (!written || body.isParameter(written) || body.opcodeOf(written) != Opcode::String)
        return { };
    return nameOfTextInKnownKeys(walker, body.value(written).string());
}

bool defineAccessorUnderNameThatKeyIs(Walker& walker, const BodyValue& definition)
{
    if (!walker.has(Mechanism::TypedOperations) || !definition.computedKeyOperand())
        return false;
    PropertyKeyID name = nameThatKeyIs(walker, walker.referenceOf(definition.computedKeyOperand()));
    if (!name)
        return false;
    Draft byName = walker.draftOf(definition);
    byName.setComputedKeyOperand({ });
    byName.setKey(name);
    walker.build(WTF::move(byName));
    return true;
}

// ---- Fields of objects that are written once

// An object that lies in memory, or that a call hands back, has left the body that made it. A store of a construction stands in front of the first point at which its body lets the object leave: SummaryAccess::isPartOfConstruction.
static constexpr OpcodeSet readsOutOfMemoryInKnownKeys {
    Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::GetCapture, Opcode::PropGet, Opcode::ElemGet, Opcode::GetArrayElement
};

// Such a body is handed an object that it goes on making: `this` of an initializer of fields and of a function that `new` hands the object (isFilledForBodyBeingReadInTypedAccess()).
// A constructor of a class makes its own, or has it from `super()`, in the body as it is written. An entry that is handed what its caller has allocated is no different from the two above.
static bool mayBeHandedObjectToGoOnMakingInKnownKeys(const Function& function)
{
    switch (function.kind) {
    case FunctionKind::Plain:
        return function.takesThisFromConstruction() && function.constructedClass;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
        return true;
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An allocation of this code, what `super()` answers (a direct call of a construction too) and a parameter of a join, which may be either, are not among them.
static bool hasLeftBodyThatMadeItInKnownKeys(const Walker& walker, Reference object)
{
    if (!object.isValue())
        return false;
    const Code& code = walker.code();
    ValueID root = rootOfCellInSweptCode(code, object.id());
    if (code.isConstant(root))
        return false;
    const Operation& record = code.at(root);
    if (readsOutOfMemoryInKnownKeys.contains(record.opcode) || callsByValue.contains(record.opcode))
        return true;
    if (record.opcode == Opcode::CallDirect)
        return !record.isConstruction();
    if (record.opcode != Opcode::Parameter || !code.at(code.blockOf(root)).isEntry)
        return false;
    return !mayBeHandedObjectToGoOnMakingInKnownKeys(walker.program().at(walker.unit().function));
}

bool slotKeepsValueOnceFilled(Walker& walker, const Draft& read)
{
    if (read.opcode != Opcode::GetSlot || read.level() != KnowledgeLevel::Proven || !walker.has(Mechanism::TypedOperations))
        return false;
    Reference object = read.objectOperand();
    TypeID typeOfObject = object ? walker.typeOf(object) : TypeID();
    if (!typeOfObject)
        return false;
    TypeArena& types = walker.types();
    LayoutID layout = singleLayoutOf(types, types.at(typeOfObject));
    std::optional<FieldReference> field = layout ? findField(walker.program(), layout, read.key()) : std::nullopt;
    if (!field)
        return false;
    const LinkedCell* cell = walker.facts().linked().findCellOfField(*field);
    if (!cell || !cell->everyWriterIsKnown || cell->isWrittenAfterConstruction || !cell->isFilled)
        return false;
    return hasLeftBodyThatMadeItInKnownKeys(walker, object);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
