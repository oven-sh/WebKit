/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTSchemaRecords.h"

namespace JSC { namespace AOT { namespace Typed {

constexpr bool admitsUnknownJavaScript(UnseenCodeCause cause)
{
    switch (cause) {
    case UnseenCodeCause::EvaluatesText:
    case UnseenCodeCause::ImportsUncompiledModule:
    case UnseenCodeCause::RunsUnseenCode:
    case UnseenCodeCause::OptionAdmitsCode:
        return true;
    case UnseenCodeCause::Unknown:
    case UnseenCodeCause::LoadsModuleByName:
    case UnseenCodeCause::LimitReached:
    case UnseenCodeCause::FlowResultDisabled:
    case UnseenCodeCause::RunsMachineCode:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// Text that eval or Function is given runs in the global scope, and what it declares with let, const or class stays its own. It gets at the host's ways of running a script only through functions
// of the library that the image holds for a call of the program's own, and such a call has a record of its own.
constexpr bool admitsScripts(UnseenCodeCause cause)
{
    return admitsUnknownJavaScript(cause) && cause != UnseenCodeCause::EvaluatesText;
}

// Unseen code opens what it can reach. Only the option that lets any code in, at any place, opens the whole program.
constexpr bool opensWholeProgram(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::OptionAdmitsCode;
}

// A module that stays outside the image is unseen code with a loader of its own. It names the modules of the library, and gets the one instance that the image holds. The run time refuses it the files of the image.
constexpr bool requiresLibraryModules(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::ImportsUncompiledModule;
}

// The text of a function or a module of the library requires modules of the library too.
constexpr bool admitsLibraryText(UnseenCodeCause cause)
{
    return requiresLibraryModules(cause) || opensWholeProgram(cause);
}

constexpr bool loadsModulesByName(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::LoadsModuleByName || opensWholeProgram(cause);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
