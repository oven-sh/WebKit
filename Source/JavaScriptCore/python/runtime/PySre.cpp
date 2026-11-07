/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBuffer.h"
#include "PyBytes.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PySreMatcher.h"
#include "PyStr.h"
#include "PyTuple.h"
#include <unicode/ustring.h>
#include <wtf/HexNumber.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _sre: what Lib/re needs that is not written in Python. Lib/re compiles patterns to code for the engine in
// PySreMatcher.cpp; here are the objects around it: re.Pattern, re.Match, the scanner and the compiled replacement template.

// ---- Characters ----

uint8_t sreLatin1Properties[256];

bool sreIsUnicodeDigitSlow(SreCode ch) { return Unicode::isDecimal(ch); }
bool sreIsUnicodeSpaceSlow(SreCode ch) { return isPythonWhitespace(ch); }
bool sreIsUnicodeWordSlow(SreCode ch) { return Unicode::isAlpha(ch) || Unicode::isNumeric(ch) || ch == '_'; }

// Where the upper case of a character is several characters, CPython takes the first of them: 'S' for U+00DF.
SreCode sreUpperUnicodeSlow(SreCode ch) { return Unicode::mapCaseToOne(ch, Unicode::CaseMapping::Upper); }

static void sreInitializeCharacterProperties()
{
    for (SreCode ch = 0; ch < 256; ++ch) {
        uint8_t properties = 0;
        if (sreIsUnicodeDigitSlow(ch))
            properties |= SreCharacterIsDigit;
        if (sreIsUnicodeSpaceSlow(ch))
            properties |= SreCharacterIsSpace;
        if (sreIsUnicodeWordSlow(ch))
            properties |= SreCharacterIsWord;
        if ((ch >= 0x0a && ch <= 0x0d) || (ch >= 0x1c && ch <= 0x1e) || ch == 0x85)
            properties |= SreCharacterIsLineBreak;
        sreLatin1Properties[ch] = properties;
    }
}

// ---- The types ----

namespace SrePattern {
enum : unsigned {
    Source, // What was compiled: a str, something bytes-like, or None.
    GroupIndex, // {name: number}. Empty: no group has a name.
    IndexGroup, // (None, name or None...). Empty likewise.
    Code, // A PyByteStorage of SreCode.
    CodeSize,
    Flags,
    Groups,
    IsBytes, // 1: the pattern is for bytes. 0: for str. -1: there is no telling.
    FieldCount
};
}

namespace SreMatch {
// `number` is how many groups there are, the whole match being one.
enum : unsigned {
    String,
    Pattern,
    Regs, // Empty until asked for.
    CharacterSize, // See SreText. 0: the subject is not a str.
    Wide, // Its UTF-32, if that is what was matched.
    Pos,
    EndPos,
    LastIndex,
    Marks // And on: where each group begins and ends. -1: it took no part in the match.
};
}

namespace SreScanner {
enum : unsigned {
    Pattern,
    String,
    Kept, // For a str, its UTF-32 if it has one: see SreText. For what might be resized, a memoryview of it, which prevents that.
    CharacterSize,
    Pos,
    EndPos,
    Start, // Where to go on from. -1: there is nothing more to find.
    MustAdvance,
    FieldCount
};
}

namespace SreTemplate {
enum : unsigned {
    Literal,
    Items // And on: pairs of a group number and the literal that follows it, empty if that is "".
};
}

// What the module remembers from one call to the next.
namespace SreModuleState {
enum : unsigned {
    CompileTemplate, // re._compile_template, once it has been needed.
    // Programs that tokenize call match(string, pos) once for each token, and what follows takes reading all of the string.
    SimpleSubject, // The last long str of 16-bit characters that was found to have no surrogate pairs,
    WideSubject, // the last that was found to have some,
    Wide, // and its UTF-32, of `number` characters.
    FieldCount
};
}

static PyRecord* sreModuleState(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (JSValue state = rt.slot(Runtime::Slot::Sre))
        return PyRecord::from(state);
    PyRecord* state = PyRecord::create(globalObject, rt.objectType(), SreModuleState::FieldCount);
    rt.setSlot(Runtime::Slot::Sre, state);
    return state;
}

static PyType* srePatternType(JSGlobalObject*);
static PyType* sreMatchType(JSGlobalObject*);
static PyType* sreScannerType(JSGlobalObject*);
static PyType* sreTemplateType(JSGlobalObject*);

static ALWAYS_INLINE ptrdiff_t sreNumberField(PyRecord* record, unsigned field) { return static_cast<ptrdiff_t>(record->get(field).asNumber()); }
static ALWAYS_INLINE const SreCode* sreCodeOf(PyRecord* pattern)
{
    return reinterpret_cast<const SreCode*>(uncheckedDowncast<PyByteStorage>(pattern->get(SrePattern::Code).asCell())->data());
}

static JSValue sreIdentity(JSGlobalObject*, JSValue thisValue) { return thisValue; }
static JSValue sreDeepCopy(JSGlobalObject*, JSValue thisValue, JSValue) { return thisValue; }

// As Argument Clinic converts a Py_ssize_t.
static bool sreToSsize(JSGlobalObject* globalObject, JSValue value, ptrdiff_t defaultValue, ptrdiff_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        result = defaultValue;
        return true;
    }
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto number = tryInt64(intValue(index));
    if (!number) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
        return false;
    }
    result = *number;
    return true;
}

// What "must be x, not y" calls the type of an argument.
static String sreArgumentTypeName(JSGlobalObject* globalObject, JSValue value)
{
    return isNone(value) ? "None"_s : typeName(globalObject, value);
}

static constexpr ptrdiff_t sreMaxSsize = std::numeric_limits<ptrdiff_t>::max();

// ---- Subjects ----

// The characters of a str or of something bytes-like, in a form the engine can match.
//
// Positions in a str count code points. A JSString that has no surrogate pairs is matched where it is; one that has is first
// decoded to UTF-32.
//
// It must be on the stack: that is how the collector finds what it refers to.
struct SreText {
    WTF_MAKE_NONCOPYABLE(SreText);
public:
    explicit SreText(JSGlobalObject* globalObject)
        : globalObject(globalObject)
    {
    }
    ~SreText() { releaseBuffer(globalObject, buffer); }

    JSGlobalObject* globalObject;
    bool isBytes { false };
    unsigned characterSize { 1 };
    const void* characters { nullptr };
    ptrdiff_t length { 0 };
    String string; // Of a str: it keeps the characters.
    PyByteStorage* wide { nullptr }; // The UTF-32.
    Buffer buffer; // Of anything else. While it is held, a bytearray cannot be resized.
};

static PyByteStorage* sreDecode(JSGlobalObject* globalObject, std::span<const char16_t> units, ptrdiff_t& length)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyByteStorage* storage = PyByteStorage::tryCreate(vm, runtime(globalObject).structure<PyByteStorage>(), units.size() * sizeof(char32_t));
    if (!storage) {
        raiseWithArgs(globalObject, scope, runtime(globalObject).MemoryError(), { });
        return nullptr;
    }
    char32_t* out = reinterpret_cast<char32_t*>(storage->data());
    size_t count = 0;
    for (size_t i = 0; i < units.size(); ++i) {
        char32_t c = units[i];
        if (U16_IS_LEAD(c) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1])) {
            c = U16_GET_SUPPLEMENTARY(c, units[i + 1]);
            ++i;
        }
        out[count++] = c;
    }
    length = static_cast<ptrdiff_t>(count);
    return storage;
}

// To the engine a null position is one that has not been set, so even nothing must be somewhere.
static const void* sreNotNull(const void* characters) { return characters ? characters : ""; }

