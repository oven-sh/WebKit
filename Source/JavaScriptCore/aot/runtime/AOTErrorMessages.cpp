/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTErrorMessages.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCompiledFrame.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTModuleHost.h"
#include "AOTNameClass.h"
#include "AOTRealmStartup.h"
#include "AOTStackTrace.h"
#include "AOTTraceFrame.h"
#include "CodeBlock.h"
#include "StackVisitor.h"
#include "ArgList.h"
#include "ConstructData.h"
#include "DeferTermination.h"
#include "Error.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "TopExceptionScope.h"
#include <wtf/ASCIICType.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT {

JSObject* createUpdateOfNullishError(JSGlobalObject* globalObject)
{
    return createTypeError(globalObject, "Cannot access property of undefined or null"_s);
}

static String describeForErrorMessage(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    String description = errorDescriptionForValue(globalObject, value);
    CLEAR_AND_RETURN_IF_EXCEPTION(scope, String());
    return description;
}

static String findQuotedSource(ModuleID module, LineColumn place)
{
    const ProgramTables& tables = Image::current()->programTables();
    std::tuple wanted { static_cast<uint32_t>(module.index()), static_cast<uint32_t>(place.line), static_cast<uint32_t>(place.column) };
    std::span<const QuotedSourceGroupData> groups = tables.quotedSourceGroups();
    auto next = std::ranges::upper_bound(groups, wanted, { }, [](const QuotedSourceGroupData& group) {
        return std::tuple { group.module, group.line, group.column };
    });
    if (next == groups.begin() || std::prev(next)->module != module.index())
        return { };
    const QuotedSourceGroupData& group = *std::prev(next);
    std::span<const uint8_t> rows = tables.quotedSourceRows();
    const uint8_t* cursor = rows.subspan(group.offsetOfRows).data();
    const uint8_t* end = next == groups.end() ? std::to_address(rows.end()) : rows.subspan(next->offsetOfRows).data();
    uint32_t line = group.line;
    uint32_t column = group.column;
    int64_t start = 0;
    while (cursor < end) {
        QuotedSourceRow row = QuotedSourceRow::read(cursor);
        line += row.lineChange;
        column = row.isOnSameLine ? column + row.columnChange : row.column;
        start += row.startChange;
        if (line == place.line && column == place.column)
            return quotedSourceBytes(static_cast<uint32_t>(start), row.length).transform([](std::span<const Latin1Character> text) { return String(text); }).value_or(String());
    }
    return { };
}

String failedExpressionSource(VM& vm)
{
    String quoted;
    forEachShownFrameOfRunAtTop(vm, HiddenFrameOnTop::IsShown, [&](const TraceFrame& frame) {
        std::optional<SourcePosition> position = Image::current()->positionOf(frame.code.index(), frame.callSite);
        if (position && position->source && FunctionDescription(frame.function).visibility() == TraceVisibility::Hidden)
            quoted = findQuotedSource(position->source, position->lineColumn);
        return IterationStatus::Done;
    });
    if (!quoted.isNull())
        return quoted;
    forEachShownFrameOfRunAtTop(vm, HiddenFrameOnTop::IsLeftOut, [&](const TraceFrame& frame) {
        std::optional<SourcePosition> position = Image::current()->positionOf(frame.code.index(), frame.callSite);
        if (position && position->source) {
            quoted = findQuotedSource(position->source, position->lineColumn);
            if (quoted.isNull() && position->constructionStart.line)
                quoted = findQuotedSource(position->source, position->constructionStart);
        } else if (position && position->lineColumn == superCallOfDefaultConstructor) {
            quoted = "super(...args)"_s;
        }
        return IterationStatus::Done;
    });
    return quoted;
}

String primitiveKindForErrorMessage(JSValue value)
{
    if (value.isObject() || value.isUndefinedOrNull())
        return { };
    const Image* image = Image::current();
    if (!image || !image->programTables().quotedSourceGroups().empty())
        return { };
    if (value.isString())
        return "a string"_s;
    if (value.isSymbol())
        return "a symbol"_s;
    if (value.isBigInt())
        return "a BigInt"_s;
    return value.isBoolean() ? "a boolean"_s : "a number"_s;
}

