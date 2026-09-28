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

#include "PyFrame.h"
#include "PythonParser.h"
#include "PythonSignatures.h"
#include "TopExceptionScope.h"
#include <unicode/uchar.h>
#include <wtf/text/StringBuilder.h>

// What is printed when an exception gets away. It is TracebackException, StackSummary and what they use, of CPython's Lib/traceback.py, function for function, which is what CPython prints
// one with. Where that has the ast module parse a piece of a line, to find what part of it to point at, this has the parser.

namespace JSC { namespace Python {

namespace {

// Python counts in code points, and so does this.
using Line = Vector<char32_t>;

Line toLine(StringView view)
{
    Line line;
    line.reserveInitialCapacity(view.length());
    for (char32_t character : view.codePoints())
        line.append(character);
    return line;
}

Line toLine(ASCIILiteral literal) { return toLine(StringView(literal)); }

void append(StringBuilder& builder, std::span<const char32_t> characters)
{
    for (char32_t character : characters)
        builder.append(character);
}

String toString(std::span<const char32_t> characters)
{
    StringBuilder builder;
    append(builder, characters);
    return builder.toString();
}

bool isBlank(std::span<const char32_t> characters)
{
    return std::ranges::all_of(characters, isSpace);
}

size_t leadingSpaces(const Line& line)
{
    size_t count = 0;
    while (count < line.size() && isSpace(line[count]))
        ++count;
    return count;
}

void stripRight(Line& line)
{
    while (!line.isEmpty() && isSpace(line.last()))
        line.removeLast();
}

// textwrap.dedent(): takes away the white space that every line begins with. A line of nothing else has none of it counted, and comes out empty.
void dedent(Vector<Line>& lines)
{
    std::optional<size_t> margin;
    const Line* first = nullptr;
    for (const Line& line : lines) {
        if (isBlank(line.span()))
            continue;
        if (!first) {
            first = &line;
            size_t count = 0;
            while (count < line.size() && (line[count] == ' ' || line[count] == '\t'))
                ++count;
            margin = count;
            continue;
        }
        size_t count = 0;
        while (count < *margin && count < line.size() && line[count] == (*first)[count])
            ++count;
        margin = count;
    }
    for (Line& line : lines) {
        if (isBlank(line.span()))
            line.clear();
        else if (margin)
            line.removeAt(0, *margin);
    }
}

// _byte_offset_to_character_offset()
size_t characterOffsetForByteOffset(const Line& line, size_t bytes)
{
    size_t used = 0;
    size_t count = 0;
    while (count < line.size() && used < bytes) {
        char32_t character = line[count++];
        used += character < 0x80 ? 1 : character < 0x800 ? 2 : character < 0x10000 ? 3 : 4;
    }
    return count;
}

// _display_width(): how many columns of a terminal it takes
size_t displayWidth(const Line& line, std::optional<size_t> offset = std::nullopt)
{
    size_t count = std::min(offset.value_or(line.size()), line.size());
    // An offset that is beyond a line that is all ASCII is given back as it is.
    if (std::ranges::all_of(line, [] (char32_t character) { return character < 0x80; }))
        return offset.value_or(line.size());
    size_t width = 0;
    for (size_t i = 0; i < count; ++i) {
        auto kind = u_getIntPropertyValue(line[i], UCHAR_EAST_ASIAN_WIDTH);
        width += kind == U_EA_WIDE || kind == U_EA_FULLWIDTH ? 2 : 1;
    }
    return width;
}

String join(const Vector<Line>& lines)
{
    StringBuilder builder;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i)
            builder.append('\n');
        append(builder, lines[i].span());
    }
    return builder.toString();
}

Module* parseQuietly(VM& vm, Arena& arena, const String& source)
{
    Vector<SyntaxWarning> warnings;
    SyntaxError error;
    return parse(vm, arena, source, Module::Kind::Module, warnings, error);
}

// ---- What part of a line to point at

struct Anchors {
    size_t leftEndLine;
    size_t leftEndOffset;
    size_t rightStartLine;
    size_t rightStartOffset;
};

// _extract_caret_anchors_from_line_segment(): for `a + b`, where the operator is, and for a[b] and a(b), where the brackets are. What fails an assertion there, or goes off the end of
// something, is caught by what calls it, and comes to nothing.
class AnchorFinder {
public:
    AnchorFinder(VM& vm, const Vector<Line>& lines)
        : m_vm(vm)
        , m_lines(lines)
    {
    }

