/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowBodies.h"
#include "AOTValueFlowRuleContext.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

void describeHost(const Program&, ValueFlowBody&);

void runModuleOperation(const ValueFlowStatement&, ValueFlowRuleContext&);
void runModuleEvaluation(const ValueFlowStatement&, ValueFlowRuleContext&);

void runHostRoots(const ValueFlowStatement&, ValueFlowRuleContext&);
void runUsedBuiltins(const ValueFlowStatement&, ValueFlowRuleContext&);
void runUnseenCode(const ValueFlowStatement&, ValueFlowRuleContext&);
void runModulesLoadedByName(const ValueFlowStatement&, ValueFlowRuleContext&);

struct ValueFlowSources {
    Vector<ValueFlowNodeID, 2> nodes;
    ClimbingSet constant;
};

ClimbingSet unknownObject(ValueFlowRuleContext&, ValueFlowUnknownSource);
void connect(ValueFlowRuleContext&, const ValueFlowSources&, ValueFlowNodeID target);
ClimbingSet valueOf(ValueFlowRuleContext&, const ValueFlowSources&);
ClimbingSet withSourcesOfCopies(ValueFlowRuleContext&, ClimbingSet knownObjects);

bool awaitsUse(const Program&, ModuleID);
void useModule(ValueFlowRuleContext&, ModuleID);
ModuleID moduleOf(const Program&, RuntimeModuleID);
void evaluateModule(ValueFlowRuleContext&, ModuleID);
ValueFlowSources requiredValueOf(ValueFlowRuleContext&, ModuleID);
ValueFlowSources exportedValueOf(ValueFlowRuleContext&, const ModuleExport&);
ValueFlowSources namespaceOf(ValueFlowRuleContext&, ModuleID, ImportPhase = ImportPhase::Evaluation);

bool isInSeenCode(ValueFlowRuleContext&, FunctionID);
void noteRequest(ValueFlowRuleContext&, FunctionID requester, ModuleID);
void applyEventsOfCall(const ValueFlowStatement&, ValueFlowRuleContext&, ClimbingSet callees);

void applyEventsOfStore(const ValueFlowStatement&, ValueFlowRuleContext&, ClimbingSet receivers, PropertyKeyID name);
void appendModuleObjectsAnsweredBy(ValueFlowRuleContext&, BuiltinID canonicalEntry, Vector<ValueFlowObjectID, 32>&);
void handChildrenToWhatReplacesTheirList(ValueFlowRuleContext&, ValueSetID children);
bool isFoundByNameAlone(BuiltinID canonicalMember);

std::optional<UnseenCodeCause> eventOfCalling(const BuiltinTable&, BuiltinID, RefusedCode);

void raiseHostFlag(ValueFlowRuleContext&, ValueFlowHostNode);
bool isHostFlagRaised(ValueFlowRuleContext&, ValueFlowHostNode);

enum class NativeCaller : bool { Anybody, NamedByTable };
void enterFromNativeCode(ValueFlowRuleContext&, FunctionID, NativeCaller = NativeCaller::Anybody);
void passWhatAnnotationsSay(ValueFlowRuleContext&, ClimbingSet callees);
void letUndeclaredNativeCodeRun(ValueFlowRuleContext&);
inline void letHostChooseOperands(ValueFlowRuleContext& context) { raiseHostFlag(context, ValueFlowHostNode::HostChoosesOperands); }
enum class UnseenCodeReach : uint8_t {
    GlobalScope,
    Loader,
    Realm,
};
void letUnseenCodeRun(ValueFlowRuleContext&, UnseenCodeReach);
inline void letUnknownCodeWrite(ValueFlowRuleContext& context) { raiseHostFlag(context, ValueFlowHostNode::UnknownCodeWrites); }
inline bool mayUnknownCodeWrite(ValueFlowRuleContext& context) { return isHostFlagRaised(context, ValueFlowHostNode::UnknownCodeWrites) || isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeRuns); }
inline void letWhateverHasEscapedBeCalled(ValueFlowRuleContext& context) { raiseHostFlag(context, ValueFlowHostNode::ValueNobodyKnowsIsCalled); }
inline bool mayCallWhateverHasEscaped(ValueFlowRuleContext& context) { return isHostFlagRaised(context, ValueFlowHostNode::ValueNobodyKnowsIsCalled) || isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeRuns); }
void letAnythingHappenTo(ValueFlowRuleContext&, ClimbingSet objects);
void letModulesBeLoadedByName(ValueFlowRuleContext&, ValueFlowHostNode answers);
bool mayAnswerToName(const Program&, ModuleID);
void enterLoaderOfHost(ValueFlowRuleContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
