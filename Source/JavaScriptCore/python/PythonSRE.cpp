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
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonBytes.h"
#include "PythonCharacters.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonLocale.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonSREEngine.h"
#include "PythonSequences.h"
#include <wtf/Scope.h>

// The module _sre: Modules/_sre/sre.c of CPython. What does the matching is in PythonSREEngine.h. What makes the code that it goes by out of a regular expression is written in Python, in the package re.

namespace JSC { namespace Python {

using namespace SRE;

namespace {

struct SREModuleState final : NativeState {
    PYTHON_NATIVE_STATE(SREModuleState);
    WriteBarrier<PyType> pattern;
    WriteBarrier<PyType> match;
    WriteBarrier<PyType> scanner;
    WriteBarrier<PyType> substitution; // SRE_Template
    WriteBarrier<Unknown> compileTemplate; // re._compile_template
};

template<typename Visitor>
void SREModuleState::visit(Visitor& visitor)
{
    visitor.append(pattern);
    visitor.append(match);
    visitor.append(scanner);
    visitor.append(substitution);
    visitor.append(compileTemplate);
}

SREModuleState& initializeSRE(JSGlobalObject*);

SREModuleState& moduleState(JSGlobalObject* globalObject)
{
    auto& state = globalObject->pyRealm()->moduleState<SREModuleState>();
    return state.pattern ? state : initializeSRE(globalObject);
}

struct PatternState final : NativeState {
    PYTHON_NATIVE_STATE(PatternState);
    int64_t groups { 0 };
    WriteBarrier<Unknown> groupIndex; // A dict, or nothing.
    WriteBarrier<Unknown> indexGroup; // A tuple, or nothing.
    WriteBarrier<Unknown> pattern; // What it was compiled from, or None.
    int flags { 0 };
    int isbytes { 0 }; // 1 if it is for bytes, 0 for str, -1 if it was compiled from None.
    Vector<SRE_CODE> code;
};

template<typename Visitor>
void PatternState::visit(Visitor& visitor)
{
    visitor.append(groupIndex);
    visitor.append(indexGroup);
    visitor.append(pattern);
}

struct MatchState final : NativeState {
    PYTHON_NATIVE_STATE(MatchState);
    WriteBarrier<Unknown> string;
    WriteBarrier<Unknown> regs; // Once it has been asked for.
    WriteBarrier<PyStateObject> pattern;
    int64_t pos { 0 };
    int64_t endpos { 0 };
    int64_t lastIndex { -1 };
    int64_t groups { 0 }; // With the whole match for one.
    Vector<int64_t> mark; // Where each begins and ends. -1 if it took no part.
};

template<typename Visitor>
void MatchState::visit(Visitor& visitor)
{
    visitor.append(string);
    visitor.append(regs);
    visitor.append(pattern);
}

// ---- What is looked through

// Where a string that has nothing in it is said to be, since the engine takes a null pointer to mean that there is no such place.
constexpr char32_t nothing = 0;

std::span<const uint8_t> bytesOf(JSCell* owner)
{
    auto now = builtinBufferOf(owner);
    return now ? *now : std::span<const uint8_t>();
}

const void* addressOf(std::span<const uint8_t> bytes) { return bytes.empty() ? static_cast<const void*>(&nothing) : bytes.data(); }

// What getstring() finds out about an object.
struct Subject {
    const void* data { nullptr };
    int64_t length { 0 }; // In characters
    int isbytes { 0 };
    int charsize { 1 };
    // Of a str. If it has characters that take two code units, they are looked through with each in a place of its own, as CPython keeps them.
    String text;
    RefPtr<ExpandedString> expanded;
    // Of anything else, whose bytes they are: something that builtinBufferOf() knows. This does not keep it alive. Where its bytes are can change whenever anything of a program's is run, so `data` is good only until then.
    JSCell* owner { nullptr };
};

// getstring(). False if it raised.
bool getSubject(JSGlobalObject* globalObject, JSValue string, Subject& subject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSString* str = stringIn(string)) {
        subject.text = str->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        subject.isbytes = 0;
        subject.length = subject.text.length();
        StringImpl* impl = subject.text.impl();
        if (!subject.length) {
            subject.charsize = 1;
            subject.data = &nothing;
        } else if (impl->is8Bit()) {
            subject.charsize = 1;
            subject.data = impl->span8().data();
        } else if (!Characters(vm, subject.text).hasPairs()) {
            subject.charsize = 2;
            subject.data = impl->span16().data();
        } else {
            subject.expanded = vm.ensurePythonSurrogatePairCache().expand(vm, *impl);
            if (!subject.expanded) {
                raiseMemoryError(globalObject, scope);
                return false;
            }
            subject.charsize = 4;
            subject.data = subject.expanded->characters().data();
            subject.length = subject.expanded->characters().size();
        }
        return true;
    }
    Buffer buffer = bufferOrNothing(globalObject, string);
    if (!buffer) {
        raiseTypeError(globalObject, scope, concatenate("expected string or bytes-like object, got '"_s, typeName(globalObject, string), '\''));
        return false;
    }
    auto bytes = buffer.span();
    // What a class of a program's gives with __buffer__() is given back to it as soon as this returns, so it is a copy that is looked through.
    if (auto own = builtinBufferOf(string); own && own->data() == bytes.data() && own->size() == bytes.size())
        subject.owner = string.asCell();
    else {
        subject.owner = newBytes(globalObject, bytes);
        RETURN_IF_EXCEPTION(scope, false);
        bytes = bytesOf(subject.owner);
    }
    subject.isbytes = 1;
    subject.charsize = 1;
    subject.length = bytes.size();
    subject.data = addressOf(bytes);
    return true;
}

// getslice(). Empty if it raised.
JSValue getSlice(JSGlobalObject* globalObject, const Subject& subject, JSValue string, int64_t start, int64_t end)
{
    VM& vm = globalObject->vm();
    if (subject.isbytes) {
        // As they are now, which may be fewer than there were.
        auto bytes = bytesOf(subject.owner);
        if (!start && end == static_cast<int64_t>(bytes.size()) && isExactly(globalObject, string, BuiltinType::Bytes))
            return string;
        start = std::min<int64_t>(start, bytes.size());
        end = std::clamp<int64_t>(end, start, bytes.size());
        return newBytes(globalObject, bytes.subspan(start, end - start));
    }
    // PyUnicode_Substring()
    end = std::min(end, subject.length);
    if (!start && end == subject.length)
        return string.isString() ? string : JSValue(jsString(vm, subject.text));
    if (start >= subject.length || end < start)
        return jsEmptyString(vm);
    if (subject.charsize != 4)
        return jsSubstring(vm, subject.text, start, end - start);
    Characters characters(vm, subject.text);
    unsigned first = characters.codeUnitOf(start);
    return jsSubstring(vm, subject.text, first, characters.codeUnitOf(end) - first);
}

// SRE_STATE, with what it is looking through
struct State final : SRE_STATE {
    WTF_MAKE_NONCOPYABLE(State);
public:
    State() = default;
    // state_fini()
    ~State()
    {
        data_stack_dealloc(this);
        repeat_pool_clear(this);
    }

    bool hasMoved() const
    {
        if (!isbytes)
            return false;
        auto now = bytesOf(subject.owner);
        return addressOf(now) != beginning || static_cast<int64_t>(now.size()) != subject.length;
    }

    // CPython has the object keep its bytes where they are for as long as there is a state. Here they are looked for again before each time that the engine is run, and everything that points among them is made to
    // point to as far along wherever they are now, or to the end if there are no longer as many.
    void findBytesAgain()
    {
        if (!hasMoved())
            return;
        auto now = bytesOf(subject.owner);
        auto* base = static_cast<const char*>(addressOf(now));
        auto moved = [&] (const void* pointer) -> const void* {
            if (!pointer)
                return nullptr;
            return base + std::min<size_t>(static_cast<const char*>(pointer) - static_cast<const char*>(beginning), now.size());
        };
        ptr = moved(ptr);
        start = moved(start);
        end = moved(end);
        beginning = base;
        subject.data = base;
        subject.length = now.size();
    }

