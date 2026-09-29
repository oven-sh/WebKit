/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "config.h"
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonText.h"

// The module _csv: Modules/_csv.c of CPython, which csv is written over. There is no other.

namespace JSC { namespace Python {

namespace {

constexpr char32_t notSet = static_cast<char32_t>(-1); // NOT_SET
constexpr char32_t endOfLine = static_cast<char32_t>(-2); // EOL

enum class Quoting : int { Minimal, All, NonNumeric, None, Strings, NotNull };
constexpr std::pair<ASCIILiteral, Quoting> quoteStyles[] = {
    { "QUOTE_MINIMAL"_s, Quoting::Minimal }, { "QUOTE_ALL"_s, Quoting::All }, { "QUOTE_NONNUMERIC"_s, Quoting::NonNumeric },
    { "QUOTE_NONE"_s, Quoting::None }, { "QUOTE_STRINGS"_s, Quoting::Strings }, { "QUOTE_NOTNULL"_s, Quoting::NotNull },
};

struct CSVModuleState final : NativeState {
    PYTHON_NATIVE_STATE(CSVModuleState);
    WriteBarrier<PyType> error;
    WriteBarrier<PyDict> dialects;
    WriteBarrier<PyType> dialectType;
    WriteBarrier<PyType> readerType;
    WriteBarrier<PyType> writerType;
    int64_t fieldLimit { 128 * 1024 };
};

template<typename Visitor>
void CSVModuleState::visit(Visitor& visitor)
{
    visitor.append(error);
    visitor.append(dialects);
    visitor.append(dialectType);
    visitor.append(readerType);
    visitor.append(writerType);
}

CSVModuleState& csvModuleState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<CSVModuleState>(); }

// _csv.Error
JSValue raiseCSVError(JSGlobalObject* globalObject, ThrowScope& scope, const String& message)
{
    JSObject* exception = createException(globalObject, csvModuleState(globalObject).error.get(), jsString(globalObject->vm(), message));
    RETURN_IF_EXCEPTION(scope, { });
    setContext(globalObject, exception);
    throwException(globalObject, scope, exception);
    return { };
}

String stringOf(std::span<const char32_t> characters)
{
    TextBuilder builder;
    for (char32_t character : characters)
        builder.append(character);
    return builder.tryFinish();
}

// ---- Dialect

struct Dialect final : NativeState {
    PYTHON_NATIVE_STATE(Dialect);
    bool doubleQuote { true }; // Whether " is written ""
    bool skipInitialSpace { false };
    bool strict { false };
    int quoting { static_cast<int>(Quoting::Minimal) };
    char32_t delimiter { ',' };
    char32_t quoteCharacter { '"' };
    char32_t escapeCharacter { notSet };
    WriteBarrier<Unknown> lineTerminator;
    // The same, a character at a time
    Vector<char32_t, 2> lineTerminatorCharacters;

    Quoting style() const { return static_cast<Quoting>(quoting); }
};

template<typename Visitor> void Dialect::visit(Visitor& visitor) { visitor.append(lineTerminator); }

// get_dialect_from_registry(). Empty if it raised.
JSValue dialectFromRegistry(JSGlobalObject* globalObject, JSValue name)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue dialect = csvModuleState(globalObject).dialects->get(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!dialect)
        return raiseCSVError(globalObject, scope, "unknown dialect"_s);
    return dialect;
}

JSValue characterOrNone(JSGlobalObject* globalObject, char32_t character)
{
    if (character == notSet)
        return jsUndefined();
    return jsString(globalObject->vm(), stringOf(std::span(&character, 1)));
}

// _set_char() and _set_char_or_none(). It may raise.
void setCharacter(JSGlobalObject* globalObject, ASCIILiteral name, char32_t& target, JSValue source, char32_t defaultValue, bool mayBeNone)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!source) {
        target = defaultValue;
        return;
    }
    if (mayBeNone && isNone(source)) {
        target = notSet;
        return;
    }
    ASCIILiteral wanted = mayBeNone ? "\" must be a unicode character or None, not "_s : "\" must be a unicode character, not "_s;
    JSString* string = stringIn(source);
    if (!string) {
        raiseTypeError(globalObject, scope, concatenate('"', name, wanted, fullyQualifiedTypeName(globalObject, source)));
        return;
    }
    auto text = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    size_t length = 0;
    for (char32_t character : text->codePoints()) {
        if (!length++)
            target = character;
    }
    if (length != 1)
        raiseTypeError(globalObject, scope, concatenate('"', name, wanted, "a string of length "_s, length));
}