static void sreSetCharactersFromBuffer(SreText& text)
{
    text.characters = sreNotNull(text.buffer.data);
    text.length = static_cast<ptrdiff_t>(text.buffer.length);
}

// False: it is neither a str nor bytes-like, which is a TypeError. A scanner, which does this for every match, passes what
// it found out the first time.
static bool sreGetText(JSGlobalObject* globalObject, JSValue value, SreText& text, PyRecord* scanner = nullptr)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (isStr(value)) {
        JSString* subject = strValue(value);
        text.isBytes = false;
        text.string = subject->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (text.string.isNull())
            text.string = emptyString();
        if (text.string.is8Bit()) {
            text.characterSize = 1;
            text.characters = sreNotNull(text.string.span8().data());
            text.length = text.string.length();
            return true;
        }

        // Not worth remembering, or too much to hold on to.
        bool remember = text.string.length() >= 256 && text.string.length() <= 4 * MB;
        PyRecord* remembered = scanner ? nullptr : sreModuleState(globalObject);
        bool isSimple;
        if (scanner)
            isSimple = sreNumberField(scanner, SreScanner::CharacterSize) == 2;
        else if (remembered->get(SreModuleState::SimpleSubject) == JSValue(subject))
            isSimple = true;
        else if (remembered->get(SreModuleState::WideSubject) == JSValue(subject))
            isSimple = false;
        else {
            isSimple = StrView(globalObject, text.string).isSimple();
            if (isSimple && remember)
                remembered->set(vm, SreModuleState::SimpleSubject, subject);
        }
        if (isSimple) {
            text.characterSize = 2;
            text.characters = sreNotNull(text.string.span16().data());
            text.length = text.string.length();
            return true;
        }

        text.characterSize = 4;
        if (scanner) {
            text.wide = uncheckedDowncast<PyByteStorage>(scanner->get(SreScanner::Kept).asCell());
            text.length = static_cast<ptrdiff_t>(scanner->number);
        } else if (remembered->get(SreModuleState::WideSubject) == JSValue(subject)) {
            text.wide = uncheckedDowncast<PyByteStorage>(remembered->get(SreModuleState::Wide).asCell());
            text.length = static_cast<ptrdiff_t>(remembered->number);
        } else {
            text.wide = sreDecode(globalObject, text.string.span16(), text.length);
            RETURN_IF_EXCEPTION(scope, false);
            if (remember) {
                remembered->set(vm, SreModuleState::WideSubject, subject);
                remembered->set(vm, SreModuleState::Wide, text.wide);
                remembered->number = text.length;
            }
        }
        text.characters = text.wide->data();
        return true;
    }

    JSValue exporter = scanner ? scanner->get(SreScanner::Kept) : JSValue();
    if (!failed(scope, getBuffer(globalObject, exporter ? exporter : value, text.buffer, BufferSimple))) {
        text.isBytes = true;
        text.characterSize = 1;
        sreSetCharactersFromBuffer(text);
        return true;
    }
    // Whatever went wrong.
    takePendingException(globalObject);
    raiseTypeError(globalObject, scope, "expected string or bytes-like object, got '"_s, typeName(globalObject, value), '\'');
    return false;
}

// subject[start:end], where `wide` is the UTF-32 of the subject if positions are not those of its UTF-16.
// Nothing, with a MemoryError: there is no room for it.
static JSValue sreSliceOfStr(JSGlobalObject* globalObject, JSString* subject, const String& string, PyByteStorage* wide, ptrdiff_t start, ptrdiff_t end)
{
    VM& vm = getVM(globalObject);
    if (start >= end)
        return jsEmptyString(vm);
    if (!wide) {
        if (!start && end == static_cast<ptrdiff_t>(string.length()))
            return subject;
        return partOfStr(vm, string, static_cast<unsigned>(start), static_cast<unsigned>(end - start));
    }
    const char32_t* characters = reinterpret_cast<const char32_t*>(wide->data());
    StrBuilder builder;
    for (ptrdiff_t i = start; i < end; ++i)
        appendCodePoint(builder, characters[i]);
    if (!builder.hasOverflowed() && builder.length() == string.length())
        return subject;
    return makeStr(globalObject, builder);
}

static JSValue sreSliceOfText(JSGlobalObject* globalObject, const SreText& text, JSValue object, ptrdiff_t start, ptrdiff_t end)
{
    if (text.isBytes) {
        if (!start && end == text.length && typeOf(globalObject, object) == runtime(globalObject).bytesType())
            return object;
        return PyBytes::create(globalObject, std::span { static_cast<const uint8_t*>(text.characters) + start, static_cast<size_t>(end - start) });
    }
    return sreSliceOfStr(globalObject, strValue(object), text.string, text.wide, start, std::min(end, text.length));
}

// All that a match or a series of matches over one subject needs. It must be on the stack: that is how the collector finds
// what it refers to.
struct SreRun {
    WTF_MAKE_NONCOPYABLE(SreRun);
public:
    explicit SreRun(JSGlobalObject* globalObject)
        : text(globalObject)
    {
    }

    SreState state;
    SreText text;
    JSValue string;
    ptrdiff_t pos { 0 };
    ptrdiff_t endpos { 0 };

    ptrdiff_t startIndex() const { return state.indexOf(state.start); }
    ptrdiff_t ptrIndex() const { return state.indexOf(state.ptr); }
    JSValue slice(JSGlobalObject* globalObject, ptrdiff_t start, ptrdiff_t end) const { return sreSliceOfText(globalObject, text, string, start, end); }

    void setPositions(ptrdiff_t start, ptrdiff_t ptr, ptrdiff_t end)
    {
        state.beginning = text.characters;
        state.start = state.positionAt(start);
        state.ptr = state.positionAt(ptr);
        state.end = state.positionAt(end);
        state.limit = state.positionAt(text.length);
    }

    // After code of the program has run: memory of WebAssembly, which is not pinned, may be elsewhere now. False: a BufferError.
    bool refresh(JSGlobalObject* globalObject)
    {
        if (!text.buffer.object)
            return true;
        ptrdiff_t start = startIndex();
        ptrdiff_t ptr = ptrIndex();
        ptrdiff_t end = state.indexOf(state.end);
        if (!refreshBuffer(globalObject, text.buffer))
            return false;
        sreSetCharactersFromBuffer(text);
        setPositions(start, ptr, end);
        return true;
    }
};

static bool sreBegin(JSGlobalObject* globalObject, SreRun& run, PyRecord* pattern, JSValue string, ptrdiff_t start, ptrdiff_t end, PyRecord* scanner = nullptr)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!run.state.allocateMarks(static_cast<size_t>(sreNumberField(pattern, SrePattern::Groups)))) {
        raiseWithArgs(globalObject, scope, runtime(globalObject).MemoryError(), { });
        return false;
    }
    bool ok = sreGetText(globalObject, string, run.text, scanner);
    RETURN_IF_EXCEPTION(scope, false);
    ASSERT_UNUSED(ok, ok);

    ptrdiff_t patternIsBytes = sreNumberField(pattern, SrePattern::IsBytes);
    if (run.text.isBytes && !patternIsBytes) {
        raiseTypeError(globalObject, scope, "cannot use a string pattern on a bytes-like object"_s);
        return false;
    }
    if (!run.text.isBytes && patternIsBytes > 0) {
        raiseTypeError(globalObject, scope, "cannot use a bytes pattern on a string-like object"_s);
        return false;
    }

    start = std::clamp<ptrdiff_t>(start, 0, run.text.length);
    end = std::clamp<ptrdiff_t>(end, 0, run.text.length);
    run.string = string;
    run.pos = start;
    run.endpos = end;
    run.state.characterSize = run.text.characterSize;
    run.setPositions(start, start, end);
    return true;
}

