/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyModuleHelpers.h"
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _csv. (Modules/_csv.c in CPython is the reference.)

enum CsvQuoting : int { CsvQuoteMinimal, CsvQuoteAll, CsvQuoteNonNumeric, CsvQuoteNone, CsvQuoteStrings, CsvQuoteNotNull };

static constexpr char32_t csvNotSet = 0xffffffff;
static constexpr char32_t csvEndOfLine = 0xfffffffe;

static constexpr TypeDef csvErrorTypeDef { "_csv.Error"_s, { TypeFlag::Base, TypeFlag::Mutable }, { }, { }, { } };

static PyType* csvError(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return rt.typeFor(csvErrorTypeDef, rt.Exception());
}

template<typename... Parts>
static NEVER_INLINE JSValue csvRaise(JSGlobalObject* globalObject, Parts... parts)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, csvError(globalObject), makeString(parts...));
}

// What the module keeps: in the dict of a type that programs never get to see.
static constexpr TypeDef csvStateTypeDef { "_csv._state"_s, { TypeFlag::DisallowInstantiation, TypeFlag::Internal }, { }, { }, { } };

static PyDict* csvState(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(csvStateTypeDef)->dict(); }

static PyDict* csvDialects(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    PyDict* state = csvState(globalObject);
    JSString* key = internString(vm, "dialects"_s);
    if (JSValue dialects = state->get(key))
        return PyDict::from(dialects);
    PyDict* dialects = PyDict::create(globalObject);
    state->set(vm, key, dialects);
    return dialects;
}

static int64_t csvFieldLimit(JSGlobalObject* globalObject)
{
    JSValue limit = csvState(globalObject)->get(internString(getVM(globalObject), "field_limit"_s));
    return limit ? tryInt64(limit).value_or(0) : 128 * 1024;
}

static bool csvContains(StringView text, char32_t character)
{
    if (character <= 0xffff)
        return text.find(static_cast<char16_t>(character)) != notFound;
    if (character > 0x10ffff || text.is8Bit())
        return false;
    char16_t pair[2] = { U16_LEAD(character), U16_TRAIL(character) };
    return text.find(StringView(std::span<const char16_t> { pair, 2 })) != notFound;
}

// Calls the function with each code point; a surrogate on its own is one. False: the function returned false.
template<typename Function>
static ALWAYS_INLINE bool csvForEachCodePoint(StringView text, const Function& function)
{
    if (text.is8Bit()) {
        for (Latin1Character character : text.span8()) {
            if (!function(static_cast<char32_t>(character)))
                return false;
        }
        return true;
    }
    auto units = text.span16();
    for (size_t i = 0; i < units.size(); ++i) {
        char32_t character = units[i];
        if (U16_IS_LEAD(character) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1]))
            character = U16_GET_SUPPLEMENTARY(character, units[++i]);
        if (!function(character))
            return false;
    }
    return true;
}

// ---- Dialect ----

enum CsvDialectField : unsigned { CsvDialectLineTerminator, CsvDialectDelimiter, CsvDialectEscapeChar, CsvDialectQuoteChar, CsvDialectFieldCount };
// In `number`: the quoting, and above it
enum CsvDialectBit : int64_t { CsvDialectDoubleQuote = 1 << 8, CsvDialectSkipInitialSpace = 1 << 9, CsvDialectStrict = 1 << 10 };

struct CsvDialect {
    CsvDialect(JSGlobalObject* globalObject, PyRecord* record)
        : delimiter(character(record, CsvDialectDelimiter))
        , escapeChar(character(record, CsvDialectEscapeChar))
        , quoteChar(character(record, CsvDialectQuoteChar))
        , quoting(static_cast<int>(record->number & 0xff))
        , doubleQuote(record->number & CsvDialectDoubleQuote)
        , skipInitialSpace(record->number & CsvDialectSkipInitialSpace)
        , strict(record->number & CsvDialectStrict)
        , lineTerminator(strValue(record->get(CsvDialectLineTerminator))->value(globalObject))
    {
    }

    static char32_t character(PyRecord* record, unsigned field)
    {
        JSValue value = record->get(field);
        return value ? static_cast<char32_t>(value.asNumber()) : csvNotSet;
    }

    char32_t delimiter;
    char32_t escapeChar;
    char32_t quoteChar;
    int quoting;
    bool doubleQuote;
    bool skipInitialSpace;
    bool strict;
    String lineTerminator;
};

static PyType* csvDialectType(JSGlobalObject*);

static JSValue csvDialectFromRegistry(JSGlobalObject* globalObject, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue dialect = csvDialects(globalObject)->get(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!dialect)
        RELEASE_AND_RETURN(scope, csvRaise(globalObject, "unknown dialect"_s));
    return dialect;
}

