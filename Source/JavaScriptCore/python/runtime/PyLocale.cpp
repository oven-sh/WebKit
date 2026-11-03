/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdlib.h>
#include <wchar.h>
#include <wtf/text/StringBuilder.h>

#if OS(LINUX) && !OS(ANDROID)
#define PY_LOCALE_HAS_GETTEXT 1
#include <libintl.h>
#else
#define PY_LOCALE_HAS_GETTEXT 0
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _locale, which Lib/locale.py is built on: the locale of the C library. (Modules/_localemodule.c in CPython.)

static JSValue localeRaiseError(JSGlobalObject* globalObject, ASCIILiteral message)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue type = posixState(globalObject)->get(internString(vm, "locale.Error"_s));
    return raise(globalObject, scope, PyType::from(type), message);
}

// ---- Text, to and from the C library ----

// What the library says, which is in the encoding of LC_CTYPE.
static JSValue localeDecode(JSGlobalObject* globalObject, const char* text)
{
    size_t length = strlen(text);
    std::span<const char> bytes { text, length };
    bool isASCII = true;
    for (char byte : bytes)
        isASCII &= !(byte & 0x80);
    if (isASCII)
        return makeStr(globalObject, String(byteCast<Latin1Character>(bytes)));
    size_t count = mbstowcs(nullptr, text, 0);
    if (count == static_cast<size_t>(-1))
        return posixDecode(globalObject, bytes);
    Vector<wchar_t, 64> wide(count + 1);
    mbstowcs(wide.mutableSpan().data(), text, wide.size());
    StrBuilder builder;
    for (size_t i = 0; i < count; ++i)
        appendCodePoint(builder, static_cast<char32_t>(wide[i]));
    return makeStr(globalObject, builder);
}

// An argument that the library takes as a C string: a str, or None (a null pointer) if `allowNone`. `position` is 0 for
// the only argument of the function.
struct LocaleText {
    PosixBytes bytes;
    bool isNull { true };
    const char* characters() const { return isNull ? nullptr : bytes.characters(); }
};

static bool localeToText(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, unsigned position, bool allowNone, LocaleText& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    result.isNull = true;
    if (allowNone && (!value || isNone(value)))
        return true;
    if (!isStr(value)) {
        String argument = position ? makeString("argument "_s, position) : "argument"_str;
        raiseTypeError(globalObject, scope, function, "() "_s, argument, " must be "_s, allowNone ? "str or None"_s : "str"_s, ", not "_s, posixTypeNameOfArgument(globalObject, value));
        return false;
    }
    JSValue encoded = strEncode(globalObject, strValue(value), nullptr, nullptr);
    RETURN_IF_EXCEPTION(scope, false);
    result.bytes.assign(byteCast<char>(PyBytes::from(encoded)->span()));
    if (result.bytes.contains('\0')) {
        raiseValueError(globalObject, scope, "embedded null character"_s);
        return false;
    }
    result.isNull = false;
    return true;
}

// False: a TypeError, "f() argument 1 must be str, not x".
static bool localeRequireStr(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, unsigned position)
{
    if (isStr(value))
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String argument = position ? makeString("argument "_s, position) : "argument"_str;
    raiseTypeError(globalObject, scope, function, "() "_s, argument, " must be str, not "_s, posixTypeNameOfArgument(globalObject, value));
    return false;
}

// A str as wide characters, with a NUL after them.
static bool localeToWide(JSGlobalObject* globalObject, JSValue value, Vector<wchar_t, 64>& result)
{
    static_assert(sizeof(wchar_t) == 4);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = strValue(value)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    for (char32_t character : StringView(text).codePoints()) {
        if (!character) {
            raiseValueError(globalObject, scope, "embedded null character"_s);
            return false;
        }
        result.append(static_cast<wchar_t>(character));
    }
    result.append(L'\0');
    return true;
}

