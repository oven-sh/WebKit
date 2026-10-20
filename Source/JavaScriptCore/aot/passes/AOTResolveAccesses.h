/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMethodDispatch.h"
#include "AOTPropertyAccessElements.h"
#include "AOTResolveCallsDirectCall.h"
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

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

std::optional<NamedAccessSite> planNamedAccessSite(FunctionPassContext&, const ValueSite&);
std::optional<ElementAccessSite> planElementAccessSite(FunctionPassContext&, const ValueSite&);

bool isOnlyCalled(FunctionAnalyses&, ValueID read);

AccessOperands operandsOfNamedAccess(const Value& access);
SlotStamp createSlotStamp(FunctionPassContext&, const Value& access, const PropertyAccessPlan&);
Effects effectsAfterRewrite(FunctionPassContext&, const PropertyAccessPlan&, const SlotStamp&);
Effects effectsAfterRewrite(const ElementAccessPlan&, const Type& key);

class AccessRewriter {
    WTF_MAKE_NONCOPYABLE(AccessRewriter);
public:
    AccessRewriter(FunctionPassContext&, const InsertValue&);

    void rewrite(const NamedAccessSite&, const AccessOperands&);
    void rewrite(const ElementAccessSite&, const AccessOperands&);

private:
    Value& valueAt(const ValueSite& site) { return m_context.function.at(site.value); }
    ValueID insert(Value&&, Origin);
    void finish(const ValueSite&, const ReplacedEffects&);

    void becomeSlotOperation(const NamedAccessSite&, const AccessOperands&);
    void becomeConstant(const NamedAccessSite&);
    void becomeBuiltin(const NamedAccessSite&);
    void becomeMethodClosure(const NamedAccessSite&, const AccessOperands&);

    void becomeVariableAccess(const NamedAccessSite&, const AccessOperands&);
    void becomeNamespace(const NamedAccessSite&);

    void becomeNamedAccess(const ElementAccessSite&);
    void becomeArrayAccess(const ElementAccessSite&, const AccessOperands&);
    void becomeTypedArrayAccess(const ElementAccessSite&, const AccessOperands&);

    FunctionPassContext& m_context;
    const InsertValue& m_insert;
};

class AccessResolver {
    WTF_MAKE_NONCOPYABLE(AccessResolver);
public:
    explicit AccessResolver(FunctionPassContext&);

    PassOutcome run();

private:
    void planSites();
    Vector<ValueID> rewriteSites();
    void expandReadsOfFieldsThatMayBeEmpty();
    void recordLoss(const NamedAccessSite&);

    FunctionPassContext& m_context;
    IRBuilder m_builder;
    Vector<NamedAccessSite> m_namedSites;
    Vector<NamedAccessSite> m_readsOfFieldsThatMayBeEmpty;
    Vector<ElementAccessSite> m_elementSites;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