static JSObject* createTypeErrorQuoting(JSGlobalObject* globalObject, JSValue value, const String& description, ASCIILiteral complaint, ErrorInstance::SourceAppender appender, const String& spelled)
{
    String expression = failedExpressionSource(globalObject->vm());
    if (expression.isNull())
        expression = spelled;
    String message = tryMakeString(description, ' ', complaint);
    if (description.isNull() || message.isNull()) [[unlikely]]
        return createOutOfMemoryError(globalObject);
    if (expression.isNull())
        return createTypeError(globalObject, message);
    return createTypeError(globalObject, appender(message, expression, runtimeTypeForValue(value), ErrorInstance::SourceTextWhereErrorOccurred::FoundExactSource));
}

static JSObject* createNotAnObjectErrorQuoting(JSGlobalObject* globalObject, JSValue base, const String& description, const String& spelled)
{
    if (!globalObject->stackTraceLimit().value_or(0))
        return JSC::createNotAnObjectError(globalObject, base);
    return createTypeErrorQuoting(globalObject, base, description, "is not an object"_s, defaultSourceAppender, spelled);
}

static bool canFollowDotInErrorMessage(StringView member)
{
    if (member.isEmpty() || isASCIIDigit(member[0]))
        return false;
    for (unsigned index = 0; index < member.length(); ++index) {
        char16_t character = member[index];
        if (isASCII(character) && !isASCIIAlphanumeric(character) && character != '_' && character != '$' && (character != '#' || index))
            return false;
    }
    return true;
}

static String spellMemberInErrorMessage(StringView object, StringView member)
{
    return canFollowDotInErrorMessage(member) ? tryMakeString(object, '.', member) : tryMakeString(object, "[\""_s, member, "\"]"_s);
}

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, StringView member)
{
    if (member.isNull())
        return JSC::createNotAnObjectError(globalObject, base);
    String description = describeForErrorMessage(globalObject, base);
    return createNotAnObjectErrorQuoting(globalObject, base, description, spellMemberInErrorMessage(description, member));
}

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, const Identifier& member)
{
    if (member.isPrivateName())
        return JSC::createNotAnObjectError(globalObject, base);
    if (!member.isSymbol())
        return createNotAnObjectError(globalObject, base, StringView(member.impl()));
    String description = describeForErrorMessage(globalObject, base);
    return createNotAnObjectErrorQuoting(globalObject, base, description, tryMakeString(description, "[Symbol("_s, StringView(member.impl()), ")]"_s));
}

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, JSValue)
{
    return JSC::createNotAnObjectError(globalObject, base);
}

JSObject* createKeyTestOfNonObjectError(JSGlobalObject* globalObject, JSValue base)
{
    return createInvalidInParameterError(globalObject, base);
}

JSObject* createDeletionFromNullishError(JSGlobalObject* globalObject, JSValue base)
{
    return JSC::createNotAnObjectError(globalObject, base);
}

JSObject* createIterationOfNullishError(JSGlobalObject* globalObject, JSValue iterated)
{
    return JSC::createNotAnObjectError(globalObject, iterated);
}

JSObject* createPrivateNameOfNullishError(JSGlobalObject* globalObject, JSValue base)
{
    return JSC::createNotAnObjectError(globalObject, base);
}

JSObject* createAbsentPrivateNameError(JSGlobalObject* globalObject, JSObject* (*create)(JSGlobalObject*))
{
    return create(globalObject);
}

JSObject* createSuperBaseNotAnObjectError(JSGlobalObject* globalObject, JSValue base, const Identifier* member)
{
    if (!member || member->isNull() || member->isSymbol())
        return JSC::createNotAnObjectError(globalObject, base);
    return createNotAnObjectErrorQuoting(globalObject, base, describeForErrorMessage(globalObject, base), spellMemberInErrorMessage("super"_s, StringView(member->impl())));
}