    std::optional<Anchors> find()
    {
        Arena arena;
        Module* tree = parseQuietly(m_vm, arena, makeString("(\n"_s, join(m_lines), "\n)"_s));
        if (!tree || tree->body.size() != 1 || !tree->body[0]->is<Expr>())
            return std::nullopt;
        Expression* expression = tree->body[0]->as<Expr>().value;
        std::optional<Anchors> result;
        if (auto* operation = expression->tryAs<BinOp>()) {
            Position position = incrementUntil(endOf(*operation->left), [] (char32_t character) { return !isSpace(character) && character != ')'; });
            if (m_failed)
                return std::nullopt;
            size_t rightColumn = position.column + 1;
            const Line& line = m_lines[position.line];
            size_t rightLine = operation->right->line - 2;
            if (rightLine >= m_lines.size())
                return std::nullopt;
            // An operator of two characters
            if (rightColumn < line.size()
                && (rightLine > position.line || rightColumn < characterOffsetForByteOffset(m_lines[rightLine], operation->right->column))
                && !isSpace(line[rightColumn]) && line[rightColumn] != '\\' && line[rightColumn] != '#')
                ++rightColumn;
            result = Anchors { position.line, position.column, position.line, rightColumn };
        } else if (auto* subscript = expression->tryAs<Subscript>())
            result = brackets(*subscript->value, *expression, '[');
        else if (auto* call = expression->tryAs<Call>())
            result = brackets(*call->function, *expression, '(');
        return m_failed ? std::nullopt : result;
    }

private:
    struct Position {
        size_t line;
        size_t column;
    };

    std::optional<Anchors> brackets(const Node& before, const Node& whole, char32_t opening)
    {
        Position left = incrementUntil(endOf(before), [&] (char32_t character) { return character == opening; });
        Position right = endOf(whole, false);
        return Anchors { left.line, left.column, right.line, right.column };
    }

    Position fail()
    {
        m_failed = true;
        return { 0, 0 };
    }

    Position nextValidCharacter(Position position)
    {
        while (position.line < m_lines.size() && position.column >= m_lines[position.line].size())
            position = { position.line + 1, 0 };
        return position.line < m_lines.size() ? position : fail();
    }

    template<typename Predicate>
    Position incrementUntil(Position position, const Predicate& stop)
    {
        while (!m_failed) {
            char32_t character = m_lines[position.line][position.column];
            if (character == '\\' || character == '#')
                position = nextValidCharacter({ position.line + 1, 0 });
            else if (!stop(character))
                position = nextValidCharacter({ position.line, position.column + 1 });
            else
                break;
        }
        return position;
    }

    // setup_positions()
    Position endOf(const Node& node, bool forceValid = true)
    {
        size_t line = node.endLine - 2;
        if (line >= m_lines.size())
            return fail();
        Position position { line, characterOffsetForByteOffset(m_lines[line], node.endColumn) };
        return forceValid ? nextValidCharacter(position) : position;
    }