// `orNone`: None stands for no character at all. False: an exception.
static bool csvSetCharacter(JSGlobalObject* globalObject, ASCIILiteral name, JSValue source, char32_t defaultValue, bool orNone, char32_t& target)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!source) {
        target = defaultValue;
        return true;
    }
    if (orNone && isNone(source)) {
        target = csvNotSet;
        return true;
    }
    ASCIILiteral what = orNone ? "\" must be a unicode character or None, not "_s : "\" must be a unicode character, not "_s;
    if (!isStr(source)) {
        raiseTypeError(globalObject, scope, '"', name, what, typeNameForMessage(globalObject, rt.typeOf(source)));
        return false;
    }
    StrView view(globalObject, strValue(source));
    RETURN_IF_EXCEPTION(scope, false);
    if (view.length() != 1) {
        raiseTypeError(globalObject, scope, '"', name, what, "a string of length "_s, view.length());
        return false;
    }
    target = view.at(0);
    return true;
}

static bool csvSetBool(JSGlobalObject* globalObject, JSValue source, bool defaultValue, bool& target)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    target = source ? isTrue(globalObject, source) : defaultValue;
    return !scope.exception();
}

static bool csvCheckCharacter(JSGlobalObject* globalObject, ASCIILiteral name, char32_t character, StringView lineTerminator, bool allowSpace)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (character == '\r' || character == '\n' || (character == ' ' && !allowSpace)) {
        raiseValueError(globalObject, scope, "bad "_s, name, " value"_s);
        return false;
    }
    if (csvContains(lineTerminator, character)) {
        raiseValueError(globalObject, scope, "bad "_s, name, " or lineterminator value"_s);
        return false;
    }
    return true;
}

static bool csvCheckCharacters(JSGlobalObject* globalObject, ASCIILiteral name1, ASCIILiteral name2, char32_t character1, char32_t character2)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (character1 == character2 && character1 != csvNotSet) {
        raiseValueError(globalObject, scope, "bad "_s, name1, " or "_s, name2, " value"_s);
        return false;
    }
    return true;
}

static constexpr ASCIILiteral csvDialectKeywords[] = {
    "dialect"_s, "delimiter"_s, "doublequote"_s, "escapechar"_s, "lineterminator"_s, "quotechar"_s, "quoting"_s, "skipinitialspace"_s, "strict"_s,
};