JSObject* createNotAFunctionError(JSGlobalObject* globalObject, JSValue callee, StringView name)
{
    return createTypeErrorQuoting(globalObject, callee, describeForErrorMessage(globalObject, callee), "is not a function"_s, notAFunctionSourceAppender, name.isEmpty() ? String() : tryMakeString(name, "()"_s));
}

JSObject* createNotAFunctionError(JSGlobalObject* globalObject, JSValue callee, const Identifier& name)
{
    return createNotAFunctionError(globalObject, callee, name.isSymbol() ? StringView() : StringView(name.impl()));
}

JSObject* createParentNotAConstructorError(JSGlobalObject* globalObject, JSValue parent)
{
    return createTypeErrorQuoting(globalObject, parent, describeForErrorMessage(globalObject, parent), "is not a constructor"_s, defaultSourceAppender, String());
}

JSObject* createNotAConstructorError(JSGlobalObject* globalObject, JSValue callee, StringView name)
{
    return createTypeErrorQuoting(globalObject, callee, describeForErrorMessage(globalObject, callee), "is not a constructor"_s, defaultSourceAppender, name.isEmpty() ? String() : tryMakeString("new "_s, name));
}

void throwNotAConstructorErrorInFrameOf(JSGlobalObject* globalObject, JSFunction* function, StringView, bool)
{
    CallData constructData;
    constructData.type = CallData::Type::Native;
    constructData.native.function = function->nativeConstructor();
    constructData.native.isBoundFunction = false;
    constructData.native.isWasm = false;
    construct(globalObject, function, constructData, ArgList());
}

JSObject* createBuiltinModulesUnavailableError(JSGlobalObject* globalObject)
{
    return createTypeError(globalObject, withAOTErrorNote("Built-in modules aren't available in this realm"_s));
}

JSObject* createNativeBindingsUnavailableError(JSGlobalObject* globalObject)
{
    return createTypeError(globalObject, withAOTErrorNote("Native bindings aren't available in this realm"_s));
}

JSObject* createBuiltinWithoutPathError(JSGlobalObject* globalObject)
{
    return createTypeError(globalObject, "Built-in has no path from the global object"_s);
}

JSObject* createBuiltinNotCompiledError(JSGlobalObject* globalObject, const String& builtin)
{
    return createError(globalObject, withAOTErrorNote(makeString(builtin, " was not compiled into this executable because your code never uses it. To keep it, use it somewhere in your code."_s)));
}

JSObject* createBuiltinBehindTargetOfEventNotCompiledError(JSGlobalObject* globalObject, const String& builtin)
{
    return createError(globalObject, withAOTErrorNote(makeString(builtin, " was not compiled into this executable because the compiler saw no use of it. Your code reached it through the `target` of an event on the global object. To keep it, write `void dispatchEvent` anywhere in your code."_s)));
}

JSObject* createBuiltinFunctionNotCompiledError(JSGlobalObject* globalObject, const String& functionName)
{
    return createBuiltinNotCompiledError(globalObject, functionName.isEmpty() ? "A built-in function"_str : makeString("The built-in function '"_s, functionName, '\''));
}

JSObject* createBuiltinsNotCompiledError(JSGlobalObject* globalObject, std::span<const String> builtins)
{
    StringBuilder alternatives;
    for (size_t index = 0; index < builtins.size(); ++index) {
        if (index)
            alternatives.append(index + 1 == builtins.size() ? " or "_s : ", "_s);
        alternatives.append(builtins[index]);
    }
    return createError(globalObject, withAOTErrorNote(makeString("None of "_s, alternatives.toString(), " was compiled into this executable because your code never uses them. To keep them, use one of them somewhere in your code."_s)));
}

JSObject* createExternalModuleNotLoadedError(JSGlobalObject* globalObject, const String& path)
{
    return createTypeError(globalObject, withAOTErrorNote(makeString("The module \""_s, path, "\" is not loaded"_s)));
}

static JSObject* withCode(JSGlobalObject* globalObject, JSObject* error, ASCIILiteral code)
{
    VM& vm = globalObject->vm();
    error->putDirect(vm, Identifier::fromString(vm, "code"_s), jsNontrivialString(vm, String(code)));
    return error;
}

