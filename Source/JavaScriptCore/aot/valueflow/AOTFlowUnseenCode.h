/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTSchemaRecords.h"

namespace JSC { namespace AOT { namespace Typed {

struct RefusedCode {
    bool evaluatedText { false };
    bool hostEvaluatedText { false };
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
    case UnseenCodeCause::FlowResultDisabled:
    case UnseenCodeCause::RunsMachineCode:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr bool admitsScripts(UnseenCodeCause cause)
{
    return admitsUnknownJavaScript(cause) && cause != UnseenCodeCause::EvaluatesText;
}

constexpr bool opensWholeProgram(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::OptionAdmitsCode;
}

constexpr bool loadsCodeOutsideImage(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::LoadsCodeByName;
}

constexpr bool requiresLibraryModules(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::ImportsUncompiledModule || cause == UnseenCodeCause::LoadsCodeByName;
}

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