static JSValue sreRaiseEngineError(JSGlobalObject* globalObject, ThrowScope& scope, ptrdiff_t status)
{
    if (status == sreErrorMemory)
        return raiseWithArgs(globalObject, scope, runtime(globalObject).MemoryError(), { });
    return raiseRuntimeError(globalObject, scope, "internal error in regular expression engine"_s);
}

static JSValue sreRaiseWrongSpan(JSGlobalObject* globalObject, ThrowScope& scope)
{
    return raiseSystemError(globalObject, scope, "The span of capturing group is wrong, please report a bug for the re module."_s);
}

// What the group matched. If it took no part in the match: "" if `emptyIfUnset`, else None.
static JSValue sreGroupOfRun(JSGlobalObject* globalObject, const SreRun& run, ptrdiff_t group, bool emptyIfUnset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ptrdiff_t mark = (group - 1) * 2;
    ptrdiff_t start = 0;
    ptrdiff_t end = 0;
    if (mark >= run.state.lastMark || !run.state.marks[mark] || !run.state.marks[mark + 1]) {
        if (!emptyIfUnset)
            return none();
    } else {
        start = run.state.indexOf(run.state.marks[mark]);
        end = run.state.indexOf(run.state.marks[mark + 1]);
        if (start > end)
            return sreRaiseWrongSpan(globalObject, scope);
    }
    RELEASE_AND_RETURN(scope, run.slice(globalObject, start, end));
}

static JSValue sreNewMatch(JSGlobalObject* globalObject, PyRecord* pattern, const SreRun& run, ptrdiff_t status)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!status)
        return none();
    if (status < 0)
        return sreRaiseEngineError(globalObject, scope, status);

    ptrdiff_t groups = sreNumberField(pattern, SrePattern::Groups);
    PyRecord* match = PyRecord::create(globalObject, sreMatchType(globalObject), static_cast<unsigned>(SreMatch::Marks + 2 * (groups + 1)));
    match->number = groups + 1;
    match->set(vm, SreMatch::String, run.string);
    match->set(vm, SreMatch::Pattern, pattern);
    match->set(vm, SreMatch::CharacterSize, jsNumber(run.text.isBytes ? 0 : run.text.characterSize));
    if (run.text.wide)
        match->set(vm, SreMatch::Wide, run.text.wide);
    match->set(vm, SreMatch::Pos, jsNumber(run.pos));
    match->set(vm, SreMatch::EndPos, jsNumber(run.endpos));
    match->set(vm, SreMatch::LastIndex, jsNumber(run.state.lastIndex));
    match->set(vm, SreMatch::Marks, jsNumber(run.startIndex()));
    match->set(vm, SreMatch::Marks + 1, jsNumber(run.ptrIndex()));
    for (ptrdiff_t mark = 0; mark < 2 * groups; mark += 2) {
        ptrdiff_t start = -1;
        ptrdiff_t end = -1;
        if (mark + 1 <= run.state.lastMark && run.state.marks[mark] && run.state.marks[mark + 1]) {
            start = run.state.indexOf(run.state.marks[mark]);
            end = run.state.indexOf(run.state.marks[mark + 1]);
            if (start > end)
                return sreRaiseWrongSpan(globalObject, scope);
        }
        match->set(vm, static_cast<unsigned>(SreMatch::Marks + 2 + mark), jsNumber(start));
        match->set(vm, static_cast<unsigned>(SreMatch::Marks + 3 + mark), jsNumber(end));
    }
    return match;
}

// ---- re.Match ----

static ALWAYS_INLINE ptrdiff_t sreMarkOf(PyRecord* match, ptrdiff_t index) { return sreNumberField(match, static_cast<unsigned>(SreMatch::Marks + index)); }

static JSValue sreGroupByIndex(JSGlobalObject* globalObject, PyRecord* match, ptrdiff_t group, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue string = match->get(SreMatch::String);
    ptrdiff_t start = sreMarkOf(match, 2 * group);
    if (isNone(string) || start < 0)
        return defaultValue;
    ptrdiff_t end = sreMarkOf(match, 2 * group + 1);

    if (match->get(SreMatch::CharacterSize).asInt32()) {
        JSString* subject = strValue(string);
        String text = subject->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue wide = match->get(SreMatch::Wide);
        RELEASE_AND_RETURN(scope, sreSliceOfStr(globalObject, subject, text, wide ? uncheckedDowncast<PyByteStorage>(wide.asCell()) : nullptr, start, end));
    }

    // A bytearray may have shrunk since.
    SreText text(globalObject);
    bool ok = sreGetText(globalObject, string, text);
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT_UNUSED(ok, ok);
    start = std::min(start, text.length);
    end = std::min(end, text.length);
    RELEASE_AND_RETURN(scope, sreSliceOfText(globalObject, text, string, start, end));
}

// The number of the group that a number or a name stands for. -1: an exception.
static ptrdiff_t sreGroupIndex(JSGlobalObject* globalObject, PyRecord* match, JSValue group)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!group)
        return 0;
    ptrdiff_t index = -1;
    if (hasIndex(globalObject, group)) {
        index = toSsize(globalObject, group, nullptr);
        RETURN_IF_EXCEPTION(scope, -1);
    } else if (JSValue groupIndex = PyRecord::from(match->get(SreMatch::Pattern))->get(SrePattern::GroupIndex)) {
        JSValue number = PyDict::from(groupIndex)->get(globalObject, group);
        RETURN_IF_EXCEPTION(scope, -1);
        if (number && isInt(number)) {
            index = toSsize(globalObject, number, nullptr);
            RETURN_IF_EXCEPTION(scope, -1);
        }
    }
    if (index < 0 || index >= match->number) {
        raiseIndexError(globalObject, scope, "no such group"_s);
        return -1;
    }
    return index;
}

static JSValue sreGroup(JSGlobalObject* globalObject, PyRecord* match, JSValue group, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ptrdiff_t index = sreGroupIndex(globalObject, match, group);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, sreGroupByIndex(globalObject, match, index, defaultValue));
}

static JSValue sreMatchGroup(JSGlobalObject* globalObject, JSValue thisValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* match = PyRecord::from(thisValue);
    if (failed(scope, rejectKeywords(globalObject, "group"_s, args)))
        return { };
    if (!args.size())
        RELEASE_AND_RETURN(scope, sreGroupByIndex(globalObject, match, 0, none()));
    if (args.size() == 1)
        RELEASE_AND_RETURN(scope, sreGroup(globalObject, match, args[0], none()));
    PyTuple* result = PyTuple::create(globalObject, args.size());
    for (unsigned i = 0; i < args.size(); ++i) {
        JSValue item = sreGroup(globalObject, match, args[i], none());
        RETURN_IF_EXCEPTION(scope, { });
        result->initializeAt(vm, i, item);
    }
    return result;
}

static JSValue sreMatchGetItem(JSGlobalObject* globalObject, JSValue thisValue, JSValue group)
{
    return sreGroup(globalObject, PyRecord::from(thisValue), group, none());
}

static JSValue sreMatchGroups(JSGlobalObject* globalObject, JSValue thisValue, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* match = PyRecord::from(thisValue);
    if (!defaultValue)
        defaultValue = none();
    PyTuple* result = PyTuple::create(globalObject, static_cast<unsigned>(match->number - 1));
    for (ptrdiff_t group = 1; group < match->number; ++group) {
        JSValue item = sreGroupByIndex(globalObject, match, group, defaultValue);
        RETURN_IF_EXCEPTION(scope, { });
        result->initializeAt(vm, static_cast<unsigned>(group - 1), item);
    }
    return result;
}

