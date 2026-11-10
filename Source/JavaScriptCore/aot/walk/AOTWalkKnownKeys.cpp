/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    ValueID written = key ? body.stripCasts(key) : ValueID();
    if (written && !body.isParameter(written) && body.opcodeOf(written) == Opcode::ToPropertyKey)
        written = body.stripCasts(body.value(written).operand(0));
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