// While one lives, LC_CTYPE is the locale of another category, so that text which belongs to that one can be decoded.
class LocaleCtypeSwitch {
    WTF_MAKE_NONCOPYABLE(LocaleCtypeSwitch);
public:
    explicit LocaleCtypeSwitch(int category, bool isNeeded = true)
    {
        if (!isNeeded)
            return;
        const char* current = setlocale(LC_CTYPE, nullptr);
        if (!current)
            return;
        PosixBytes saved;
        saved.assign({ current, strlen(current) });
        const char* other = setlocale(category, nullptr);
        if (!other || !strcmp(other, saved.characters()))
            return;
        m_saved = WTF::move(saved);
        m_isSwitched = true;
        setlocale(LC_CTYPE, other);
    }

    ~LocaleCtypeSwitch()
    {
        if (m_isSwitched)
            setlocale(LC_CTYPE, m_saved.characters());
    }

private:
    PosixBytes m_saved;
    bool m_isSwitched { false };
};

static bool localeIsAllASCII(const char* text)
{
    for (; *text; ++text) {
        if (*text & 0x80)
            return false;
    }
    return true;
}

// ---- The functions ----

static JSValue localeSetlocale(JSGlobalObject* globalObject, JSValue categoryValue, JSValue localeValue)
{
    int category;
    LocaleText locale;
    if (!posixToInt(globalObject, categoryValue, category) || !localeToText(globalObject, localeValue, "setlocale"_s, 2, true, locale))
        return { };
    const char* result = setlocale(category, locale.characters());
    if (!result)
        return localeRaiseError(globalObject, locale.isNull ? "locale query failed"_s : "unsupported locale setting"_s);
    return localeDecode(globalObject, result);
}

// How digits are grouped: the sizes of the groups from the right, up to a 0 (the last size goes on) or CHAR_MAX (no more groups).
static JSValue localeCopyGrouping(JSGlobalObject* globalObject, const char* grouping)
{
    JSArray* result = newList(globalObject);
    if (!grouping[0])
        return result;
    for (size_t i = 0;; ++i) {
        listAppend(globalObject, result, jsNumber(grouping[i]));
        if (!grouping[i] || grouping[i] == CHAR_MAX)
            break;
    }
    return result;
}

static JSValue localeLocaleconv(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    PyDict* result = PyDict::create(globalObject);
    struct lconv* conventions = localeconv();
    auto set = [&](ASCIILiteral key, JSValue value) { result->set(vm, internString(vm, key), value); };
    auto isOneASCIICharacter = [](const char* text) { return strlen(text) == 1 && !(text[0] & 0x80); };
    {
        LocaleCtypeSwitch ctype(LC_MONETARY, !isOneASCIICharacter(conventions->int_curr_symbol) || !isOneASCIICharacter(conventions->currency_symbol)
            || !isOneASCIICharacter(conventions->mon_decimal_point) || !isOneASCIICharacter(conventions->mon_thousands_sep));
        set("int_curr_symbol"_s, localeDecode(globalObject, conventions->int_curr_symbol));
        set("currency_symbol"_s, localeDecode(globalObject, conventions->currency_symbol));
        set("mon_decimal_point"_s, localeDecode(globalObject, conventions->mon_decimal_point));
        set("mon_thousands_sep"_s, localeDecode(globalObject, conventions->mon_thousands_sep));
    }
    set("mon_grouping"_s, localeCopyGrouping(globalObject, conventions->mon_grouping));
    set("positive_sign"_s, localeDecode(globalObject, conventions->positive_sign));
    set("negative_sign"_s, localeDecode(globalObject, conventions->negative_sign));
    set("int_frac_digits"_s, jsNumber(conventions->int_frac_digits));
    set("frac_digits"_s, jsNumber(conventions->frac_digits));
    set("p_cs_precedes"_s, jsNumber(conventions->p_cs_precedes));
    set("p_sep_by_space"_s, jsNumber(conventions->p_sep_by_space));
    set("n_cs_precedes"_s, jsNumber(conventions->n_cs_precedes));
    set("n_sep_by_space"_s, jsNumber(conventions->n_sep_by_space));
    set("p_sign_posn"_s, jsNumber(conventions->p_sign_posn));
    set("n_sign_posn"_s, jsNumber(conventions->n_sign_posn));
    {
        LocaleCtypeSwitch ctype(LC_NUMERIC, strlen(conventions->decimal_point) > 1 || !localeIsAllASCII(conventions->decimal_point)
            || strlen(conventions->thousands_sep) > 1 || !localeIsAllASCII(conventions->thousands_sep));
        set("decimal_point"_s, localeDecode(globalObject, conventions->decimal_point));
        set("thousands_sep"_s, localeDecode(globalObject, conventions->thousands_sep));
    }
    set("grouping"_s, localeCopyGrouping(globalObject, conventions->grouping));
    return result;
}