static JSValue csvDialectNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // The messages are those of a function that has not said what it is called.
    constexpr unsigned count = std::size(csvDialectKeywords);
    JSValue values[count];
    if (args.totalCount() > count)
        return raiseTypeError(globalObject, scope, "function takes at most "_s, count, args.size() ? " arguments ("_s : " keyword arguments ("_s, args.totalCount(), " given)"_s);
    for (unsigned i = 0; i < args.size(); ++i)
        values[i] = args[i];
    for (unsigned k = 0; k < args.keywordCount(); ++k) {
        String keyword = args.keywordName(k)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        unsigned index = 0;
        while (index < count && keyword != csvDialectKeywords[index])
            ++index;
        if (index == count)
            return raiseTypeError(globalObject, scope, "this function got an unexpected keyword argument '"_s, keyword, '\'');
        if (index < args.size())
            return raiseTypeError(globalObject, scope, "argument for function given by name ('"_s, keyword, "') and position ("_s, index + 1, ')');
        values[index] = args.keywordValue(k);
    }

    JSValue dialect = values[0];
    if (dialect) {
        if (isStr(dialect)) {
            dialect = csvDialectFromRegistry(globalObject, dialect);
            RETURN_IF_EXCEPTION(scope, { });
        }
        bool hasOptions = false;
        for (unsigned i = 1; i < count; ++i)
            hasOptions |= !!values[i];
        if (!hasOptions && isInstanceOf(globalObject, dialect, csvDialectType(globalObject)))
            return dialect;
        for (unsigned i = 1; i < count; ++i) {
            if (values[i])
                continue;
            values[i] = getAttrIfPresent(globalObject, dialect, internString(vm, csvDialectKeywords[i]));
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    JSValue delimiterValue = values[1], doubleQuoteValue = values[2], escapeCharValue = values[3], lineTerminatorValue = values[4];
    JSValue quoteCharValue = values[5], quotingValue = values[6], skipInitialSpaceValue = values[7], strictValue = values[8];

    char32_t delimiter, escapeChar, quoteChar;
    bool doubleQuote, skipInitialSpace, strict;
    int quoting = CsvQuoteMinimal;
    if (failed(scope, csvSetCharacter(globalObject, "delimiter"_s, delimiterValue, ',', false, delimiter)))
        return { };
    if (failed(scope, csvSetBool(globalObject, doubleQuoteValue, true, doubleQuote)))
        return { };
    if (failed(scope, csvSetCharacter(globalObject, "escapechar"_s, escapeCharValue, csvNotSet, true, escapeChar)))
        return { };
    if (!lineTerminatorValue)
        lineTerminatorValue = makeStr(vm, "\r\n"_s);
    else if (!isStr(lineTerminatorValue))
        return raiseTypeError(globalObject, scope, "\"lineterminator\" must be a string, not "_s, typeNameForMessage(globalObject, rt.typeOf(lineTerminatorValue)));
    if (failed(scope, csvSetCharacter(globalObject, "quotechar"_s, quoteCharValue, '"', true, quoteChar)))
        return { };
    if (quotingValue) {
        if (!isExactInt(quotingValue))
            return raiseTypeError(globalObject, scope, "\"quoting\" must be an integer, not "_s, typeNameForMessage(globalObject, rt.typeOf(quotingValue)));
        auto value = tryInt64(quotingValue);
        if (!value || *value != static_cast<int>(*value))
            return raiseOverflowError(globalObject, scope, "Python int too large to convert to C int"_s);
        quoting = static_cast<int>(*value);
    }
    if (failed(scope, csvSetBool(globalObject, skipInitialSpaceValue, false, skipInitialSpace)))
        return { };
    if (failed(scope, csvSetBool(globalObject, strictValue, false, strict)))
        return { };

    if (quoting < CsvQuoteMinimal || quoting > CsvQuoteNotNull)
        return raiseTypeError(globalObject, scope, "bad \"quoting\" value"_s);
    if (quoteCharValue && isNone(quoteCharValue) && !quotingValue)
        quoting = CsvQuoteNone;
    if (quoting != CsvQuoteNone && quoteChar == csvNotSet)
        return raiseTypeError(globalObject, scope, "quotechar must be set if quoting enabled"_s);
    String lineTerminator = strValue(lineTerminatorValue)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (failed(scope, csvCheckCharacter(globalObject, "delimiter"_s, delimiter, lineTerminator, true))
        || failed(scope, csvCheckCharacter(globalObject, "escapechar"_s, escapeChar, lineTerminator, !skipInitialSpace))
        || failed(scope, csvCheckCharacter(globalObject, "quotechar"_s, quoteChar, lineTerminator, !skipInitialSpace))
        || failed(scope, csvCheckCharacters(globalObject, "delimiter"_s, "escapechar"_s, delimiter, escapeChar))
        || failed(scope, csvCheckCharacters(globalObject, "delimiter"_s, "quotechar"_s, delimiter, quoteChar))
        || failed(scope, csvCheckCharacters(globalObject, "escapechar"_s, "quotechar"_s, escapeChar, quoteChar)))
        return { };

    PyRecord* self = PyRecord::create(globalObject, PyType::from(cls), CsvDialectFieldCount);
    auto characterValue = [](char32_t character) { return character == csvNotSet ? JSValue() : jsNumber(static_cast<uint32_t>(character)); };
    self->set(vm, CsvDialectLineTerminator, lineTerminatorValue);
    self->set(vm, CsvDialectDelimiter, characterValue(delimiter));
    self->set(vm, CsvDialectEscapeChar, characterValue(escapeChar));
    self->set(vm, CsvDialectQuoteChar, characterValue(quoteChar));
    self->number = quoting | (doubleQuote ? CsvDialectDoubleQuote : 0) | (skipInitialSpace ? CsvDialectSkipInitialSpace : 0) | (strict ? CsvDialectStrict : 0);
    return self;
}

template<unsigned field>
static JSValue csvDialectGetCharacter(JSGlobalObject* globalObject, JSValue self)
{
    JSValue value = PyRecord::from(self)->get(field);
    if (!value)
        return jsUndefined();
    return makeStr(globalObject, stringFromCodePoint(static_cast<char32_t>(value.asNumber())));
}

template<int64_t bit>
static JSValue csvDialectGetBit(JSGlobalObject*, JSValue self) { return jsBoolean(PyRecord::from(self)->number & bit); }

static JSValue csvDialectGetLineTerminator(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(CsvDialectLineTerminator); }
static JSValue csvDialectGetQuoting(JSGlobalObject*, JSValue self) { return jsNumber(static_cast<int>(PyRecord::from(self)->number & 0xff)); }

static JSValue csvDialectReduce(JSGlobalObject* globalObject, JSValue self, const CallArgs&)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "cannot pickle '"_s, typeOf(globalObject, self)->name(), "' instances"_s);
}

static constexpr NativeDef csvDialectMethods[] = {
    PY_NEW(csvDialectNew, "($type, *args, **kwargs)"),
    PY_METHOD("__reduce__", csvDialectReduce, "($self, *args)"),
    PY_METHOD("__reduce_ex__", csvDialectReduce, "($self, *args)"),
};