    VM& m_vm;
    const Vector<Line>& m_lines;
    bool m_failed { false };
};

// StackSummary._should_show_carets()
bool shouldShowCarets(VM& vm, size_t startOffset, size_t endOffset, const Vector<Line>& lines, bool hasAnchors)
{
    Arena arena;
    if (Module* tree = parseQuietly(vm, arena, join(lines))) {
        if (tree->body.empty())
            return false;
        // `return f(x)` and `a = f(x)` say all that there is to say.
        Statement* statement = tree->body[0];
        Expression* value = nullptr;
        if (statement->is<Return>()) {
            auto& node = statement->as<Return>();
            if (node.value && node.value->is<Call>() && node.value->as<Call>().function->is<Name>())
                value = node.value;
        } else if (statement->is<Assign>()) {
            auto& node = statement->as<Assign>();
            if (node.value->is<Call>() && node.targets.size() == 1 && node.targets[0]->is<Name>())
                value = node.value;
        }
        if (value && value->line == 1 && value->endLine == lines.size() && value->column == startOffset && value->endColumn == endOffset)
            return false;
    }
    if (hasAnchors)
        return true;
    const Line& first = lines.first();
    const Line& last = lines.last();
    return !isBlank(first.span().first(std::min(startOffset, first.size()))) || !isBlank(last.span().subspan(std::min(endOffset, last.size())));
}

// ---- A frame

struct FrameSummary {
    String filename;
    String name;
    unsigned line;
    String text;
};

// What linecache has no lines for
bool isNameOfNoFile(const String& filename)
{
    return filename.isEmpty() || (filename.startsWith('<') && filename.endsWith('>'));
}

// StackSummary.format_frame_summary(), from where it has the lines
void appendSource(VM& vm, StringBuilder& builder, StringView source, unsigned start, unsigned end)
{
    start = std::min(start, source.length());
    end = std::clamp(end, start, source.length());
    unsigned lineStart = start;
    while (lineStart && source[lineStart - 1] != '\n')
        --lineStart;

    Vector<Line> original;
    size_t startOffset = toLine(source.substring(lineStart, start - lineStart)).size();
    size_t endOffset = 0;
    for (unsigned cursor = lineStart;;) {
        unsigned lineEnd = cursor;
        while (lineEnd < source.length() && source[lineEnd] != '\n')
            ++lineEnd;
        original.append(toLine(source.substring(cursor, lineEnd - cursor)));
        stripRight(original.last());
        if (end <= lineEnd || lineEnd == source.length()) {
            endOffset = toLine(source.substring(cursor, end - cursor)).size();
            break;
        }
        cursor = lineEnd + 1;
    }

    Vector<Line> lines = original;
    dedent(lines);
    if (std::ranges::all_of(lines, [] (const Line& line) { return line.isEmpty(); }))
        return;

    size_t dedentedBy = original[0].size() - lines[0].size();
    startOffset -= std::min(startOffset, dedentedBy);
    endOffset -= std::min(endOffset, dedentedBy);
    size_t displayedStart = displayWidth(lines.first(), startOffset);
    size_t displayedEnd = displayWidth(lines.last(), endOffset);

    // What is between the two
    Vector<Line> segment = lines;
    segment.last().shrink(std::min(endOffset, segment.last().size()));
    segment.first().removeAt(0, std::min(startOffset, segment.first().size()));

    std::optional<Anchors> anchors = AnchorFinder(vm, segment).find();
    bool showCarets = shouldShowCarets(vm, startOffset, endOffset, lines, !!anchors);

    // The first and the last, and those about what is pointed at.
    Vector<bool> isSignificant;
    isSignificant.fill(false, lines.size());
    isSignificant.first() = isSignificant.last() = true;
    size_t leftEndOffset = 0;
    size_t rightStartOffset = 0;
    char32_t primary = '^';
    char32_t secondary = '^';
    if (anchors) {
        leftEndOffset = displayWidth(lines[anchors->leftEndLine], anchors->leftEndOffset + (anchors->leftEndLine ? 0 : startOffset));
        rightStartOffset = displayWidth(lines[anchors->rightStartLine], anchors->rightStartOffset + (anchors->rightStartLine ? 0 : startOffset));
        primary = '~';
        for (size_t line : { anchors->leftEndLine, anchors->rightStartLine }) {
            for (size_t near = line ? line - 1 : 0; near <= line + 1 && near < lines.size(); ++near)
                isSignificant[near] = true;
        }
    }

    Vector<Line> result;
    auto outputLine = [&] (size_t index) {
        const Line& line = lines[index];
        result.append(line);
        if (!showCarets)
            return;
        size_t spaces = leadingSpaces(line);
        size_t count = index == lines.size() - 1 ? displayedEnd : displayWidth(line);
        Line carets;
        for (size_t column = 0; column < count; ++column) {
            if (column < spaces || (!index && column < displayedStart))
                carets.append(' ');
            else if (anchors
                && (index > anchors->leftEndLine || (index == anchors->leftEndLine && column >= leftEndOffset))
                && (index < anchors->rightStartLine || (index == anchors->rightStartLine && column < rightStartOffset)))
                carets.append(secondary);
            else
                carets.append(primary);
        }
        result.append(WTF::move(carets));
    };

    std::optional<size_t> previous;
    for (size_t index = 0; index < lines.size(); ++index) {
        if (!isSignificant[index])
            continue;
        if (previous) {
            size_t difference = index - *previous;
            if (difference == 2)
                outputLine(index - 1);
            else if (difference > 2)
                result.append(toLine(StringView(makeString("...<"_s, difference - 1, " lines>..."_s))));
        }
        outputLine(index);
        previous = index;
    }

    dedent(result);
    for (const Line& line : result) {
        builder.append("    "_s);
        append(builder, line.span());
        builder.append('\n');
    }
}

FrameSummary summarize(JSGlobalObject* globalObject, PyFrame* frame, unsigned bytecodeOffset, unsigned line)
{
    VM& vm = globalObject->vm();
    SourceProvider* provider = frame->executable()->source().provider();
    FrameSummary summary { provider->sourceURL(), frame->functionInfo().name.string(), line, { } };
    StringBuilder builder;
    builder.append("  File \""_s, summary.filename, "\", line "_s, line, ", in "_s, summary.name, '\n');
    if (!isNameOfNoFile(summary.filename)) {
        if (auto range = frame->sourceRangeAt(vm, BytecodeIndex(bytecodeOffset)))
            appendSource(vm, builder, provider->source(), range->first, range->second);
    }
    summary.text = builder.toString();
    return summary;
}

} // anonymous namespace

