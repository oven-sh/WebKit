/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

extern const OpcodeSet opcodesThatBuildDeferredDefinitionTakes;
AOT_LINK_OF_WALK(buildDeferredDefinition, opcodesThatBuildDeferredDefinitionTakes)
extern const OpcodeSet opcodesThatFoldOperationTakes;
AOT_LINK_OF_WALK(foldOperation, opcodesThatFoldOperationTakes)
extern const OpcodeSet opcodesThatBuildFreshObjectTakes;
AOT_LINK_OF_WALK(buildFreshObject, opcodesThatBuildFreshObjectTakes)
extern const OpcodeSet opcodesThatCallBuiltinTakes;
AOT_LINK_OF_WALK(callBuiltin, opcodesThatCallBuiltinTakes)
inline constexpr OpcodeSet opcodesThatInlineCalleeTakes { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::CallDirect, Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };
AOT_LINK_OF_WALK(inlineCallee, opcodesThatInlineCalleeTakes)
inline constexpr OpcodeSet opcodesThatCallDirectlyTakes { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::CallList, Opcode::CallDirect, Opcode::ConstructList, Opcode::SuperConstructList };
AOT_LINK_OF_WALK(callDirectly, opcodesThatCallDirectlyTakes)
extern const OpcodeSet opcodesThatBuildTypedIterationTakes;
AOT_LINK_OF_WALK(buildTypedIteration, opcodesThatBuildTypedIterationTakes)
AOT_LINK_OF_WALK(buildTypedConstruction, opcodesThatBuildTypedConstructionTakes)
extern const OpcodeSet opcodesThatReplaceOperationTakes;
AOT_LINK_OF_WALK(replaceOperation, opcodesThatReplaceOperationTakes)
inline constexpr OpcodeSet opcodesThatBuildTypedJoinOfTextsTakes = operationsThatJoinTexts;
AOT_LINK_OF_WALK(buildTypedJoinOfTexts, opcodesThatBuildTypedJoinOfTextsTakes)
inline constexpr OpcodeSet opcodesThatBuildFormOnShapesTakes { Opcode::InstanceOf };
AOT_LINK_OF_WALK_WITH_INPUTS(buildFormOnShapes, opcodesThatBuildFormOnShapesTakes)
inline constexpr OpcodeSet opcodesThatBuildInstanceOfBuiltinTakes { Opcode::InstanceOf };
AOT_LINK_OF_WALK_WITH_INPUTS(buildInstanceOfBuiltin, opcodesThatBuildInstanceOfBuiltinTakes)
inline constexpr OpcodeSet opcodesThatBuildAccessByIndexOffExpectedViewTakes { Opcode::ElemGet, Opcode::ElemSet };
AOT_LINK_OF_WALK(buildAccessByIndexOffExpectedView, opcodesThatBuildAccessByIndexOffExpectedViewTakes)
inline constexpr OpcodeSet opcodesThatAskPlainVectorBehindRowTakes { Opcode::HasContiguousIndex, Opcode::GetOwnContiguousIndex };
AOT_LINK_OF_WALK(askPlainVectorBehindRow, opcodesThatAskPlainVectorBehindRowTakes)
extern const OpcodeSet opcodesThatOpenGuessAtTakes;
GuessOpened openGuessAtOfItsOpcodes(Walker&, const BodyValue&);
ALWAYS_INLINE GuessOpened openGuessAt(Walker& walker, const BodyValue& value) { return opcodesThatOpenGuessAtTakes.contains(value.opcode()) ? openGuessAtOfItsOpcodes(walker, value) : GuessOpened::None; }
template<Opcode known> ALWAYS_INLINE GuessOpened openGuessAt(Walker& walker, const BodyValueOf<known>& value) { return opcodesThatOpenGuessAtTakes.contains(known) ? openGuessAtOfItsOpcodes(walker, value) : GuessOpened::None; }

ALWAYS_INLINE bool Walker::inlineCalleeAsLinkOfChain(const BodyValue& call) { return opcodesThatInlineCalleeTakes.contains(call.opcode()) && inlineCalleeAsLinkOfChainOfItsOpcodes(call); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