static JSObject* createErrorWithCode(JSGlobalObject* globalObject, const String& message, ASCIILiteral code)
{
    return withCode(globalObject, createError(globalObject, message), code);
}

JSObject* createLoadByNameRefusedError(JSGlobalObject* globalObject, StringView name, ASCIILiteral code)
{
    bool isModuleOfBun = name.startsWith("node:"_s) || name.startsWith("bun:"_s);
    const Image* image = Image::current();
    if (!isModuleOfBun && image && hasFlag(image->header().options.admitted(), AdmittedCode::ReplacedBuiltins))
        return createErrorWithCode(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, name, "': no import() or require() that the build saw can reach it by a dynamic path. Import the file by a constant path, or add --aot-allow-code-from-disk."_s)), code);
    if (!isModuleOfBun && image && image->loadsCodeOutsideImage())
        return createErrorWithCode(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, name, "': it was compiled into this executable, and a dynamic path finds a compiled file only if the path is a string somewhere in your code. Pass the file to bun compile as another entry point."_s)), code);
    ASCIILiteral advice = isModuleOfBun ? "The compiler couldn't tell at build time that this call reaches a module loader. Call require(), import() or process.getBuiltinModule() directly."_s : "Pass the file to bun compile as another entry point, or build with --aot-allow-code-from-disk."_s;
    return createErrorWithCode(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, name, "': this executable can't load a module from a dynamic path. "_s, advice)), code);
}

JSObject* createCompiledModuleAccessError(JSGlobalObject* globalObject, StringView path, ASCIILiteral code)
{
    return createErrorWithCode(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, path, "': code that is loaded from disk can't import files that were compiled into this executable. Pass it what it needs as arguments or globals."_s)), code);
}

JSObject* createFormNotHeldError(JSGlobalObject* globalObject, StringView path, ASCIILiteral type)
{
    return createErrorWithCode(globalObject, withAOTErrorNote(makeString("Cannot import '"_s, path, "' as "_s, type, ": it was compiled, and its source isn't in this executable. Write the type out in the call: import(path, { with: { type: \""_s, type, "\" } })."_s)), "ERR_MODULE_NOT_FOUND"_s);
}

JSObject* createExportNotHandedOutError(JSGlobalObject* globalObject, StringView path, StringView name)
{
    return withCode(globalObject, createReferenceError(globalObject, withAOTErrorNote(makeString("Cannot read '"_s, name, "' from the namespace object of '"_s, path, "': the build kept only the exports that it saw your code read. Import '"_s, name, "' by name."_s))), "ERR_MODULE_NOT_FOUND"_s);
}

JSObject* createModuleInOtherRealmError(JSGlobalObject* globalObject, StringView path)
{
    return createSyntaxError(globalObject, withAOTErrorNote(makeString("The module "_s, path, " was compiled ahead of time and can't be imported in another realm"_s)));
}

JSObject* createLoaderChangeError(JSGlobalObject* globalObject, LoaderChange change, StringView changed)
{
    if (change == LoaderChange::TakeOutOfRequireCache)
        return createError(globalObject, makeString("Cannot delete '"_s, changed, "' from require.cache: the build saw no code that deletes it, so the module was compiled to load once. Write the path out where you delete it, in a file that the tests import: delete require.cache[require.resolve(\"./module\")]"_s));
    if (change == LoaderChange::StoreUnderNameOfModuleInRequireCache)
        return createError(globalObject, withAOTErrorNote(makeString("Cannot store an entry under '"_s, changed, "' in require.cache: the build saw no store under that key, so require(\""_s, changed, "\") always returns the compiled module. Write the key out where you store it, as in require.cache[\""_s, changed, "\"] = entry, or use mock.module() in a test."_s)));
    if (change == LoaderChange::EvaluateModuleAgain)
        return createError(globalObject, withAOTErrorNote(makeString("Cannot run '"_s, changed, "' a second time: a loader in Module._extensions evaluates the source of a file, and this executable has only the compiled file. Use require() to get its exports."_s)));
    if (change == LoaderChange::PassSourceToCompile)
        return createError(globalObject, withAOTErrorNote(makeString("Cannot pass the source of '"_s, changed, "' to module._compile: the file was compiled, and its source isn't in this executable. If your program depends on a loader that replaces module._compile, build it with --no-aot."_s)));
    bool isInRequireCache = change == LoaderChange::ReplaceInRequireCache;
    return createError(globalObject, withAOTErrorNote(makeString("Cannot replace "_s, isInRequireCache ? "'"_s : ""_s, changed, isInRequireCache ? "' in require.cache"_s : ""_s, ": imports in this executable were resolved at build time, so the module loader can't be patched."_s)));
}