// dialect_check_char(). It may raise.
void checkCharacter(JSGlobalObject* globalObject, ASCIILiteral name, char32_t character, Dialect& dialect, bool allowsSpace)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (character == '\r' || character == '\n' || (character == ' ' && !allowsSpace))
        raiseValueError(globalObject, scope, concatenate("bad "_s, name, " value"_s));
    else if (dialect.lineTerminatorCharacters.contains(character))
        raiseValueError(globalObject, scope, concatenate("bad "_s, name, " or lineterminator value"_s));
}

// dialect_check_chars(). It may raise.
void checkCharacters(JSGlobalObject* globalObject, ASCIILiteral firstName, ASCIILiteral secondName, char32_t first, char32_t second)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (first == second && first != notSet)
        raiseValueError(globalObject, scope, concatenate("bad "_s, firstName, " or "_s, secondName, " value"_s));
}

// _call_dialect(): a Dialect, from what a reader, a writer or register_dialect() was given: something that has what one has, or the name of one, or nothing, and whatever was given by name. Null if it raised.
PyStateObject* callDialect(JSGlobalObject* globalObject, JSValue given, const NativeArguments& args)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    MarkedArgumentBuffer arguments;
    if (given)
        arguments.append(given);
    for (unsigned i = 0; i < args.keywordCount(); ++i)
        arguments.append(args.keywordValue(i));
    JSValue dialect = callWithKeywords(globalObject, csvModuleState(globalObject).dialectType->object(), arguments, args.keywordNames());
    RETURN_IF_EXCEPTION(scope, nullptr);
    return uncheckedDowncast<PyStateObject>(dialect.asCell());
}

} // anonymous namespace

