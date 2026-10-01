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
#include "PythonSignatures.h"

#include "PythonBuiltins.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace Python {

WTF_MAKE_TZONE_ALLOCATED_IMPL(NativeSignature);

#include "PythonBuiltinDescriptions.h"

// ---- Descriptions

// The key is `first`, `separator` and `second`, without their being put together.
static const BuiltinDescription* find(StringView first, char separator, StringView second)
{
    auto compare = [&] (ASCIILiteral key) -> int {
        auto characters = key.span8();
        unsigned length = first.length() + (separator ? 1 : 0) + second.length();
        for (unsigned i = 0; i < length; ++i) {
            if (i >= characters.size())
                return 1;
            char16_t wanted = i < first.length() ? first[i] : i == first.length() && separator ? separator : second[i - first.length() - (separator ? 1 : 0)];
            if (wanted != characters[i])
                return wanted < characters[i] ? -1 : 1;
        }
        return characters.size() > length ? -1 : 0;
    };
    size_t low = 0;
    size_t high = std::size(s_builtinDescriptions);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = compare(s_builtinDescriptions[middle].key);
        if (!order)
            return &s_builtinDescriptions[middle];
        if (order < 0)
            high = middle;
        else
            low = middle + 1;
    }
    return nullptr;
}

const BuiltinTypeLayout* findTypeLayout(StringView type)
{
    for (auto& layout : s_builtinTypeLayouts) {
        if (type == layout.name)
            return &layout;
    }
    return nullptr;
}

const BuiltinDescription* findTypeDescription(StringView type) { return find(type, 0, { }); }
const BuiltinDescription* findAttributeDescription(StringView type, StringView attribute) { return find(type, '.', attribute); }
const BuiltinDescription* findModuleDescription(StringView module) { return find(module, ':', { }); }
const BuiltinDescription* findFunctionDescription(StringView module, StringView function) { return find(module, ':', function); }