    int64_t offsetOf(const void* pointer) const { return (static_cast<const char*>(pointer) - static_cast<const char*>(beginning)) / charsize; }

    Subject subject;
    Vector<const void*> marks;
};

} // anonymous namespace

int SRE_STATE::checkSignals()
{
    VM& vm = globalObject->vm();
    if (!vm.hasPythonWork()) [[likely]]
        return 0;
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isWell = Python::checkSignals(globalObject);
    RETURN_IF_EXCEPTION(scope, 1);
    if (!isWell)
        return 1;
    // In CPython this is raised where the size of it is changed.
    if (static_cast<State*>(this)->hasMoved()) {
        raise(globalObject, scope, BuiltinType::BufferError, "Existing exports of data: object cannot be re-sized"_s);
        return 1;
    }
    return 0;
}

SRE_LOCALE SRE_STATE::currentLocale()
{
#if OS(UNIX)
    return characterLocale(globalObject);
#else
    return nullptr;
#endif
}

namespace {

// state_reset()
void resetState(State& state)
{
    state.lastmark = -1;
    state.lastindex = -1;
    state.repeat = nullptr;
    data_stack_dealloc(&state);
}

// state_init(). False if it raised.
bool initializeState(JSGlobalObject* globalObject, State& state, const PatternState& pattern, JSValue string, int64_t start, int64_t end)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    state.globalObject = globalObject;
    if (!state.marks.tryGrow(pattern.groups * 2)) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    state.mark = state.marks.mutableSpan().data();
    getSubject(globalObject, string, state.subject);
    RETURN_IF_EXCEPTION(scope, false);
    auto& subject = state.subject;
    if (subject.isbytes && !pattern.isbytes) {
        raiseTypeError(globalObject, scope, "cannot use a string pattern on a bytes-like object"_s);
        return false;
    }
    if (!subject.isbytes && pattern.isbytes > 0) {
        raiseTypeError(globalObject, scope, "cannot use a bytes pattern on a string-like object"_s);
        return false;
    }
    start = std::clamp<int64_t>(start, 0, subject.length);
    end = std::clamp<int64_t>(end, 0, subject.length);
    state.isbytes = subject.isbytes;
    state.charsize = subject.charsize;
    state.beginning = subject.data;
    state.start = static_cast<const char*>(subject.data) + start * subject.charsize;
    state.end = static_cast<const char*>(subject.data) + end * subject.charsize;
    state.pos = start;
    state.endpos = end;
    return true;
}

// state_getslice(). Empty if it raised.
JSValue getSliceOfGroup(JSGlobalObject* globalObject, State& state, int64_t index, JSValue string, bool empty)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int64_t i = 0;
    int64_t j = 0;
    index = (index - 1) * 2;
    if (isNone(string) || index >= state.lastmark || !state.mark[index] || !state.mark[index + 1]) {
        if (!empty)
            return jsUndefined();
    } else {
        i = state.offsetOf(state.mark[index]);
        j = state.offsetOf(state.mark[index + 1]);
        if (i > j)
            return raise(globalObject, scope, BuiltinType::SystemError, "The span of capturing group is wrong, please report a bug for the re module."_s);
    }
    RELEASE_AND_RETURN(scope, getSlice(globalObject, state.subject, string, i, j));
}

// pattern_error()
void raiseForStatus(JSGlobalObject* globalObject, ptrdiff_t status)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    switch (status) {
    case SRE_ERROR_RECURSION_LIMIT:
        raise(globalObject, scope, BuiltinType::RecursionError, "maximum recursion limit exceeded"_s);
        break;
    case SRE_ERROR_MEMORY:
        raiseMemoryError(globalObject, scope);
        break;
    case SRE_ERROR_INTERRUPTED:
        // Something has been raised already.
        break;
    default:
        raise(globalObject, scope, BuiltinType::RuntimeError, "internal error in regular expression engine"_s);
    }
}

// sre_match()
ptrdiff_t runMatch(State& state, const PatternState& pattern)
{
    state.findBytesAgain();
    const SRE_CODE* code = pattern.code.span().data();
    if (state.charsize == 1)
        return sre_match<uint8_t>(&state, code, 1);
    if (state.charsize == 2)
        return sre_match<char16_t>(&state, code, 1);
    return sre_match<char32_t>(&state, code, 1);
}

// sre_search()
ptrdiff_t runSearch(State& state, const PatternState& pattern)
{
    state.findBytesAgain();
    auto* code = const_cast<SRE_CODE*>(pattern.code.span().data());
    if (state.charsize == 1)
        return sre_search<uint8_t>(&state, code);
    if (state.charsize == 2)
        return sre_search<char16_t>(&state, code);
    return sre_search<char32_t>(&state, code);
}

// pattern_new_match(). Empty if it raised.
JSValue newMatch(JSGlobalObject* globalObject, PyStateObject* patternObject, State& state, JSValue string, ptrdiff_t status)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!status)
        return jsUndefined();
    if (status < 0) {
        raiseForStatus(globalObject, status);
        return { };
    }
    auto& pattern = patternObject->state<PatternState>();
    auto fields = makeUnique<MatchState>();
    if (!fields->mark.tryGrow(2 * (pattern.groups + 1)))
        return raiseMemoryError(globalObject, scope);
    auto& mark = fields->mark;
    fields->groups = pattern.groups + 1;
    mark[0] = state.offsetOf(state.start);
    mark[1] = state.offsetOf(state.ptr);
    for (int64_t i = 0, j = 0; i < pattern.groups; ++i, j += 2) {
        if (j + 1 <= state.lastmark && state.mark[j] && state.mark[j + 1]) {
            mark[j + 2] = state.offsetOf(state.mark[j]);
            mark[j + 3] = state.offsetOf(state.mark[j + 1]);
            if (mark[j + 2] > mark[j + 3])
                return raise(globalObject, scope, BuiltinType::SystemError, "The span of capturing group is wrong, please report a bug for the re module."_s);
        } else
            mark[j + 2] = mark[j + 3] = -1;
    }
    fields->pos = state.pos;
    fields->endpos = state.endpos;
    fields->lastIndex = state.lastindex;
    auto* match = PyStateObject::create(vm, moduleState(globalObject).match->instanceStructure(), WTF::move(fields));
    auto& made = match->state<MatchState>();
    made.pattern.set(vm, match, patternObject);
    made.string.set(vm, match, string);
    return match;
}

// What a Py_ssize_t is made from, if anything was given.
bool toPosition(JSGlobalObject* globalObject, JSValue value, int64_t& position)
{
    if (!value)
        return true;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto given = toSsize(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    position = *given;
    return true;
}

PyStateObject* asPattern(JSValue value) { return uncheckedDowncast<PyStateObject>(value.asCell()); }

// As much of what repr() gives as PyUnicode_FromFormat() puts in for "%.50R"
String truncatedRepr(JSGlobalObject* globalObject, JSValue value, unsigned limit)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String shown = repr(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    Characters characters(vm, shown);
    return characters.count() <= limit ? shown : shown.left(characters.codeUnitOf(limit));
}

} // anonymous namespace

// ---- Pattern

enum class How : uint8_t { Match, FullMatch, Search };

