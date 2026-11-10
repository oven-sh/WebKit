/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsOfOperations.h"
#include "AOTFoldingOfConstants.h"
#include "AOTOpcodeSets.h"

namespace JSC { namespace AOT { namespace Typed {

extern const OpcodeSet opcodesThatBuildDeferredDefinitionTakes;
inline constexpr OpcodeSet opcodesThatBuildDeferredDefinitionTakesInChains = inputPassThroughOpcodes | OpcodeSet { Opcode::NewRegExp, Opcode::Data, Opcode::GetModuleVariable, Opcode::PutModuleVariable };
bool buildDeferredDefinitionOfItsOpcodes(Walker&, const BodyValue&);
ALWAYS_INLINE bool buildDeferredDefinition(Walker& walker, const BodyValue& value) { return opcodesThatBuildDeferredDefinitionTakesInChains.contains(value.opcode()) && buildDeferredDefinitionOfItsOpcodes(walker, value); }
template<Opcode known> ALWAYS_INLINE bool buildDeferredDefinition(Walker& walker, const BodyValueOf<known>& value)
{
    if constexpr (!opcodesThatBuildDeferredDefinitionTakesInChains.contains(known))
        return false;
    else if constexpr (known == Opcode::GetModuleVariable)
        return walker.plan().deferredDefinitions().isDeferred.get(value.template as<Opcode::GetModuleVariable>().variable().index()) && buildDeferredDefinitionOfItsOpcodes(walker, value);
    else
        return walker.plan().deferredDefinitions().bodiesThatDefine.get(walker.currentFunction().index()) && buildDeferredDefinitionOfItsOpcodes(walker, value);
}
extern const OpcodeSet opcodesThatFoldOperationTakes;
inline constexpr OpcodeSet opcodesThatFoldOperationTakesInChains = operationsOnPrimitivesInFolding | castsOfFolding | passThroughChecks | OpcodeSet { Opcode::TypeTest, Opcode::IsClosureOf, Opcode::ToBoolean, Opcode::Eq, Opcode::StrictEq,
    Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::PropGet, Opcode::ElemGet, Opcode::PutModuleVariable, Opcode::PutEnvironmentField,
    Opcode::ToObject, Opcode::ToThis, Opcode::TypeOf, Opcode::ToLength };
AOT_LINK_OF_WALK(foldOperation, opcodesThatFoldOperationTakesInChains)
inline constexpr OpcodeSet allocationsOfEscapeAnalysis = opcodesWhere([](Opcode opcode) {
    if (opcode == Opcode::NewClosure || opcode == Opcode::Data)
        return true;
    if (resultRuleOf(opcode) != ResultRule::Allocation || effectRuleOf(opcode) != EffectRule::Declared || boundToOf(opcode) == BoundTo::Site)
        return false;
    Effects bound = declaredBoundOf(opcode);
    return bound.writes.containsOnly(unobservableWriteHeaps) && !bound.mayRunOtherCode();
});
static_assert(allocationsOfEscapeAnalysis.contains(Opcode::NewObject) && allocationsOfEscapeAnalysis.contains(Opcode::NewArray) && allocationsOfEscapeAnalysis.contains(Opcode::CreateIteratorResult));
static_assert(!allocationsOfEscapeAnalysis.contains(Opcode::NewTemplateObject) && !allocationsOfEscapeAnalysis.contains(Opcode::TruncateNewArray));

inline constexpr OpcodeSet namesOfOperandInEscapeAnalysis = opcodesWithPassThrough({ PassThroughKind::PassedCheck, PassThroughKind::Mark });
inline constexpr OpcodeSet argumentsObjectsOfEscapeAnalysis { Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments };
static_assert(allocationsOfEscapeAnalysis.contains(Opcode::CreateUnmappedArguments) && allocationsOfEscapeAnalysis.contains(Opcode::CreateMappedArguments));
inline constexpr OpcodeSet opcodesOfEscapeAnalysis = allocationsOfEscapeAnalysis | namesOfOperandInEscapeAnalysis | OpcodeSet { Opcode::StrictEq, Opcode::GetMappedArgument, Opcode::PropDelete };
AOT_LINK_OF_WALK(buildFreshObject, opcodesOfEscapeAnalysis)
extern const OpcodeSet opcodesThatCallBuiltinTakes;
inline constexpr OpcodeSet opcodesThatCallBuiltinTakesInChains { Opcode::Call, Opcode::Construct, Opcode::CallList, Opcode::ConstructList };
AOT_LINK_OF_WALK(callBuiltin, opcodesThatCallBuiltinTakesInChains)
inline constexpr OpcodeSet opcodesThatInlineCalleeTakes { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::CallDirect, Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };
AOT_LINK_OF_WALK(inlineCallee, opcodesThatInlineCalleeTakes)
inline constexpr OpcodeSet opcodesThatCallDirectlyTakes { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::CallList, Opcode::CallDirect, Opcode::ConstructList, Opcode::SuperConstructList };
AOT_LINK_OF_WALK(callDirectly, opcodesThatCallDirectlyTakes)
extern const OpcodeSet opcodesThatBuildTypedIterationTakes;
inline constexpr OpcodeSet opcodesThatBuildTypedIterationTakesInChains { Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::IteratorClose, Opcode::AppendSpread, Opcode::AppendElement, Opcode::AppendHole,
    Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList, Opcode::ResultAt };
AOT_LINK_OF_WALK(buildTypedIteration, opcodesThatBuildTypedIterationTakesInChains)
inline constexpr OpcodeSet opcodesThatBuildIterationThatAsksItsListItselfTakes { Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::IteratorClose, Opcode::ResultAt };
AOT_LINK_OF_WALK(buildIterationThatAsksItsListItself, opcodesThatBuildIterationThatAsksItsListItselfTakes)
extern const OpcodeSet opcodesThatBuildTypedConstructionTakes;
inline constexpr OpcodeSet opcodesThatBuildTypedConstructionTakesInChains { Opcode::GetConstructPrototype, Opcode::NewObject };
AOT_LINK_OF_WALK(buildTypedConstruction, opcodesThatBuildTypedConstructionTakesInChains)
extern const OpcodeSet opcodesThatReplaceOperationTakes;
inline constexpr OpcodeSet opcodesThatReplaceOperationTakesInChains = opcodesThatStateTypes | callsByValue | constructionsByValue | OpcodeSet { Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::IteratorClose, Opcode::AppendSpread,
    Opcode::Await, Opcode::Yield, Opcode::InitialYield, Opcode::ReadScope, Opcode::HasJSType, Opcode::IsHolderWithoutAddedNames, Opcode::ArraySpeciesCreate, Opcode::CreateIteratorResult, Opcode::StepArrayIterator, Opcode::StepMapIterator, Opcode::StepSetIterator,
    Opcode::EvaluateModule, Opcode::RecordRequire, Opcode::Call, Opcode::DefineMethod, Opcode::DefineGetter, Opcode::DefineSetter, Opcode::PropGet, Opcode::PropSet };
AOT_LINK_OF_WALK(replaceOperation, opcodesThatReplaceOperationTakesInChains)
inline constexpr OpcodeSet opcodesThatBuildTypedJoinOfTextsTakes = operationsThatJoinTexts;
AOT_LINK_OF_WALK(buildTypedJoinOfTexts, opcodesThatBuildTypedJoinOfTextsTakes)
inline constexpr OpcodeSet opcodesThatBuildFormOnShapesTakes { Opcode::InstanceOf };
AOT_LINK_OF_WALK_WITH_INPUTS(buildFormOnShapes, opcodesThatBuildFormOnShapesTakes)
inline constexpr OpcodeSet opcodesThatBuildInstanceOfBuiltinTakes { Opcode::InstanceOf };
AOT_LINK_OF_WALK_WITH_INPUTS(buildInstanceOfBuiltin, opcodesThatBuildInstanceOfBuiltinTakes)
inline constexpr OpcodeSet opcodesThatBuildAccessByIndexOffExpectedViewTakes { Opcode::ElemGet, Opcode::ElemSet };
AOT_LINK_OF_WALK(buildAccessByIndexOffExpectedView, opcodesThatBuildAccessByIndexOffExpectedViewTakes)
inline constexpr OpcodeSet opcodesThatBuildReadByIndexOffAnyBaseTakes { Opcode::ElemGet };
AOT_LINK_OF_WALK(buildReadByIndexOffAnyBase, opcodesThatBuildReadByIndexOffAnyBaseTakes)
inline constexpr OpcodeSet opcodesThatAskPlainVectorBehindRowTakes { Opcode::HasContiguousIndex, Opcode::GetOwnContiguousIndex, Opcode::TryPutByValDirect };
AOT_LINK_OF_WALK(askPlainVectorBehindRow, opcodesThatAskPlainVectorBehindRowTakes)
extern const OpcodeSet opcodesThatOpenGuessAtTakes;
inline constexpr OpcodeSet opcodesThatOpenGuessAtTakesInChains = opcodesOfFamily(OperationFamily::Access) | opcodesOfFamily(OperationFamily::PrivateAccess) | opcodesOfFamily(OperationFamily::Operator) | opcodesOfFamily(OperationFamily::ArrayIntrinsic)
    | OpcodeSet { Opcode::SuperConstruct, Opcode::SuperConstructList, Opcode::Call, Opcode::Construct, Opcode::CallList, Opcode::RequireObjectCoercible };
GuessOpened openGuessAtOfItsOpcodes(Walker&, const BodyValue&);
GuessOpened openGuessAtOnlyProvenHolderOfName(Walker&, const BodyValue&);
bool tellRowsApartByKindOfStorage(Walker&, const BodyValue& read);
inline constexpr OpcodeSet opcodesThatAskRowOnceWhereStretchStoresIntoItTakes { Opcode::ElemGet, Opcode::ElemSet };
AOT_LINK_OF_WALK(askRowOnceWhereStretchStoresIntoIt, opcodesThatAskRowOnceWhereStretchStoresIntoItTakes)
ALWAYS_INLINE GuessOpened openGuessAt(Walker& walker, const BodyValue& value) { return opcodesThatOpenGuessAtTakesInChains.contains(value.opcode()) ? openGuessAtOfItsOpcodes(walker, value) : GuessOpened::None; }
template<Opcode known> ALWAYS_INLINE GuessOpened openGuessAt(Walker& walker, const BodyValueOf<known>& value) { return opcodesThatOpenGuessAtTakesInChains.contains(known) ? openGuessAtOfItsOpcodes(walker, value) : GuessOpened::None; }

ALWAYS_INLINE bool Walker::inlineCalleeAsLinkOfChain(const BodyValue& call) { return opcodesThatInlineCalleeTakes.contains(call.opcode()) && inlineCalleeAsLinkOfChainOfItsOpcodes(call); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
