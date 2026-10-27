/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMethodDispatch.h"
#include "AOTPropertyAccessElements.h"
#include "AOTRefusedContents.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// The operations that resolveAccesses() looks at. A deletion by a constant key becomes a deletion by name.
// They name another receiver than the object at which the lookup starts.
inline constexpr OpcodeSet readsForAnotherReceiver { Opcode::SuperGet, Opcode::GetByValWithThis };
inline constexpr OpcodeSet accessesToResolve = accessesByNameOrKey | readsForAnotherReceiver | OpcodeSet { Opcode::ElemDelete, Opcode::GetInternalField };

struct AccessOperands {
    ValueID receiver;
    ValueID stored;
    ValueID guard;
};

struct NamedAccessSite {
    ValueSite site;
    PropertyAccessQuery query;
    PropertyAccessPlan plan;
};

struct ElementAccessSite {
    ValueSite site;
    ElementAccessPlan plan;
};

// All that propertyAccessQueryOf() and planPropertyAccess() take from a named access and its function, but for who uses what is read. None where it names what an arena gave out.
std::optional<SiteContent> contentOfNamedAccess(const FunctionPassContext&, const Value& access, uint8_t planner);
std::optional<ElementAccessSite> planElementAccessSite(FunctionPassContext&, const ValueSite&);

bool isOnlyCalled(FunctionAnalyses&, ValueID read);

AccessOperands operandsOfNamedAccess(const Value& access);
SlotStamp createSlotStamp(FunctionPassContext&, const Value& access, const PropertyAccessPlan&);
PassOutcome addEmptySlotCases(FunctionPassContext&);
PassOutcome addCasesOfFieldsWithoutStorage(FunctionPassContext&);
Effects effectsAfterRewrite(FunctionPassContext&, const PropertyAccessPlan&, const SlotStamp&);
Effects effectsAfterRewrite(const ElementAccessPlan&, const Type& key);

struct AccessRewriter {
    void rewrite(const NamedAccessSite&, const AccessOperands&);
    void rewrite(const ElementAccessSite&, const AccessOperands&);

    FunctionPassContext& context;
    const InsertValue& insert;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