static constexpr NativeGetSet csvDialectGetSets[] = {
    PY_MEMBER("skipinitialspace", csvDialectGetBit<CsvDialectSkipInitialSpace>),
    PY_MEMBER("doublequote", csvDialectGetBit<CsvDialectDoubleQuote>),
    PY_MEMBER("strict", csvDialectGetBit<CsvDialectStrict>),
    { "delimiter"_s, csvDialectGetCharacter<CsvDialectDelimiter> },
    { "escapechar"_s, csvDialectGetCharacter<CsvDialectEscapeChar> },
    { "lineterminator"_s, csvDialectGetLineTerminator },
    { "quotechar"_s, csvDialectGetCharacter<CsvDialectQuoteChar> },
    { "quoting"_s, csvDialectGetQuoting },
};

static constexpr TypeDef csvDialectTypeDef {
    "_csv.Dialect"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable }, csvDialectMethods, csvDialectGetSets,
    "CSV dialect\n\nThe Dialect type records CSV parsing and generation options.\n"_s
};

static PyType* csvDialectType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(csvDialectTypeDef); }

// Dialect(dialect, **keywords), for the functions that take a dialect and options.
static PyRecord* csvCallDialect(JSGlobalObject* globalObject, JSValue dialect, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer values;
    if (dialect)
        values.append(dialect);
    for (unsigned i = 0; i < args.keywordCount(); ++i)
        values.append(args.keywordValue(i));
    JSValue result = call(globalObject, csvDialectType(globalObject), CallArgs(valuesOf(values), dialect ? 1 : 0, args.keywordNames()));
    RETURN_IF_EXCEPTION(scope, nullptr);
    return PyRecord::from(result);
}

// ---- reader ----

enum CsvReaderField : unsigned {
    CsvReaderDialect,
    CsvReaderInput,
    CsvReaderFields, // The list being filled. Empty: the record has been handed out.
    // Where the parser is, for as long as it is waiting for the next line: asking for one may run anything.
    CsvReaderPartialField,
    CsvReaderPartialLength,
    CsvReaderState,
    CsvReaderFieldCount
}; // number: line_num

enum class CsvState : uint8_t {
    StartRecord, StartField, EscapedChar, InField, InQuotedField, EscapeInQuotedField, QuoteInQuotedField, EatCRNL, AfterEscapedCRNL
};

class CsvParser {
public:
    CsvParser(JSGlobalObject* globalObject, PyRecord* reader)
        : m_globalObject(globalObject)
        , m_reader(reader)
        , m_dialect(globalObject, PyRecord::from(reader->get(CsvReaderDialect)))
        , m_fieldLimit(csvFieldLimit(globalObject))
    {
    }

    CsvState state() const { return m_state; }
    bool hasPartialField() const { return m_fieldLength; }
    bool isStrict() const { return m_dialect.strict; }

    void suspend()
    {
        VM& vm = getVM(m_globalObject);
        m_reader->set(vm, CsvReaderPartialField, m_fieldLength ? JSValue(makeStr(vm, m_field.toString())) : JSValue());
        m_reader->set(vm, CsvReaderPartialLength, jsNumber(static_cast<double>(m_fieldLength)));
        m_reader->set(vm, CsvReaderState, jsNumber(static_cast<unsigned>(m_state) | (m_isUnquotedField ? 0x100 : 0)));
    }

    void resume()
    {
        m_field.clear();
        if (JSValue partial = m_reader->get(CsvReaderPartialField))
            m_field.append(stringOf(asString(partial)));
        m_fieldLength = static_cast<int64_t>(m_reader->get(CsvReaderPartialLength).asNumber());
        unsigned state = m_reader->get(CsvReaderState).asInt32();
        m_state = static_cast<CsvState>(state & 0xff);
        m_isUnquotedField = state & 0x100;
    }