static JSValue sreMatchGroupDict(JSGlobalObject* globalObject, JSValue thisValue, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* match = PyRecord::from(thisValue);
    if (!defaultValue)
        defaultValue = none();
    PyDict* result = PyDict::create(globalObject);
    JSValue groupIndex = PyRecord::from(match->get(SreMatch::Pattern))->get(SrePattern::GroupIndex);
    if (!groupIndex)
        return result;
    unsigned position = 0;
    JSValue name;
    JSValue number;
    while (PyDict::from(groupIndex)->next(position, name, number)) {
        JSValue item = sreGroup(globalObject, match, name, defaultValue);
        RETURN_IF_EXCEPTION(scope, { });
        result->set(globalObject, name, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue sreMatchStart(JSGlobalObject* globalObject, JSValue thisValue, JSValue group)
{
    PyRecord* match = PyRecord::from(thisValue);
    ptrdiff_t index = sreGroupIndex(globalObject, match, group);
    if (index < 0)
        return { };
    return jsNumber(sreMarkOf(match, 2 * index));
}

static JSValue sreMatchEnd(JSGlobalObject* globalObject, JSValue thisValue, JSValue group)
{
    PyRecord* match = PyRecord::from(thisValue);
    ptrdiff_t index = sreGroupIndex(globalObject, match, group);
    if (index < 0)
        return { };
    return jsNumber(sreMarkOf(match, 2 * index + 1));
}

static JSValue sreSpanOf(JSGlobalObject* globalObject, PyRecord* match, ptrdiff_t group)
{
    return PyTuple::create(globalObject, { jsNumber(sreMarkOf(match, 2 * group)), jsNumber(sreMarkOf(match, 2 * group + 1)) });
}

static JSValue sreMatchSpan(JSGlobalObject* globalObject, JSValue thisValue, JSValue group)
{
    PyRecord* match = PyRecord::from(thisValue);
    ptrdiff_t index = sreGroupIndex(globalObject, match, group);
    if (index < 0)
        return { };
    return sreSpanOf(globalObject, match, index);
}

static JSValue sreMatchRegs(JSGlobalObject* globalObject, JSValue thisValue)
{
    VM& vm = getVM(globalObject);
    PyRecord* match = PyRecord::from(thisValue);
    if (JSValue regs = match->get(SreMatch::Regs))
        return regs;
    PyTuple* regs = PyTuple::create(globalObject, static_cast<unsigned>(match->number));
    for (ptrdiff_t group = 0; group < match->number; ++group)
        regs->initializeAt(vm, static_cast<unsigned>(group), sreSpanOf(globalObject, match, group));
    match->set(vm, SreMatch::Regs, regs);
    return regs;
}

static JSValue sreMatchLastIndex(JSGlobalObject*, JSValue thisValue)
{
    JSValue lastIndex = PyRecord::from(thisValue)->get(SreMatch::LastIndex);
    return lastIndex.asNumber() >= 0 ? lastIndex : none();
}

static JSValue sreMatchLastGroup(JSGlobalObject*, JSValue thisValue)
{
    PyRecord* match = PyRecord::from(thisValue);
    ptrdiff_t lastIndex = sreNumberField(match, SreMatch::LastIndex);
    JSValue indexGroup = PyRecord::from(match->get(SreMatch::Pattern))->get(SrePattern::IndexGroup);
    if (indexGroup && lastIndex >= 0 && lastIndex < static_cast<ptrdiff_t>(PyTuple::from(indexGroup)->length()))
        return PyTuple::from(indexGroup)->at(static_cast<unsigned>(lastIndex));
    return none();
}

template<unsigned field>
static JSValue sreField(JSGlobalObject*, JSValue thisValue) { return PyRecord::from(thisValue)->get(field); }

static JSValue sreMatchRepr(JSGlobalObject* globalObject, JSValue thisValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* match = PyRecord::from(thisValue);
    JSValue whole = sreGroupByIndex(globalObject, match, 0, none());
    RETURN_IF_EXCEPTION(scope, { });
    String wholeRepr = shortRepr(globalObject, whole, 50);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString("<re.Match object; span=("_s, sreMarkOf(match, 0), ", "_s, sreMarkOf(match, 1), "), match="_s, wholeRepr, '>'));
}

// ---- Replacement templates ----

// re._compile_template(pattern, template): Lib/re parses the template, and makes the object with _sre.template().
static PyRecord* sreCompileTemplate(JSGlobalObject* globalObject, PyRecord* pattern, JSValue source)
{
    VM& vm = getVM(globalObject);
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* state = sreModuleState(globalObject);
    JSValue function = state->get(SreModuleState::CompileTemplate);
    if (!function) {
        JSValue module = importModule(globalObject, "re"_s);
        RETURN_IF_EXCEPTION(scope, nullptr);
        function = getAttr(globalObject, module, makeStr(vm, "_compile_template"_s));
        RETURN_IF_EXCEPTION(scope, nullptr);
        state->set(vm, SreModuleState::CompileTemplate, function);
    }

    JSValue result = call(globalObject, function, JSValue(pattern), source);
    if (scope.exception() && pendingExceptionMatches(globalObject, rt.TypeError())) {
        // The function caches what it returns, and the template may be unhashable: a bytearray is.
        if (isStr(source) && !isExactStr(source)) {
            takePendingException(globalObject);
            source = strValue(source);
        } else if (isBytesLike(source) && typeOf(globalObject, source) != rt.bytesType()) {
            takePendingException(globalObject);
            source = bytesFromObject(globalObject, source);
            RETURN_IF_EXCEPTION(scope, nullptr);
        } else
            return nullptr;
        result = call(globalObject, function, JSValue(pattern), source);
    }
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (typeOf(globalObject, result) != sreTemplateType(globalObject)) {
        raiseRuntimeError(globalObject, scope, "the result of compiling a replacement string is "_s, typeName(globalObject, result));
        return nullptr;
    }
    return PyRecord::from(result);
}

static JSValue sreJoin(JSGlobalObject* globalObject, bool isBytes, JSArray* list)
{
    VM& vm = getVM(globalObject);
    if (isBytes)
        return callMethod(globalObject, PyBytes::create(globalObject, std::span<const uint8_t> { }), makeStr(vm, "join"_s), JSValue(list));
    return strJoin(globalObject, jsEmptyString(vm), list);
}

static JSValue sreExpandTemplate(JSGlobalObject* globalObject, PyRecord* replacement, PyRecord* match)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue literal = replacement->get(SreTemplate::Literal);
    if (replacement->fieldCount() == SreTemplate::Items)
        return literal;

    JSArray* list = newList(globalObject, replacement->fieldCount());
    RETURN_IF_EXCEPTION(scope, { });
    listAppend(globalObject, list, literal);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = SreTemplate::Items; i < replacement->fieldCount(); i += 2) {
        ptrdiff_t group = sreNumberField(replacement, i);
        if (group >= match->number)
            return raiseIndexError(globalObject, scope, "no such group"_s);
        JSValue item = sreGroupByIndex(globalObject, match, group, none());
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNone(item)) {
            listAppend(globalObject, list, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (JSValue next = replacement->get(i + 1)) {
            listAppend(globalObject, list, next);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, sreJoin(globalObject, !isStr(literal), list));
}

static JSValue sreMatchExpand(JSGlobalObject* globalObject, JSValue thisValue, JSValue source)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* match = PyRecord::from(thisValue);
    PyRecord* replacement = sreCompileTemplate(globalObject, PyRecord::from(match->get(SreMatch::Pattern)), source);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, sreExpandTemplate(globalObject, replacement, match));
}