JSObject* createEntryOfRequireCacheRefusedError(JSGlobalObject* globalObject, StringView path)
{
    return createError(globalObject, makeString("Cannot read the entry of '"_s, path, "' in require.cache: the build saw no code that reads the cache, so the module has no entry. Write the read out in a file that the tests import: require.cache[require.resolve(\"./module\")]"_s));
}

String moduleObjectRefusedMessage(StringView path)
{
    return withAOTErrorNote(makeString("Cannot access the module object of '"_s, path, "': the build saw no code that uses it, so the file has none. To keep it, write `void module.children;` in one of your files."_s));
}

JSObject* createComparisonOfPrivateFieldRefusedError(JSGlobalObject* globalObject, const String& field)
{
    return createError(globalObject, withAOTErrorNote(makeString("Cannot compare the private field "_s, field, ": the build saw no object with this field reach Bun.deepMatch(), toMatchObject() or expect.objectContaining(). Call the comparison directly, and create one of the two objects where you call it."_s)));
}

JSObject* createModuleMissingAtBuildError(JSGlobalObject* globalObject, const String& specifier, const String& importer, ASCIILiteral code)
{
    return createErrorWithCode(globalObject, makeString("Cannot find module '"_s, specifier, "' from '"_s, importer, "'"_s), code);
}

JSObject* createExportStatementFindsNothingError(JSGlobalObject* globalObject, const String& exportName, const String& specifier)
{
    return createSyntaxError(globalObject, makeString("export '"_s, exportName, "' not found in '"_s, specifier, "'"_s));
}

JSObject* createExportStatementFindsSeveralError(JSGlobalObject* globalObject, const String& exportName, const String& specifier)
{
    return createSyntaxError(globalObject, makeString("Cannot export '"_s, exportName, "' multiple times in '"_s, specifier, "'"_s));
}

JSObject* createSuperCallInDirectEvalError(JSGlobalObject* globalObject)
{
    return createTypeError(globalObject, withAOTErrorNote("super() cannot be called from direct eval in a compiled executable"_s));
}

static String messageOfFunctionNotCompiled(const String& description, const String& advice = "call it directly somewhere in your code"_s)
{
    return withAOTErrorNote(makeString(description, " was not compiled into this executable because nothing that the compiler can follow calls it. To keep it, "_s, advice, '.'));
}

String functionNotCompiledMessage(const String& functionName)
{
    return messageOfFunctionNotCompiled(makeString('\'', functionName, '\''));
}

String exportNotCompiledMessage(const String& functionName, const String& module)
{
    return messageOfFunctionNotCompiled(makeString('\'', functionName, '\''), makeString("import it by name: import { "_s, functionName, " } from \""_s, module, '"'));
}

String functionNotCompiledMessage(const String& path, unsigned line)
{
    return messageOfFunctionNotCompiled(makeString("The function at "_s, path, ':', line));
}

String functionNotCompiledMessage()
{
    return messageOfFunctionNotCompiled("This function"_s);
}

ASCIILiteral rejectedEvaluationMessage()
{
    return "Code generation from strings disallowed for this context: the compiler couldn't tell at build time that this call reaches eval() or the Function constructor. Call eval() or new Function() directly."_s;
}

