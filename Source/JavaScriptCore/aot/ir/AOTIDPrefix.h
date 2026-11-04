/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include <optional>
#include <wtf/Assertions.h>

namespace JSC { namespace AOT { namespace Typed {

enum class IDPrefix : uint8_t { Value, Block, Function, Layout, Variable, Module, ModuleFailure, TypeSite, EvalScope, ExpectedResult, Data };

constexpr std::optional<IDPrefix> idPrefixOf(uint8_t byte)
{
    switch (byte) {
    case 'v':
        return IDPrefix::Value;
    case 'b':
        return IDPrefix::Block;
    case 'f':
        return IDPrefix::Function;
    case 'L':
        return IDPrefix::Layout;
    case 'g':
        return IDPrefix::Variable;
    case 'm':
        return IDPrefix::Module;
    case 'q':
        return IDPrefix::ModuleFailure;
    case 'w':
        return IDPrefix::TypeSite;
    case 'e':
        return IDPrefix::EvalScope;
    case 'x':
        return IDPrefix::ExpectedResult;
    case 'd':
        return IDPrefix::Data;
    default:
        return std::nullopt;
    }
}

constexpr char letterOf(IDPrefix prefix)
{
    switch (prefix) {
    case IDPrefix::Value:
        return 'v';
    case IDPrefix::Block:
        return 'b';
    case IDPrefix::Function:
        return 'f';
    case IDPrefix::Layout:
        return 'L';
    case IDPrefix::Variable:
        return 'g';
    case IDPrefix::Module:
        return 'm';
    case IDPrefix::ModuleFailure:
        return 'q';
    case IDPrefix::TypeSite:
        return 'w';
    case IDPrefix::EvalScope:
        return 'e';
    case IDPrefix::ExpectedResult:
        return 'x';
    case IDPrefix::Data:
        return 'd';
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static_assert(prefixLetterOf(ProgramList::Functions) == letterOf(IDPrefix::Function));
static_assert(prefixLetterOf(ProgramList::Layouts) == letterOf(IDPrefix::Layout));
static_assert(prefixLetterOf(ProgramList::Variables) == letterOf(IDPrefix::Variable));
static_assert(prefixLetterOf(ProgramList::Modules) == letterOf(IDPrefix::Module));
static_assert(prefixLetterOf(ProgramList::ModuleFailures) == letterOf(IDPrefix::ModuleFailure));
static_assert(prefixLetterOf(ProgramList::TypeSites) == letterOf(IDPrefix::TypeSite));
static_assert(prefixLetterOf(ProgramList::EvalScopes) == letterOf(IDPrefix::EvalScope));
static_assert(prefixLetterOf(ProgramList::ExpectedResults) == letterOf(IDPrefix::ExpectedResult));
static_assert(prefixLetterOf(ProgramList::Data) == letterOf(IDPrefix::Data));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
