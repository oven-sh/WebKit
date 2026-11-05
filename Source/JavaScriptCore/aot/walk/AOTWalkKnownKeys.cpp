/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations: a key that is one name is the name (`{ get [KEY]() { .. } }`).
// At a suspension: it rests on nothing that other code can change, so it holds on. At a throw: a definition by name throws where the definition by key would.
// Withheld: the definition stays under its computed key.

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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