// Dialect(dialect=, delimiter=, doublequote=, escapechar=, lineterminator=, quotechar=, quoting=, skipinitialspace=, strict=)
PYTHON_NATIVE(dialectNew)
{
    NATIVE_PROLOGUE();
    JSValue dialect = args.at(1);
    JSValue delimiter = args.at(2);
    JSValue doubleQuote = args.at(3);
    JSValue escapeCharacter = args.at(4);
    JSValue lineTerminator = args.at(5);
    JSValue quoteCharacter = args.at(6);
    JSValue quoting = args.at(7);
    JSValue skipInitialSpace = args.at(8);
    JSValue strict = args.at(9);
    auto& module = csvModuleState(globalObject);

    if (dialect) {
        if (stringIn(dialect)) {
            dialect = dialectFromRegistry(globalObject, dialect);
            RETURN_IF_EXCEPTION(scope, { });
        }
        // One that is not to be any different will do as it is.
        if (typeOf(globalObject, dialect)->isSubtypeOf(module.dialectType.get()) && !delimiter && !doubleQuote && !escapeCharacter && !lineTerminator && !quoteCharacter && !quoting && !skipInitialSpace && !strict)
            return JSValue::encode(dialect);
    }

    auto* object = PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<Dialect>());
    auto& self = object->state<Dialect>();
    if (dialect) {
        // What it has not got is as if it had not been given, whatever went wrong with getting it.
        auto take = [&] (JSValue& value, ASCIILiteral name) {
            if (value)
                return true;
            value = getAttribute(globalObject, dialect, Identifier::fromString(vm, name));
            if (scope.exception()) [[unlikely]] {
                value = { };
                return scope.tryClearException();
            }
            return true;
        };
        if (!take(delimiter, "delimiter"_s) || !take(doubleQuote, "doublequote"_s) || !take(escapeCharacter, "escapechar"_s) || !take(lineTerminator, "lineterminator"_s) || !take(quoteCharacter, "quotechar"_s)
            || !take(quoting, "quoting"_s) || !take(skipInitialSpace, "skipinitialspace"_s) || !take(strict, "strict"_s))
            return { };
    }

    auto setBool = [&] (bool& target, JSValue source, bool defaultValue) {
        target = source ? isTrue(globalObject, source) : defaultValue;
    };
    setCharacter(globalObject, "delimiter"_s, self.delimiter, delimiter, ',', false);
    RETURN_IF_EXCEPTION(scope, { });
    setBool(self.doubleQuote, doubleQuote, true);
    RETURN_IF_EXCEPTION(scope, { });
    setCharacter(globalObject, "escapechar"_s, self.escapeCharacter, escapeCharacter, notSet, true);
    RETURN_IF_EXCEPTION(scope, { });
    if (!lineTerminator)
        lineTerminator = jsNontrivialString(vm, "\r\n"_s);
    else if (!stringIn(lineTerminator))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("\"lineterminator\" must be a string, not "_s, fullyQualifiedTypeName(globalObject, lineTerminator))));
    self.lineTerminator.set(vm, object, lineTerminator);
    {
        auto text = stringIn(lineTerminator)->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        for (char32_t character : text->codePoints())
            self.lineTerminatorCharacters.append(character);
    }
    setCharacter(globalObject, "quotechar"_s, self.quoteCharacter, quoteCharacter, '"', true);
    RETURN_IF_EXCEPTION(scope, { });
    if (quoting) {
        if (typeOf(globalObject, quoting) != realm->typeInt())
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("\"quoting\" must be an integer, not "_s, fullyQualifiedTypeName(globalObject, quoting))));
        auto value = toCInt(globalObject, quoting);
        RETURN_IF_EXCEPTION(scope, { });
        self.quoting = *value;
    }
    setBool(self.skipInitialSpace, skipInitialSpace, false);
    RETURN_IF_EXCEPTION(scope, { });
    setBool(self.strict, strict, false);
    RETURN_IF_EXCEPTION(scope, { });

    if (self.quoting < 0 || self.quoting > static_cast<int>(Quoting::NotNull))
        return JSValue::encode(raiseTypeError(globalObject, scope, "bad \"quoting\" value"_s));
    if (quoteCharacter && isNone(quoteCharacter) && !quoting)
        self.quoting = static_cast<int>(Quoting::None);
    if (self.style() != Quoting::None && self.quoteCharacter == notSet)
        return JSValue::encode(raiseTypeError(globalObject, scope, "quotechar must be set if quoting enabled"_s));
    checkCharacter(globalObject, "delimiter"_s, self.delimiter, self, true);
    RETURN_IF_EXCEPTION(scope, { });
    checkCharacter(globalObject, "escapechar"_s, self.escapeCharacter, self, !self.skipInitialSpace);
    RETURN_IF_EXCEPTION(scope, { });
    checkCharacter(globalObject, "quotechar"_s, self.quoteCharacter, self, !self.skipInitialSpace);
    RETURN_IF_EXCEPTION(scope, { });
    checkCharacters(globalObject, "delimiter"_s, "escapechar"_s, self.delimiter, self.escapeCharacter);
    RETURN_IF_EXCEPTION(scope, { });
    checkCharacters(globalObject, "delimiter"_s, "quotechar"_s, self.delimiter, self.quoteCharacter);
    RETURN_IF_EXCEPTION(scope, { });
    checkCharacters(globalObject, "escapechar"_s, "quotechar"_s, self.escapeCharacter, self.quoteCharacter);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(object);
}

// __reduce__() and __reduce_ex__()
PYTHON_NATIVE(dialectReduce)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("cannot pickle '"_s, typeOf(globalObject, args[0])->nameWithoutModule(globalObject), "' instances"_s)));
}

// ---- reader

namespace {

enum class ParserState : uint8_t { StartRecord, StartField, EscapedCharacter, InField, InQuotedField, EscapeInQuotedField, QuoteInQuotedField, EatEndOfLine, AfterEscapedEndOfLine };

struct Reader final : NativeState {
    PYTHON_NATIVE_STATE(Reader);
    WriteBarrier<Unknown> input; // What gives the lines
    WriteBarrier<PyStateObject> dialect;
    WriteBarrier<JSArray> fields; // Of the record that it is on. Null when it is on none.
    ParserState state { ParserState::StartRecord };
    Vector<char32_t> field;
    bool isUnquotedField { false };
    uint64_t lineNumber { 0 };
};

template<typename Visitor>
void Reader::visit(Visitor& visitor)
{
    visitor.append(input);
    visitor.append(dialect);
    visitor.append(fields);
}

// parse_save_field(). It may raise.
void saveField(JSGlobalObject* globalObject, Reader& self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Quoting quoting = self.dialect->state<Dialect>().style();
    JSValue field;
    if (self.isUnquotedField && self.field.isEmpty() && (quoting == Quoting::NotNull || quoting == Quoting::Strings))
        field = jsUndefined();
    else {
        field = strOrMemoryError(globalObject, stringOf(self.field.span()));
        RETURN_IF_EXCEPTION(scope, void());
        if (self.isUnquotedField && !self.field.isEmpty() && (quoting == Quoting::NonNumeric || quoting == Quoting::Strings)) {
            field = call(globalObject, globalObject->pyRealm()->typeFloat()->object(), field);
            RETURN_IF_EXCEPTION(scope, void());
        }
        self.field.shrink(0);
    }
    scope.release();
    listAppend(globalObject, self.fields.get(), field);
}

// parse_add_char(). It may raise.
void addCharacter(JSGlobalObject* globalObject, Reader& self, char32_t character)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int64_t limit = csvModuleState(globalObject).fieldLimit;
    if (static_cast<int64_t>(self.field.size()) >= limit) {
        raiseCSVError(globalObject, scope, concatenate("field larger than field limit ("_s, limit, ')'));
        return;
    }
    if (!self.field.tryAppend(character))
        raiseMemoryError(globalObject, scope);
}