// StackSummary.format(). Empty if there is nothing in it.
String formatStack(JSGlobalObject* globalObject, JSValue traceback)
{
    // _RECURSIVE_CUTOFF
    static constexpr unsigned cutoff = 3;
    StringBuilder builder;
    std::optional<FrameSummary> last;
    unsigned count = 0;
    auto finishRun = [&] {
        if (count > cutoff)
            builder.append("  [Previous line repeated "_s, count - cutoff, " more time"_s, count - cutoff > 1 ? "s"_s : ""_s, "]\n"_s);
    };
    forEachTracebackEntry(globalObject, traceback, [&] (PyFrame* frame, unsigned bytecodeOffset, unsigned line) {
        // Only what is going to be shown need be looked into.
        String filename = frame->executable()->source().provider()->sourceURL();
        if (last && last->filename == filename && last->line == line && last->name == frame->functionInfo().name.string()) {
            if (++count > cutoff)
                return;
            builder.append(summarize(globalObject, frame, bytecodeOffset, line).text);
            return;
        }
        finishRun();
        last = summarize(globalObject, frame, bytecodeOffset, line);
        count = 1;
        builder.append(last->text);
    });
    finishRun();
    return builder.toString();
}

namespace {

// ---- An exception

// _safe_string()
template<typename Function>
String safeString(JSGlobalObject* globalObject, JSValue value, ASCIILiteral what, ASCIILiteral functionName, const Function& function)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    String result = function(globalObject, value);
    if (scope.exception()) {
        scope.clearException();
        return makeString('<', what, ' ', functionName, "() failed>"_s);
    }
    return result;
}

String safeStr(JSGlobalObject* globalObject, JSValue value, ASCIILiteral what) { return safeString(globalObject, value, what, "str"_s, static_cast<String (*)(JSGlobalObject*, JSValue)>(str)); }
String safeRepr(JSGlobalObject* globalObject, JSValue value, ASCIILiteral what) { return safeString(globalObject, value, what, "repr"_s, static_cast<String (*)(JSGlobalObject*, JSValue)>(repr)); }