static JSValue localeStrcoll(JSGlobalObject* globalObject, JSValue first, JSValue second)
{
    Vector<wchar_t, 64> wideFirst;
    Vector<wchar_t, 64> wideSecond;
    if (!localeRequireStr(globalObject, first, "strcoll"_s, 1) || !localeRequireStr(globalObject, second, "strcoll"_s, 2)
        || !localeToWide(globalObject, first, wideFirst) || !localeToWide(globalObject, second, wideSecond))
        return { };
    return jsNumber(wcscoll(wideFirst.span().data(), wideSecond.span().data()));
}

static JSValue localeStrxfrm(JSGlobalObject* globalObject, JSValue string)
{
    Vector<wchar_t, 64> wide;
    if (!localeRequireStr(globalObject, string, "strxfrm"_s, 0) || !localeToWide(globalObject, string, wide))
        return { };
    wchar_t none[1];
    errno = 0;
    size_t length = wcsxfrm(none, wide.span().data(), 1);
    if (errno && errno != ERANGE)
        return posixError(globalObject);
    Vector<wchar_t, 64> buffer(length + 1);
    errno = 0;
    length = wcsxfrm(buffer.mutableSpan().data(), wide.span().data(), buffer.size());
    if (errno)
        return posixError(globalObject);
    // The result is numbers to compare, not characters: one that is beyond 16 bits becomes two that keep the order.
    StrBuilder builder;
    for (size_t i = 0; i < length; ++i) {
        uint32_t number = static_cast<uint32_t>(buffer[i]);
        if (number > 0x10000) {
            appendCodePoint(builder, (number >> 16) | 0x10000);
            appendCodePoint(builder, number & 0xffff);
        } else
            appendCodePoint(builder, number);
    }
    return makeStr(globalObject, builder);
}

struct LocaleLanginfoConstant {
    ASCIILiteral name;
    nl_item value;
    int category;
};

#define LOCALE_LANGINFO(name, category) { #name ""_s, name, category }

