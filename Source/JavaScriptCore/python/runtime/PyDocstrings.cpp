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

static const Docstring* findDocstring(const char* wanted)
{
    auto* end = std::end(docstrings);
    auto* found = std::lower_bound(std::begin(docstrings), end, wanted, [](const Docstring& entry, const char* wanted) {
        return strcmp(entry.key, wanted) < 0;
    });
    return found == end || strcmp(found->key, wanted) ? nullptr : found;
}

JSValue docstringFor(JSGlobalObject* globalObject, const String& key)
{
    auto utf8 = key.utf8();
    auto* found = findDocstring(reinterpret_cast<const char*>(utf8.data()));
    if (!found)
        return jsUndefined();
    return makeStr(getVM(globalObject), String::fromUTF8(unsafeSpan(found->text)));
}

// Whether there is an entry for "OWNER.MEMBER". Every built-in type asks several times as it is made, so this allocates nothing.
bool hasDocstring(ASCIILiteral owner, ASCIILiteral member)
{
    char key[128];
    if (owner.length() + member.length() + 2 > sizeof(key))
        return false;
    memcpy(key, owner.characters(), owner.length());
    key[owner.length()] = '.';
    memcpy(key + owner.length() + 1, member.characters(), member.length() + 1);
    return findDocstring(key);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