// match(string, pos=0, endpos=sys.maxsize), fullmatch() and search()
PYTHON_NATIVE(patternMatch)
{
    auto how = unpack<How>(callFrame, 0);
    NATIVE_PROLOGUE();
    PyStateObject* self = asPattern(args[0]);
    auto& pattern = self->state<PatternState>();
    JSValue string = args[1];
    int64_t pos = 0;
    int64_t endpos = std::numeric_limits<int64_t>::max();
    if (!toPosition(globalObject, args.at(2), pos) || !toPosition(globalObject, args.at(3), endpos))
        return { };
    State state;
    initializeState(globalObject, state, pattern, string, pos, endpos);
    RETURN_IF_EXCEPTION(scope, { });
    ptrdiff_t status;
    if (how == How::Search)
        status = runSearch(state, pattern);
    else {
        state.ptr = state.start;
        state.match_all = how == How::FullMatch;
        status = runMatch(state, pattern);
    }
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newMatch(globalObject, self, state, string, status)));
}

// findall(string, pos=0, endpos=sys.maxsize)
PYTHON_NATIVE(patternFindAll)
{
    NATIVE_PROLOGUE();
    auto& pattern = stateOf<PatternState>(args[0]);
    JSValue string = args[1];
    int64_t pos = 0;
    int64_t endpos = std::numeric_limits<int64_t>::max();
    if (!toPosition(globalObject, args.at(2), pos) || !toPosition(globalObject, args.at(3), endpos))
        return { };
    State state;
    initializeState(globalObject, state, pattern, string, pos, endpos);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    while (state.start <= state.end) {
        resetState(state);
        state.ptr = state.start;
        ptrdiff_t status = runSearch(state, pattern);
        RETURN_IF_EXCEPTION(scope, { });
        if (status <= 0) {
            if (!status)
                break;
            raiseForStatus(globalObject, status);
            return { };
        }
        // There is no need of a match object.
        JSValue item;
        switch (pattern.groups) {
        case 0:
            item = getSlice(globalObject, state.subject, string, state.offsetOf(state.start), state.offsetOf(state.ptr));
            break;
        case 1:
            item = getSliceOfGroup(globalObject, state, 1, string, true);
            break;
        default: {
            MarkedArgumentBuffer items;
            for (int64_t i = 0; i < pattern.groups; ++i) {
                items.append(getSliceOfGroup(globalObject, state, i + 1, string, true));
                RETURN_IF_EXCEPTION(scope, { });
            }
            item = PyTuple::createFromArguments(globalObject, items);
            break;
        }
        }
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, list, item);
        RETURN_IF_EXCEPTION(scope, { });
        state.must_advance = state.ptr == state.start;
        state.start = state.ptr;
    }
    return JSValue::encode(list);
}

// split(string, maxsplit=0)
PYTHON_NATIVE(patternSplit)
{
    NATIVE_PROLOGUE();
    auto& pattern = stateOf<PatternState>(args[0]);
    JSValue string = args[1];
    int64_t maxSplit = 0;
    if (!toPosition(globalObject, args.at(2), maxSplit))
        return { };
    State state;
    initializeState(globalObject, state, pattern, string, 0, std::numeric_limits<int64_t>::max());
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t n = 0;
    // Where the last one ended. It is a number and not a pointer, since nothing sees to it that a pointer kept here is still good.
    int64_t last = state.offsetOf(state.start);
    while (!maxSplit || n < maxSplit) {
        resetState(state);
        state.ptr = state.start;
        ptrdiff_t status = runSearch(state, pattern);
        RETURN_IF_EXCEPTION(scope, { });
        if (status <= 0) {
            if (!status)
                break;
            raiseForStatus(globalObject, status);
            return { };
        }
        // What is before this match
        JSValue item = getSlice(globalObject, state.subject, string, last, state.offsetOf(state.start));
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, list, item);
        RETURN_IF_EXCEPTION(scope, { });
        for (int64_t i = 0; i < pattern.groups; ++i) {
            item = getSliceOfGroup(globalObject, state, i + 1, string, false);
            RETURN_IF_EXCEPTION(scope, { });
            listAppend(globalObject, list, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        ++n;
        state.must_advance = state.ptr == state.start;
        state.start = state.ptr;
        last = state.offsetOf(state.start);
    }
    // What is after the last, even if that is nothing
    JSValue item = getSlice(globalObject, state.subject, string, last, state.endpos);
    RETURN_IF_EXCEPTION(scope, { });
    listAppend(globalObject, list, item);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(list);
}

// ---- What a replacement string is compiled to

namespace {

struct TemplateState final : NativeState {
    PYTHON_NATIVE_STATE(TemplateState);
    struct Item {
        int64_t index; // Of a group
        bool hasLiteral; // After it. One that has nothing in it is left out.
    };
    WriteBarrier<Unknown> literal; // What it begins with
    Vector<Item> items;
    WriteBarrier<PyTuple> literals; // As many as there are items, with None where there is none.
};

template<typename Visitor>
void TemplateState::visit(Visitor& visitor)
{
    visitor.append(literal);
    visitor.append(literals);
}

JSValue join(JSGlobalObject* globalObject, JSValue joiner, JSArray* list)
{
    return callMethodNamed(globalObject, joiner, Identifier::fromString(globalObject->vm(), "join"_s), list);
}

// match_getslice_by_index(). Empty if it raised.
JSValue getGroup(JSGlobalObject* globalObject, MatchState& match, int64_t index, JSValue defaultValue)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    index *= 2;
    JSValue string = match.string.get();
    if (isNone(string) || match.mark[index] < 0)
        return defaultValue;
    Subject subject;
    getSubject(globalObject, string, subject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getSlice(globalObject, subject, string, std::min(match.mark[index], subject.length), std::min(match.mark[index + 1], subject.length)));
}

// expand_template(). Empty if it raised.
JSValue expandTemplate(JSGlobalObject* globalObject, TemplateState& self, MatchState& match)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (self.items.isEmpty())
        return self.literal.get();
    MarkedArgumentBuffer chunks;
    chunks.append(self.literal.get());
    for (unsigned i = 0; i < self.items.size(); ++i) {
        int64_t index = self.items[i].index;
        if (index >= match.groups)
            return raise(globalObject, scope, BuiltinType::IndexError, "no such group"_s);
        JSValue item = getGroup(globalObject, match, index, jsUndefined());
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNone(item))
            chunks.append(item);
        if (self.items[i].hasLiteral)
            chunks.append(self.literals->at(i));
    }
    JSArray* list = newList(globalObject, chunks);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue joiner = stringIn(self.literal.get()) ? JSValue(jsEmptyString(vm)) : JSValue(newBytes(globalObject, std::span<const uint8_t>()));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, join(globalObject, joiner, list));
}

// compile_template(): it is Python that does it. Empty if it raised.
JSValue compileTemplate(JSGlobalObject* globalObject, JSValue pattern, JSValue replacement)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& module = moduleState(globalObject);
    JSValue function = module.compileTemplate.get();
    if (!function) {
        function = importModuleAttribute(globalObject, "re"_s, "_compile_template"_s);
        RETURN_IF_EXCEPTION(scope, { });
        module.compileTemplate.set(vm, globalObject->pyRealm(), function);
    }
    JSValue result = call(globalObject, function, pattern, replacement);
    if (scope.exception()) [[unlikely]] {
        // If it cannot be hashed, as a bytearray cannot, it is tried again as a str or as bytes.
        bool isDerivedFromStr = stringIn(replacement) && !replacement.isString();
        bool isOtherBytes = !isDerivedFromStr && hasBuffer(globalObject, replacement) && !isExactly(globalObject, replacement, BuiltinType::Bytes);
        if ((!isDerivedFromStr && !isOtherBytes) || !catchException(globalObject, BuiltinType::TypeError))
            return { };
        if (isDerivedFromStr)
            replacement = stringIn(replacement);
        else {
            Buffer buffer = bufferOf(globalObject, replacement);
            RETURN_IF_EXCEPTION(scope, { });
            replacement = newBytes(globalObject, buffer.span());
            RETURN_IF_EXCEPTION(scope, { });
        }
        result = call(globalObject, function, pattern, replacement);
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (typeOf(globalObject, result) != module.substitution.get())
        return raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("the result of compiling a replacement string is "_s, typeName(globalObject, result)));
    return result;
}

} // anonymous namespace