static bool isWrittenBehindDot(StringView name)
{
    if (name.isEmpty() || isASCIIDigit(name[0]))
        return false;
    for (unsigned index = 0; index < name.length(); ++index) {
        char16_t unit = name[index];
        if (!isASCIIAlphanumeric(unit) && unit != '_' && unit != '$')
            return false;
    }
    return true;
}

static String spelledInBrackets(const String& object, StringView name)
{
    StringBuilder spelled;
    spelled.append(object, '[');
    spelled.appendQuotedJSONString(name.toString());
    spelled.append(']');
    return spelled.toString();
}

static bool mayBeKeyOfElement(const UniquedStringImpl& key)
{
    return !key.isSymbol() && key.is8Bit() && Typed::mayBeElementKey(Typed::classifyName(byteCast<uint8_t>(key.span8())));
}

String memberOfLockedBuiltin(BuiltinID holder, UniquedStringImpl* key)
{
    String text = BuiltinTable::singleton().textOf(holder);
    if (key->isSymbol())
        return makeString(text, "[Symbol("_s, StringView(key), ")]"_s);
    if (parseIndex(*key))
        return makeString(text, '[', StringView(key), ']');
    if (!isWrittenBehindDot(StringView(key)))
        return spelledInBrackets(text, StringView(key));
    return holder == Builtins::GlobalThis ? String(key) : makeString(text, '.', StringView(key));
}

String deletionFromLockedBuiltinMessage(const String& property)
{
    return withAOTErrorNote(makeString("Cannot delete "_s, property, " because built-in objects are read-only in a compiled executable"_s));
}

String assignmentToLockedBuiltinMessage(const String& property)
{
    return withAOTErrorNote(makeString("Cannot assign to "_s, property, " because built-in objects are read-only in a compiled executable"_s));
}

String changeThatCompiledCodeRulesOutMessage(ASCIILiteral change)
{
    return withAOTErrorNote(makeString("Cannot "_s, change, " because no code in the executable does, and compiled code relies on that"_s));
}

