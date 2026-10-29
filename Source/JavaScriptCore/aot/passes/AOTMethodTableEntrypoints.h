/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFamilyTables.h"
#include "AOTIR.h"
#include "AOTThreshold.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

// A call through the method table finds its target under the shape of the receiver and the selector: the name of the method, the number of arguments that the call passes, and how it passes them.
enum class ArgumentForm : uint8_t { Boxed, Typed };

struct MethodSelector {
    friend bool operator==(const MethodSelector&, const MethodSelector&) = default;
    friend auto operator<=>(const MethodSelector&, const MethodSelector&) = default;

    PropertyKeyID key;
    uint32_t numberOfArguments { 0 };
    ArgumentForm form { ArgumentForm::Boxed };
};

AOT_THRESHOLD(uint32_t, maxTableCallArguments, 4);

// Enters the method where `this` needs no test: the row of the table is the layout of the receiver.
DerivedFunctionKey tableEntrypointKey(FunctionID method, MethodSelector);
// Calls what the receiver has under the name of the selector. The entry of a layout whose objects may find something else than the class definitions say.
DerivedFunctionKey tableLookupKey(MethodSelector);
bool canHaveTableEntrypoint(const Program&, TypeArena&, FunctionID method, MethodSelector);
// `given`: parameterTypesOf() the selector, which walks every method of the name: whoever asks about many methods of one selector works it out once.
bool canHaveTableEntrypoint(const Program&, TypeArena&, FunctionID method, std::span<const TypeID> given);
// The layouts whose objects find the method under the name as the classes are defined. The entrypoint takes `this` to be of one of them, so the table has it in the rows of no other.
LayoutList receiversOfTableEntrypoint(const Program&, PropertyKeyID, FunctionID method);

MethodSelector selectorOfTableFunction(const Function& lookupOrEntrypoint);
MethodSelector selectorOf(const Program&, const Value& tableCall);
Vector<TypeID, 4> parameterTypesOf(const Program&, TypeArena&, MethodSelector);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
