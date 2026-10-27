/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayBufferViewBuiltinEmitter.h"
#include "AOTArrayBuiltinEmitter.h"
#include "AOTArrayStorageBuiltinEmitter.h"
#include "AOTBufferBuiltinEmitter.h"
#include "AOTDateBuiltinEmitter.h"
#include "AOTErrorBuiltinEmitter.h"
#include "AOTFunctionBuiltinEmitter.h"
#include "AOTHashTableBuiltinEmitter.h"
#include "AOTIntlBuiltinEmitter.h"
#include "AOTMapSetBuiltinEmitter.h"
#include "AOTMathBuiltinEmitter.h"
#include "AOTObjectBuiltinEmitter.h"
#include "AOTOwnPropertyBuiltinEmitter.h"
#include "AOTPromiseBuiltinEmitter.h"
#include "AOTRegExpBuiltinEmitter.h"
#include "AOTStringBuiltinEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

void InlineBuiltinEmitter::generate(const InlineCall& call)
{
    switch (call.builtin()) {
#include "AOTSchemaInlineBuiltinDispatch.inc"
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
