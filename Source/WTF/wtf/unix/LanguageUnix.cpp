/*
 * Copyright (C) 2007 Alp Toker <alp@atoker.com>
 * Copyright (C) 2016, 2017 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include <wtf/Language.h>

#include <locale.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WTF {

// Using pango_language_get_default() here is not an option, because
// it doesn't support changing the locale in runtime, so it returns
// always the same value.
static String platformLanguage()
{
    // language[_territory][.codeset][@modifier]: only language and territory name a locale. Strip the
    // rest first so "C.UTF-8" (glibc, and bionic's default) is recognized as the C locale below.
    auto localeDefault = String::fromLatin1(setlocale(LC_CTYPE, nullptr));
    if (auto suffix = localeDefault.find([](UChar c) { return c == '.' || c == '@'; }); suffix != notFound)
        localeDefault = localeDefault.left(suffix);
    if (localeDefault.isEmpty() || equalIgnoringASCIICase(localeDefault, "C"_s) || equalIgnoringASCIICase(localeDefault, "POSIX"_s))
        return "en-US"_s;

    return makeStringByReplacingAll(localeDefault, '_', '-');
}

Vector<String> platformUserPreferredLanguages(ShouldMinimizeLanguages)
{
    return { platformLanguage() };
}

} // namespace WTF
