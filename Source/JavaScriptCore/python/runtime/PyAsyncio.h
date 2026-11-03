/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyObject.h"

namespace JSC {
namespace Python {

// For `await future`, with a future of _asyncio: the value of the expression need not travel in a StopIteration.

// Whether `next`, the __next__ of the type of an iterator, says that the iterator is one of those that such futures make.
bool isNextOfFutureIter(JSValue next);
// next(iterator) then: the future, which is still pending. Or empty, with the result of the future in `result`; or, with that empty
// too, an exception.
JSValue stepFutureIter(JSGlobalObject*, JSValue iterator, JSValue& result);

} // namespace Python
} // namespace JSC