// getattr(value, name, None), and None too if getting it raises.
JSValue attributeOrNone(JSGlobalObject* globalObject, JSValue value, const Identifier& name, bool* raised = nullptr)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    JSValue result = getAttributeIfPresent(globalObject, value, name);
    if (scope.exception()) {
        if (!raised)
            scope.clearException();
        else
            *raised = true;
        return jsUndefined();
    }
    return result ? result : jsUndefined();
}

JSValue attributeOrNone(JSGlobalObject* globalObject, JSValue value, ASCIILiteral name) { return attributeOrNone(globalObject, value, Identifier::fromString(globalObject->vm(), name)); }

// The names in something that can be gone through, those of them that are strings. False if it raises.
bool appendNames(JSGlobalObject* globalObject, JSValue iterable, Vector<String>& names)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    forEach(globalObject, iterable, [&] (JSValue item) {
        if (item.isString())
            names.append(asString(item)->value(globalObject));
        return true;
    });
    if (!scope.exception())
        return true;
    scope.clearException();
    return false;
}

// _get_safe___dir__()
bool appendDirectory(JSGlobalObject* globalObject, JSValue object, Vector<String>& names)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Identifier dunderDir = Identifier::fromString(vm, "__dir__"_s);
    JSValue method = getAttribute(globalObject, object, dunderDir);
    JSValue directory = scope.exception() ? JSValue() : call(globalObject, method);
    // Of a class, it is the one for its instances.
    if (scope.exception() && catchException(globalObject, BuiltinType::TypeError)) {
        method = getAttribute(globalObject, typeOf(globalObject, object), dunderDir);
        directory = scope.exception() ? JSValue() : call(globalObject, method, object);
    }
    if (scope.exception()) {
        scope.clearException();
        return false;
    }
    if (!appendNames(globalObject, directory, names))
        return false;
    std::ranges::sort(names, [] (const String& a, const String& b) { return codePointCompareLessThan(a, b); });
    return true;
}

JSValue lastFrameOf(JSGlobalObject* globalObject, JSValue traceback)
{
    JSValue last;
    forEachTracebackEntry(globalObject, traceback, [&] (PyFrame* frame, unsigned, unsigned) {
        last = frame;
    });
    return last;
}

// frame.f_locals['self'], if there is one
JSValue selfOf(JSGlobalObject* globalObject, JSValue locals)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue self = getItem(globalObject, locals, jsString(vm, String("self"_s)));
    if (scope.exception()) {
        scope.clearException();
        return { };
    }
    return self;
}

void removeUnderscored(Vector<String>& names)
{
    names.removeAllMatching([] (const String& name) { return name.startsWith('_'); });
}