// parse_process_char(). It may raise.
void processCharacter(JSGlobalObject* globalObject, Reader& self, char32_t c)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Dialect& dialect = self.dialect->state<Dialect>();
    bool isEndOfLineCharacter = c == '\n' || c == '\r';
    switch (self.state) {
    case ParserState::StartRecord:
        // A line with nothing on it is a record with nothing in it.
        if (c == endOfLine)
            break;
        if (isEndOfLineCharacter) {
            self.state = ParserState::EatEndOfLine;
            break;
        }
        self.state = ParserState::StartField;
        [[fallthrough]];
    case ParserState::StartField:
        self.isUnquotedField = true;
        if (isEndOfLineCharacter || c == endOfLine) {
            saveField(globalObject, self);
            RETURN_IF_EXCEPTION(scope, void());
            self.state = c == endOfLine ? ParserState::StartRecord : ParserState::EatEndOfLine;
        } else if (c == dialect.quoteCharacter && dialect.style() != Quoting::None) {
            self.isUnquotedField = false;
            self.state = ParserState::InQuotedField;
        } else if (c == dialect.escapeCharacter)
            self.state = ParserState::EscapedCharacter;
        else if (c == ' ' && dialect.skipInitialSpace) {
            // Spaces at the start of a field are passed over.
        } else if (c == dialect.delimiter) {
            saveField(globalObject, self);
            RETURN_IF_EXCEPTION(scope, void());
        } else {
            addCharacter(globalObject, self, c);
            RETURN_IF_EXCEPTION(scope, void());
            self.state = ParserState::InField;
        }
        break;
    case ParserState::EscapedCharacter:
        if (isEndOfLineCharacter) {
            addCharacter(globalObject, self, c);
            RETURN_IF_EXCEPTION(scope, void());
            self.state = ParserState::AfterEscapedEndOfLine;
            break;
        }
        if (c == endOfLine)
            c = '\n';
        addCharacter(globalObject, self, c);
        RETURN_IF_EXCEPTION(scope, void());
        self.state = ParserState::InField;
        break;
    case ParserState::AfterEscapedEndOfLine:
        if (c == endOfLine)
            break;
        [[fallthrough]];
    case ParserState::InField:
        if (isEndOfLineCharacter || c == endOfLine) {
            saveField(globalObject, self);
            RETURN_IF_EXCEPTION(scope, void());
            self.state = c == endOfLine ? ParserState::StartRecord : ParserState::EatEndOfLine;
        } else if (c == dialect.escapeCharacter)
            self.state = ParserState::EscapedCharacter;
        else if (c == dialect.delimiter) {
            saveField(globalObject, self);
            RETURN_IF_EXCEPTION(scope, void());
            self.state = ParserState::StartField;
        } else {
            addCharacter(globalObject, self, c);
            RETURN_IF_EXCEPTION(scope, void());
        }
        break;
    case ParserState::InQuotedField:
        if (c == endOfLine) {
            // It goes on on the next line.
        } else if (c == dialect.escapeCharacter)
            self.state = ParserState::EscapeInQuotedField;
        else if (c == dialect.quoteCharacter && dialect.style() != Quoting::None)
            self.state = dialect.doubleQuote ? ParserState::QuoteInQuotedField : ParserState::InField;
        else {
            addCharacter(globalObject, self, c);
            RETURN_IF_EXCEPTION(scope, void());
        }
        break;
    case ParserState::EscapeInQuotedField:
        if (c == endOfLine)
            c = '\n';
        addCharacter(globalObject, self, c);
        RETURN_IF_EXCEPTION(scope, void());
        self.state = ParserState::InQuotedField;
        break;
    case ParserState::QuoteInQuotedField:
        if (dialect.style() != Quoting::None && c == dialect.quoteCharacter) {
            // "" is "
            addCharacter(globalObject, self, c);
            RETURN_IF_EXCEPTION(scope, void());
            self.state = ParserState::InQuotedField;
        } else if (c == dialect.delimiter) {
            saveField(globalObject, self);
            RETURN_IF_EXCEPTION(scope, void());
            self.state = ParserState::StartField;
        } else if (isEndOfLineCharacter || c == endOfLine) {
            saveField(globalObject, self);
            RETURN_IF_EXCEPTION(scope, void());
            self.state = c == endOfLine ? ParserState::StartRecord : ParserState::EatEndOfLine;
        } else if (!dialect.strict) {
            addCharacter(globalObject, self, c);
            RETURN_IF_EXCEPTION(scope, void());
            self.state = ParserState::InField;
        } else
            raiseCSVError(globalObject, scope, concatenate('\'', stringOf(std::span(&dialect.delimiter, 1)), "' expected after '"_s, stringOf(std::span(&dialect.quoteCharacter, 1)), '\''));
        break;
    case ParserState::EatEndOfLine:
        if (isEndOfLineCharacter) {
            // Any number of them
        } else if (c == endOfLine)
            self.state = ParserState::StartRecord;
        else
            raiseCSVError(globalObject, scope, "new-line character seen in unquoted field - do you need to open the file with newline=''?"_s);
        break;
    }
}

