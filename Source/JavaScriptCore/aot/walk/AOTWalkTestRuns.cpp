/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A test run compiles what an executable compiles.
// At a suspension, at a throw: nothing. A constant stands in the place of a read that finds the same object in every run and runs no code.
// Withheld (Folding): the read is copied in its closed form.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTTypeArena.h"
#include "AOTWalkTestRuns.h"

namespace JSC { namespace AOT { namespace Typed {

bool buildBuiltinThatExecutableNamesOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    Reference object = walker.referenceOf(read.objectOperand());
    TypeID typeOfObject = object ? walker.typeOf(object) : TypeID();
    if (!typeOfObject || !read.key())
        return false;
    Type type = walker.types().at(typeOfObject);
    BuiltinID holder = type.isObject() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    BuiltinID location = holder ? findConstantMemberFrom(program, walker.facts(), holder, read.key()) : BuiltinID();
    if (!location)
        return false;
    // A member that any program may replace is a read in an executable too.
    bool isLockedInExecutable = table.at(location).mutability == BuiltinMutability::AlwaysLocked || table.isLocationOfLockedObject(location) || program.isLockLeftToProgram(location);
    if (!isLockedInExecutable || !isObjectKind(table.at(table.canonicalID(location)).kind))
        return false;
    return buildReadOfBuiltinMember(walker, read, typeOfObject, object) == TypedForm::Built;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