static constexpr NativeDef sreMatchMethods[] = {
    PY_SLOT("__repr__", sreMatchRepr, "($self, /)"),
    PY_SLOT("__getitem__", sreMatchGetItem, "($self, key, /)"),
    PY_METHOD("group", sreMatchGroup, "($self, /, *args)"),
    PY_METHOD("start", sreMatchStart, "($self, group=0, /)"),
    PY_METHOD("end", sreMatchEnd, "($self, group=0, /)"),
    PY_METHOD("span", sreMatchSpan, "($self, group=0, /)"),
    PY_METHOD("groups", sreMatchGroups, "($self, /, default=None)"),
    PY_METHOD("groupdict", sreMatchGroupDict, "($self, /, default=None)"),
    PY_METHOD("expand", sreMatchExpand, "($self, /, template)"),
    PY_METHOD("__copy__", sreIdentity, "($self, /)"),
    PY_METHOD("__deepcopy__", sreDeepCopy, "($self, memo, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet sreMatchGetSets[] = {
    PY_MEMBER("string", sreField<SreMatch::String>),
    PY_MEMBER("re", sreField<SreMatch::Pattern>),
    PY_MEMBER("pos", sreField<SreMatch::Pos>),
    PY_MEMBER("endpos", sreField<SreMatch::EndPos>),
    { "lastindex"_s, sreMatchLastIndex },
    { "lastgroup"_s, sreMatchLastGroup },
    { "regs"_s, sreMatchRegs },
};

static constexpr TypeDef sreMatchTypeDef {
    "re.Match"_s, { }, sreMatchMethods, sreMatchGetSets,
    "The result of re.search(), re.prefixmatch(), and re.fullmatch().\nMatch objects always have a boolean value of True."_s
};
static PyType* sreMatchType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(sreMatchTypeDef); }

// ---- re.Pattern ----

enum class SreMode : uint8_t { PrefixMatch, FullMatch, Search };

static JSValue sreMatchOnce(JSGlobalObject* globalObject, JSValue thisValue, JSValue string, JSValue pos, JSValue endpos, SreMode mode)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* pattern = PyRecord::from(thisValue);
    ptrdiff_t start;
    ptrdiff_t end;
    if (failed(scope, sreToSsize(globalObject, pos, 0, start)) || failed(scope, sreToSsize(globalObject, endpos, sreMaxSsize, end)))
        return { };

    SreRun run(globalObject);
    if (failed(scope, sreBegin(globalObject, run, pattern, string, start, end)))
        return { };
    run.state.ptr = run.state.start;
    run.state.matchAll = mode == SreMode::FullMatch;
    ptrdiff_t status = mode == SreMode::Search ? sreSearch(run.state, sreCodeOf(pattern)) : sreMatch(run.state, sreCodeOf(pattern));
    ensureStillAliveHere(pattern);
    RELEASE_AND_RETURN(scope, sreNewMatch(globalObject, pattern, run, status));
}

static JSValue srePatternPrefixMatch(JSGlobalObject* globalObject, JSValue thisValue, JSValue string, JSValue pos, JSValue endpos)
{
    return sreMatchOnce(globalObject, thisValue, string, pos, endpos, SreMode::PrefixMatch);
}

static JSValue srePatternFullMatch(JSGlobalObject* globalObject, JSValue thisValue, JSValue string, JSValue pos, JSValue endpos)
{
    return sreMatchOnce(globalObject, thisValue, string, pos, endpos, SreMode::FullMatch);
}

static JSValue srePatternSearch(JSGlobalObject* globalObject, JSValue thisValue, JSValue string, JSValue pos, JSValue endpos)
{
    return sreMatchOnce(globalObject, thisValue, string, pos, endpos, SreMode::Search);
}

// The next match of a series. After it, a match may begin where this one ended, but not be empty there if this one was.
static ALWAYS_INLINE ptrdiff_t sreSearchNext(SreRun& run, PyRecord* pattern)
{
    run.state.reset();
    run.state.ptr = run.state.start;
    return sreSearch(run.state, sreCodeOf(pattern));
}
static ALWAYS_INLINE void sreMoveOn(SreRun& run)
{
    run.state.mustAdvance = run.state.ptr == run.state.start;
    run.state.start = run.state.ptr;
}

static JSValue srePatternFindAll(JSGlobalObject* globalObject, JSValue thisValue, JSValue string, JSValue pos, JSValue endpos)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* pattern = PyRecord::from(thisValue);
    ptrdiff_t start;
    ptrdiff_t end;
    if (failed(scope, sreToSsize(globalObject, pos, 0, start)) || failed(scope, sreToSsize(globalObject, endpos, sreMaxSsize, end)))
        return { };

    SreRun run(globalObject);
    if (failed(scope, sreBegin(globalObject, run, pattern, string, start, end)))
        return { };
    ptrdiff_t groups = sreNumberField(pattern, SrePattern::Groups);
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    while (run.state.start <= run.state.end) {
        ptrdiff_t status = sreSearchNext(run, pattern);
        if (status < 0)
            return sreRaiseEngineError(globalObject, scope, status);
        if (!status)
            break;

        JSValue item;
        if (!groups)
            item = run.slice(globalObject, run.startIndex(), run.ptrIndex());
        else if (groups == 1)
            item = sreGroupOfRun(globalObject, run, 1, true);
        else {
            PyTuple* tuple = PyTuple::create(globalObject, static_cast<unsigned>(groups));
            for (ptrdiff_t group = 1; group <= groups; ++group) {
                JSValue part = sreGroupOfRun(globalObject, run, group, true);
                RETURN_IF_EXCEPTION(scope, { });
                tuple->initializeAt(vm, static_cast<unsigned>(group - 1), part);
            }
            item = tuple;
        }
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, list, item);
        RETURN_IF_EXCEPTION(scope, { });
        sreMoveOn(run);
    }
    ensureStillAliveHere(pattern);
    return list;
}

static JSValue srePatternSplit(JSGlobalObject* globalObject, JSValue thisValue, JSValue string, JSValue maxSplitValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* pattern = PyRecord::from(thisValue);
    ptrdiff_t maxSplit;
    if (failed(scope, sreToSsize(globalObject, maxSplitValue, 0, maxSplit)))
        return { };

    SreRun run(globalObject);
    if (failed(scope, sreBegin(globalObject, run, pattern, string, 0, sreMaxSsize)))
        return { };
    ptrdiff_t groups = sreNumberField(pattern, SrePattern::Groups);
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    ptrdiff_t count = 0;
    ptrdiff_t last = run.startIndex();
    while (!maxSplit || count < maxSplit) {
        ptrdiff_t status = sreSearchNext(run, pattern);
        if (status < 0)
            return sreRaiseEngineError(globalObject, scope, status);
        if (!status)
            break;

        JSValue before = run.slice(globalObject, last, run.startIndex());
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, list, before);
        RETURN_IF_EXCEPTION(scope, { });
        for (ptrdiff_t group = 1; group <= groups; ++group) {
            JSValue part = sreGroupOfRun(globalObject, run, group, false);
            RETURN_IF_EXCEPTION(scope, { });
            listAppend(globalObject, list, part);
            RETURN_IF_EXCEPTION(scope, { });
        }
        ++count;
        sreMoveOn(run);
        last = run.startIndex();
    }
    JSValue rest = run.slice(globalObject, last, run.endpos);
    RETURN_IF_EXCEPTION(scope, { });
    listAppend(globalObject, list, rest);
    RETURN_IF_EXCEPTION(scope, { });
    ensureStillAliveHere(pattern);
    return list;
}