// parse_reset()
void resetReader(JSGlobalObject* globalObject, PyStateObject* owner, Reader& self)
{
    self.fields.set(globalObject->vm(), owner, newList(globalObject));
    self.field.shrink(0);
    self.state = ParserState::StartRecord;
    self.isUnquotedField = false;
}

} // anonymous namespace

PYTHON_NATIVE(readerNext)
{
    NATIVE_PROLOGUE();
    auto* owner = uncheckedDowncast<PyStateObject>(args[0].asCell());
    auto& self = owner->state<Reader>();
    resetReader(globalObject, owner, self);
    do {
        JSValue line = iteratorNext(globalObject, self.input.get());
        RETURN_IF_EXCEPTION(scope, { });
        if (!line) {
            // There is no more. What was begun is all there is going to be of it.
            if (!self.field.isEmpty() || self.state == ParserState::InQuotedField) {
                if (self.dialect->state<Dialect>().strict)
                    return JSValue::encode(raiseCSVError(globalObject, scope, "unexpected end of data"_s));
                saveField(globalObject, self);
                RETURN_IF_EXCEPTION(scope, { });
                break;
            }
            return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
        }
        JSString* string = stringIn(line);
        if (!string)
            return JSValue::encode(raiseCSVError(globalObject, scope, concatenate("iterator should return strings, not "_s, typeName(globalObject, line), " (the file should be opened in text mode)"_s)));
        // What gave the line asked this for a record while it was about it.
        if (!self.fields)
            return JSValue::encode(raiseCSVError(globalObject, scope, "iterator has already advanced the reader"_s));
        ++self.lineNumber;
        String text = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        for (char32_t character : StringView(text).codePoints()) {
            processCharacter(globalObject, self, character);
            RETURN_IF_EXCEPTION(scope, { });
        }
        processCharacter(globalObject, self, endOfLine);
        RETURN_IF_EXCEPTION(scope, { });
    } while (self.state != ParserState::StartRecord);
    JSArray* fields = self.fields.get();
    self.fields.clear();
    return JSValue::encode(fields);
}

PYTHON_NATIVE(readerIter)
{
    UNUSED_PARAM(globalObject);
    return JSValue::encode(callFrame->argument(0));
}

// reader(iterable, dialect='excel', /, **fmtparams)
PYTHON_NATIVE(csvReader)
{
    NATIVE_PROLOGUE();
    if (!args.size() || args.size() > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, !args.size() ? String("reader expected at least 1 argument, got 0"_s) : concatenate("reader expected at most 2 arguments, got "_s, args.size())));
    auto* object = PyStateObject::create(vm, csvModuleState(globalObject).readerType->instanceStructure(), makeUnique<Reader>());
    auto& self = object->state<Reader>();
    resetReader(globalObject, object, self);
    JSValue iterator = getIterator(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    self.input.set(vm, object, iterator);
    PyStateObject* dialect = callDialect(globalObject, args.size() > 1 ? args[1] : JSValue(), args);
    RETURN_IF_EXCEPTION(scope, { });
    self.dialect.set(vm, object, dialect);
    return JSValue::encode(object);
}