// _compute_suggestion_error(): a name that there is, that the one that there is not may have been meant for. Null if there is none.
String computeSuggestion(JSGlobalObject* globalObject, JSValue exception, JSValue traceback, JSValue wrongNameValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    if (!wrongNameValue.isString())
        return { };
    String wrongName = asString(wrongNameValue)->value(globalObject);
    Vector<String> names;
    if (isInstance(globalObject, exception, realm->typeAttributeError())) {
        JSValue object = attributeOrNone(globalObject, exception, "obj"_s);
        if (!appendDirectory(globalObject, object, names))
            return { };
        bool hidesUnderscored = !wrongName.startsWith('_');
        // What is private is not, to a method of the object.
        if (JSValue frame = hidesUnderscored ? lastFrameOf(globalObject, traceback) : JSValue()) {
            if (JSValue self = selfOf(globalObject, attributeOrNone(globalObject, frame, "f_locals"_s)); self && self == object)
                hidesUnderscored = false;
        }
        if (hidesUnderscored)
            removeUnderscored(names);
    } else if (isInstance(globalObject, exception, realm->typeImportError())) {
        JSValue importer = getStoredAttribute(vm, realm->builtinsModule(), Identifier::fromString(vm, "__import__"_s));
        JSValue module = importer ? call(globalObject, importer, attributeOrNone(globalObject, exception, "name"_s)) : JSValue();
        if (scope.exception() || !module) {
            scope.clearException();
            return { };
        }
        if (!appendDirectory(globalObject, module, names))
            return { };
        if (!wrongName.startsWith('_'))
            removeUnderscored(names);
    } else {
        JSValue frame = lastFrameOf(globalObject, traceback);
        if (!frame)
            return { };
        JSValue locals = attributeOrNone(globalObject, frame, "f_locals"_s);
        for (JSValue mapping : { locals, attributeOrNone(globalObject, frame, "f_globals"_s), attributeOrNone(globalObject, frame, "f_builtins"_s) }) {
            if (!appendNames(globalObject, mapping, names))
                return { };
        }
        if (JSValue self = selfOf(globalObject, locals)) {
            JSValue found = getAttributeIfPresent(globalObject, self, Identifier::fromString(vm, wrongName));
            if (scope.exception())
                scope.clearException();
            else if (found)
                return makeString("self."_s, wrongName);
        }
    }
    return calculateSuggestion(names, wrongName);
}

class ExceptionSummary {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(ExceptionSummary);
public:
    JSValue exception;
    String stack;
    String type; // exc_type_str
    String message; // _str
    String notes; // As they are printed.
    bool suppressesContext { false };
    std::unique_ptr<ExceptionSummary> cause;
    std::unique_ptr<ExceptionSummary> context;
    std::optional<Vector<std::unique_ptr<ExceptionSummary>>> exceptions;
};

class Formatter {
public:
    explicit Formatter(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
    {
    }

    String format(JSValue exception)
    {
        auto summary = summarizeAll(exception);
        format(*summary);
        return m_builder.toString();
    }

private:
    static constexpr unsigned maxGroupWidth = 15;
    static constexpr unsigned maxGroupDepth = 10;

    // ---- TracebackException.__init__()

    std::unique_ptr<ExceptionSummary> summarizeOne(JSValue exception)
    {
        JSGlobalObject* globalObject = m_globalObject;
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        auto& names = m_vm.pythonNames();
        PyRealm* realm = globalObject->pyRealm();
        auto summary = makeUnique<ExceptionSummary>();
        summary->exception = exception;
        m_kept.append(exception);
        if (exception.isCell())
            m_seen.add(exception.asCell());

        JSValue traceback = attributeOrNone(globalObject, exception, names.dunder_traceback);
        summary->stack = formatStack(globalObject, traceback);
        summary->message = safeStr(globalObject, exception, "exception"_s);

        bool raised = false;
        JSValue notes = attributeOrNone(globalObject, exception, names.dunder_notes, &raised);
        StringBuilder noteLines;
        if (raised) {
            JSValue error = scope.exception()->value();
            scope.clearException();
            noteLines.append("Ignored error getting __notes__: "_s, safeRepr(globalObject, error, "__notes__"_s), '\n');
        } else if (isList(notes) || isTuple(notes)) {
            forEach(globalObject, notes, [&] (JSValue note) {
                noteLines.append(safeStr(globalObject, note, "note"_s), '\n');
                return true;
            });
            scope.clearException();
        } else if (!isNone(notes))
            noteLines.append(safeRepr(globalObject, notes, "__notes__"_s), '\n');
        summary->notes = noteLines.toString();

        // exc_type_str
        PyType* type = typeOf(globalObject, exception);
        JSValue module = attributeOrNone(globalObject, type, names.dunder_module);
        String qualifiedName = safeStr(globalObject, attributeOrNone(globalObject, type, names.dunder_qualname), "exception"_s);
        String moduleName = module.isString() ? asString(module)->value(globalObject) : String("<unknown>"_s);
        summary->type = moduleName == "__main__"_s || moduleName == "builtins"_s ? qualifiedName : makeString(moduleName, '.', qualifiedName);

        if (isInstance(globalObject, exception, realm->typeSyntaxError())) {
            // Where it is comes with the message, when that is printed.
        } else if (JSValue nameFrom = isInstance(globalObject, exception, realm->typeImportError()) ? attributeOrNone(globalObject, exception, "name_from"_s) : jsUndefined(); !isNone(nameFrom)) {
            if (String suggestion = computeSuggestion(globalObject, exception, traceback, nameFrom); !suggestion.isNull())
                summary->message = makeString(summary->message, ". Did you mean: '"_s, suggestion, "'?"_s);
        } else if (bool isNameError = isInstance(globalObject, exception, realm->typeNameError()); isNameError || isInstance(globalObject, exception, realm->typeAttributeError())) {
            if (JSValue wrongName = attributeOrNone(globalObject, exception, "name"_s); !isNone(wrongName)) {
                String suggestion = computeSuggestion(globalObject, exception, traceback, wrongName);
                if (!suggestion.isNull())
                    summary->message = makeString(summary->message, ". Did you mean: '"_s, suggestion, "'?"_s);
                if (isNameError && isNameOfStandardModule(wrongName))
                    summary->message = makeString(summary->message, suggestion.isNull() ? ". Did"_s : " Or did"_s, " you forget to import '"_s, safeStr(globalObject, wrongName, "exception"_s), "'?"_s);
            }
        }
        scope.clearException();

        JSValue suppresses = attributeOrNone(globalObject, exception, "__suppress_context__"_s);
        summary->suppressesContext = isTrue(globalObject, suppresses);
        scope.clearException();
        return summary;
    }

    // wrong_name in sys.stdlib_module_names
    bool isNameOfStandardModule(JSValue name)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        JSValue modules = sysAttribute(m_globalObject, "stdlib_module_names"_s);
        bool result = modules && contains(m_globalObject, modules, name);
        scope.clearException();
        return result;
    }