// sub(repl, string, count=0) and subn(): pattern_subx()
PYTHON_NATIVE(patternSub)
{
    bool isWithCount = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    PyStateObject* self = asPattern(args[0]);
    auto& pattern = self->state<PatternState>();
    JSValue replacement = args[1];
    JSValue string = args[2];
    int64_t count = 0;
    if (!toPosition(globalObject, args.at(3), count))
        return { };

    enum { Literal, Template, Callable } filterType;
    JSValue filter;
    if (isCallable(globalObject, replacement)) {
        filter = replacement;
        filterType = Callable;
    } else {
        // If there is no backslash in it, it is put in as it is.
        bool isLiteral = false;
        {
            Subject given;
            getSubject(globalObject, replacement, given);
            if (scope.exception())
                scope.tryClearException();
            else if (given.isbytes) {
                auto bytes = bytesOf(given.owner);
                isLiteral = bytes.empty() || !memchr(bytes.data(), '\\', bytes.size());
            } else
                isLiteral = given.text.find('\\') == notFound;
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (isLiteral) {
            filter = replacement;
            filterType = Literal;
        } else {
            filter = compileTemplate(globalObject, self, replacement);
            RETURN_IF_EXCEPTION(scope, { });
            auto& compiled = stateOf<TemplateState>(filter);
            if (compiled.items.isEmpty()) {
                filter = compiled.literal.get();
                filterType = Literal;
            } else
                filterType = Template;
        }
    }

    State state;
    initializeState(globalObject, state, pattern, string, 0, std::numeric_limits<int64_t>::max());
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t n = 0;
    int64_t i = 0;
    while (!count || n < count) {
        resetState(state);
        state.ptr = state.start;
        ptrdiff_t status = runSearch(state, pattern);
        RETURN_IF_EXCEPTION(scope, { });
        if (status <= 0) {
            if (!status)
                break;
            raiseForStatus(globalObject, status);
            return { };
        }
        int64_t b = state.offsetOf(state.start);
        int64_t e = state.offsetOf(state.ptr);
        if (i < b) {
            // What is before this match
            JSValue item = getSlice(globalObject, state.subject, string, i, b);
            RETURN_IF_EXCEPTION(scope, { });
            listAppend(globalObject, list, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        JSValue item = filter;
        if (filterType != Literal) {
            JSValue match = newMatch(globalObject, self, state, string, 1);
            RETURN_IF_EXCEPTION(scope, { });
            item = filterType == Template ? expandTemplate(globalObject, stateOf<TemplateState>(filter), stateOf<MatchState>(match)) : call(globalObject, filter, match);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (!isNone(item)) {
            listAppend(globalObject, list, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        i = e;
        ++n;
        state.must_advance = state.ptr == state.start;
        state.start = state.ptr;
    }
    // What is after the last
    if (i < state.endpos) {
        JSValue item = getSlice(globalObject, state.subject, string, i, state.endpos);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, list, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue result = getSlice(globalObject, state.subject, string, 0, 0);
    RETURN_IF_EXCEPTION(scope, { });
    if (list->length()) {
        result = join(globalObject, result, list);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (isWithCount)
        return JSValue::encode(PyTuple::create(globalObject, { result, intFromInt64(globalObject, n) }));
    return JSValue::encode(result);
}

// __copy__() and __deepcopy__(memo, /), of a pattern and of a match
PYTHON_NATIVE(sreCopy)
{
    return JSValue::encode(callFrame->uncheckedArgument(0));
}

PYTHON_NATIVE(patternRepr)
{
    NATIVE_PROLOGUE();
    static constexpr std::pair<ASCIILiteral, int> flagNames[] = {
        { "re.IGNORECASE"_s, SRE_FLAG_IGNORECASE },
        { "re.LOCALE"_s, SRE_FLAG_LOCALE },
        { "re.MULTILINE"_s, SRE_FLAG_MULTILINE },
        { "re.DOTALL"_s, SRE_FLAG_DOTALL },
        { "re.UNICODE"_s, SRE_FLAG_UNICODE },
        { "re.VERBOSE"_s, SRE_FLAG_VERBOSE },
        { "re.DEBUG"_s, SRE_FLAG_DEBUG },
        { "re.ASCII"_s, SRE_FLAG_ASCII },
    };
    auto& pattern = stateOf<PatternState>(args[0]);
    int flags = pattern.flags;
    // re.UNICODE goes without saying of one that is for str.
    if (!pattern.isbytes && (flags & (SRE_FLAG_LOCALE | SRE_FLAG_UNICODE | SRE_FLAG_ASCII)) == SRE_FLAG_UNICODE)
        flags &= ~SRE_FLAG_UNICODE;
    StringBuilder shownFlags;
    for (auto& [name, value] : flagNames) {
        if (flags & value) {
            shownFlags.append(shownFlags.isEmpty() ? ""_s : "|"_s, name);
            flags &= ~value;
        }
    }
    if (flags)
        shownFlags.append(shownFlags.isEmpty() ? ""_s : "|"_s, "0x"_s, hex(static_cast<unsigned>(flags), Lowercase));
    String shown = truncatedRepr(globalObject, pattern.pattern.get(), 200);
    RETURN_IF_EXCEPTION(scope, { });
    if (shownFlags.isEmpty())
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("re.compile("_s, shown, ')'))));
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("re.compile("_s, shown, ", "_s, shownFlags.toString(), ')'))));
}

PYTHON_NATIVE(patternHash)
{
    NATIVE_PROLOGUE();
    auto& pattern = stateOf<PatternState>(args[0]);
    int64_t result = hash(globalObject, pattern.pattern.get());
    RETURN_IF_EXCEPTION(scope, { });
    result ^= hashOfBytes(asBytes(pattern.code.span()));
    result ^= pattern.flags;
    result ^= pattern.isbytes;
    result ^= static_cast<int64_t>(pattern.code.size());
    if (result == -1)
        result = -2;
    return JSValue::encode(intFromInt64(globalObject, result));
}

PYTHON_NATIVE(patternCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!isEquality(op) || typeOf(globalObject, args[1]) != moduleState(globalObject).pattern.get())
        RETURN_NOT_IMPLEMENTED();
    bool isEq = op == ComparisonOperator::Eq;
    if (args[0] == args[1])
        return JSValue::encode(jsBoolean(isEq));
    auto& left = stateOf<PatternState>(args[0]);
    auto& right = stateOf<PatternState>(args[1]);
    // The code is compared as well as what it was compiled from, since with re.LOCALE the one depends on more than the other.
    bool isSame = left.flags == right.flags && left.isbytes == right.isbytes && left.code == right.code;
    if (isSame) {
        isSame = isEqual(globalObject, left.pattern.get(), right.pattern.get());
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(jsBoolean(isSame == isEq));
}

// ---- Whether code is such as the compiler makes, so that the engine can trust it

namespace {

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#define FAIL do { return -1; } while (0)
#define GET_OP \
    do { \
        if (code >= end) \
            FAIL; \
        op = *code++; \
    } while (0)
#define GET_ARG \
    do { \
        if (code >= end) \
            FAIL; \
        arg = *code++; \
    } while (0)
#define GET_SKIP_ADJ(adj) \
    do { \
        if (code >= end) \
            FAIL; \
        skip = *code; \
        if (skip - adj > static_cast<uintptr_t>(end - code)) \
            FAIL; \
        code++; \
    } while (0)
#define GET_SKIP GET_SKIP_ADJ(0)

// _validate_charset()
int validateCharset(const SRE_CODE* code, const SRE_CODE* end)
{
    SRE_CODE op;
    SRE_CODE arg;
    SRE_CODE offset;
    while (code < end) {
        GET_OP;
        switch (op) {
        case SRE_OP_NEGATE:
            break;
        case SRE_OP_LITERAL:
            GET_ARG;
            break;
        case SRE_OP_RANGE:
        case SRE_OP_RANGE_UNI_IGNORE:
            GET_ARG;
            GET_ARG;
            break;
        case SRE_OP_CHARSET:
            offset = 256 / SRE_CODE_BITS; // A bitmap of 256 bits
            if (offset > static_cast<uintptr_t>(end - code))
                FAIL;
            code += offset;
            break;
        case SRE_OP_BIGCHARSET:
            GET_ARG; // How many blocks
            offset = 256 / sizeof(SRE_CODE); // A table of 256 bytes
            if (offset > static_cast<uintptr_t>(end - code))
                FAIL;
            // Each byte is to be the number of a block that there is.
            for (int i = 0; i < 256; ++i) {
                if (reinterpret_cast<const uint8_t*>(code)[i] >= arg)
                    FAIL;
            }
            code += offset;
            offset = arg * (256 / SRE_CODE_BITS); // A bitmap of 256 bits for each
            if (offset > static_cast<uintptr_t>(end - code))
                FAIL;
            code += offset;
            break;
        case SRE_OP_CATEGORY:
            GET_ARG;
            if (arg > SRE_CATEGORY_UNI_NOT_LINEBREAK)
                FAIL;
            break;
        default:
            FAIL;
        }
    }
    return 0;
}

// _validate_inner(): 0 if it will do, -1 if not, and 1 if the last thing in it is a JUMP.
int validateInner(const SRE_CODE* code, const SRE_CODE* end, int64_t groups)
{
    SRE_CODE op;
    SRE_CODE arg;
    SRE_CODE skip;
    if (code > end)
        FAIL;
    while (code < end) {
        GET_OP;
        switch (op) {
        case SRE_OP_MARK:
            // Whether they are properly nested is not looked into. The engine comes to no harm if they are not.
            GET_ARG;
            if (arg >= 2 * static_cast<size_t>(groups))
                FAIL;
            break;
        case SRE_OP_LITERAL:
        case SRE_OP_NOT_LITERAL:
        case SRE_OP_LITERAL_IGNORE:
        case SRE_OP_NOT_LITERAL_IGNORE:
        case SRE_OP_LITERAL_UNI_IGNORE:
        case SRE_OP_NOT_LITERAL_UNI_IGNORE:
        case SRE_OP_LITERAL_LOC_IGNORE:
        case SRE_OP_NOT_LITERAL_LOC_IGNORE:
            GET_ARG;
            break;
        case SRE_OP_SUCCESS:
        case SRE_OP_FAILURE:
            break;
        case SRE_OP_AT:
            GET_ARG;
            if (arg > SRE_AT_UNI_NON_BOUNDARY)
                FAIL;
            break;
        case SRE_OP_ANY:
        case SRE_OP_ANY_ALL:
            break;
        case SRE_OP_IN:
        case SRE_OP_IN_IGNORE:
        case SRE_OP_IN_UNI_IGNORE:
        case SRE_OP_IN_LOC_IGNORE:
            GET_SKIP;
            // As far as one before the end, which is to be a FAILURE.
            if (validateCharset(code, code + skip - 2))
                FAIL;
            if (code[skip - 2] != SRE_OP_FAILURE)
                FAIL;
            code += skip - 1;
            break;
        case SRE_OP_INFO: {
            // At the least it is <INFO> <1=skip> <2=flags> <3=min> <4=max>. There is more if the flags have SRE_INFO_PREFIX or SRE_INFO_CHARSET.
            GET_SKIP;
            const SRE_CODE* newCode = code + skip - 1;
            GET_ARG;
            SRE_CODE flags = arg;
            GET_ARG;
            GET_ARG;
            if (flags & ~(SRE_INFO_PREFIX | SRE_INFO_LITERAL | SRE_INFO_CHARSET))
                FAIL;
            if ((flags & SRE_INFO_PREFIX) && (flags & SRE_INFO_CHARSET))
                FAIL;
            if ((flags & SRE_INFO_LITERAL) && !(flags & SRE_INFO_PREFIX))
                FAIL;
            if (flags & SRE_INFO_PREFIX) {
                GET_ARG;
                SRE_CODE prefixLength = arg;
                GET_ARG;
                // The prefix
                if (prefixLength > static_cast<uintptr_t>(newCode - code))
                    FAIL;
                code += prefixLength;
                // The overlap table, in which each is to be less than the length of the prefix
                if (prefixLength > static_cast<uintptr_t>(newCode - code))
                    FAIL;
                for (SRE_CODE i = 0; i < prefixLength; ++i) {
                    if (code[i] >= prefixLength)
                        FAIL;
                }
                code += prefixLength;
            }
            if (flags & SRE_INFO_CHARSET) {
                if (validateCharset(code, newCode - 1))
                    FAIL;
                if (newCode[-1] != SRE_OP_FAILURE)
                    FAIL;
                code = newCode;
            } else if (code != newCode)
                FAIL;
            break;
        }
        case SRE_OP_BRANCH: {
            const SRE_CODE* target = nullptr;
            for (;;) {
                GET_SKIP;
                if (!skip)
                    break;
                // As far as two before the end, which is to be a JUMP. Each JUMP is to be to the same place.
                if (validateInner(code, code + skip - 3, groups))
                    FAIL;
                code += skip - 3;
                GET_OP;
                if (op != SRE_OP_JUMP)
                    FAIL;
                GET_SKIP;
                if (!target)
                    target = code + skip - 1;
                else if (code + skip - 1 != target)
                    FAIL;
            }
            if (code != target)
                FAIL;
            break;
        }
        case SRE_OP_REPEAT_ONE:
        case SRE_OP_MIN_REPEAT_ONE:
        case SRE_OP_POSSESSIVE_REPEAT_ONE: {
            GET_SKIP;
            GET_ARG;
            SRE_CODE min = arg;
            GET_ARG;
            SRE_CODE max = arg;
            if (min > max)
                FAIL;
            if (validateInner(code, code + skip - 4, groups))
                FAIL;
            code += skip - 4;
            GET_OP;
            if (op != SRE_OP_SUCCESS)
                FAIL;
            break;
        }
        case SRE_OP_REPEAT:
        case SRE_OP_POSSESSIVE_REPEAT: {
            SRE_CODE first = op;
            GET_SKIP;
            GET_ARG;
            SRE_CODE min = arg;
            GET_ARG;
            SRE_CODE max = arg;
            if (min > max)
                FAIL;
            if (validateInner(code, code + skip - 3, groups))
                FAIL;
            code += skip - 3;
            GET_OP;
            if (first == SRE_OP_POSSESSIVE_REPEAT) {
                if (op != SRE_OP_SUCCESS)
                    FAIL;
            } else if (op != SRE_OP_MAX_UNTIL && op != SRE_OP_MIN_UNTIL)
                FAIL;
            break;
        }
        case SRE_OP_ATOMIC_GROUP:
            GET_SKIP;
            if (validateInner(code, code + skip - 2, groups))
                FAIL;
            code += skip - 2;
            GET_OP;
            if (op != SRE_OP_SUCCESS)
                FAIL;
            break;
        case SRE_OP_GROUPREF:
        case SRE_OP_GROUPREF_IGNORE:
        case SRE_OP_GROUPREF_UNI_IGNORE:
        case SRE_OP_GROUPREF_LOC_IGNORE:
            GET_ARG;
            if (arg >= static_cast<size_t>(groups))
                FAIL;
            break;
        case SRE_OP_GROUPREF_EXISTS: {
            // (?(group)then|else), of which the else may be left out. With one, the then ends in a JUMP over it. There is no telling which it is but by looking for a JUMP just before where the skip is to.
            GET_ARG;
            if (arg >= static_cast<size_t>(groups))
                FAIL;
            GET_SKIP_ADJ(1);
            code--; // The skip is from the first argument.
            int result = validateInner(code + 1, code + skip - 1, groups);
            if (result == 1) {
                code += skip - 2; // After the JUMP, at how far it is to skip
                GET_SKIP;
                result = validateInner(code, code + skip - 1, groups);
            }
            if (result)
                FAIL;
            code += skip - 1;
            break;
        }
        case SRE_OP_ASSERT:
        case SRE_OP_ASSERT_NOT:
            GET_SKIP;
            GET_ARG; // 0 to look ahead, and how far to look behind
            code--;
            // As far as one before the end, which is to be a SUCCESS.
            if (validateInner(code + 1, code + skip - 2, groups))
                FAIL;
            code += skip - 2;
            GET_OP;
            if (op != SRE_OP_SUCCESS)
                FAIL;
            break;
        case SRE_OP_JUMP:
            if (code + 1 != end)
                FAIL;
            return 1;
        default:
            FAIL;
        }
    }
    return 0;
}

// _validate_outer()
int validateOuter(const SRE_CODE* code, const SRE_CODE* end, int64_t groups)
{
    if (groups < 0 || static_cast<size_t>(groups) > SRE_MAXGROUPS || code >= end || end[-1] != SRE_OP_SUCCESS)
        FAIL;
    return validateInner(code, end - 1, groups);
}

#undef FAIL
#undef GET_OP
#undef GET_ARG
#undef GET_SKIP_ADJ
#undef GET_SKIP

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

// PyLong_AsUnsignedLong(). Nothing if it raised.
std::optional<uint64_t> toUnsignedLong(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Number number = classify(value);
    if (!number.isInt()) {
        raiseTypeError(globalObject, scope, "an integer is required"_s);
        return std::nullopt;
    }
    bool isNegative = number.kind == Number::Kind::Small ? number.small < 0 : number.big->sign();
    if (isNegative) {
        raise(globalObject, scope, BuiltinType::OverflowError, "can't convert negative value to unsigned int"_s);
        return std::nullopt;
    }
    if (number.kind == Number::Kind::Small)
        return number.small;
    if (number.big->length() > 1) {
        raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C unsigned long"_s);
        return std::nullopt;
    }
    return number.big->length() ? number.big->digit(0) : 0;
}

JSValue raiseBadArgument(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral function, ASCIILiteral argument, ASCIILiteral expected, JSValue given)
{
    return raiseTypeError(globalObject, scope, concatenate(function, "() "_s, argument, " must be "_s, expected, ", not "_s, typeNameOfArgument(globalObject, given)));
}

} // anonymous namespace

// ---- The functions of the module

// compile(pattern, flags, code, groups, groupindex, indexgroup)
PYTHON_NATIVE(sreCompile)
{
    NATIVE_PROLOGUE();
    JSValue source = args[0];
    auto flags = toCInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isList(args[2]))
        return JSValue::encode(raiseBadArgument(globalObject, scope, "compile"_s, "argument 'code'"_s, "list"_s, args[2]));
    auto groups = toSsize(globalObject, args[3]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isDict(args[4]))
        return JSValue::encode(raiseBadArgument(globalObject, scope, "compile"_s, "argument 'groupindex'"_s, "dict"_s, args[4]));
    if (!isTuple(args[5]))
        return JSValue::encode(raiseBadArgument(globalObject, scope, "compile"_s, "argument 'indexgroup'"_s, "tuple"_s, args[5]));

    auto* list = uncheckedDowncast<JSArray>(args[2].asCell());
    unsigned length = list->length();
    auto fields = makeUnique<PatternState>();
    if (!fields->code.tryReserveInitialCapacity(length))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    for (unsigned i = 0; i < length; ++i) {
        auto value = toUnsignedLong(globalObject, list->getIndexQuickly(i));
        RETURN_IF_EXCEPTION(scope, { });
        if (*value > std::numeric_limits<SRE_CODE>::max())
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "regular expression code size limit exceeded"_s));
        fields->code.append(static_cast<SRE_CODE>(*value));
    }
    if (isNone(source))
        fields->isbytes = -1;
    else {
        Subject subject;
        getSubject(globalObject, source, subject);
        RETURN_IF_EXCEPTION(scope, { });
        fields->isbytes = subject.isbytes;
    }
    fields->flags = *flags;
    fields->groups = *groups;
    auto code = fields->code.span();
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    if (validateOuter(code.data(), code.data() + code.size(), *groups))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "invalid SRE code"_s));
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    auto* object = PyStateObject::create(vm, moduleState(globalObject).pattern->instanceStructure(), WTF::move(fields));
    auto& pattern = object->state<PatternState>();
    pattern.pattern.set(vm, object, source);
    if (asDict(args[4])->size()) {
        pattern.groupIndex.set(vm, object, args[4]);
        if (asTuple(args[5])->length())
            pattern.indexGroup.set(vm, object, args[5]);
    }
    return JSValue::encode(object);
}