static JSValue sreSubstitute(JSGlobalObject* globalObject, JSValue thisValue, JSValue replacement, JSValue string, JSValue countValue, bool withCount)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* pattern = PyRecord::from(thisValue);
    ptrdiff_t maxCount;
    if (failed(scope, sreToSsize(globalObject, countValue, 0, maxCount)))
        return { };

    enum { Literal, Template, Callable } kind;
    if (isCallable(globalObject, replacement))
        kind = Callable;
    else {
        // Without a backslash there is nothing to expand.
        bool isLiteral = false;
        if (isStr(replacement)) {
            auto view = strValue(replacement)->view(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            isLiteral = view->find('\\') == notFound;
        } else if (isBytesLike(replacement)) {
            std::span<const uint8_t> bytes;
            getBytesLike(globalObject, replacement, bytes);
            RETURN_IF_EXCEPTION(scope, { });
            isLiteral = !bytes.size() || !memchr(bytes.data(), '\\', bytes.size());
        }
        if (isLiteral)
            kind = Literal;
        else {
            PyRecord* compiled = sreCompileTemplate(globalObject, pattern, replacement);
            RETURN_IF_EXCEPTION(scope, { });
            if (compiled->fieldCount() == SreTemplate::Items) {
                replacement = compiled->get(SreTemplate::Literal);
                kind = Literal;
            } else {
                replacement = compiled;
                kind = Template;
            }
        }
    }

    SreRun run(globalObject);
    if (failed(scope, sreBegin(globalObject, run, pattern, string, 0, sreMaxSsize)))
        return { };
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    ptrdiff_t count = 0;
    ptrdiff_t copied = 0; // The subject up to here is in the list.
    while (!maxCount || count < maxCount) {
        ptrdiff_t status = sreSearchNext(run, pattern);
        if (status < 0)
            return sreRaiseEngineError(globalObject, scope, status);
        if (!status)
            break;

        ptrdiff_t matchStart = run.startIndex();
        if (copied < matchStart) {
            JSValue before = run.slice(globalObject, copied, matchStart);
            RETURN_IF_EXCEPTION(scope, { });
            listAppend(globalObject, list, before);
            RETURN_IF_EXCEPTION(scope, { });
        }

        JSValue item = replacement;
        if (kind != Literal) {
            JSValue match = sreNewMatch(globalObject, pattern, run, 1);
            RETURN_IF_EXCEPTION(scope, { });
            item = kind == Template ? sreExpandTemplate(globalObject, PyRecord::from(replacement), PyRecord::from(match)) : call(globalObject, replacement, match);
            RETURN_IF_EXCEPTION(scope, { });
            if (failed(scope, run.refresh(globalObject)))
                return { };
        }
        if (!isNone(item)) {
            listAppend(globalObject, list, item);
            RETURN_IF_EXCEPTION(scope, { });
        }

        copied = run.ptrIndex();
        ++count;
        sreMoveOn(run);
    }
    if (copied < run.endpos) {
        JSValue rest = run.slice(globalObject, copied, run.endpos);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, list, rest);
        RETURN_IF_EXCEPTION(scope, { });
    }
    ensureStillAliveHere(pattern);

    JSValue result = listLength(list) ? sreJoin(globalObject, run.text.isBytes, list) : run.slice(globalObject, 0, 0);
    RETURN_IF_EXCEPTION(scope, { });
    if (withCount)
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { result, jsNumber(count) }));
    return result;
}

static JSValue srePatternSub(JSGlobalObject* globalObject, JSValue thisValue, JSValue replacement, JSValue string, JSValue count)
{
    return sreSubstitute(globalObject, thisValue, replacement, string, count, false);
}

static JSValue srePatternSubn(JSGlobalObject* globalObject, JSValue thisValue, JSValue replacement, JSValue string, JSValue count)
{
    return sreSubstitute(globalObject, thisValue, replacement, string, count, true);
}

static JSValue srePatternScanner(JSGlobalObject* globalObject, JSValue thisValue, JSValue string, JSValue pos, JSValue endpos)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* pattern = PyRecord::from(thisValue);
    ptrdiff_t start;
    ptrdiff_t end;
    if (failed(scope, sreToSsize(globalObject, pos, 0, start)) || failed(scope, sreToSsize(globalObject, endpos, sreMaxSsize, end)))
        return { };

    SreRun run(globalObject);
    if (failed(scope, sreBegin(globalObject, run, pattern, string, start, end)))
        return { };
    PyRecord* scanner = PyRecord::create(globalObject, sreScannerType(globalObject), SreScanner::FieldCount);
    scanner->set(vm, SreScanner::Pattern, pattern);
    scanner->set(vm, SreScanner::String, string);
    if (run.text.wide)
        scanner->set(vm, SreScanner::Kept, run.text.wide);
    else if (run.text.isBytes && typeOf(globalObject, string) != runtime(globalObject).bytesType()) {
        JSValue view = makeMemoryView(globalObject, string, BufferSimple);
        RETURN_IF_EXCEPTION(scope, { });
        scanner->set(vm, SreScanner::Kept, view);
    }
    scanner->number = run.text.length;
    scanner->set(vm, SreScanner::CharacterSize, jsNumber(run.text.characterSize));
    scanner->set(vm, SreScanner::Pos, jsNumber(run.pos));
    scanner->set(vm, SreScanner::EndPos, jsNumber(run.endpos));
    scanner->set(vm, SreScanner::Start, jsNumber(run.pos));
    scanner->set(vm, SreScanner::MustAdvance, jsBoolean(false));
    return scanner;
}

static JSValue srePatternFindIter(JSGlobalObject* globalObject, JSValue thisValue, JSValue string, JSValue pos, JSValue endpos)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue scanner = srePatternScanner(globalObject, thisValue, string, pos, endpos);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue search = getAttr(globalObject, scanner, makeStr(vm, "search"_s));
    RETURN_IF_EXCEPTION(scope, { });
    PyIterator* iterator = PyIterator::create(globalObject, PyIterator::Kind::Callable, search);
    iterator->setExtra(vm, none());
    return iterator;
}

static JSValue srePatternGroupIndex(JSGlobalObject* globalObject, JSValue thisValue)
{
    JSValue groupIndex = PyRecord::from(thisValue)->get(SrePattern::GroupIndex);
    if (!groupIndex)
        return PyDict::create(globalObject);
    return makeMappingProxy(globalObject, groupIndex);
}