    bool saveField()
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSValue field;
        int quoting = m_dialect.quoting;
        if (m_isUnquotedField && !m_fieldLength && (quoting == CsvQuoteNotNull || quoting == CsvQuoteStrings))
            field = jsUndefined();
        else {
            field = m_fieldLength ? makeStr(vm, m_field.toString()) : runtime(m_globalObject).names.empty();
            if (m_isUnquotedField && m_fieldLength && (quoting == CsvQuoteNonNumeric || quoting == CsvQuoteStrings)) {
                field = call(m_globalObject, runtime(m_globalObject).floatType(), field);
                RETURN_IF_EXCEPTION(scope, false);
            }
            m_field.clear();
            m_fieldLength = 0;
        }
        listAppend(m_globalObject, asList(m_reader->get(CsvReaderFields)), field);
        return !scope.exception();
    }

    // False: an exception.
    bool process(char32_t c)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        switch (m_state) {
        case CsvState::StartRecord:
            if (c == csvEndOfLine)
                return true; // An empty line is an empty record.
            if (c == '\n' || c == '\r') {
                m_state = CsvState::EatCRNL;
                return true;
            }
            m_state = CsvState::StartField;
            [[fallthrough]];
        case CsvState::StartField:
            m_isUnquotedField = true;
            if (isEndOfLine(c)) {
                if (failed(scope, saveField()))
                    return false;
                m_state = c == csvEndOfLine ? CsvState::StartRecord : CsvState::EatCRNL;
            } else if (c == m_dialect.quoteChar && m_dialect.quoting != CsvQuoteNone) {
                m_isUnquotedField = false;
                m_state = CsvState::InQuotedField;
            } else if (c == m_dialect.escapeChar)
                m_state = CsvState::EscapedChar;
            else if (c == ' ' && m_dialect.skipInitialSpace) {
            } else if (c == m_dialect.delimiter)
                RELEASE_AND_RETURN(scope, saveField());
            else {
                if (failed(scope, add(c)))
                    return false;
                m_state = CsvState::InField;
            }
            return true;

        case CsvState::EscapedChar:
            if (c == '\n' || c == '\r') {
                if (failed(scope, add(c)))
                    return false;
                m_state = CsvState::AfterEscapedCRNL;
                return true;
            }
            if (failed(scope, add(c == csvEndOfLine ? '\n' : c)))
                return false;
            m_state = CsvState::InField;
            return true;

        case CsvState::AfterEscapedCRNL:
            if (c == csvEndOfLine)
                return true;
            [[fallthrough]];
        case CsvState::InField:
            if (isEndOfLine(c)) {
                if (failed(scope, saveField()))
                    return false;
                m_state = c == csvEndOfLine ? CsvState::StartRecord : CsvState::EatCRNL;
            } else if (c == m_dialect.escapeChar)
                m_state = CsvState::EscapedChar;
            else if (c == m_dialect.delimiter) {
                if (failed(scope, saveField()))
                    return false;
                m_state = CsvState::StartField;
            } else
                RELEASE_AND_RETURN(scope, add(c));
            return true;

        case CsvState::InQuotedField:
            if (c == csvEndOfLine)
                return true;
            if (c == m_dialect.escapeChar)
                m_state = CsvState::EscapeInQuotedField;
            else if (c == m_dialect.quoteChar && m_dialect.quoting != CsvQuoteNone)
                m_state = m_dialect.doubleQuote ? CsvState::QuoteInQuotedField : CsvState::InField;
            else
                RELEASE_AND_RETURN(scope, add(c));
            return true;

        case CsvState::EscapeInQuotedField:
            if (failed(scope, add(c == csvEndOfLine ? '\n' : c)))
                return false;
            m_state = CsvState::InQuotedField;
            return true;

        case CsvState::QuoteInQuotedField:
            if (m_dialect.quoting != CsvQuoteNone && c == m_dialect.quoteChar) {
                if (failed(scope, add(c)))
                    return false;
                m_state = CsvState::InQuotedField;
            } else if (c == m_dialect.delimiter) {
                if (failed(scope, saveField()))
                    return false;
                m_state = CsvState::StartField;
            } else if (isEndOfLine(c)) {
                if (failed(scope, saveField()))
                    return false;
                m_state = c == csvEndOfLine ? CsvState::StartRecord : CsvState::EatCRNL;
            } else if (!m_dialect.strict) {
                if (failed(scope, add(c)))
                    return false;
                m_state = CsvState::InField;
            } else {
                scope.release();
                csvRaise(m_globalObject, '\'', stringFromCodePoint(m_dialect.delimiter), "' expected after '"_s, stringFromCodePoint(m_dialect.quoteChar), '\'');
                return false;
            }
            return true;

        case CsvState::EatCRNL:
            if (c == '\n' || c == '\r')
                return true;
            if (c == csvEndOfLine) {
                m_state = CsvState::StartRecord;
                return true;
            }
            scope.release();
            csvRaise(m_globalObject, "new-line character seen in unquoted field - do you need to open the file with newline=''?"_s);
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

private:
    static bool isEndOfLine(char32_t c) { return c == '\n' || c == '\r' || c == csvEndOfLine; }

    ALWAYS_INLINE bool add(char32_t c)
    {
        if (m_fieldLength >= m_fieldLimit) [[unlikely]] {
            csvRaise(m_globalObject, "field larger than field limit ("_s, m_fieldLimit, ')');
            return false;
        }
        appendCodePoint(m_field, c);
        ++m_fieldLength;
        return true;
    }

    JSGlobalObject* m_globalObject;
    PyRecord* m_reader;
    CsvDialect m_dialect;
    int64_t m_fieldLimit;
    StrBuilder m_field;
    int64_t m_fieldLength { 0 };
    CsvState m_state { CsvState::StartRecord };
    bool m_isUnquotedField { false };
};