// template(pattern, template, /), where the template is [literal1, group1, ..., literalN, groupN, literal], as re._parser.parse_template() gives it
PYTHON_NATIVE(sreTemplate)
{
    NATIVE_PROLOGUE();
    if (!isList(args[1]))
        return JSValue::encode(raiseBadArgument(globalObject, scope, "template"_s, "argument 2"_s, "list"_s, args[1]));
    auto* list = uncheckedDowncast<JSArray>(args[1].asCell());
    unsigned n = list->length();
    if (!(n & 1))
        return JSValue::encode(raiseTypeError(globalObject, scope, "invalid template"_s));
    n /= 2;
    auto fields = makeUnique<TemplateState>();
    MarkedArgumentBuffer literals;
    for (unsigned i = 0; i < n; ++i) {
        auto index = toSsizeOfInt(globalObject, list->getIndexQuickly(2 * i + 1));
        RETURN_IF_EXCEPTION(scope, { });
        if (*index < 0)
            return JSValue::encode(raiseTypeError(globalObject, scope, "invalid template"_s));
        JSValue literal = list->getIndexQuickly(2 * i + 2);
        bool isEmpty = false;
        if (JSString* string = stringIn(literal))
            isEmpty = !string->length();
        else if (isBytes(literal))
            isEmpty = bytesOf(literal.asCell()).empty();
        fields->items.append({ *index, !isEmpty });
        literals.append(isEmpty ? jsUndefined() : literal);
    }
    PyTuple* tuple = PyTuple::createFromArguments(globalObject, literals);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = PyStateObject::create(vm, moduleState(globalObject).substitution->instanceStructure(), WTF::move(fields));
    auto& made = object->state<TemplateState>();
    made.literal.set(vm, object, list->getIndexQuickly(0));
    made.literals.set(vm, object, tuple);
    return JSValue::encode(object);
}