static JSValue srePatternRepr(JSGlobalObject* globalObject, JSValue thisValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* pattern = PyRecord::from(thisValue);
    static constexpr std::pair<ASCIILiteral, int> names[] = {
        { "re.IGNORECASE"_s, SreFlagIgnoreCase },
        { "re.LOCALE"_s, SreFlagLocale },
        { "re.MULTILINE"_s, SreFlagMultiLine },
        { "re.DOTALL"_s, SreFlagDotAll },
        { "re.UNICODE"_s, SreFlagUnicode },
        { "re.VERBOSE"_s, SreFlagVerbose },
        { "re.DEBUG"_s, SreFlagDebug },
        { "re.ASCII"_s, SreFlagASCII },
    };

    int flags = static_cast<int>(sreNumberField(pattern, SrePattern::Flags));
    // It goes without saying for a str pattern.
    if (!sreNumberField(pattern, SrePattern::IsBytes) && (flags & (SreFlagLocale | SreFlagUnicode | SreFlagASCII)) == SreFlagUnicode)
        flags &= ~SreFlagUnicode;

    String source = shortRepr(globalObject, pattern->get(SrePattern::Source), 200);
    RETURN_IF_EXCEPTION(scope, { });
    StrBuilder builder;
    builder.append("re.compile("_s, source);
    ASCIILiteral separator = ", "_s;
    for (auto [name, flag] : names) {
        if (flags & flag) {
            builder.append(separator, name);
            separator = "|"_s;
            flags &= ~flag;
        }
    }
    if (flags)
        builder.append(separator, "0x"_s, hex(static_cast<unsigned>(flags), Lowercase));
    builder.append(')');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static std::span<const uint8_t> sreCodeBytes(PyRecord* pattern)
{
    return { reinterpret_cast<const uint8_t*>(sreCodeOf(pattern)), static_cast<size_t>(sreNumberField(pattern, SrePattern::CodeSize)) * sizeof(SreCode) };
}

static JSValue srePatternHash(JSGlobalObject* globalObject, JSValue thisValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* pattern = PyRecord::from(thisValue);
    int64_t result = hash(globalObject, pattern->get(SrePattern::Source));
    RETURN_IF_EXCEPTION(scope, { });
    result ^= hashOfBytes(sreCodeBytes(pattern));
    result ^= sreNumberField(pattern, SrePattern::Flags);
    result ^= sreNumberField(pattern, SrePattern::IsBytes);
    result ^= sreNumberField(pattern, SrePattern::CodeSize);
    if (result == -1)
        result = -2;
    RELEASE_AND_RETURN(scope, makeInt(globalObject, result));
}

// The code is compared as well as the source: with re.LOCALE, what a pattern compiles to depends on the locale at the time.
static JSValue srePatternCompare(JSGlobalObject* globalObject, JSValue thisValue, JSValue other, bool wantEqual)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (typeOf(globalObject, other) != srePatternType(globalObject))
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    if (thisValue == other)
        return jsBoolean(wantEqual);
    PyRecord* left = PyRecord::from(thisValue);
    PyRecord* right = PyRecord::from(other);
    bool equal = sreNumberField(left, SrePattern::Flags) == sreNumberField(right, SrePattern::Flags)
        && sreNumberField(left, SrePattern::IsBytes) == sreNumberField(right, SrePattern::IsBytes)
        && sreNumberField(left, SrePattern::CodeSize) == sreNumberField(right, SrePattern::CodeSize)
        && equalSpans(sreCodeBytes(left), sreCodeBytes(right));
    if (equal) {
        equal = isEqual(globalObject, left->get(SrePattern::Source), right->get(SrePattern::Source));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsBoolean(equal == wantEqual);
}

static JSValue srePatternEq(JSGlobalObject* globalObject, JSValue thisValue, JSValue other) { return srePatternCompare(globalObject, thisValue, other, true); }
static JSValue srePatternNe(JSGlobalObject* globalObject, JSValue thisValue, JSValue other) { return srePatternCompare(globalObject, thisValue, other, false); }

static constexpr NativeDef srePatternMethods[] = {
    PY_SLOT("__repr__", srePatternRepr, "($self, /)"),
    PY_SLOT("__hash__", srePatternHash, "($self, /)"),
    PY_SLOT("__eq__", srePatternEq, "($self, value, /)"),
    PY_SLOT("__ne__", srePatternNe, "($self, value, /)"),
    PY_METHOD("prefixmatch", srePatternPrefixMatch, "($self, /, string, pos=0, endpos=sys.maxsize)"),
    PY_METHOD("match", srePatternPrefixMatch, "($self, /, string, pos=0, endpos=sys.maxsize)"),
    PY_METHOD("fullmatch", srePatternFullMatch, "($self, /, string, pos=0, endpos=sys.maxsize)"),
    PY_METHOD("search", srePatternSearch, "($self, /, string, pos=0, endpos=sys.maxsize)"),
    PY_METHOD("sub", srePatternSub, "($self, /, repl, string, count=0)"),
    PY_METHOD("subn", srePatternSubn, "($self, /, repl, string, count=0)"),
    PY_METHOD("findall", srePatternFindAll, "($self, /, string, pos=0, endpos=sys.maxsize)"),
    PY_METHOD("split", srePatternSplit, "($self, /, string, maxsplit=0)"),
    PY_METHOD("finditer", srePatternFindIter, "($self, /, string, pos=0, endpos=sys.maxsize)"),
    PY_METHOD("scanner", srePatternScanner, "($self, /, string, pos=0, endpos=sys.maxsize)"),
    PY_METHOD("__copy__", sreIdentity, "($self, /)"),
    PY_METHOD("__deepcopy__", sreDeepCopy, "($self, memo, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet srePatternGetSets[] = {
    PY_MEMBER("pattern", sreField<SrePattern::Source>),
    PY_MEMBER("flags", sreField<SrePattern::Flags>),
    PY_MEMBER("groups", sreField<SrePattern::Groups>),
    { "groupindex"_s, srePatternGroupIndex },
};

static constexpr TypeDef srePatternTypeDef { "re.Pattern"_s, { TypeFlag::HasWeakref }, srePatternMethods, srePatternGetSets, "Compiled regular expression object."_s };
static PyType* srePatternType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(srePatternTypeDef); }

// ---- The scanner ----
//
// CPython's holds the state of the engine from one match to the next. Here a cell cannot own memory that is not the
// collector's, so it holds what the state is made from.

static JSValue sreScan(JSGlobalObject* globalObject, JSValue thisValue, SreMode mode)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* scanner = PyRecord::from(thisValue);
    PyRecord* pattern = PyRecord::from(scanner->get(SreScanner::Pattern));

    ptrdiff_t start = sreNumberField(scanner, SreScanner::Start);
    if (start < 0)
        return none();

    SreRun run(globalObject);
    if (failed(scope, sreBegin(globalObject, run, pattern, scanner->get(SreScanner::String), sreNumberField(scanner, SreScanner::Pos), sreNumberField(scanner, SreScanner::EndPos), scanner)))
        return { };
    run.state.start = run.state.positionAt(start);
    run.state.ptr = run.state.start;
    run.state.mustAdvance = scanner->get(SreScanner::MustAdvance).asBoolean();

    ptrdiff_t status = mode == SreMode::Search ? sreSearch(run.state, sreCodeOf(pattern)) : sreMatch(run.state, sreCodeOf(pattern));
    JSValue match = sreNewMatch(globalObject, pattern, run, status);
    RETURN_IF_EXCEPTION(scope, { });
    if (!status)
        scanner->set(vm, SreScanner::Start, jsNumber(-1));
    else {
        scanner->set(vm, SreScanner::MustAdvance, jsBoolean(run.state.ptr == run.state.start));
        scanner->set(vm, SreScanner::Start, jsNumber(run.ptrIndex()));
    }
    return match;
}

static JSValue sreScannerPrefixMatch(JSGlobalObject* globalObject, JSValue thisValue) { return sreScan(globalObject, thisValue, SreMode::PrefixMatch); }
static JSValue sreScannerSearch(JSGlobalObject* globalObject, JSValue thisValue) { return sreScan(globalObject, thisValue, SreMode::Search); }

static constexpr NativeDef sreScannerMethods[] = {
    PY_METHOD("prefixmatch", sreScannerPrefixMatch, "($self, /)"),
    PY_METHOD("match", sreScannerPrefixMatch, "($self, /)"),
    PY_METHOD("search", sreScannerSearch, "($self, /)"),
};

static constexpr NativeGetSet sreScannerGetSets[] = {
    PY_MEMBER("pattern", sreField<SreScanner::Pattern>),
};

static constexpr TypeDef sreScannerTypeDef { "_sre.SRE_Scanner"_s, { }, sreScannerMethods, sreScannerGetSets, { } };
static PyType* sreScannerType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(sreScannerTypeDef); }

static constexpr TypeDef sreTemplateTypeDef { "_sre.SRE_Template"_s, { }, { }, { }, { } };
static PyType* sreTemplateType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(sreTemplateTypeDef); }

// ---- The module ----