static JSValue csvReaderIter(JSGlobalObject*, JSValue self) { return self; }

static JSValue csvReaderNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* reader = PyRecord::from(self);
    CsvParser parser(globalObject, reader);
    RETURN_IF_EXCEPTION(scope, { });
    reader->set(vm, CsvReaderFields, newList(globalObject));

    do {
        parser.suspend();
        JSValue line = iterNext(globalObject, reader->get(CsvReaderInput));
        RETURN_IF_EXCEPTION(scope, { });
        parser.resume();
        if (!line) {
            if (parser.hasPartialField() || parser.state() == CsvState::InQuotedField) {
                if (parser.isStrict())
                    RELEASE_AND_RETURN(scope, csvRaise(globalObject, "unexpected end of data"_s));
                if (!reader->get(CsvReaderFields))
                    reader->set(vm, CsvReaderFields, newList(globalObject));
                bool saved = parser.saveField();
                RETURN_IF_EXCEPTION(scope, { });
                if (saved)
                    break;
            }
            return raiseStopIteration(globalObject, scope);
        }
        if (!isStr(line))
            RELEASE_AND_RETURN(scope, csvRaise(globalObject, "iterator should return strings, not "_s, typeName(globalObject, line), " (the file should be opened in text mode)"_s));
        if (!reader->get(CsvReaderFields))
            RELEASE_AND_RETURN(scope, csvRaise(globalObject, "iterator has already advanced the reader"_s));
        ++reader->number;
        String text = strValue(line)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        bool ok = csvForEachCodePoint(text, [&](char32_t character) { return !failed(scope, parser.process(character)); });
        RETURN_IF_EXCEPTION(scope, { });
        ASSERT_UNUSED(ok, ok);
        parser.process(csvEndOfLine);
        RETURN_IF_EXCEPTION(scope, { });
    } while (parser.state() != CsvState::StartRecord);

    JSValue fields = reader->get(CsvReaderFields);
    reader->set(vm, CsvReaderFields, { });
    return fields;
}

static JSValue csvReaderGetDialect(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(CsvReaderDialect); }
static JSValue csvReaderGetLineNum(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, PyRecord::from(self)->number); }

static constexpr NativeDef csvReaderMethods[] = {
    PY_SLOT("__iter__", csvReaderIter, "($self, /)"),
    PY_SLOT("__next__", csvReaderNext, "($self, /)"),
};

static constexpr NativeGetSet csvReaderGetSets[] = {
    PY_MEMBER("dialect", csvReaderGetDialect),
    PY_MEMBER("line_num", csvReaderGetLineNum),
};

static constexpr TypeDef csvReaderTypeDef {
    "_csv.reader"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable, TypeFlag::DisallowInstantiation }, csvReaderMethods, csvReaderGetSets,
    "CSV reader\n\nReader objects are responsible for reading and parsing tabular data\nin CSV format.\n"_s
};

static JSValue csvReader(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPositional(globalObject, "reader"_s, args, 1, 2)))
        return { };
    JSValue input = getIter(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* dialect = csvCallDialect(globalObject, args.get(1), args);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* reader = PyRecord::create(globalObject, runtime(globalObject).typeFor(csvReaderTypeDef), CsvReaderFieldCount);
    reader->set(vm, CsvReaderDialect, dialect);
    reader->set(vm, CsvReaderInput, input);
    return reader;
}

// ---- writer ----

enum CsvWriterField : unsigned { CsvWriterDialect, CsvWriterWrite, CsvWriterFieldCount };

class CsvJoiner {
public:
    CsvJoiner(JSGlobalObject* globalObject, PyRecord* dialect)
        : m_globalObject(globalObject)
        , m_dialect(globalObject, dialect)
    {
    }

    const CsvDialect& dialect() const { return m_dialect; }
    bool isEmpty() const { return m_record.isEmpty(); }
    unsigned fieldCount() const { return m_fieldCount; }
    void forgetLastField() { --m_fieldCount; }

    // A null `field` is None. False: an exception.
    bool append(const String* field, bool quoted)
    {
        if ((!field || field->isEmpty()) && m_dialect.delimiter == ' ' && m_dialect.skipInitialSpace) {
            if (m_dialect.quoting == CsvQuoteNone || (!field && (m_dialect.quoting == CsvQuoteStrings || m_dialect.quoting == CsvQuoteNotNull))) {
                csvRaise(m_globalObject, "empty field must be quoted if delimiter is a space and skipinitialspace is true"_s);
                return false;
            }
            quoted = true;
        }
        // Whether the field is quoted depends on all that is in it.
        if (!appendData<false>(field, quoted))
            return false;
        appendData<true>(field, quoted);
        ++m_fieldCount;
        return true;
    }