PYTHON_NATIVE(sreGetCodeSize)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsNumber(static_cast<int>(sizeof(SRE_CODE))));
}

enum class Question : uint8_t { AsciiIsCased, UnicodeIsCased, AsciiToLower, UnicodeToLower };

// ascii_iscased(character, /) and the like
PYTHON_NATIVE(sreCharacter)
{
    auto question = unpack<Question>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto character = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto ch = static_cast<unsigned>(*character);
    switch (question) {
    case Question::AsciiIsCased:
        return JSValue::encode(jsBoolean(ch < 128 && isASCIIAlpha(ch)));
    case Question::UnicodeIsCased:
        return JSValue::encode(jsBoolean(ch != sre_lower_unicode(ch) || ch != sre_upper_unicode(ch)));
    case Question::AsciiToLower:
        return JSValue::encode(jsNumber(static_cast<int>(sre_lower_ascii(ch))));
    case Question::UnicodeToLower:
        return JSValue::encode(jsNumber(static_cast<int>(sre_lower_unicode(ch))));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- Match

namespace {

// match_getindex(): which group is meant by a number or by a name. Less than nothing if it raised. Nothing at all is the whole match.
int64_t getGroupIndex(JSGlobalObject* globalObject, MatchState& match, JSValue index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!index)
        return 0;
    int64_t i = -1;
    if (classify(index).isInt() || typeOf(globalObject, index)->lookup(vm, vm.pythonNames().dunder_index)) {
        // PyNumber_AsSsize_t(index, NULL), to which nothing is too large
        auto given = toSliceIndex(globalObject, index, false);
        RETURN_IF_EXCEPTION(scope, -1);
        i = *given;
    } else if (JSValue groupIndex = match.pattern->state<PatternState>().groupIndex.get()) {
        JSValue found = asDict(groupIndex)->get(globalObject, index);
        RETURN_IF_EXCEPTION(scope, -1);
        if (found && classify(found).isInt()) {
            auto number = toSsize(globalObject, found);
            RETURN_IF_EXCEPTION(scope, -1);
            i = *number;
        }
    }
    if (i < 0 || i >= match.groups) {
        raise(globalObject, scope, BuiltinType::IndexError, "no such group"_s);
        return -1;
    }
    return i;
}

// match_getslice(). Empty if it raised.
JSValue getGroup(JSGlobalObject* globalObject, MatchState& match, JSValue index, JSValue defaultValue)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int64_t i = getGroupIndex(globalObject, match, index);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getGroup(globalObject, match, i, defaultValue));
}

