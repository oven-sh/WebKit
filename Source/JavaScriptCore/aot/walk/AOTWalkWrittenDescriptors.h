/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet opcodesThatBuildDefinitionByWrittenDescriptorTakes { Opcode::Call };
inline constexpr unsigned numberOfArgumentsOfDescriptorCall = 3;
bool buildDefinitionByWrittenDescriptorOfItsOpcodes(Walker&, const BodyValue&);
ALWAYS_INLINE bool buildDefinitionByWrittenDescriptor(Walker& walker, const BodyValue& value) { return value.opcode() == Opcode::Call && value.as<Opcode::Call>().argumentsOperands().size() == numberOfArgumentsOfDescriptorCall && buildDefinitionByWrittenDescriptorOfItsOpcodes(walker, value); }
template<Opcode known> ALWAYS_INLINE bool buildDefinitionByWrittenDescriptor(Walker& walker, const BodyValueOf<known>& value) { return known == Opcode::Call && value.template as<Opcode::Call>().argumentsOperands().size() == numberOfArgumentsOfDescriptorCall && buildDefinitionByWrittenDescriptorOfItsOpcodes(walker, value); }

inline constexpr unsigned objectCreateArgumentCount = 2;
uint32_t objectCreateDescriptorCount(Walker&, const BodyValue& objectCreateCall);
AOT_LINK_OF_WALK(expandObjectCreateDescriptors, opcodesThatExpandObjectCreateDescriptorsTakes)
AOT_LINK_OF_WALK(expandObjectCreateAtDefinition, opcodesThatExpandObjectCreateAtDefinitionTakes)

std::optional<Vector<PropertyOfFreshObject, 8>> propertiesOfLiteralWrittenForCall(Walker&, const BodyValue& call, ValueID written, Reference literal);
bool literalsFindNothingUnder(const Program&, PropertyKeyID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
