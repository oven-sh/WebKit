/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTSchemaRecords.h"

namespace JSC { namespace AOT { namespace Typed {

// What the build excludes and the run time therefore refuses, whoever asks for it.
struct RefusedCode {
    // eval, Function and its relatives are refused every text, so a call of one throws and runs nothing.
    bool evaluatedText { false };
    // A load finds the modules of the image and data. A file with code that is not in the image is refused.
    bool codeOutsideImage { false };
};

constexpr bool admitsUnknownJavaScript(UnseenCodeCause cause)
{
    switch (cause) {
    case UnseenCodeCause::EvaluatesText:
    case UnseenCodeCause::ImportsUncompiledModule:
    case UnseenCodeCause::RunsUnseenCode:
    case UnseenCodeCause::OptionAdmitsCode:
    case UnseenCodeCause::LoadsCodeByName:
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

// Text that eval or Function is given runs in the global scope, and what it declares with let, const or class stays its own.
constexpr bool admitsScripts(UnseenCodeCause cause)
{
    return admitsUnknownJavaScript(cause) && cause != UnseenCodeCause::EvaluatesText;
}

// Unseen code opens what it can reach. Only the option that lets any code in, at any place, opens the whole program.
constexpr bool opensWholeProgram(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::OptionAdmitsCode;
}

// A name that the build does not know may be the path of a file that the build has not seen. The run time loads the file and interprets it.
constexpr bool loadsCodeOutsideImage(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::LoadsCodeByName;
}

// A module that stays outside the image is unseen code with a loader of its own. It names the modules of the library, and gets the one instance that the image holds. The run time refuses it the files of the image.
constexpr bool requiresLibraryModules(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::ImportsUncompiledModule || cause == UnseenCodeCause::LoadsCodeByName;
}

// The text of a function or a module of the library requires modules of the library too.
constexpr bool admitsLibraryText(UnseenCodeCause cause)
{
    return requiresLibraryModules(cause) || opensWholeProgram(cause);
}

constexpr bool loadsModulesByName(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::LoadsModuleByName || loadsCodeOutsideImage(cause) || opensWholeProgram(cause);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