static JSValue sreCompile(JSGlobalObject* globalObject, JSValue source, JSValue flagsValue, JSValue codeValue, JSValue groupsValue, JSValue groupIndex, JSValue indexGroup)
{
    VM& vm = getVM(globalObject);
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    int flags = toCInt(globalObject, flagsValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isList(codeValue))
        return raiseTypeError(globalObject, scope, "compile() argument 'code' must be list, not "_s, sreArgumentTypeName(globalObject, codeValue));
    ptrdiff_t groups;
    if (failed(scope, sreToSsize(globalObject, groupsValue, 0, groups)))
        return { };
    if (!PyDict::is(groupIndex))
        return raiseTypeError(globalObject, scope, "compile() argument 'groupindex' must be dict, not "_s, sreArgumentTypeName(globalObject, groupIndex));
    if (!PyTuple::is(indexGroup))
        return raiseTypeError(globalObject, scope, "compile() argument 'indexgroup' must be tuple, not "_s, sreArgumentTypeName(globalObject, indexGroup));

    JSArray* list = asList(codeValue);
    unsigned size = listLength(list);
    PyByteStorage* storage = PyByteStorage::tryCreate(vm, rt.structure<PyByteStorage>(), static_cast<size_t>(size) * sizeof(SreCode));
    if (!storage)
        return raiseWithArgs(globalObject, scope, rt.MemoryError(), { });
    SreCode* code = reinterpret_cast<SreCode*>(storage->data());
    for (unsigned i = 0; i < size; ++i) {
        JSValue item = listGet(list, i);
        if (!isInt(item))
            return raiseTypeError(globalObject, scope, "an integer is required"_s);
        item = intValue(item);
        auto word = tryUInt64(item);
        if (!word) {
            if (tryInt64(item))
                return raiseOverflowError(globalObject, scope, "can't convert negative value to unsigned int"_s);
            return raiseOverflowError(globalObject, scope, "Python int too large to convert to C unsigned long"_s);
        }
        if (*word > std::numeric_limits<SreCode>::max())
            return raiseOverflowError(globalObject, scope, "regular expression code size limit exceeded"_s);
        code[i] = static_cast<SreCode>(*word);
    }

    int isBytes = -1;
    if (!isNone(source)) {
        SreText text(globalObject);
        if (failed(scope, sreGetText(globalObject, source, text)))
            return { };
        isBytes = text.isBytes;
    }

    if (!sreValidate(code, size, groups))
        return raiseRuntimeError(globalObject, scope, "invalid SRE code"_s);

    PyRecord* pattern = PyRecord::create(globalObject, srePatternType(globalObject), SrePattern::FieldCount);
    pattern->set(vm, SrePattern::Source, source);
    if (PyDict::from(groupIndex)->size()) {
        pattern->set(vm, SrePattern::GroupIndex, groupIndex);
        if (PyTuple::from(indexGroup)->length())
            pattern->set(vm, SrePattern::IndexGroup, indexGroup);
    }
    pattern->set(vm, SrePattern::Code, storage);
    pattern->set(vm, SrePattern::CodeSize, jsNumber(size));
    pattern->set(vm, SrePattern::Flags, jsNumber(flags));
    pattern->set(vm, SrePattern::Groups, jsNumber(groups));
    pattern->set(vm, SrePattern::IsBytes, jsNumber(isBytes));
    return pattern;
}

// What re._parser.parse_template() returns, [literal, group, literal, group ... literal], as an object.
static JSValue sreMakeTemplate(JSGlobalObject* globalObject, JSValue, JSValue parts)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isList(parts))
        return raiseTypeError(globalObject, scope, "template() argument 2 must be list, not "_s, sreArgumentTypeName(globalObject, parts));
    JSArray* list = asList(parts);
    unsigned length = listLength(list);
    if (!(length & 1))
        return raiseTypeError(globalObject, scope, "invalid template"_s);

    PyRecord* result = PyRecord::create(globalObject, sreTemplateType(globalObject), length);
    static_assert(SreTemplate::Items == 1);
    result->set(vm, SreTemplate::Literal, listGet(list, 0));
    for (unsigned i = SreTemplate::Items; i < length; i += 2) {
        JSValue group = listGet(list, i);
        if (!isInt(group))
            return raiseTypeError(globalObject, scope, "an integer is required"_s);
        auto index = tryInt64(intValue(group));
        if (!index)
            return raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
        if (*index < 0)
            return raiseTypeError(globalObject, scope, "invalid template"_s);
        result->set(vm, i, jsNumber(*index));

        JSValue literal = listGet(list, i + 1);
        bool isEmpty = isStr(literal) ? !strValue(literal)->length() : PyBytes::is(literal) && PyBytes::from(literal)->span().empty();
        if (!isEmpty)
            result->set(vm, i + 1, literal);
    }
    return result;
}

static JSValue sreGetCodeSize(JSGlobalObject*) { return jsNumber(sizeof(SreCode)); }

template<typename Function>
static JSValue sreWithCharacter(JSGlobalObject* globalObject, JSValue character, const Function& function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int ch = toCInt(globalObject, character);
    RETURN_IF_EXCEPTION(scope, { });
    return function(static_cast<SreCode>(ch));
}

static JSValue sreASCIIIsCased(JSGlobalObject* globalObject, JSValue character)
{
    return sreWithCharacter(globalObject, character, [](SreCode ch) { return jsBoolean(ch < 128 && isASCIIAlpha(static_cast<char32_t>(ch))); });
}

static JSValue sreUnicodeIsCased(JSGlobalObject* globalObject, JSValue character)
{
    return sreWithCharacter(globalObject, character, [](SreCode ch) { return jsBoolean(ch != sreLowerUnicode(ch) || ch != sreUpperUnicode(ch)); });
}

static JSValue sreASCIIToLower(JSGlobalObject* globalObject, JSValue character)
{
    return sreWithCharacter(globalObject, character, [](SreCode ch) { return jsNumber(static_cast<int>(sreLowerASCII(ch))); });
}

static JSValue sreUnicodeToLower(JSGlobalObject* globalObject, JSValue character)
{
    return sreWithCharacter(globalObject, character, [](SreCode ch) { return jsNumber(static_cast<int>(sreLowerUnicode(ch))); });
}

static constexpr NativeDef sreFunctions[] = {
    PY_FUNCTION("compile", sreCompile, "(pattern, flags, code, groups, groupindex, indexgroup)"),
    PY_FUNCTION("template", sreMakeTemplate, "(pattern, template, /)"),
    PY_FUNCTION("getcodesize", sreGetCodeSize, "()"),
    PY_FUNCTION("ascii_iscased", sreASCIIIsCased, "(character, /)"),
    PY_FUNCTION("unicode_iscased", sreUnicodeIsCased, "(character, /)"),
    PY_FUNCTION("ascii_tolower", sreASCIIToLower, "(character, /)"),
    PY_FUNCTION("unicode_tolower", sreUnicodeToLower, "(character, /)"),
};

void initializeSreModule(JSGlobalObject* globalObject, PyModule* module)
{
    VM& vm = getVM(globalObject);
    sreInitializeCharacterProperties();
    module->addFunctions(globalObject, sreFunctions);
    module->add(globalObject, "MAGIC"_s, jsNumber(sreMagic));
    module->add(globalObject, "CODESIZE"_s, jsNumber(sizeof(SreCode)));
    module->add(globalObject, "MAXREPEAT"_s, jsNumber(sreMaxRepeat));
    module->add(globalObject, "MAXGROUPS"_s, jsNumber(sreMaxGroups));
    module->add(globalObject, "copyright"_s, makeStr(vm, " SRE 2.2.2 Copyright (c) 1997-2002 by Secret Labs AB "_s));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