JSValue pair(JSGlobalObject* globalObject, int64_t first, int64_t second)
{
    return PyTuple::create(globalObject, { intFromInt64(globalObject, first), intFromInt64(globalObject, second) });
}

} // anonymous namespace

// expand(template)
PYTHON_NATIVE(matchExpand)
{
    NATIVE_PROLOGUE();
    auto& match = stateOf<MatchState>(args[0]);
    JSValue filter = compileTemplate(globalObject, match.pattern.get(), args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(expandTemplate(globalObject, stateOf<TemplateState>(filter), match)));
}

// group([group1, ...])
PYTHON_NATIVE(matchGroup)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "Match.group"_s))
        return { };
    auto& match = stateOf<MatchState>(args[0]);
    unsigned size = args.size() - 1;
    if (!size)
        RELEASE_AND_RETURN(scope, JSValue::encode(getGroup(globalObject, match, static_cast<int64_t>(0), jsUndefined())));
    if (size == 1)
        RELEASE_AND_RETURN(scope, JSValue::encode(getGroup(globalObject, match, args[1], jsUndefined())));
    MarkedArgumentBuffer items;
    for (unsigned i = 0; i < size; ++i) {
        items.append(getGroup(globalObject, match, args[i + 1], jsUndefined()));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::createFromArguments(globalObject, items)));
}

PYTHON_NATIVE(matchGetItem)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(getGroup(globalObject, stateOf<MatchState>(args[0]), args[1], jsUndefined())));
}

// groups(default=None)
PYTHON_NATIVE(matchGroups)
{
    NATIVE_PROLOGUE();
    auto& match = stateOf<MatchState>(args[0]);
    JSValue defaultValue = args.at(1) ? args.at(1) : jsUndefined();
    MarkedArgumentBuffer items;
    for (int64_t index = 1; index < match.groups; ++index) {
        items.append(getGroup(globalObject, match, index, defaultValue));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::createFromArguments(globalObject, items)));
}