    String finish()
    {
        m_record.append(m_dialect.lineTerminator);
        return m_record.toStringOrNull();
    }

private:
    template<bool copies>
    bool appendData(const String* field, bool& quoted)
    {
        auto add = [&](char32_t character) {
            if constexpr (copies)
                appendCodePoint(m_record, character);
        };
        if (m_fieldCount)
            add(m_dialect.delimiter);
        if (copies && quoted)
            add(m_dialect.quoteChar);
        if (field) {
            bool ok = csvForEachCodePoint(*field, [&](char32_t c) {
                bool wantsEscape = false;
                if (c == m_dialect.delimiter || c == m_dialect.escapeChar || c == m_dialect.quoteChar || c == '\n' || c == '\r' || csvContains(m_dialect.lineTerminator, c)) {
                    if (m_dialect.quoting == CsvQuoteNone)
                        wantsEscape = true;
                    else {
                        if (c == m_dialect.quoteChar) {
                            if (m_dialect.doubleQuote)
                                add(m_dialect.quoteChar);
                            else
                                wantsEscape = true;
                        } else if (c == m_dialect.escapeChar)
                            wantsEscape = true;
                        if (!wantsEscape)
                            quoted = true;
                    }
                    if (wantsEscape) {
                        if (m_dialect.escapeChar == csvNotSet) {
                            csvRaise(m_globalObject, "need to escape, but no escapechar set"_s);
                            return false;
                        }
                        add(m_dialect.escapeChar);
                    }
                }
                add(c);
                return true;
            });
            if (!ok)
                return false;
        }
        if (copies && quoted)
            add(m_dialect.quoteChar);
        return true;
    }

    JSGlobalObject* m_globalObject;
    CsvDialect m_dialect;
    StrBuilder m_record;
    unsigned m_fieldCount { 0 };
};

// What PyNumber_Check() says.
static bool csvIsNumber(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isNumber())
        return true;
    Runtime& rt = runtime(globalObject);
    PyType* type = rt.typeOf(value);
    return type->lookup(Special::Index) || type->lookup(Special::Int) || type->lookup(Special::Float) || type->isSubtypeOf(rt.complexType());
}

static JSValue csvWriterWriteRow(JSGlobalObject* globalObject, JSValue self, JSValue row)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* writer = PyRecord::from(self);

    JSValue iterator = getIter(globalObject, row);
    if (scope.exception()) {
        if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            RELEASE_AND_RETURN(scope, csvRaise(globalObject, "iterable expected, not "_s, typeName(globalObject, row)));
        return { };
    }

    CsvJoiner joiner(globalObject, PyRecord::from(writer->get(CsvWriterDialect)));
    RETURN_IF_EXCEPTION(scope, { });
    int quoting = joiner.dialect().quoting;
    bool isNullField = false;
    while (true) {
        JSValue field = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!field)
            break;
        bool quoted = false;
        switch (quoting) {
        case CsvQuoteNonNumeric:
            quoted = !csvIsNumber(globalObject, field);
            break;
        case CsvQuoteAll:
            quoted = true;
            break;
        case CsvQuoteStrings:
            quoted = isStr(field);
            break;
        case CsvQuoteNotNull:
            quoted = !isNone(field);
            break;
        default:
            break;
        }
        isNullField = isNone(field);
        if (isNullField)
            joiner.append(nullptr, quoted);
        else {
            JSString* string = isStr(field) ? strValue(field) : str(globalObject, field);
            RETURN_IF_EXCEPTION(scope, { });
            String text = string->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            joiner.append(&text, quoted);
        }
        RETURN_IF_EXCEPTION(scope, { });
    }

    if (joiner.fieldCount() && joiner.isEmpty()) {
        if (quoting == CsvQuoteNone || (isNullField && (quoting == CsvQuoteStrings || quoting == CsvQuoteNotNull)))
            RELEASE_AND_RETURN(scope, csvRaise(globalObject, "single empty field record must be quoted"_s));
        joiner.forgetLastField();
        joiner.append(nullptr, true);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSString* line = makeStrUnlessTooLong(globalObject, joiner.finish());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, call(globalObject, writer->get(CsvWriterWrite), line));
}

static JSValue csvWriterWriteRows(JSGlobalObject* globalObject, JSValue self, JSValue rows)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    forEach(globalObject, rows, [&](JSValue row) {
        return !!csvWriterWriteRow(globalObject, self, row);
    });
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue csvWriterGetDialect(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(CsvWriterDialect); }

static constexpr NativeDef csvWriterMethods[] = {
    PY_METHOD("writerow", csvWriterWriteRow, "($self, row, /)"),
    PY_METHOD("writerows", csvWriterWriteRows, "($self, rows, /)"),
};

static constexpr NativeGetSet csvWriterGetSets[] = {
    PY_MEMBER("dialect", csvWriterGetDialect),
};

