/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOwnAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTIdentifierNumbering.h"
#include "AOTNameClass.h"
#include "AOTStubsPropertyKeys.h"

namespace JSC { namespace AOT { namespace Typed {

static bool isArrayAndNothingElse(const Type& type)
{
    return !type.primitives && !type.anyObject && !type.anyFunction && !type.layouts && type.functions.isEmpty() && type.builtins.isEmpty()
        && type.classes.size() == 1 && type.classes[0].builtinClass == BuiltinClasses::Array;
}

void OwnAccessEmitter::generatePropHasOwn(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    std::optional<uint32_t> identifier = build().identifierNumbering().numberOf(value->key());
    if (!identifier) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        defineTest(value, boolean(false));
        return;
    }
    auto askStub = [&] {
        AccessEmitter& accesses = emitter<AccessEmitter>();
        if (accesses.isSymbolOfRealm(value->key()))
            return Condition(m_out.castToInt32(callStub(value, inByValCached(PresenceQuestion::HasOwn), { object.value(), accesses.cellOfKey(value, value->key()).value() }).first));
        return Condition(m_out.castToInt32(callStub(value, inById(PresenceQuestion::HasOwn), { object.value() }, { .immediate = *identifier }).first));
    };

    // An array has of its own its length, its elements and what its Structure lists, and a Structure to which no property was ever added lists nothing.
    NameClass nameClass = m_program.nameClassOf(value->key());
    if (!isArrayAndNothingElse(typeOf(value->objectOperand())) || nameClass == NameClass::Length || mayBeElementKey(nameClass)) {
        defineTest(value, askStub());
        return;
    }
    static_assert(sizeof(Structure::SeenProperties) == sizeof(void*));
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock mayHaveNames = newBlock();
        m_out.branch(m_out.isNull(m_out.loadPtr(loadStructure(unboxCell(object)).value(), m_heaps.Structure_seenProperties)), targets.notTaken, normalEdge(mayHaveNames));
        m_out.appendTo(mayHaveNames);
        branch(askStub(), targets);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