String changeThatCompiledCodeRulesOutMessage(ObjectMutation mutation)
{
    auto change = [&]() -> ASCIILiteral {
        switch (mutation) {
        case ObjectMutation::DeleteField:
            return "delete this property"_s;
        case ObjectMutation::RedefineProperty:
            return "redefine this property"_s;
        case ObjectMutation::AddProperty:
            return "add a property to this object"_s;
        case ObjectMutation::ChangeIntegrity:
            return "freeze, seal or prevent extensions of this object"_s;
        case ObjectMutation::ChangePrototype:
            return "change the prototype of this object"_s;
        case ObjectMutation::BecomePrototype:
            return "make this object a prototype"_s;
        case ObjectMutation::StoreOfOtherType:
            return "store a value of this type in this property"_s;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    return changeThatCompiledCodeRulesOutMessage(change());
}

String callOfBuiltinChangedBeforeFirstUseMessage(const String& property)
{
    return withAOTErrorNote(makeString("Cannot call "_s, property, " because it was replaced, redefined or deleted before its first use, and compiled code calls the built-in function without looking it up"_s));
}

String assignmentThatBuildDidNotSeeMessage(const String& property)
{
    if (const Image* image = Image::current(); image && image->loadsCodeOutsideImage())
        return withAOTErrorNote(makeString("Cannot assign to "_s, property, " because built-in objects are read-only in a compiled executable, and the build saw no assignment to this one. Code that is loaded from disk can replace it only where the executable's own code assigns to it too, spelled out as in "_s, property, " = value"_s));
    return withAOTErrorNote(makeString("Cannot assign to "_s, property, " because built-in objects are read-only in a compiled executable, and the build didn't see your code assign to this one. Spell out the object and the name where it does, as in "_s, property, " = value"_s));
}

static constexpr ASCIILiteral pageOnCompiledTests = " (https://bun.com/docs/compiler/debugging#run-your-tests-compiled)"_s;

static String refusalThatNoTestRunLifts(ASCIILiteral change, const String& member, ASCIILiteral does)
{
    return makeString(change, member, " because built-in objects are read-only in compiled code, and a test can't "_s, does, " this one: it is read without being looked up. Replace a function of your own that uses it"_s, pageOnCompiledTests);
}

static String refusalInTestRun(VM& vm, ASCIILiteral change, BuiltinID holder, UniquedStringImpl* key)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    String member = memberOfLockedBuiltin(holder, key);
    BuiltinName name = table.nameOf(vm, key);
    BuiltinID found = name ? table.findOwnMember(holder, name) : BuiltinID();
    bool isText = !key->isSymbol() && !parseIndex(*key);
    bool canBeOpened = found ? table.canBeReplacedInTestRuns(found) || table.at(found).mutability == BuiltinMutability::ConstantUnlessWritten : isText && !mayBeKeyOfElement(*key) && table.canTakeNameInTestRuns(holder, name);
    ASCIILiteral does = found ? "replace"_s : "add"_s;
    if (!canBeOpened)
        return refusalThatNoTestRunLifts(change, member, does);
    String object = table.textOf(holder);
    String spelled = !isText ? makeString(object, '[', StringView(key), ']') : isWrittenBehindDot(StringView(key)) ? makeString(object, '.', StringView(key)) : spelledInBrackets(object, StringView(key));
    String example = found
        ? makeString(spelled, " = f or spyOn("_s, object, ", "_s, isText ? "\""_s : ""_s, StringView(key), isText ? "\""_s : ""_s, ')')
        : makeString(spelled, " = value"_s);
    return makeString(change, member, " because built-in objects are read-only in compiled code, and the build didn't see a test "_s, does, " this one. Spell out the object and the name where the test does it, as in "_s, example, pageOnCompiledTests);
}

bool isWrittenByFileOfProduct(VM& vm)
{
    if (!vm.hasCompiledCode() || !vm.topCallFrame || Image::current()->programTables().modulesWithReadOnlyBuiltins().empty())
        return false;
    std::optional<bool> answer;
    StackVisitor::visitWithCompiledFrames(vm.topCallFrame, vm, [&](StackVisitor& visitor) {
        if (visitor->isCompiledFrame()) {
            for (CompiledFrameIterator& frames = visitor.compiledFrames(); !answer && !frames.isAtEnd(); frames.advance()) {
                std::optional<SourcePosition> position = Image::current()->positionOf(frames.frame().code.index(), frames.frame().callSite);
                if (position && position->source)
                    answer = hasReadOnlyBuiltins(position->source);
            }
            return answer ? IterationStatus::Done : IterationStatus::Continue;
        }
        CodeBlock* codeBlock = visitor->codeBlock();
        if (!codeBlock)
            return IterationStatus::Continue;
        const SourceOrigin& origin = codeBlock->ownerExecutable()->sourceOrigin();
        if (ScriptFetcher* fetcher = origin.fetcher(); fetcher && fetcher->fetcherType() == ScriptFetcher::Type::NodeVM) {
            answer = static_cast<FetcherOfTextHandedIn*>(fetcher)->isMadeByFileOfProduct();
            return IterationStatus::Done;
        }
        if (!origin.url().protocolIsFile())
            return IterationStatus::Continue;
        if (std::optional<ModuleID> file = findModule(origin.url().fileSystemPath()))
            answer = hasReadOnlyBuiltins(*file);
        return IterationStatus::Done;
    }, false, StackVisitor::CompiledFrameVisit::EachRunOnce);
    return answer.value_or(false);
}

FetcherOfTextHandedIn::FetcherOfTextHandedIn(VM& vm)
    : m_isMadeByFileOfProduct(isWrittenByFileOfProduct(vm))
{
}

String assignmentToLockedBuiltinMessage(VM& vm, BuiltinID holder, UniquedStringImpl* key, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return refusalInTestRun(vm, "Cannot assign to "_s, holder, key);
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinName name = table.nameOf(vm, key);
    BuiltinID found = name ? table.findOwnMember(holder, name) : BuiltinID();
    bool isNewName = !key->isSymbol() && !mayBeKeyOfElement(*key) && !found && (!name || table.findMember(holder, name).outcome != BuiltinMemberLookup::Outcome::Found);
    bool isOpenWhereBuildSeesWrite = found ? table.at(found).mutability == BuiltinMutability::ConstantUnlessWritten : isNewName && table.canonicalID(holder) != Builtins::ObjectPrototype && table.canTakeNameInTestRuns(holder, name);
    if (isOpenWhereBuildSeesWrite)
        return assignmentThatBuildDidNotSeeMessage(memberOfLockedBuiltin(holder, key));
    return assignmentToLockedBuiltinMessage(memberOfLockedBuiltin(holder, key));
}

String deletionFromLockedBuiltinMessage(VM& vm, BuiltinID holder, UniquedStringImpl* key, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return refusalInTestRun(vm, "Cannot delete "_s, holder, key);
    return deletionFromLockedBuiltinMessage(memberOfLockedBuiltin(holder, key));
}

String assignmentToFoldedMemberMessage(const String& property, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return refusalThatNoTestRunLifts("Cannot assign to "_s, property, "replace"_s);
    return assignmentToLockedBuiltinMessage(property);
}

String deletionOfFoldedMemberMessage(const String& property, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return refusalThatNoTestRunLifts("Cannot delete "_s, property, "replace"_s);
    return deletionFromLockedBuiltinMessage(property);
}

String assignmentToNameThatBuiltinFunctionInheritsMessage(UniquedStringImpl* uid)
{
    return assignmentToLockedBuiltinMessage(makeString(uid->isSymbol() ? '[' : '\'', StringView(uid), uid->isSymbol() ? ']' : '\'', " of a built-in function"_s));
}

String prototypeChangeOfLockedBuiltinMessage(const String& holder, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return makeString("Cannot change the prototype of "_s, holder, " because built-in objects are read-only in compiled code, and a test can't change this"_s, pageOnCompiledTests);
    return withAOTErrorNote(makeString("Cannot change the prototype of "_s, holder, " because built-in objects are read-only in a compiled executable"_s));
}

static String makeUncompiledCallerReason(const String& file, ASCIILiteral made)
{
    return makeString(" because \""_s, file, "\", which makes the "_s, made, ", wasn't compiled with your tests: the build couldn't follow the path that loads it. Import that file by a constant path, or put its path together from import.meta.dir: import(import.meta.dir + \"/folder/\" + name)"_s, pageOnCompiledTests);
}

String makeSpyOnExportErrorMessage(UniquedStringImpl* name, const String& path, LockedIn lockedIn, const String& uncompiledCaller)
{
    if (lockedIn == LockedIn::TestRun && !uncompiledCaller.isNull())
        return makeString("Cannot spy on '"_s, StringView(name), "' of \""_s, path, '"', makeUncompiledCallerReason(uncompiledCaller, "spy"_s));
    if (lockedIn == LockedIn::TestRun)
        return makeString("Cannot spy on '"_s, StringView(name), "' of \""_s, path, "\" because the build didn't see a test spy on it. Spell out the module and the name where the spy is made: import * as module from \"<path>\"; spyOn(module, \""_s, StringView(name), "\")"_s, pageOnCompiledTests);
    return makeString("Cannot spy on '"_s, StringView(name), "' of \""_s, path, "\" because module exports can't be replaced in a compiled executable. Spy on it in a test, under bun test"_s);
}

String makeMockModuleErrorMessage(const String& path, LockedIn lockedIn, const String& uncompiledCaller)
{
    if (lockedIn == LockedIn::TestRun && !uncompiledCaller.isNull())
        return makeString("mock(module, fn) cannot replace \""_s, path, '"', makeUncompiledCallerReason(uncompiledCaller, "mock"_s));
    if (lockedIn == LockedIn::TestRun)
        return makeString("mock(module, fn) cannot replace \""_s, path, "\" because the build didn't see a test mock it. Spell out the module's path where the mock is made: mock.module(\"<path>\", factory)"_s, pageOnCompiledTests);
    return makeString("mock(module, fn) cannot replace \""_s, path, "\" because it was compiled ahead of time. Mock it in a test, under bun test"_s);
}

} } // namespace JSC::AOT

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