// ---- writer

namespace {

struct Writer final : NativeState {
    PYTHON_NATIVE_STATE(Writer);
    WriteBarrier<Unknown> write; // What is called with each line
    WriteBarrier<PyStateObject> dialect;
    Vector<char32_t> record;
    unsigned fieldCount { 0 };
};

template<typename Visitor>
void Writer::visit(Visitor& visitor)
{
    visitor.append(write);
    visitor.append(dialect);
}

// join_append(), with join_append_data(). CPython goes through the field twice, once to find how much room it wants and whether it is to be quoted, and once to write it. Here what is written the first time is thrown away if it
// turns out that it was to be quoted. `field` is null for None. It may raise.
void appendField(JSGlobalObject* globalObject, Writer& self, JSString* field, bool isQuoted)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Dialect& dialect = self.dialect->state<Dialect>();
    String text = field ? String(field->value(globalObject)) : String();
    RETURN_IF_EXCEPTION(scope, void());
    if (text.isEmpty() && dialect.delimiter == ' ' && dialect.skipInitialSpace) {
        if (dialect.style() == Quoting::None || (!field && (dialect.style() == Quoting::Strings || dialect.style() == Quoting::NotNull))) {
            raiseCSVError(globalObject, scope, "empty field must be quoted if delimiter is a space and skipinitialspace is true"_s);
            return;
        }
        isQuoted = true;
    }

    size_t start = self.record.size();
    // False if it is to be begun again, quoted, or if it raised.
    auto write = [&] {
        auto add = [&] (char32_t character) { self.record.append(character); };
        if (self.fieldCount)
            add(dialect.delimiter);
        bool beganQuoted = isQuoted;
        if (beganQuoted)
            add(dialect.quoteCharacter);
        for (char32_t c : StringView(text).codePoints()) {
            bool wantsEscape = false;
            if (c == dialect.delimiter || c == dialect.escapeCharacter || c == dialect.quoteCharacter || c == '\n' || c == '\r' || dialect.lineTerminatorCharacters.contains(c)) {
                if (dialect.style() == Quoting::None)
                    wantsEscape = true;
                else {
                    if (c == dialect.quoteCharacter) {
                        if (dialect.doubleQuote)
                            add(dialect.quoteCharacter);
                        else
                            wantsEscape = true;
                    } else if (c == dialect.escapeCharacter)
                        wantsEscape = true;
                    if (!wantsEscape)
                        isQuoted = true;
                }
                if (wantsEscape) {
                    if (dialect.escapeCharacter == notSet) {
                        raiseCSVError(globalObject, scope, "need to escape, but no escapechar set"_s);
                        return false;
                    }
                    add(dialect.escapeCharacter);
                }
            }
            add(c);
        }
        if (isQuoted && !beganQuoted)
            return false;
        if (isQuoted)
            add(dialect.quoteCharacter);
        return true;
    };
    bool isDone = write();
    RETURN_IF_EXCEPTION(scope, void());
    if (!isDone) {
        self.record.shrink(start);
        write();
    }
    ++self.fieldCount;
}

} // anonymous namespace

PYTHON_NATIVE(writerWriteRow)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<Writer>(args[0]);
    Dialect& dialect = self.dialect->state<Dialect>();
    JSValue iterator = getIterator(globalObject, args[1]);
    if (scope.exception()) [[unlikely]] {
        if (catchException(globalObject, BuiltinType::TypeError))
            return JSValue::encode(raiseCSVError(globalObject, scope, concatenate("iterable expected, not "_s, typeName(globalObject, args[1]))));
        return { };
    }
    self.record.shrink(0);
    self.fieldCount = 0;
    bool isNullField = false;
    for (;;) {
        JSValue field = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!field)
            break;
        bool isQuoted = false;
        switch (dialect.style()) {
        case Quoting::NonNumeric:
            isQuoted = !isNumber(globalObject, field);
            break;
        case Quoting::All:
            isQuoted = true;
            break;
        case Quoting::Strings:
            isQuoted = !!stringIn(field);
            break;
        case Quoting::NotNull:
            isQuoted = !isNone(field);
            break;
        default:
            break;
        }
        isNullField = isNone(field);
        JSString* string = stringIn(field);
        if (!string && !isNullField) {
            String text = str(globalObject, field);
            RETURN_IF_EXCEPTION(scope, { });
            string = jsString(vm, text);
        }
        appendField(globalObject, self, string, isQuoted);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (self.fieldCount && self.record.isEmpty()) {
        if (dialect.style() == Quoting::None || (isNullField && (dialect.style() == Quoting::Strings || dialect.style() == Quoting::NotNull)))
            return JSValue::encode(raiseCSVError(globalObject, scope, "single empty field record must be quoted"_s));
        --self.fieldCount;
        appendField(globalObject, self, nullptr, true);
        RETURN_IF_EXCEPTION(scope, { });
    }
    self.record.appendVector(dialect.lineTerminatorCharacters);
    JSValue line = strOrMemoryError(globalObject, stringOf(self.record.span()));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, self.write.get(), line)));
}