    bool hasBeenSeen(JSValue exception) { return exception.isCell() && m_seen.contains(exception.asCell()); }

    // With what led to it, and what is in it if it is a group. What has been come to before is left out, so that it ends.
    std::unique_ptr<ExceptionSummary> summarizeAll(JSValue root)
    {
        JSGlobalObject* globalObject = m_globalObject;
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        auto result = summarizeOne(root);
        Vector<ExceptionSummary*> queue { result.get() };
        while (!queue.isEmpty()) {
            ExceptionSummary* summary = queue.takeLast();
            JSValue exception = summary->exception;
            JSValue cause = attributeOrNone(globalObject, exception, "__cause__"_s);
            if (!isNone(cause) && !hasBeenSeen(cause))
                summary->cause = summarizeOne(cause);
            JSValue context = attributeOrNone(globalObject, exception, "__context__"_s);
            if (!isNone(context) && !summary->cause && !summary->suppressesContext && !hasBeenSeen(context))
                summary->context = summarizeOne(context);
            if (isInstance(globalObject, exception, globalObject->pyRealm()->typeBaseExceptionGroup())) {
                summary->exceptions.emplace();
                forEach(globalObject, attributeOrNone(globalObject, exception, "exceptions"_s), [&] (JSValue member) {
                    summary->exceptions->append(summarizeOne(member));
                    return true;
                });
                scope.clearException();
            }
            if (summary->cause)
                queue.append(summary->cause.get());
            if (summary->context)
                queue.append(summary->context.get());
            if (summary->exceptions) {
                for (auto& member : *summary->exceptions)
                    queue.append(member.get());
            }
        }
        return result;
    }

    // ---- _ExceptionPrintContext

    void appendIndent() 
    {
        for (unsigned i = 0; i < 2 * m_groupDepth; ++i)
            m_builder.append(' ');
    }

    // Each line of it, with what is in front of a line at this depth.
    void emit(StringView text, char margin = '|')
    {
        while (!text.isEmpty()) {
            size_t end = text.find('\n');
            StringView line = end == notFound ? text : text.left(end + 1);
            appendIndent();
            if (m_groupDepth)
                m_builder.append(margin, ' ');
            m_builder.append(line);
            text = text.substring(line.length());
        }
    }

