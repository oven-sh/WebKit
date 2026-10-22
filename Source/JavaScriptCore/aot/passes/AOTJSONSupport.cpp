/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONSupport.h"

#if ENABLE(AOT)

#include "AOTExpandOwnKeysOrders.h"
#include "AOTFlowPrototypes.h"
#include "AOTFlowResults.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPropertyAccess.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ProofSet proofsTestedBySerializers { Proof::NoExtraProperties, Proof::PrototypeLinkUnchanged };

static bool canHaveSerializer(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
        return true;
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canReadWithGetSlot(const PropertyAccessPlan& plan)
{
    return plan.form == AccessForm::Slot && (plan.operation == Opcode::GetSlot || plan.operation == Opcode::GetSlotOrUndefined);
}

bool isJSONSerializedKey(const Program& program, PropertyKeyID key)
{
    if (!key)
        return false;
    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
        return true;
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A class object inherits from the class that it extends, which may have a static method of the name.
static bool findsNoToJSON(const Program& program, const FlowResults& flow, FunctionID closureFunction)
{
    const Function& function = program.at(closureFunction);
    if (function.constructedClass)
        return false;

    PropertyKeyID toJSON = program.keyOf(BuiltinNames::toJSON);
    if (!flow.areClosuresAsCreated(closureFunction, singleElementSpan(toJSON)))
        return false;
    MemberAnswer inherited = findBuiltinObjectMember({ program, &flow }, prototypeOfFunctionObject(function), toJSON, Access::Read);
    return inherited.kind == MemberKind::Absent && inherited.needsNoTest();
}

// SerializeJSONProperty answers undefined for undefined, for a symbol, and for a function whose toJSON is not callable.
bool hasNoJSONText(const Program& program, const FlowResults& flow, const Type& type)
{
    if (type.isNever() || !type.isJavaScriptValue() || (type.primitives & ~primitivesWithoutJSONText))
        return false;
    if (type.anyObject || type.anyFunction || type.layouts || !type.classes.isEmpty() || !type.builtins.isEmpty())
        return false;
    return std::ranges::all_of(type.functions, [&](FunctionID function) {
        return findsNoToJSON(program, flow, function);
    });
}

JSONMemberSupport jsonMemberSupportOf(const Program& program, const FlowResults& flow, LayoutID id, uint32_t fieldIndex)
{
    const Field& field = program.at(id).fields[fieldIndex];
    if (!isEnumerableStringField(program, field))
        return JSONMemberSupport::NotSerialized;
    if (hasNoJSONText(program, flow, program.at(slotTypeOf(program, field))))
        return JSONMemberSupport::WithoutText;

    PropertyAccessContext context { program };
    PropertyAccessQuery query;
    query.access = Access::Read;
    query.start = MemberStart::Instance;
    query.key = field.key;
    query.receivers = ReceiverCandidates::underGuard(singleElementSpan(id));
    return canReadWithGetSlot(planPropertyAccess(context, query)) ? JSONMemberSupport::Slot : JSONMemberSupport::ByName;
}

bool hasNoToJSON(const Program& program, const FlowResults& flow, LayoutID id)
{
    PropertyAccessContext context { program, &flow };
    MemberQuery query { MemberStart::Instance, id, program.keyOf(BuiltinNames::toJSON), Access::Read, false };
    MemberAnswer toJSON = findMember(context, query);
    if (toJSON.kind != MemberKind::Absent || !toJSON.conditions.isEmpty())
        return false;
    return (toJSON.receiverProofs | proofsTestedBySerializers) == proofsTestedBySerializers;
}

JSONSupport jsonSupportOf(const Program& program, const FlowResults& flow, LayoutID id)
{
    const Layout& layout = program.at(id);
    if (!canHaveSerializer(layout.kind) || layout.flow.allocation == AllocationState::NeverAllocated)
        return JSONSupport::Unknown;
    return hasNoToJSON(program, flow, id) ? JSONSupport::Compiled : JSONSupport::Unknown;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