static constexpr TypeDef csvWriterTypeDef {
    "_csv.writer"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable, TypeFlag::DisallowInstantiation }, csvWriterMethods, csvWriterGetSets,
    "CSV writer\n\nWriter objects are responsible for generating tabular data\nin CSV format from sequence input.\n"_s
};

static JSValue csvWriter(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPositional(globalObject, "writer"_s, args, 1, 2)))
        return { };
    JSValue write = getAttrIfPresent(globalObject, args[0], internString(vm, "write"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (!write || !isCallable(globalObject, write))
        return raiseTypeError(globalObject, scope, "argument 1 must have a \"write\" method"_s);
    PyRecord* dialect = csvCallDialect(globalObject, args.get(1), args);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* writer = PyRecord::create(globalObject, runtime(globalObject).typeFor(csvWriterTypeDef), CsvWriterFieldCount);
    writer->set(vm, CsvWriterDialect, dialect);
    writer->set(vm, CsvWriterWrite, write);
    return writer;
}

// ---- The registry of dialects ----

static JSValue csvListDialects(JSGlobalObject* globalObject)
{
    return listFromIterable(globalObject, csvDialects(globalObject));
}

static JSValue csvRegisterDialect(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPositional(globalObject, "register_dialect"_s, args, 1, 2)))
        return { };
    if (!isStr(args[0]))
        return raiseTypeError(globalObject, scope, "dialect name must be a string"_s);
    PyRecord* dialect = csvCallDialect(globalObject, args.get(1), args);
    RETURN_IF_EXCEPTION(scope, { });
    csvDialects(globalObject)->set(globalObject, args[0], dialect);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue csvUnregisterDialect(JSGlobalObject* globalObject, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dialects = csvDialects(globalObject);
    // Nothing is looked up, nor hashed, in an empty dict.
    bool removed = !dialects->isEmpty() && dialects->remove(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!removed)
        RELEASE_AND_RETURN(scope, csvRaise(globalObject, "unknown dialect"_s));
    return jsUndefined();
}

static JSValue csvGetDialect(JSGlobalObject* globalObject, JSValue name) { return csvDialectFromRegistry(globalObject, name); }

static JSValue csvFieldSizeLimit(JSGlobalObject* globalObject, JSValue newLimit)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t oldLimit = csvFieldLimit(globalObject);
    if (newLimit) {
        if (!isExactInt(newLimit))
            return raiseTypeError(globalObject, scope, "limit must be an integer"_s);
        if (!tryInt64(newLimit))
            return raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
        csvState(globalObject)->set(vm, internString(vm, "field_limit"_s), newLimit);
    }
    RELEASE_AND_RETURN(scope, makeInt(globalObject, oldLimit));
}

static constexpr NativeDef csvFunctions[] = {
    PY_FUNCTION("reader", csvReader, "!(iterable, /, dialect='excel', **fmtparams)"),
    PY_FUNCTION("writer", csvWriter, "!(fileobj, /, dialect='excel', **fmtparams)"),
    PY_FUNCTION("list_dialects", csvListDialects, "()"),
    PY_FUNCTION("register_dialect", csvRegisterDialect, "!(name, /, dialect='excel', **fmtparams)"),
    PY_FUNCTION("unregister_dialect", csvUnregisterDialect, "(name)"),
    PY_FUNCTION("get_dialect", csvGetDialect, "(name)"),
    PY_FUNCTION("field_size_limit", csvFieldSizeLimit, "(new_limit=<unrepresentable>)"),
};

void initializeCsvModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->add(globalObject, "__doc__"_s, makeStr(rt.vm(), "CSV parsing and writing.\n"_s));
    module->addFunctions(globalObject, csvFunctions);
    module->add(globalObject, "Dialect"_s, csvDialectType(globalObject));
    module->add(globalObject, "Reader"_s, rt.typeFor(csvReaderTypeDef));
    module->add(globalObject, "Writer"_s, rt.typeFor(csvWriterTypeDef));
    module->add(globalObject, "_dialects"_s, csvDialects(globalObject));
    module->add(globalObject, "QUOTE_MINIMAL"_s, jsNumber(CsvQuoteMinimal));
    module->add(globalObject, "QUOTE_ALL"_s, jsNumber(CsvQuoteAll));
    module->add(globalObject, "QUOTE_NONNUMERIC"_s, jsNumber(CsvQuoteNonNumeric));
    module->add(globalObject, "QUOTE_NONE"_s, jsNumber(CsvQuoteNone));
    module->add(globalObject, "QUOTE_STRINGS"_s, jsNumber(CsvQuoteStrings));
    module->add(globalObject, "QUOTE_NOTNULL"_s, jsNumber(CsvQuoteNotNull));
    module->add(globalObject, "Error"_s, csvError(globalObject));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