static constexpr LocaleLanginfoConstant localeLanginfoConstants[] = {
    LOCALE_LANGINFO(DAY_1, LC_TIME), LOCALE_LANGINFO(DAY_2, LC_TIME), LOCALE_LANGINFO(DAY_3, LC_TIME), LOCALE_LANGINFO(DAY_4, LC_TIME),
    LOCALE_LANGINFO(DAY_5, LC_TIME), LOCALE_LANGINFO(DAY_6, LC_TIME), LOCALE_LANGINFO(DAY_7, LC_TIME),
    LOCALE_LANGINFO(ABDAY_1, LC_TIME), LOCALE_LANGINFO(ABDAY_2, LC_TIME), LOCALE_LANGINFO(ABDAY_3, LC_TIME), LOCALE_LANGINFO(ABDAY_4, LC_TIME),
    LOCALE_LANGINFO(ABDAY_5, LC_TIME), LOCALE_LANGINFO(ABDAY_6, LC_TIME), LOCALE_LANGINFO(ABDAY_7, LC_TIME),
    LOCALE_LANGINFO(MON_1, LC_TIME), LOCALE_LANGINFO(MON_2, LC_TIME), LOCALE_LANGINFO(MON_3, LC_TIME), LOCALE_LANGINFO(MON_4, LC_TIME),
    LOCALE_LANGINFO(MON_5, LC_TIME), LOCALE_LANGINFO(MON_6, LC_TIME), LOCALE_LANGINFO(MON_7, LC_TIME), LOCALE_LANGINFO(MON_8, LC_TIME),
    LOCALE_LANGINFO(MON_9, LC_TIME), LOCALE_LANGINFO(MON_10, LC_TIME), LOCALE_LANGINFO(MON_11, LC_TIME), LOCALE_LANGINFO(MON_12, LC_TIME),
    LOCALE_LANGINFO(ABMON_1, LC_TIME), LOCALE_LANGINFO(ABMON_2, LC_TIME), LOCALE_LANGINFO(ABMON_3, LC_TIME), LOCALE_LANGINFO(ABMON_4, LC_TIME),
    LOCALE_LANGINFO(ABMON_5, LC_TIME), LOCALE_LANGINFO(ABMON_6, LC_TIME), LOCALE_LANGINFO(ABMON_7, LC_TIME), LOCALE_LANGINFO(ABMON_8, LC_TIME),
    LOCALE_LANGINFO(ABMON_9, LC_TIME), LOCALE_LANGINFO(ABMON_10, LC_TIME), LOCALE_LANGINFO(ABMON_11, LC_TIME), LOCALE_LANGINFO(ABMON_12, LC_TIME),
    LOCALE_LANGINFO(RADIXCHAR, LC_NUMERIC), LOCALE_LANGINFO(THOUSEP, LC_NUMERIC), LOCALE_LANGINFO(CRNCYSTR, LC_MONETARY),
    LOCALE_LANGINFO(D_T_FMT, LC_TIME), LOCALE_LANGINFO(D_FMT, LC_TIME), LOCALE_LANGINFO(T_FMT, LC_TIME),
    LOCALE_LANGINFO(AM_STR, LC_TIME), LOCALE_LANGINFO(PM_STR, LC_TIME),
    LOCALE_LANGINFO(CODESET, LC_CTYPE), LOCALE_LANGINFO(T_FMT_AMPM, LC_TIME),
    LOCALE_LANGINFO(ERA, LC_TIME), LOCALE_LANGINFO(ERA_D_FMT, LC_TIME), LOCALE_LANGINFO(ERA_D_T_FMT, LC_TIME), LOCALE_LANGINFO(ERA_T_FMT, LC_TIME),
    LOCALE_LANGINFO(ALT_DIGITS, LC_TIME), LOCALE_LANGINFO(YESEXPR, LC_MESSAGES), LOCALE_LANGINFO(NOEXPR, LC_MESSAGES),
#ifdef _DATE_FMT
    LOCALE_LANGINFO(_DATE_FMT, LC_TIME),
#endif
};

#undef LOCALE_LANGINFO

#if defined(__GLIBC__)
// There, the items of ERA and ALT_DIGITS have a NUL between them, where POSIX would have a semicolon.
static JSValue localeDecodeStrings(JSGlobalObject* globalObject, const char* strings, size_t maxCount)
{
    Vector<char, 256> joined;
    size_t position = 0;
    for (size_t count = 0; count < maxCount && strings[position]; ++count) {
        size_t length = strlen(strings + position);
        if (count)
            joined.append(';');
        joined.append(std::span<const char> { strings + position, length });
        position += length + 1;
    }
    joined.append('\0');
    return localeDecode(globalObject, joined.span().data());
}
#endif