    // ---- TracebackException.format_exception_only()

    void emitExceptionOnly(const ExceptionSummary& summary)
    {
        StringBuilder builder;
        String message = appendSyntaxErrorLocation(m_globalObject, builder, summary.exception);
        if (message.isNull())
            message = summary.message;
        builder.append(summary.type);
        if (!message.isEmpty())
            builder.append(": "_s, message);
        builder.append('\n', summary.notes);
        emit(builder.toString());
    }

    // ---- TracebackException.format()

    void format(const ExceptionSummary& last)
    {
        // What led to it comes first.
        Vector<std::pair<ASCIILiteral, const ExceptionSummary*>> output;
        for (const ExceptionSummary* summary = &last; summary;) {
            if (summary->cause) {
                output.append({ "\nThe above exception was the direct cause of the following exception:\n\n"_s, summary });
                summary = summary->cause.get();
            } else if (summary->context && !summary->suppressesContext) {
                output.append({ "\nDuring handling of the above exception, another exception occurred:\n\n"_s, summary });
                summary = summary->context.get();
            } else {
                output.append({ { }, summary });
                summary = nullptr;
            }
        }

        for (auto& [chainedMessage, summary] : output | std::views::reverse) {
            if (!chainedMessage.isNull())
                emit(chainedMessage);
            if (!summary->exceptions) {
                if (!summary->stack.isEmpty()) {
                    emit("Traceback (most recent call last):\n"_s);
                    emit(summary->stack);
                }
                emitExceptionOnly(*summary);
                continue;
            }
            if (m_groupDepth > maxGroupDepth) {
                emit(makeString("... (max_group_depth is "_s, maxGroupDepth, ")\n"_s));
                continue;
            }

            bool isTopLevel = !m_groupDepth;
            if (isTopLevel)
                ++m_groupDepth;
            if (!summary->stack.isEmpty()) {
                emit("Exception Group Traceback (most recent call last):\n"_s, isTopLevel ? '+' : '|');
                emit(summary->stack);
            }
            emitExceptionOnly(*summary);
            size_t total = summary->exceptions->size();
            size_t count = total <= maxGroupWidth ? total : maxGroupWidth + 1;
            m_needsClose = false;
            for (size_t i = 0; i < count; ++i) {
                bool isLast = i == count - 1;
                if (isLast)
                    m_needsClose = true;
                bool isTruncated = i >= maxGroupWidth;
                appendIndent();
                m_builder.append(i ? "  "_s : "+-"_s, "+---------------- "_s);
                if (isTruncated)
                    m_builder.append("..."_s);
                else
                    m_builder.append(i + 1);
                m_builder.append(" ----------------\n"_s);
                ++m_groupDepth;
                if (!isTruncated)
                    format(*summary->exceptions->at(i));
                else {
                    size_t remaining = total - maxGroupWidth;
                    emit(makeString("and "_s, remaining, " more exception"_s, remaining > 1 ? "s"_s : ""_s, '\n'));
                }
                if (isLast && m_needsClose) {
                    appendIndent();
                    m_builder.append("+------------------------------------\n"_s);
                    m_needsClose = false;
                }
                --m_groupDepth;
            }
            if (isTopLevel)
                m_groupDepth = 0;
        }
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    StringBuilder m_builder;
    MarkedArgumentBuffer m_kept;
    UncheckedKeyHashSet<JSCell*> m_seen;
    unsigned m_groupDepth { 0 };
    bool m_needsClose { false };
};

} // anonymous namespace

String formatTraceback(JSGlobalObject* globalObject, JSValue traceback)
{
    String stack = formatStack(globalObject, traceback);
    return stack.isEmpty() ? stack : makeString("Traceback (most recent call last):\n"_s, stack);
}

String formatException(JSGlobalObject* globalObject, JSValue exception)
{
    return Formatter(globalObject).format(exception);
}

} } // namespace JSC::Python
