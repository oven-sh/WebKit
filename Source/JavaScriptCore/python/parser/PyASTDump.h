/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyAST.h"
#include "PyParserHost.h"

namespace JSC {
namespace Python {

// For testing the parser against CPython's: see tools/dump_ast_reference.py.
JS_EXPORT_PRIVATE String dumpAST(AST::Mod*);
JS_EXPORT_PRIVATE String dumpSyntaxError(const SyntaxErrorInfo&);

} // namespace Python
} // namespace JSC