String findModuleText(ASCIILiteral key)
{
    for (auto& text : s_builtinTexts) {
        if (text.key == key)
            return String::fromUTF8(text.utf8);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- Signatures

NativeSignature::NativeSignature(ASCIILiteral text)
    : m_text(text)
{
    auto characters = text.span8();
    if (size_t open = StringView(characters).find('('); open) {
        m_functionName = String(characters.first(open));
        characters = characters.subspan(open);
    }
    ASSERT(characters.size() >= 2 && characters.front() == '(' && characters.back() == ')');

    // The parameters are between commas that are not inside anything: a default can be () or ', '. Those that are in square brackets can be left out
    // though they have no default, as in "sub[, start[, end]]".
    struct Parameter {
        StringView text;
        bool isInBrackets;
    };
    Vector<Parameter> parameters;
    unsigned depth = 0;
    unsigned brackets = 0;
    bool isInDefault = false;
    char quote = 0;
    size_t start = 1;
    auto finishParameter = [&] (size_t end) {
        auto parameter = StringView(characters.subspan(start, end - start)).trim(isASCIIWhitespace<char16_t>);
        if (!parameter.isEmpty())
            parameters.append({ parameter, !!brackets });
        start = end + 1;
        isInDefault = false;
    };
    for (size_t i = 1; i < characters.size(); ++i) {
        char c = characters[i];
        if (quote) {
            if (c == '\\')
                ++i;
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (i + 1 == characters.size()) {
            finishParameter(i);
            break;
        }
        if (c == '\'' || c == '"')
            quote = c;
        else if (c == '=')
            isInDefault = true;
        else if (c == '[' && !isInDefault) {
            finishParameter(i);
            ++brackets;
        } else if (c == ']' && !depth && brackets) {
            finishParameter(i);
            --brackets;
        } else if (c == '(' || c == '[' || c == '{' || c == '<')
            ++depth;
        else if (c == ')' || c == ']' || c == '}' || c == '>')
            --depth;
        else if (c == ',' && !depth)
            finishParameter(i);
    }

    bool isKeywordOnly = false;
    bool sawDefault = false;
    for (auto [parameter, isInBrackets] : parameters) {
        if (parameter[0] == '$') {
            m_hasImplicitFirst = true;
            continue;
        }
        if (parameter == "/"_s) {
            m_positionalOnlyCount = m_names.size();
            continue;
        }
        if (parameter == "*"_s) {
            isKeywordOnly = true;
            continue;
        }
        if (parameter.startsWith("**"_s)) {
            m_hasVarKeywords = true;
            continue;
        }
        if (parameter[0] == '*') {
            m_hasVarPositional = true;
            isKeywordOnly = true;
            continue;
        }
        size_t equals = parameter.find('=');
        bool hasDefault = equals != notFound || isInBrackets;
        m_names.append((equals != notFound ? parameter.left(equals) : parameter).toString());
        if (isKeywordOnly) {
            // Those that are required come first, in every signature that there is.
            if (!hasDefault)
                ++m_requiredKeywordOnlyCount;
            continue;
        }
        ++m_positionalCount;
        if (!hasDefault && !sawDefault)
            ++m_requiredPositionalCount;
        sawDefault |= hasDefault;
    }

    bool canBeGivenByName = m_names.size() > m_positionalOnlyCount || m_hasVarKeywords;
    if (m_hasVarPositional && m_hasVarKeywords && m_names.isEmpty())
        m_family = Family::Unchecked;
    else if (canBeGivenByName)
        m_family = Family::Keywords;
    else if (m_hasVarPositional || m_requiredPositionalCount != m_positionalCount || m_positionalCount > 1)
        m_family = Family::Positional;
    else
        m_family = m_positionalCount ? Family::OneArgument : Family::NoArguments;
}

// ---- Checking

// "list.append()", "len()", "math.sqrt()": _PyObject_FunctionStr()
static String functionString(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto* function = uncheckedDowncast<PyNativeFunction>(callFrame->jsCallee());
    String name = function->name(vm);
    JSObject* owner = function->owner();
    if (!owner)
        return concatenate(name, "()"_s);
    // `instance.method` that was got first and called afterwards calls the function from C++, which comes back into the engine to do it.
    auto boundMethodThatCalled = [&] () -> PyBoundMethod* {
        EntryFrame* entryFrame = vm.topEntryFrame;
        CallFrame* caller = callFrame->callerFrame(entryFrame);
        if (!caller || caller->isNativeCalleeFrame() || caller->codeBlock())
            return nullptr;
        auto* method = tryBoundMethod(caller->jsCallee());
        return method && method->function() == JSValue(function) ? method : nullptr;
    };
    if (function->takesArgumentsByParseTuple()) {
        if (!isType(owner) || function->kind() != PyNativeFunction::Kind::Method || boundMethodThatCalled())
            return concatenate(name, "()"_s);
    } else if (function->signature() && !function->signature()->functionName().isNull()) {
        // A method that says what it is called is one that CPython gives its arguments in a tuple. That goes by its name alone if it is the method that is called, and as any other does if it is called where it is got.
        if (!isType(owner) || function->kind() != PyNativeFunction::Kind::Method || boundMethodThatCalled())
            return concatenate(function->signature()->functionName(), "()"_s);
    }
    if (function->takesArgumentsOfTheClass())
        return concatenate(asType(owner)->nameWithoutModule(globalObject), "()"_s);
    if (isType(owner)) {
        // What was called is the function itself, which goes by the class that it is in, unless it is `instance.method` that was got first and called
        // afterwards. That goes by the class that it was got by way of.
        PyType* type = asType(owner);
        if (auto* method = boundMethodThatCalled())
            type = isClass(method->self()) ? asType(method->self()) : typeOf(globalObject, method->self());
        return concatenate(qualifiedNameWithoutModule(globalObject, type), '.', name, "()"_s);
    }
    JSValue moduleName = owner->getDirect(vm, vm.pythonNames().dunder_name);
    if (!moduleName || !moduleName.isString())
        return concatenate(name, "()"_s);
    String module = asString(moduleName)->value(globalObject);
    return module == "builtins"_s ? concatenate(name, "()"_s) : concatenate(module, '.', name, "()"_s);
}

bool checkArgumentsSlow(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* function = uncheckedDowncast<PyNativeFunction>(callFrame->jsCallee());
    const NativeSignature& signature = *function->signature();
    NativeArguments args(callFrame);
    auto fail = [&] (const String& message) {
        raiseTypeError(globalObject, scope, message);
        return false;
    };
    auto plural = [] (unsigned count) { return count == 1 ? ""_s : "s"_s; };

    // The instance or the class that comes first has been seen to, and is not something that whoever called it thinks of having given.
    unsigned implicit = function->hasImplicitFirst();
    unsigned given = args.size() - implicit;
    unsigned keywordCount = args.keywordCount();
    unsigned minimum = signature.requiredPositionalCount();
    unsigned maximum = signature.positionalCount();
    String name = !signature.functionName().isNull() ? signature.functionName() : function->takesArgumentsOfTheClass() ? asType(function->owner())->nameWithoutModule(globalObject) : function->name(vm);
    // set and frozenset give the name of the class that is being made, which can be one derived from them.
    if (PyRealm* realm = globalObject->pyRealm(); function->takesArgumentsOfTheClass() && implicit && (function->owner() == realm->typeSet() || function->owner() == realm->typeFrozenSet()))
        name = (isClass(args[0]) ? asType(args[0]) : typeOf(globalObject, args[0]))->nameWithoutModule(globalObject);

    if (function->kind() == PyNativeFunction::Kind::Wrapper && !function->takesArgumentsOfTheClass()) {
        if (signature.family() == NativeSignature::Family::Unchecked)
            return true;
        if (keywordCount)
            return fail(concatenate("wrapper "_s, name, "() takes no keyword arguments"_s));
        if (given >= minimum && given <= maximum)
            return true;
        // In CPython each kind of slot has a function that takes the arguments of its wrappers apart, and they do not all go about it the same way.
        if (name.endsWith("pow__"_s))
            return fail(concatenate("expected "_s, minimum, " or "_s, maximum, " arguments, got "_s, given));
        bool saysItsName = maximum > 1 || name == "__buffer__"_s || name == "__release_buffer__"_s;
        String prefix = saysItsName ? concatenate(name, ' ') : emptyString();
        if (minimum == maximum)
            return fail(concatenate(prefix, "expected "_s, minimum, " argument"_s, plural(minimum), ", got "_s, given));
        if (given < minimum)
            return fail(concatenate(prefix, "expected at least "_s, minimum, " argument"_s, plural(minimum), ", got "_s, given));
        return fail(concatenate(prefix, "expected at most "_s, maximum, " argument"_s, plural(maximum), ", got "_s, given));
    }

    // (type == base_tp || type->tp_init == base_tp->tp_init) && !_PyArg_NoKeywords(), which is what Argument Clinic writes for a __new__() that takes no keywords, and the same the other way about for an __init__(). Both are
    // given whatever the class was called with. If a class derived from this one has its own of the other, the keywords are taken to be for that.
    auto keywordsAreForADerivedClass = [&] {
        if (!function->takesArgumentsOfTheClass() || !implicit)
            return false;
        bool isNew = function->kind() == PyNativeFunction::Kind::New;
        PyType* owner = asType(function->owner());
        // These are written by hand in CPython, and let none by.
        PyRealm* realm = globalObject->pyRealm();
        if (owner == realm->typeSet() || owner == realm->typeClassMethod() || owner == realm->typeStaticMethod() || owner == realm->typeGenericAlias())
            return false;
        PyType* type = isNew ? asType(args[0]) : typeOf(globalObject, args[0]);
        const Identifier& other = isNew ? vm.pythonNames().dunder_init : vm.pythonNames().dunder_new;
        return type != owner && type->lookup(vm, other) != owner->lookup(vm, other);
    };

    auto family = signature.family();
    // A class that takes one is not called in the way that a method that takes one is.
    if (function->takesArgumentsOfTheClass() && family == NativeSignature::Family::OneArgument)
        family = NativeSignature::Family::Positional;
    if (function->takesDefiningClass()) {
        if (family == NativeSignature::Family::NoArguments)
            return !given && !keywordCount ? true : fail(concatenate(name, "() takes no arguments"_s));
        family = NativeSignature::Family::Keywords;
    }
    // vgetargs1() of CPython's Python/getargs.c
    if (function->takesArgumentsByParseTuple() && family != NativeSignature::Family::Keywords) {
        if (keywordCount)
            return fail(concatenate(functionString(globalObject, callFrame), " takes no keyword arguments"_s));
        // One that takes them apart in more ways than one says for itself how many it takes.
        if (signature.hasVarPositional() || (given >= minimum && given <= maximum))
            return true;
        unsigned bound = given < minimum ? minimum : maximum;
        return fail(concatenate(name, "() takes "_s, minimum == maximum ? "exactly "_s : given < minimum ? "at least "_s : "at most "_s, bound, " argument"_s, plural(bound), " ("_s, given, " given)"_s));
    }
    switch (family) {
    case NativeSignature::Family::Unchecked:
        return true;
    case NativeSignature::Family::NoArguments:
    case NativeSignature::Family::OneArgument:
        if (keywordCount)
            return fail(concatenate(functionString(globalObject, callFrame), " takes no keyword arguments"_s));
        if (given == maximum)
            return true;
        return fail(concatenate(functionString(globalObject, callFrame), maximum ? " takes exactly one argument ("_s : " takes no arguments ("_s, given, " given)"_s));
    case NativeSignature::Family::Positional:
        if (keywordCount && !keywordsAreForADerivedClass())
            return fail(concatenate(functionString(globalObject, callFrame), " takes no keyword arguments"_s));
        if (given >= minimum && (signature.hasVarPositional() || given <= maximum))
            return true;
        if (minimum == maximum && !signature.hasVarPositional())
            return fail(concatenate(name, " expected "_s, minimum, " argument"_s, plural(minimum), ", got "_s, given));
        if (given < minimum)
            return fail(concatenate(name, " expected at least "_s, minimum, " argument"_s, plural(minimum), ", got "_s, given));
        return fail(concatenate(name, " expected at most "_s, maximum, " argument"_s, plural(maximum), ", got "_s, given));
    case NativeSignature::Family::Keywords:
        break;
    }

    // _PyArg_UnpackKeywords() of CPython's Python/getargs.c
    // What does not say what it is called, in what it takes its arguments apart by, is "function", or "this function" where something is said to have been done to it. A signature says so by "?" for the name.
    bool isAnonymous = signature.functionName() == "?"_s;
    String called = isAnonymous ? String("function"_s) : concatenate(name, "()"_s);
    String subject = isAnonymous ? String("this function"_s) : called;
    auto& names = signature.names();
    unsigned positionalOnly = signature.positionalOnlyCount();
    unsigned minimumPositionalOnly = std::min(positionalOnly, minimum);
    unsigned total = names.size();
    unsigned requiredLimit = signature.requiredKeywordOnlyCount() ? maximum + signature.requiredKeywordOnlyCount() : minimum;
    bool hasVarPositional = signature.hasVarPositional();

    // zip_new() and map_new() take their keywords apart on their own, as those of something that is given nothing else.
    if (PyRealm* realm = globalObject->pyRealm(); keywordCount > 1 && function->takesArgumentsOfTheClass() && (function->owner() == realm->typeZip() || function->owner() == realm->typeMap()))
        return fail(concatenate(called, " takes at most 1 keyword argument ("_s, keywordCount, " given)"_s));
    if (!hasVarPositional && !signature.hasVarKeywords() && given + keywordCount > total)
        return fail(concatenate(called, " takes at most "_s, total, given ? " "_s : " keyword "_s, "argument"_s, plural(total), " ("_s, given + keywordCount, " given)"_s));
    if (!hasVarPositional && given > maximum) {
        if (!maximum)
            return fail(concatenate(called, " takes no positional arguments"_s));
        return fail(concatenate(called, " takes "_s, minimum < maximum ? "at most "_s : "exactly "_s, maximum, " positional argument"_s, plural(maximum), " ("_s, given, " given)"_s));
    }
    if (given < minimumPositionalOnly)
        return fail(concatenate(called, " takes "_s, hasVarPositional || minimumPositionalOnly < maximum ? "at least "_s : "exactly "_s, minimumPositionalOnly, " positional argument"_s, plural(minimumPositionalOnly), " ("_s, given, " given)"_s));

    auto isGivenByName = [&] (const String& parameter) {
        for (unsigned k = 0; k < keywordCount; ++k) {
            if (args.keywordName(k)->value(globalObject).data == parameter)
                return true;
        }
        return false;
    };
    unsigned byPosition = std::min(given, maximum);
    unsigned unmatched = keywordCount;
    for (unsigned i = std::max(byPosition, positionalOnly); i < total; ++i) {
        if (isGivenByName(names[i])) {
            --unmatched;
            continue;
        }
        if (i < minimum || (maximum <= i && i < requiredLimit))
            return fail(concatenate(called, " missing required argument '"_s, names[i], "' (pos "_s, i + 1, ')'));
    }
    if (!unmatched || signature.hasVarKeywords())
        return true;

    for (unsigned i = positionalOnly; i < byPosition; ++i) {
        if (isGivenByName(names[i]))
            return fail(concatenate("argument for "_s, called, " given by name ('"_s, names[i], "') and position ("_s, i + 1, ')'));
    }
    Vector<String> candidates;
    for (unsigned i = positionalOnly; i < total; ++i)
        candidates.append(names[i]);
    for (unsigned k = 0; k < keywordCount; ++k) {
        String keyword = args.keywordName(k)->value(globalObject);
        if (candidates.contains(keyword))
            continue;
        String suggestion = calculateSuggestion(candidates, keyword);
        if (!suggestion.isNull())
            return fail(concatenate(subject, " got an unexpected keyword argument '"_s, keyword, "'. Did you mean '"_s, suggestion, "'?"_s));
        return fail(concatenate(subject, " got an unexpected keyword argument '"_s, keyword, '\''));
    }
    return fail(concatenate("invalid keyword argument for "_s, subject));
}

JSValue NativeArguments::givenByName(unsigned index) const
{
    auto* function = dynamicDowncast<PyNativeFunction>(m_callFrame->jsCallee());
    if (!function || !function->signature())
        return { };
    const NativeSignature& signature = *function->signature();
    // After *args, what position something has in the signature is not where it is among the arguments.
    if (signature.hasVarPositional())
        return { };
    unsigned implicit = function->hasImplicitFirst();
    if (index < implicit + signature.positionalOnlyCount() || index - implicit >= signature.names().size())
        return { };
    const String& name = signature.names()[index - implicit];
    for (unsigned k = 0; k < keywordCount(); ++k) {
        // What is between the brackets of a call is never a rope.
        if (WTF::equal(keywordName(k)->tryGetValueImpl(), name.impl()))
            return keywordValue(k);
    }
    return { };
}

// ---- Suggestions: CPython's Python/suggestions.c

static constexpr size_t maximumCandidates = 750;
static constexpr size_t maximumStringSize = 40;
static constexpr size_t moveCost = 2;
static constexpr size_t caseCost = 1;

static size_t substitutionCost(uint8_t a, uint8_t b)
{
    // Neither the same nor the same but for its case.
    if ((a & 31) != (b & 31))
        return moveCost;
    if (a == b)
        return 0;
    return toASCIILower(a) == toASCIILower(b) ? caseCost : moveCost;
}

// How much it takes to make one into the other, or more than `maximumCost` if it is more than that.
static size_t levenshteinDistance(std::span<const uint8_t> a, std::span<const uint8_t> b, size_t maximumCost)
{
    while (!a.empty() && !b.empty() && a.front() == b.front()) {
        a = a.subspan(1);
        b = b.subspan(1);
    }
    while (!a.empty() && !b.empty() && a.back() == b.back()) {
        a = a.first(a.size() - 1);
        b = b.first(b.size() - 1);
    }
    if (a.empty() || b.empty())
        return (a.size() + b.size()) * moveCost;
    if (a.size() > maximumStringSize || b.size() > maximumStringSize)
        return maximumCost + 1;
    if (b.size() < a.size())
        std::swap(a, b);
    if ((b.size() - a.size()) * moveCost > maximumCost)
        return maximumCost + 1;

    // One row of the table at a time, over the one before.
    std::array<size_t, maximumStringSize> row;
    for (size_t i = 0; i < a.size(); ++i)
        row[i] = (i + 1) * moveCost;
    size_t result = 0;
    for (size_t bIndex = 0; bIndex < b.size(); ++bIndex) {
        size_t distance = result = bIndex * moveCost;
        size_t minimum = std::numeric_limits<size_t>::max();
        for (size_t index = 0; index < a.size(); ++index) {
            size_t substitute = distance + substitutionCost(b[bIndex], a[index]);
            distance = row[index];
            result = std::min(std::min(result, distance) + moveCost, substitute);
            row[index] = result;
            minimum = std::min(minimum, result);
        }
        if (minimum > maximumCost)
            return maximumCost + 1;
    }
    return result;
}

String calculateSuggestion(const Vector<String>& candidates, const String& name)
{
    if (candidates.size() >= maximumCandidates)
        return { };
    // A name for whose bytes there is no room is like nothing else.
    auto nameConverted = name.tryGetUTF8();
    if (!nameConverted)
        return { };
    CString nameBytes = WTF::move(*nameConverted);
    size_t best = std::numeric_limits<size_t>::max();
    String suggestion;
    for (auto& candidate : candidates) {
        if (candidate == name)
            continue;
        auto candidateConverted = candidate.tryGetUTF8();
        if (!candidateConverted)
            continue;
        CString candidateBytes = WTF::move(*candidateConverted);
        // No more than a third of the characters are to need changing, and it is to beat what there is.
        size_t maximumDistance = std::min((nameBytes.length() + candidateBytes.length() + 3) * moveCost / 6, best - 1);
        size_t distance = levenshteinDistance(byteCast<uint8_t>(nameBytes.span()), byteCast<uint8_t>(candidateBytes.span()), maximumDistance);
        if (distance > maximumDistance)
            continue;
        suggestion = candidate;
        best = distance;
    }
    return suggestion;
}

// ---- _suggestions: Modules/_suggestions.c. It is how traceback.py, which is written in Python, comes by the above, to say which keyword may have been meant.

PYTHON_NATIVE(suggestionsGenerateSuggestions)
{
    NATIVE_PROLOGUE();
    if (!stringIn(args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("_generate_suggestions() argument 2 must be str, not "_s, typeNameOfArgument(globalObject, args[1]))));
    if (!isExactly(globalObject, args[0], realm->typeList()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "candidates must be a list"_s));
    JSArray* list = asList(args[0]);
    Vector<String> candidates;
    for (unsigned i = 0; i < list->length(); ++i) {
        JSValue element = listGet(globalObject, list, i);
        RETURN_IF_EXCEPTION(scope, { });
        if (!stringIn(element))
            return JSValue::encode(raiseTypeError(globalObject, scope, "all elements in 'candidates' must be strings"_s));
    }
    String name = textForC(globalObject, stringIn(args[1]), EndsAtNull::No);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; i < list->length(); ++i) {
        JSValue element = listGet(globalObject, list, i);
        RETURN_IF_EXCEPTION(scope, { });
        candidates.append(textForC(globalObject, stringIn(element), EndsAtNull::No));
        RETURN_IF_EXCEPTION(scope, { });
    }
    String suggestion = calculateSuggestion(candidates, name);
    if (suggestion.isNull())
        RETURN_NONE();
    // It is the one that is in the list.
    RELEASE_AND_RETURN(scope, JSValue::encode(listGet(globalObject, list, candidates.find(suggestion))));
}

JSObject* createSuggestionsModule(JSGlobalObject* globalObject)
{
    JSObject* module = newBuiltinModule(globalObject, "_suggestions"_s);
    addFunction(globalObject, module, "_generate_suggestions"_s, suggestionsGenerateSuggestions);
    return module;
}

} } // namespace JSC::Python
