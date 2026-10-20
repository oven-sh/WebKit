/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowNames.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

ASCIILiteral nameOf(FlowContext context)
{
    switch (context) {
    case FlowContext::Generic:
        return "Generic"_s;
    case FlowContext::Proven:
        return "Proven"_s;
    case FlowContext::ProvenUnderGeneric:
        return "ProvenUnderGeneric"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowCertainty certainty)
{
    switch (certainty) {
    case FlowCertainty::Predicted:
        return "Predicted"_s;
    case FlowCertainty::Proven:
        return "Proven"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowKeyClass keyClass)
{
    switch (keyClass) {
    case FlowKeyClass::String:
        return "String"_s;
    case FlowKeyClass::Index:
        return "Index"_s;
    case FlowKeyClass::Symbol:
        return "Symbol"_s;
    case FlowKeyClass::PrivateName:
        return "PrivateName"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowObjectKind kind)
{
    switch (kind) {
    case FlowObjectKind::Unknown:
        return "Unknown"_s;
    case FlowObjectKind::Allocation:
        return "Allocation"_s;
    case FlowObjectKind::Closure:
        return "Closure"_s;
    case FlowObjectKind::Prototype:
        return "Prototype"_s;
    case FlowObjectKind::Instance:
        return "Instance"_s;
    case FlowObjectKind::Opaque:
        return "Opaque"_s;
    case FlowObjectKind::ModuleNamespace:
        return "ModuleNamespace"_s;
    case FlowObjectKind::Builtin:
        return "Builtin"_s;
    case FlowObjectKind::ModuleBuiltin:
        return "ModuleBuiltin"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowSiteKind kind)
{
    switch (kind) {
    case FlowSiteKind::Layout:
        return "Layout"_s;
    case FlowSiteKind::Closure:
        return "Closure"_s;
    case FlowSiteKind::Instance:
        return "Instance"_s;
    case FlowSiteKind::Opaque:
        return "Opaque"_s;
    case FlowSiteKind::PlainObject:
        return "PlainObject"_s;
    case FlowSiteKind::PlainArray:
        return "PlainArray"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowSiteOrigin origin)
{
    switch (origin) {
    case FlowSiteOrigin::Operation:
        return "Operation"_s;
    case FlowSiteOrigin::DataNode:
        return "DataNode"_s;
    case FlowSiteOrigin::CoroutineResult:
        return "CoroutineResult"_s;
    case FlowSiteOrigin::RestParameter:
        return "RestParameter"_s;
    case FlowSiteOrigin::BuiltinResult:
        return "BuiltinResult"_s;
    case FlowSiteOrigin::BuiltinElement:
        return "BuiltinElement"_s;
    case FlowSiteOrigin::BuiltinConstructor:
        return "BuiltinConstructor"_s;
    case FlowSiteOrigin::TypedResult:
        return "TypedResult"_s;
    case FlowSiteOrigin::RuntimeError:
        return "RuntimeError"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowNodeKind kind)
{
    switch (kind) {
    case FlowNodeKind::NativeCode:
        return "NativeCode"_s;
    case FlowNodeKind::UnknownJavaScript:
        return "UnknownJavaScript"_s;
    case FlowNodeKind::Variable:
        return "Variable"_s;
    case FlowNodeKind::Heritage:
        return "Heritage"_s;
    case FlowNodeKind::DeclaredPart:
        return "DeclaredPart"_s;
    case FlowNodeKind::Position:
        return "Position"_s;
    case FlowNodeKind::UndeclaredPart:
        return "UndeclaredPart"_s;
    case FlowNodeKind::PrototypeLink:
        return "PrototypeLink"_s;
    case FlowNodeKind::IndexedProperties:
        return "IndexedProperties"_s;
    case FlowNodeKind::StringProperties:
        return "StringProperties"_s;
    case FlowNodeKind::SymbolProperties:
        return "SymbolProperties"_s;
    case FlowNodeKind::Property:
        return "Property"_s;
    case FlowNodeKind::Getter:
        return "Getter"_s;
    case FlowNodeKind::Setter:
        return "Setter"_s;
    case FlowNodeKind::Temporary:
        return "Temporary"_s;
    case FlowNodeKind::Local:
        return "Local"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowInstanceState state)
{
    switch (state) {
    case FlowInstanceState::Created:
        return "Created"_s;
    case FlowInstanceState::Initialized:
        return "Initialized"_s;
    case FlowInstanceState::PastWorkLimit:
        return "PastWorkLimit"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowStatementKind statementKind)
{
    switch (statementKind) {
#define FLOW_STATEMENT_NAME(kind, type, member, cursors) \
    case FlowStatementKind::kind: \
        return #kind ""_s;
        FOR_EACH_FLOW_STATEMENT_KIND(FLOW_STATEMENT_NAME)
#undef FLOW_STATEMENT_NAME
    case FlowStatementKind::BuiltinCall:
        return "BuiltinCall"_s;
    case FlowStatementKind::Lookup:
        return "Lookup"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowSeedKind kind)
{
    switch (kind) {
    case FlowSeedKind::Primitives:
        return "Primitives"_s;
    case FlowSeedKind::Integers:
        return "Integers"_s;
    case FlowSeedKind::String:
        return "String"_s;
    case FlowSeedKind::Symbol:
        return "Symbol"_s;
    case FlowSeedKind::Site:
        return "Site"_s;
    case FlowSeedKind::Builtin:
        return "Builtin"_s;
    case FlowSeedKind::ModuleBuiltin:
        return "ModuleBuiltin"_s;
    case FlowSeedKind::Namespace:
        return "Namespace"_s;
    case FlowSeedKind::TypeAsRead:
        return "TypeAsRead"_s;
    case FlowSeedKind::Unknown:
        return "Unknown"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowExternalKind kind)
{
    switch (kind) {
    case FlowExternalKind::Variable:
        return "Variable"_s;
    case FlowExternalKind::Heritage:
        return "Heritage"_s;
    case FlowExternalKind::NativeCode:
        return "NativeCode"_s;
    case FlowExternalKind::UnknownJavaScript:
        return "UnknownJavaScript"_s;
    case FlowExternalKind::SiteField:
        return "SiteField"_s;
    case FlowExternalKind::SiteCapture:
        return "SiteCapture"_s;
    case FlowExternalKind::SiteSlot:
        return "SiteSlot"_s;
    case FlowExternalKind::SitePosition:
        return "SitePosition"_s;
    case FlowExternalKind::SiteUndeclaredPart:
        return "SiteUndeclaredPart"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowFilterKind kind)
{
    switch (kind) {
    case FlowFilterKind::Inside:
        return "Inside"_s;
    case FlowFilterKind::Outside:
        return "Outside"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowAccessKind kind)
{
    switch (kind) {
    case FlowAccessKind::Load:
        return "Load"_s;
    case FlowAccessKind::Store:
        return "Store"_s;
    case FlowAccessKind::Define:
        return "Define"_s;
    case FlowAccessKind::DefineGetter:
        return "DefineGetter"_s;
    case FlowAccessKind::DefineSetter:
        return "DefineSetter"_s;
    case FlowAccessKind::Delete:
        return "Delete"_s;
    case FlowAccessKind::Test:
        return "Test"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowReadUse use)
{
    switch (use) {
    case FlowReadUse::Value:
        return "Value"_s;
    case FlowReadUse::CalleeOnly:
        return "CalleeOnly"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowAccessFlag flag)
{
    switch (flag) {
    case FlowAccessFlag::StartsAtPrototype:
        return "StartsAtPrototype"_s;
    case FlowAccessFlag::IsFreeName:
        return "IsFreeName"_s;
    case FlowAccessFlag::IsForTypeOf:
        return "IsForTypeOf"_s;
    case FlowAccessFlag::IsSloppy:
        return "IsSloppy"_s;
    case FlowAccessFlag::IsInCycle:
        return "IsInCycle"_s;
    case FlowAccessFlag::IsOwnOnly:
        return "IsOwnOnly"_s;
    case FlowAccessFlag::NamesReceivers:
        return "NamesReceivers"_s;
    case FlowAccessFlag::IsWithinLength:
        return "IsWithinLength"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowCallMode mode)
{
    switch (mode) {
    case FlowCallMode::Call:
        return "Call"_s;
    case FlowCallMode::Construct:
        return "Construct"_s;
    case FlowCallMode::SuperConstruct:
        return "SuperConstruct"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowCallFlag flag)
{
    switch (flag) {
    case FlowCallFlag::IsInCycle:
        return "IsInCycle"_s;
    case FlowCallFlag::IsTailCall:
        return "IsTailCall"_s;
    case FlowCallFlag::IsAnnotatedPure:
        return "IsAnnotatedPure"_s;
    case FlowCallFlag::ResultGetsProperties:
        return "ResultGetsProperties"_s;
    case FlowCallFlag::ResultIsHandedUp:
        return "ResultIsHandedUp"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowIterationStep step)
{
    switch (step) {
    case FlowIterationStep::Open:
        return "Open"_s;
    case FlowIterationStep::Resume:
        return "Resume"_s;
    case FlowIterationStep::Close:
        return "Close"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowModuleOperationKind kind)
{
    switch (kind) {
    case FlowModuleOperationKind::Namespace:
        return "Namespace"_s;
    case FlowModuleOperationKind::Import:
        return "Import"_s;
    case FlowModuleOperationKind::Require:
        return "Require"_s;
    case FlowModuleOperationKind::ImportDynamic:
        return "ImportDynamic"_s;
    case FlowModuleOperationKind::RequireDynamic:
        return "RequireDynamic"_s;
    case FlowModuleOperationKind::RequireBuiltinModule:
        return "RequireBuiltinModule"_s;
    case FlowModuleOperationKind::ResolveDynamic:
        return "ResolveDynamic"_s;
    case FlowModuleOperationKind::Use:
        return "Use"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowClosureUse use)
{
    switch (use) {
    case FlowClosureUse::Other:
        return "Other"_s;
    case FlowClosureUse::StoredOnce:
        return "StoredOnce"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowClosureCapture capture)
{
    switch (capture) {
    case FlowClosureCapture::Other:
        return "Other"_s;
    case FlowClosureCapture::Nothing:
        return "Nothing"_s;
    case FlowClosureCapture::StoredIntoObject:
        return "StoredIntoObject"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowSummaryFlag flag)
{
    switch (flag) {
    case FlowSummaryFlag::PassesParameterOn:
        return "PassesParameterOn"_s;
    case FlowSummaryFlag::CreatesObjects:
        return "CreatesObjects"_s;
    case FlowSummaryFlag::HasCalls:
        return "HasCalls"_s;
    case FlowSummaryFlag::HandsUpNewObject:
        return "HandsUpNewObject"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowObjectUse use)
{
    switch (use) {
    case FlowObjectUse::ReadByName:
        return "ReadByName"_s;
    case FlowObjectUse::ReadByUnknownKey:
        return "ReadByUnknownKey"_s;
    case FlowObjectUse::Enumerated:
        return "Enumerated"_s;
    case FlowObjectUse::Called:
        return "Called"_s;
    case FlowObjectUse::Constructed:
        return "Constructed"_s;
    case FlowObjectUse::CalledRepeatedly:
        return "CalledRepeatedly"_s;
    case FlowObjectUse::TestedByInstanceOf:
        return "TestedByInstanceOf"_s;
    case FlowObjectUse::Serialized:
        return "Serialized"_s;
    case FlowObjectUse::ReachesTypedParse:
        return "ReachesTypedParse"_s;
    case FlowObjectUse::Awaited:
        return "Awaited"_s;
    case FlowObjectUse::Iterated:
        return "Iterated"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowObjectEvent event)
{
    switch (event) {
    case FlowObjectEvent::Allocated:
        return "Allocated"_s;
    case FlowObjectEvent::AllocatedByConstructor:
        return "AllocatedByConstructor"_s;
    case FlowObjectEvent::AllocatedRepeatedly:
        return "AllocatedRepeatedly"_s;
    case FlowObjectEvent::StoredByUnknownKey:
        return "StoredByUnknownKey"_s;
    case FlowObjectEvent::StoredByComputedKey:
        return "StoredByComputedKey"_s;
    case FlowObjectEvent::DeletedByUnknownKey:
        return "DeletedByUnknownKey"_s;
    case FlowObjectEvent::OtherNewTarget:
        return "OtherNewTarget"_s;
    case FlowObjectEvent::ElementsMoved:
        return "ElementsMoved"_s;
    case FlowObjectEvent::ContentsStored:
        return "ContentsStored"_s;
    case FlowObjectEvent::StoredUnderNumericName:
        return "StoredUnderNumericName"_s;
    case FlowObjectEvent::HoleMade:
        return "HoleMade"_s;
    case FlowObjectEvent::StoredWithinLength:
        return "StoredWithinLength"_s;
    case FlowObjectEvent::CopiedUnderUnknownKeys:
        return "CopiedUnderUnknownKeys"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowLocationEvent event)
{
    switch (event) {
    case FlowLocationEvent::WrittenByUnknownCode:
        return "WrittenByUnknownCode"_s;
    case FlowLocationEvent::ReadByUnknownCode:
        return "ReadByUnknownCode"_s;
    case FlowLocationEvent::InitializedAtAllocation:
        return "InitializedAtAllocation"_s;
    case FlowLocationEvent::MissingAtAllocation:
        return "MissingAtAllocation"_s;
    case FlowLocationEvent::StoredByOperation:
        return "StoredByOperation"_s;
    case FlowLocationEvent::StoredByManyOperations:
        return "StoredByManyOperations"_s;
    case FlowLocationEvent::ReadAsValue:
        return "ReadAsValue"_s;
    case FlowLocationEvent::ReadAsCallee:
        return "ReadAsCallee"_s;
    case FlowLocationEvent::PresenceObserved:
        return "PresenceObserved"_s;
    case FlowLocationEvent::StoredDeferrableClosure:
        return "StoredDeferrableClosure"_s;
    case FlowLocationEvent::StoredOtherValue:
        return "StoredOtherValue"_s;
    case FlowLocationEvent::Deleted:
        return "Deleted"_s;
    case FlowLocationEvent::Redefined:
        return "Redefined"_s;
    case FlowLocationEvent::StoredOutsideType:
        return "StoredOutsideType"_s;
    case FlowLocationEvent::ReadBeforeInitialization:
        return "ReadBeforeInitialization"_s;
    case FlowLocationEvent::StoredWithoutNamingLocation:
        return "StoredWithoutNamingLocation"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowCallLinkKind kind)
{
    switch (kind) {
    case FlowCallLinkKind::Call:
        return "Call"_s;
    case FlowCallLinkKind::Construct:
        return "Construct"_s;
    case FlowCallLinkKind::Accessor:
        return "Accessor"_s;
    case FlowCallLinkKind::BuiltinCallback:
        return "BuiltinCallback"_s;
    case FlowCallLinkKind::Protocol:
        return "Protocol"_s;
    case FlowCallLinkKind::Host:
        return "Host"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(FlowIndirectCallKind kind)
{
    switch (kind) {
    case FlowIndirectCallKind::Accessor:
        return "Accessor"_s;
    case FlowIndirectCallKind::BuiltinCallback:
        return "BuiltinCallback"_s;
    case FlowIndirectCallKind::Protocol:
        return "Protocol"_s;
    case FlowIndirectCallKind::Host:
        return "Host"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(LegacyRegExpStatics statics)
{
    switch (statics) {
    case LegacyRegExpStatics::Unknown:
        return "Unknown"_s;
    case LegacyRegExpStatics::NeverRead:
        return "NeverRead"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(ProgramInstances instances)
{
    switch (instances) {
    case ProgramInstances::Unknown:
        return "Unknown"_s;
    case ProgramInstances::AtMostOne:
        return "AtMostOne"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