// groupdict(default=None)
PYTHON_NATIVE(matchGroupDict)
{
    NATIVE_PROLOGUE();
    auto& match = stateOf<MatchState>(args[0]);
    JSValue defaultValue = args.at(1) ? args.at(1) : jsUndefined();
    PyDict* result = PyDict::create(globalObject);
    JSValue groupIndex = match.pattern->state<PatternState>().groupIndex.get();
    if (!groupIndex)
        return JSValue::encode(result);
    asDict(groupIndex)->forEach(globalObject, [&] (JSValue key, JSValue) {
        JSValue value = getGroup(globalObject, match, key, defaultValue);
        RETURN_IF_EXCEPTION(scope, false);
        result->set(globalObject, key, value);
        return !scope.exception();
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

enum class Which : uint8_t { Start, End, Span };

// start(group=0, /), end() and span()
PYTHON_NATIVE(matchSpan)
{
    auto which = unpack<Which>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto& match = stateOf<MatchState>(args[0]);
    int64_t index = getGroupIndex(globalObject, match, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    // They are -1 if the group took no part.
    switch (which) {
    case Which::Start:
        return JSValue::encode(intFromInt64(globalObject, match.mark[index * 2]));
    case Which::End:
        return JSValue::encode(intFromInt64(globalObject, match.mark[index * 2 + 1]));
    case Which::Span:
        return JSValue::encode(pair(globalObject, match.mark[index * 2], match.mark[index * 2 + 1]));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PYTHON_NATIVE(matchRepr)
{
    NATIVE_PROLOGUE();
    auto& match = stateOf<MatchState>(args[0]);
    JSValue whole = getGroup(globalObject, match, static_cast<int64_t>(0), jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });
    String shown = truncatedRepr(globalObject, whole, 50);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', typeName(globalObject, args[0]), " object; span=("_s, match.mark[0], ", "_s, match.mark[1], "), match="_s, shown, '>'))));
}

// ---- Scanner

namespace {

struct ScannerState final : NativeState {
    PYTHON_NATIVE_STATE(ScannerState);
    WriteBarrier<PyStateObject> pattern;
    WriteBarrier<Unknown> string;
    WriteBarrier<JSCell> owner; // Of the bytes, which the state does not keep alive.
    State state;
    bool isExecuting { false };
};

template<typename Visitor>
void ScannerState::visit(Visitor& visitor)
{
    visitor.append(pattern);
    visitor.append(string);
    visitor.append(owner);
}

// pattern_scanner(). Null if it raised.
PyStateObject* newScanner(JSGlobalObject* globalObject, PyStateObject* pattern, JSValue string, int64_t pos, int64_t endpos)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = PyStateObject::create(vm, moduleState(globalObject).scanner->instanceStructure(), makeUnique<ScannerState>());
    auto& scanner = object->state<ScannerState>();
    initializeState(globalObject, scanner.state, pattern->state<PatternState>(), string, pos, endpos);
    RETURN_IF_EXCEPTION(scope, nullptr);
    scanner.pattern.set(vm, object, pattern);
    scanner.string.set(vm, object, string);
    if (JSCell* owner = scanner.state.subject.owner)
        scanner.owner.set(vm, object, owner);
    return object;
}

} // anonymous namespace

// match() and search()
PYTHON_NATIVE(scannerNext)
{
    bool isSearch = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (args.size() > 1 || args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(isSearch ? "search"_s : "match"_s, "() takes no arguments"_s)));
    auto& scanner = stateOf<ScannerState>(args[0]);
    State& state = scanner.state;
    if (std::exchange(scanner.isExecuting, true))
        return JSValue::encode(raiseValueError(globalObject, scope, "regular expression scanner already executing"_s));
    auto whenDone = makeScopeExit([&] { scanner.isExecuting = false; });
    if (!state.start)
        RETURN_NONE();
    auto& pattern = scanner.pattern->state<PatternState>();
    resetState(state);
    state.ptr = state.start;
    ptrdiff_t status = isSearch ? runSearch(state, pattern) : runMatch(state, pattern);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue match = newMatch(globalObject, scanner.pattern.get(), state, scanner.string.get(), status);
    if (!status)
        state.start = nullptr;
    else {
        state.must_advance = state.ptr == state.start;
        state.start = state.ptr;
    }
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(match);
}

// scanner(string, pos=0, endpos=sys.maxsize) and finditer()
PYTHON_NATIVE(patternScanner)
{
    bool isIterator = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    int64_t pos = 0;
    int64_t endpos = std::numeric_limits<int64_t>::max();
    if (!toPosition(globalObject, args.at(2), pos) || !toPosition(globalObject, args.at(3), endpos))
        return { };
    PyStateObject* scanner = newScanner(globalObject, asPattern(args[0]), args[1], pos, endpos);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isIterator)
        return JSValue::encode(scanner);
    JSValue search = getAttribute(globalObject, scanner, Identifier::fromString(vm, "search"_s));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyIterator::create(globalObject, PyIterator::Kind::Callable, search, jsUndefined()));
}

// ---- The module

namespace {

SREModuleState& initializeSRE(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = realm->moduleState<SREModuleState>();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    auto create = [&] (WriteBarrier<PyType>& member, ASCIILiteral name, unsigned flags) {
        PyType* type = createBuiltinType(globalObject, name, realm->typeObject(), PyType::Layout::Native, flags);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        member.set(vm, realm, type);
        return type;
    };

    PyType* pattern = create(state.pattern, "re.Pattern"_s, PyType::HasWeakReferences);
    addMethods(globalObject, pattern, {
        { "__repr__"_s, patternRepr },
        { "__hash__"_s, patternHash },
        { "match"_s, patternMatch, Kind::Method, pack(How::Match) },
        { "fullmatch"_s, patternMatch, Kind::Method, pack(How::FullMatch) },
        { "search"_s, patternMatch, Kind::Method, pack(How::Search) },
        { "sub"_s, patternSub, Kind::Method, pack(false) },
        { "subn"_s, patternSub, Kind::Method, pack(true) },
        { "findall"_s, patternFindAll },
        { "split"_s, patternSplit },
        { "finditer"_s, patternScanner, Kind::Method, pack(true) },
        { "scanner"_s, patternScanner, Kind::Method, pack(false) },
        { "__copy__"_s, sreCopy },
        { "__deepcopy__"_s, sreCopy },
    });
    addComparisons(globalObject, pattern, patternCompare);
    addClassGetItemIfGeneric(globalObject, pattern);
    addGetSet(globalObject, pattern, "groupindex"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        JSValue groupIndex = stateOf<PatternState>(self).groupIndex.get();
        if (!groupIndex)
            return PyDict::create(globalObject);
        return PyNativeObject::create(globalObject, BuiltinType::MappingProxy, groupIndex);
    });
    addMember(globalObject, pattern, "pattern"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<PatternState>(self).pattern.get(); });
    addMember(globalObject, pattern, "flags"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<PatternState>(self).flags); });
    addMember(globalObject, pattern, "groups"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return intFromInt64(globalObject, stateOf<PatternState>(self).groups); });

    PyType* match = create(state.match, "re.Match"_s, 0);
    addMethods(globalObject, match, {
        { "__repr__"_s, matchRepr },
        { "__getitem__"_s, matchGetItem },
        { "group"_s, matchGroup, Kind::Method, 0, "($self, /, *args)"_s, Arguments::AreNotChecked },
        { "start"_s, matchSpan, Kind::Method, pack(Which::Start) },
        { "end"_s, matchSpan, Kind::Method, pack(Which::End) },
        { "span"_s, matchSpan, Kind::Method, pack(Which::Span) },
        { "groups"_s, matchGroups },
        { "groupdict"_s, matchGroupDict },
        { "expand"_s, matchExpand },
        { "__copy__"_s, sreCopy },
        { "__deepcopy__"_s, sreCopy },
    });
    addClassGetItemIfGeneric(globalObject, match);
    addGetSet(globalObject, match, "lastindex"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        int64_t lastIndex = stateOf<MatchState>(self).lastIndex;
        return lastIndex >= 0 ? intFromInt64(globalObject, lastIndex) : jsUndefined();
    });
    addGetSet(globalObject, match, "lastgroup"_s, [] (JSGlobalObject*, JSValue self) -> JSValue {
        auto& match = stateOf<MatchState>(self);
        JSValue indexGroup = match.pattern->state<PatternState>().indexGroup.get();
        if (indexGroup && match.lastIndex >= 0 && match.lastIndex < static_cast<int64_t>(asTuple(indexGroup)->length()))
            return asTuple(indexGroup)->at(static_cast<unsigned>(match.lastIndex));
        return jsUndefined();
    });
    addGetSet(globalObject, match, "regs"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        auto& match = stateOf<MatchState>(self);
        if (JSValue regs = match.regs.get())
            return regs;
        MarkedArgumentBuffer items;
        for (int64_t index = 0; index < match.groups; ++index)
            items.append(pair(globalObject, match.mark[index * 2], match.mark[index * 2 + 1]));
        PyTuple* regs = PyTuple::createFromArguments(globalObject, items);
        match.regs.set(globalObject->vm(), self.asCell(), regs);
        return regs;
    });
    addMember(globalObject, match, "string"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<MatchState>(self).string.get(); });
    addMember(globalObject, match, "re"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<MatchState>(self).pattern.get(); });
    addMember(globalObject, match, "pos"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return intFromInt64(globalObject, stateOf<MatchState>(self).pos); });
    addMember(globalObject, match, "endpos"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return intFromInt64(globalObject, stateOf<MatchState>(self).endpos); });

    PyType* scanner = create(state.scanner, "_sre.SRE_Scanner"_s, 0);
    addMethods(globalObject, scanner, {
        { "match"_s, scannerNext, Kind::Method, pack(false), { }, Arguments::AreNotChecked },
        { "search"_s, scannerNext, Kind::Method, pack(true), { }, Arguments::AreNotChecked },
    });
    addMember(globalObject, scanner, "pattern"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<ScannerState>(self).pattern.get(); });

    create(state.substitution, "_sre.SRE_Template"_s, 0);
    return state;
}

} // anonymous namespace

JSObject* createSREModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    moduleState(globalObject);
    JSObject* module = newBuiltinModule(globalObject, "_sre"_s);
    addFunction(globalObject, module, "compile"_s, sreCompile);
    addFunction(globalObject, module, "template"_s, sreTemplate);
    addFunction(globalObject, module, "getcodesize"_s, sreGetCodeSize);
    addFunction(globalObject, module, "ascii_iscased"_s, sreCharacter, pack(Question::AsciiIsCased));
    addFunction(globalObject, module, "unicode_iscased"_s, sreCharacter, pack(Question::UnicodeIsCased));
    addFunction(globalObject, module, "ascii_tolower"_s, sreCharacter, pack(Question::AsciiToLower));
    addFunction(globalObject, module, "unicode_tolower"_s, sreCharacter, pack(Question::UnicodeToLower));
    auto add = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };
    add("MAGIC"_s, intFromUInt64(globalObject, SRE_MAGIC));
    add("CODESIZE"_s, jsNumber(static_cast<int>(sizeof(SRE_CODE))));
    add("MAXREPEAT"_s, intFromInt64(globalObject, SRE_MAXREPEAT));
    add("MAXGROUPS"_s, intFromInt64(globalObject, SRE_MAXGROUPS));
    add("copyright"_s, jsNontrivialString(vm, " SRE 2.2.2 Copyright (c) 1997-2002 by Secret Labs AB "_s));
    return module;
}

} } // namespace JSC::Python