PYTHON_NATIVE(writerWriteRows)
{
    NATIVE_PROLOGUE();
    JSValue rows = getIterator(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    // csv_writerow() itself, and not whatever a class derived from this may have by that name
    JSValue writeRow = csvModuleState(globalObject).writerType->lookup(vm, Identifier::fromString(vm, "writerow"_s));
    for (;;) {
        JSValue row = iteratorNext(globalObject, rows);
        RETURN_IF_EXCEPTION(scope, { });
        if (!row)
            break;
        call(globalObject, writeRow, args[0], row);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// writer(fileobj, dialect='excel', /, **fmtparams)
PYTHON_NATIVE(csvWriter)
{
    NATIVE_PROLOGUE();
    if (!args.size() || args.size() > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, !args.size() ? String("writer expected at least 1 argument, got 0"_s) : concatenate("writer expected at most 2 arguments, got "_s, args.size())));
    JSValue write = getAttributeIfPresent(globalObject, args[0], Identifier::fromString(vm, "write"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (!write || !isCallable(globalObject, write))
        return JSValue::encode(raiseTypeError(globalObject, scope, "argument 1 must have a \"write\" method"_s));
    PyStateObject* dialect = callDialect(globalObject, args.size() > 1 ? args[1] : JSValue(), args);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = PyStateObject::create(vm, csvModuleState(globalObject).writerType->instanceStructure(), makeUnique<Writer>());
    object->state<Writer>().write.set(vm, object, write);
    object->state<Writer>().dialect.set(vm, object, dialect);
    return JSValue::encode(object);
}

// ---- The dialects that have names

PYTHON_NATIVE(csvListDialects)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    RELEASE_AND_RETURN(scope, JSValue::encode(listFromIterable(globalObject, csvModuleState(globalObject).dialects.get())));
}

// register_dialect(name, dialect=, /, **fmtparams)
PYTHON_NATIVE(csvRegisterDialect)
{
    NATIVE_PROLOGUE();
    if (!args.size() || args.size() > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, !args.size() ? String("register_dialect expected at least 1 argument, got 0"_s) : concatenate("register_dialect expected at most 2 arguments, got "_s, args.size())));
    if (!stringIn(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "dialect name must be a string"_s));
    PyStateObject* dialect = callDialect(globalObject, args.size() > 1 ? args[1] : JSValue(), args);
    RETURN_IF_EXCEPTION(scope, { });
    csvModuleState(globalObject).dialects->set(globalObject, args[0], dialect);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(csvUnregisterDialect)
{
    NATIVE_PROLOGUE();
    JSValue removed = csvModuleState(globalObject).dialects->remove(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    if (!removed)
        return JSValue::encode(raiseCSVError(globalObject, scope, "unknown dialect"_s));
    RETURN_NONE();
}

PYTHON_NATIVE(csvGetDialect)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(dialectFromRegistry(globalObject, args.at(0))));
}

// field_size_limit(new_limit=<none>)
PYTHON_NATIVE(csvFieldSizeLimit)
{
    NATIVE_PROLOGUE();
    auto& module = csvModuleState(globalObject);
    int64_t old = module.fieldLimit;
    if (JSValue given = args.at(0)) {
        if (typeOf(globalObject, given) != realm->typeInt())
            return JSValue::encode(raiseTypeError(globalObject, scope, "limit must be an integer"_s));
        auto limit = toSsizeOfInt(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        module.fieldLimit = *limit;
    }
    return JSValue::encode(intFromInt64(globalObject, old));
}

// ---- The module

JSObject* createCSVModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    auto& state = csvModuleState(globalObject);
    if (!state.dialectType) {
        auto make = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name) {
            PyType* type = createBuiltinType(globalObject, name, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType);
            type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
            slot.set(vm, realm, type);
            return type;
        };
        PyType* dialect = make(state.dialectType, "_csv.Dialect"_s);
        addMethods(globalObject, dialect, {
            { "__new__"_s, dialectNew, Kind::New, 0, "?(dialect=None, delimiter=None, doublequote=None, escapechar=None, lineterminator=None, quotechar=None, quoting=None, skipinitialspace=None, strict=None)"_s, Arguments::AreThoseOfTheClass },
            { "__reduce__"_s, dialectReduce, Kind::Method, 0, "($self, /, *args)"_s, Arguments::AreNotChecked },
            { "__reduce_ex__"_s, dialectReduce, Kind::Method, 0, "($self, /, *args)"_s, Arguments::AreNotChecked },
        });
        addMember(globalObject, dialect, "skipinitialspace"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOf<Dialect>(self).skipInitialSpace); });
        addMember(globalObject, dialect, "doublequote"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOf<Dialect>(self).doubleQuote); });
        addMember(globalObject, dialect, "strict"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOf<Dialect>(self).strict); });
        addGetSet(globalObject, dialect, "delimiter"_s, [] (JSGlobalObject* globalObject, JSValue self) { return characterOrNone(globalObject, stateOf<Dialect>(self).delimiter); });
        addGetSet(globalObject, dialect, "escapechar"_s, [] (JSGlobalObject* globalObject, JSValue self) { return characterOrNone(globalObject, stateOf<Dialect>(self).escapeCharacter); });
        addGetSet(globalObject, dialect, "lineterminator"_s, [] (JSGlobalObject*, JSValue self) { return stateOf<Dialect>(self).lineTerminator.get(); });
        addGetSet(globalObject, dialect, "quotechar"_s, [] (JSGlobalObject* globalObject, JSValue self) { return characterOrNone(globalObject, stateOf<Dialect>(self).quoteCharacter); });
        addGetSet(globalObject, dialect, "quoting"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<Dialect>(self).quoting); });

        PyType* reader = make(state.readerType, "_csv.reader"_s);
        addMethods(globalObject, reader, {
            { "__iter__"_s, readerIter, Kind::Wrapper },
            { "__next__"_s, readerNext, Kind::Wrapper },
        });
        addMember(globalObject, reader, "dialect"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Reader>(self).dialect.get(); });
        addMember(globalObject, reader, "line_num"_s, [] (JSGlobalObject* globalObject, JSValue self) { return intFromUInt64(globalObject, stateOf<Reader>(self).lineNumber); });

        PyType* writer = make(state.writerType, "_csv.writer"_s);
        addMethods(globalObject, writer, {
            { "writerow"_s, writerWriteRow },
            { "writerows"_s, writerWriteRows },
        });
        addMember(globalObject, writer, "dialect"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Writer>(self).dialect.get(); });

        state.dialects.set(vm, realm, PyDict::create(globalObject));
        // It is made as the classes above are, and not as a class statement would make it, so there are no weak references to one.
        state.error.set(vm, realm, createBuiltinType(globalObject, "_csv.Error"_s, realm->type(BuiltinType::Exception), PyType::Layout::Exception, PyType::IsBaseType | PyType::IsExceptionType));
    }

    JSObject* module = newBuiltinModule(globalObject, "_csv"_s);
    addFunction(globalObject, module, "reader"_s, csvReader, 0, { }, Arguments::AreNotChecked);
    addFunction(globalObject, module, "writer"_s, csvWriter, 0, { }, Arguments::AreNotChecked);
    addFunction(globalObject, module, "register_dialect"_s, csvRegisterDialect, 0, { }, Arguments::AreNotChecked);
    addFunction(globalObject, module, "list_dialects"_s, csvListDialects);
    addFunction(globalObject, module, "unregister_dialect"_s, csvUnregisterDialect);
    addFunction(globalObject, module, "get_dialect"_s, csvGetDialect);
    addFunction(globalObject, module, "field_size_limit"_s, csvFieldSizeLimit);
    auto add = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };
    add("Dialect"_s, state.dialectType->object());
    add("Reader"_s, state.readerType->object());
    add("Writer"_s, state.writerType->object());
    add("_dialects"_s, state.dialects.get());
    for (auto& [name, style] : quoteStyles)
        add(name, jsNumber(static_cast<int>(style)));
    add("Error"_s, state.error->object());
    return module;
}

} } // namespace JSC::Python