static JSValue localeNlLanginfo(JSGlobalObject* globalObject, JSValue keyValue)
{
    int key;
    if (!posixToInt(globalObject, keyValue, key))
        return { };
    // For other numbers the library may answer with something that is not a string.
    for (auto& constant : localeLanginfoConstants) {
        if (constant.value != key)
            continue;
        const char* result = nl_langinfo(constant.value);
        if (!result)
            result = "";
        bool isList = false;
#if defined(__GLIBC__)
        isList = key == ALT_DIGITS || key == ERA;
#endif
        LocaleCtypeSwitch ctype(constant.category, constant.category != LC_CTYPE && *result && (isList || !localeIsAllASCII(result)));
#if defined(__GLIBC__)
        if (isList && *result)
            return localeDecodeStrings(globalObject, result, key == ALT_DIGITS ? 100 : std::numeric_limits<size_t>::max());
#endif
        return localeDecode(globalObject, result);
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseValueError(globalObject, scope, "unsupported langinfo constant"_s);
}

static JSValue localeGetencoding(JSGlobalObject* globalObject)
{
    const char* encoding = nl_langinfo(CODESET);
    if (!encoding || !encoding[0])
        return makeStr(getVM(globalObject), "utf-8"_s);
    return localeDecode(globalObject, encoding);
}

#if PY_LOCALE_HAS_GETTEXT
static JSValue localeGettext(JSGlobalObject* globalObject, JSValue messageValue)
{
    LocaleText message;
    if (!localeToText(globalObject, messageValue, "gettext"_s, 0, false, message))
        return { };
    return localeDecode(globalObject, gettext(message.characters()));
}

static JSValue localeDgettext(JSGlobalObject* globalObject, JSValue domainValue, JSValue messageValue)
{
    LocaleText domain;
    LocaleText message;
    if (!localeToText(globalObject, domainValue, "dgettext"_s, 1, true, domain) || !localeToText(globalObject, messageValue, "dgettext"_s, 2, false, message))
        return { };
    return localeDecode(globalObject, dgettext(domain.characters(), message.characters()));
}

static JSValue localeDcgettext(JSGlobalObject* globalObject, JSValue domainValue, JSValue messageValue, JSValue categoryValue)
{
    LocaleText domain;
    LocaleText message;
    int category;
    if (!localeToText(globalObject, domainValue, "dcgettext"_s, 1, true, domain) || !localeToText(globalObject, messageValue, "dcgettext"_s, 2, false, message)
        || !posixToInt(globalObject, categoryValue, category))
        return { };
    return localeDecode(globalObject, dcgettext(domain.characters(), message.characters(), category));
}

static JSValue localeTextdomain(JSGlobalObject* globalObject, JSValue domainValue)
{
    LocaleText domain;
    if (!localeToText(globalObject, domainValue, "textdomain"_s, 0, true, domain))
        return { };
    const char* result = textdomain(domain.characters());
    if (!result)
        return posixError(globalObject);
    return localeDecode(globalObject, result);
}

static JSValue localeBindtextdomain(JSGlobalObject* globalObject, JSValue domainValue, JSValue directoryValue)
{
    LocaleText domain;
    if (!localeToText(globalObject, domainValue, "bindtextdomain"_s, 1, false, domain))
        return { };
    if (!domain.bytes.length())
        return localeRaiseError(globalObject, "domain must be a non-empty string"_s);
    PosixBytes directory;
    bool hasDirectory = !isNone(directoryValue);
    if (hasDirectory && !posixToFSBytes(globalObject, directoryValue, directory))
        return { };
    const char* result = bindtextdomain(domain.characters(), hasDirectory ? directory.characters() : nullptr);
    if (!result)
        return posixError(globalObject);
    return localeDecode(globalObject, result);
}

static JSValue localeBindTextdomainCodeset(JSGlobalObject* globalObject, JSValue domainValue, JSValue codesetValue)
{
    LocaleText domain;
    LocaleText codeset;
    if (!localeToText(globalObject, domainValue, "bind_textdomain_codeset"_s, 1, false, domain) || !localeToText(globalObject, codesetValue, "bind_textdomain_codeset"_s, 2, true, codeset))
        return { };
    const char* result = bind_textdomain_codeset(domain.characters(), codeset.characters());
    if (!result)
        return jsUndefined();
    return localeDecode(globalObject, result);
}
#endif // PY_LOCALE_HAS_GETTEXT

static constexpr NativeDef localeFunctions[] = {
    PY_FUNCTION("setlocale", localeSetlocale, "(category, locale=None, /)"),
    PY_FUNCTION("localeconv", localeLocaleconv, "()"),
    PY_FUNCTION("strcoll", localeStrcoll, "(os1, os2, /)"),
    PY_FUNCTION("strxfrm", localeStrxfrm, "(string, /)"),
    PY_FUNCTION("nl_langinfo", localeNlLanginfo, "(key, /)"),
    PY_FUNCTION("getencoding", localeGetencoding, "()"),
#if PY_LOCALE_HAS_GETTEXT
    PY_FUNCTION("gettext", localeGettext, "(msg, /)"),
    PY_FUNCTION("dgettext", localeDgettext, "(domain, msg, /)"),
    PY_FUNCTION("dcgettext", localeDcgettext, "(domain, msg, category, /)"),
    PY_FUNCTION("textdomain", localeTextdomain, "(domain, /)"),
    PY_FUNCTION("bindtextdomain", localeBindtextdomain, "(domain, dir, /)"),
    PY_FUNCTION("bind_textdomain_codeset", localeBindTextdomainCodeset, "(domain, codeset, /)"),
#endif
};

static constexpr PosixConstant localeConstants[] = {
    { "LC_CTYPE"_s, LC_CTYPE },
    { "LC_TIME"_s, LC_TIME },
    { "LC_COLLATE"_s, LC_COLLATE },
    { "LC_MONETARY"_s, LC_MONETARY },
    { "LC_MESSAGES"_s, LC_MESSAGES },
    { "LC_NUMERIC"_s, LC_NUMERIC },
    { "LC_ALL"_s, LC_ALL },
    { "CHAR_MAX"_s, CHAR_MAX },
};

// What python does as it starts: LC_CTYPE, and nothing else, is as the environment has it; and if that is the C locale, which
// knows only ASCII, a locale that differs from it in nothing but the encoding is used instead (PEP 538).
static void localeSetCtypeFromEnvironment()
{
    static bool isDone = false;
    if (std::exchange(isDone, true))
        return;
    const char* current = setlocale(LC_CTYPE, "");
    if (!current || strcmp(current, "C"))
        return;
    const char* override = getenv("LC_ALL");
    if (override && *override)
        return;
    for (const char* name : { "C.UTF-8", "C.utf8", "UTF-8" }) {
        if (setlocale(LC_CTYPE, name))
            return;
    }
}

void initializeLocaleModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    localeSetCtypeFromEnvironment();
    module->addFunctions(globalObject, localeFunctions);
    addPosixConstants(globalObject, module, localeConstants);
    for (auto& constant : localeLanginfoConstants)
        module->add(globalObject, constant.name, jsNumber(static_cast<int32_t>(constant.value)));

    // class Error(Exception), of the module locale.
    PyDict* namespaceDict = PyDict::create(globalObject);
    namespaceDict->set(vm, rt.names.dModule(), makeStr(vm, "locale"_s));
    JSValue error = call(globalObject, rt.typeType(), makeStr(vm, "Error"_s), PyTuple::create(globalObject, { JSValue(rt.Exception()) }), namespaceDict);
    if (!error)
        return;
    posixState(globalObject)->set(vm, internString(vm, "locale.Error"_s), error);
    module->add(globalObject, "Error"_s, error);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
