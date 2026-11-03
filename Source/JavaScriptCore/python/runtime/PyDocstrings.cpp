/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"

#include "JSCInlines.h"
#include "PyStr.h"
#include <cstring>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The docstrings of what is written in C++ here and in C in CPython. They are CPython's, as data: tools/make_docstrings.py says
// what the keys are.

namespace {

struct Docstring {
    const char* key;
    const char* text;
};

constexpr Docstring docstrings[] = {
#include "PyDocstrings.inc"
};

}

JSValue docstringFor(JSGlobalObject* globalObject, const String& key)
{
    auto utf8 = key.utf8();
    const char* wanted = reinterpret_cast<const char*>(utf8.data());
    auto* end = std::end(docstrings);
    auto* found = std::lower_bound(std::begin(docstrings), end, wanted, [](const Docstring& entry, const char* wanted) {
        return strcmp(entry.key, wanted) < 0;
    });
    if (found == end || strcmp(found->key, wanted))
        return jsUndefined();
    return makeStr(getVM(globalObject), String::fromUTF8(unsafeSpan(found->text)));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
