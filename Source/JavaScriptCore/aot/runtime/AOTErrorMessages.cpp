/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTErrorMessages.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTImage.h"
#include "AOTModuleHost.h"
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

static String findQuotedText(ModuleID module, LineColumn place)
{
    const ProgramTables& tables = Image::current()->programTables();
    std::tuple wanted { static_cast<uint32_t>(module.index()), static_cast<uint32_t>(place.line), static_cast<uint32_t>(place.column) };
    std::span<const QuotedTextGroupData> groups = tables.quotedTextGroups();
    auto next = std::ranges::upper_bound(groups, wanted, { }, [](const QuotedTextGroupData& group) {
        return std::tuple { group.module, group.line, group.column };
    });
    if (next == groups.begin() || std::prev(next)->module != module.index())
        return { };
    const QuotedTextGroupData& group = *std::prev(next);
    std::span<const uint8_t> rows = tables.quotedTextRows();
    const uint8_t* cursor = rows.subspan(group.offsetOfRows).data();
    const uint8_t* end = next == groups.end() ? std::to_address(rows.end()) : rows.subspan(next->offsetOfRows).data();
    uint32_t line = group.line;
    uint32_t column = group.column;
    int64_t start = 0;
    while (cursor < end) {
        QuotedTextRow row = QuotedTextRow::read(cursor);
        line += row.lineChange;
        column = row.isOnSameLine ? column + row.columnChange : row.column;
        start += row.startChange;
        if (line == place.line && column == place.column)
            return String(byteCast<Latin1Character>(tables.textOfQuotes().subspan(static_cast<size_t>(start), row.length)));
    }
    return { };
}

String quotedTextOfFailedExpression(VM& vm)
{
    String quoted;
    forEachShownFrameOfRunAtTop(vm, [&](const TraceFrame& frame) {
        std::optional<SourcePosition> position = Image::current()->positionOf(frame.code.index(), frame.callSite);
        if (position && position->source) {
            quoted = findQuotedText(position->source, position->lineColumn);
            if (quoted.isNull() && position->constructionStart.line)
                quoted = findQuotedText(position->source, position->constructionStart);
        }
        return IterationStatus::Done;
    });
    return quoted;
}

// `spelled` stands where the engine quotes the source text of the expression that failed, unless the image holds that text. Null: nothing is spelled.
static JSObject* createTypeErrorQuoting(JSGlobalObject* globalObject, JSValue value, const String& description, ASCIILiteral complaint, ErrorInstance::SourceAppender appender, const String& spelled)
{
    String expression = quotedTextOfFailedExpression(globalObject->vm());
    if (expression.isNull())
        expression = spelled;
    String message = tryMakeString(description, ' ', complaint);
    if (description.isNull() || message.isNull()) [[unlikely]]
        return createOutOfMemoryError(globalObject);
    if (expression.isNull())
        return createTypeError(globalObject, message);
    return createTypeError(globalObject, appender(message, expression, runtimeTypeForValue(value), ErrorInstance::SourceTextWhereErrorOccurred::FoundExactSource));
}

// The engine quotes a call that failed whatever Error.stackTraceLimit is, and anything else only in an error whose trace has a frame: ErrorInstance::finishCreation().
static JSObject* createNotAnObjectErrorQuoting(JSGlobalObject* globalObject, JSValue base, const String& description, const String& spelled)
{
    if (!globalObject->stackTraceLimit().value_or(0))
        return JSC::createNotAnObjectError(globalObject, base);
    return createTypeErrorQuoting(globalObject, base, description, "is not an object"_s, defaultSourceAppender, spelled);
}

// `o.name`, `o.#name`. Any other key goes in brackets: `o["two words"]`.
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

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, JSValue key)
{
    // An object is described by its class, which is not the key that it would become.
    if (!key || key.isObject())
        return JSC::createNotAnObjectError(globalObject, base);
    String description = describeForErrorMessage(globalObject, base);
    String descriptionOfKey = describeForErrorMessage(globalObject, key);
    if (descriptionOfKey.isNull())
        return JSC::createNotAnObjectError(globalObject, base);
    return createNotAnObjectErrorQuoting(globalObject, base, description, tryMakeString(description, '[', descriptionOfKey, ']'));
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
    // So reads the constructor that the engine gives a derived class without one. The image has no text for it.
    return createTypeErrorQuoting(globalObject, parent, describeForErrorMessage(globalObject, parent), "is not a constructor"_s, defaultSourceAppender, "super(...args)"_str);
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

static JSObject* createErrorWithCode(JSGlobalObject* globalObject, const String& message, ASCIILiteral code)
{
    VM& vm = globalObject->vm();
    JSObject* error = createError(globalObject, message);
    error->putDirect(vm, Identifier::fromString(vm, "code"_s), jsNontrivialString(vm, String(code)));
    return error;
}

JSObject* createLoadByNameRefusedError(JSGlobalObject* globalObject, StringView name, ASCIILiteral code)
{
    // A module of Bun's is no file that could be another entry point. The call went through a value that the compiler did not follow to the loader.
    bool isModuleOfBun = name.startsWith("node:"_s) || name.startsWith("bun:"_s);
    const Image* image = Image::current();
    if (!isModuleOfBun && image && hasFlag(image->header().options.admitted(), AdmittedCode::ReplacedBuiltins))
        return createErrorWithCode(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, name, "': the build saw no import() or require() of a computed path, so compiled files answer only to the paths it saw. It can't see code from a string, as in eval() or new Function(). An executable refuses this too. Import the file by a constant path."_s)), code);
    if (!isModuleOfBun && image && image->loadsCodeOutsideImage())
        return createErrorWithCode(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, name, "': it was compiled into this executable, and a dynamic path finds a compiled file only if the path is a string somewhere in your code. Pass the file to bun compile as another entry point."_s)), code);
    ASCIILiteral advice = isModuleOfBun ? "The compiler couldn't tell at build time that this call reaches a module loader. Call require(), import() or process.getBuiltinModule() directly."_s : "Pass the file to bun compile as another entry point, or build with --aot-allow-code-from-disk."_s;
    return createErrorWithCode(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, name, "': this executable can't load a module from a dynamic path. "_s, advice)), code);
}

JSObject* createFileOfImageRefusedError(JSGlobalObject* globalObject, StringView path, ASCIILiteral code)
{
    return createErrorWithCode(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, path, "': code that this executable loads from disk can't load the files that were compiled into it. It can use globals, built-in modules, files and packages on disk, and what the program hands it."_s)), code);
}

JSObject* createModuleInOtherRealmError(JSGlobalObject* globalObject, StringView path)
{
    return createSyntaxError(globalObject, withAOTErrorNote(makeString("The module "_s, path, " was compiled ahead of time and can't be imported in another realm"_s)));
}

JSObject* createLoaderChangeRefusedError(JSGlobalObject* globalObject, LoaderChange change, StringView changed)
{
    // Only a test run takes files out, and only those that its build saw taken out.
    if (change == LoaderChange::TakeOutOfRequireCache)
        return createError(globalObject, makeString("Cannot take '"_s, changed, "' out of require.cache: the build found nothing that does, so the code that imports it was compiled for a module that loads once. Spell the path where it is deleted, in a file that the tests import: delete require.cache[require.resolve(\"./module\")]"_s));
    if (change == LoaderChange::StoreUnderNameOfModuleInRequireCache)
        return createError(globalObject, withAOTErrorNote(makeString("Cannot store an entry under '"_s, changed, "' in require.cache: the build read no store under that key, so require(\""_s, changed, "\") was compiled to answer with the module. Spell the key where it is stored: require.cache[\""_s, changed, "\"] = entry. In a test, mock.module(\""_s, changed, "\", () => ({ .. })) replaces the module too."_s)));
    bool isInRequireCache = change == LoaderChange::ReplaceInRequireCache;
    return createError(globalObject, withAOTErrorNote(makeString("Cannot replace "_s, isInRequireCache ? "'"_s : ""_s, changed, isInRequireCache ? "' in require.cache"_s : ""_s, ": imports in this executable were resolved at build time, so the module loader can't be patched."_s)));
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

ASCIILiteral evaluationRefusedMessage()
{
    return "Code generation from strings disallowed for this context: the compiler couldn't tell at build time that this call reaches eval() or the Function constructor. Call eval() or new Function() directly."_s;
}

String memberOfLockedBuiltin(BuiltinID holder, UniquedStringImpl* key)
{
    String text = BuiltinTable::singleton().textOf(holder);
    if (key->isSymbol())
        return makeString(text, "[Symbol("_s, StringView(key), ")]"_s);
    if (parseIndex(*key))
        return makeString(text, '[', StringView(key), ']');
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

String callOfBuiltinChangedBeforeFirstUseMessage(const String& property)
{
    return withAOTErrorNote(makeString("Cannot call "_s, property, " because it was replaced, redefined or deleted before its first use, and compiled code calls the built-in function without looking it up"_s));
}

String assignmentThatBuildDidNotSeeMessage(const String& property)
{
    // The build never sees a file that is loaded from disk, so spelling the assignment out there would not help.
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
    // A member that is constant unless written is locked only where the build has followed no write to it.
    bool canBeOpened = found ? table.canBeReplacedInTestRuns(found) || table.at(found).mutability == BuiltinMutability::ConstantUnlessWritten : isText && table.canTakeNameInTestRuns(holder, name);
    // The language lets nobody replace `Bun`, so a build reads it without a lookup in a test run too.
    canBeOpened &= !found || table.canonicalID(found) != Builtins::Bun;
    ASCIILiteral does = found ? "replace"_s : "add"_s;
    if (!canBeOpened)
        return refusalThatNoTestRunLifts(change, member, does);
    String object = table.textOf(holder);
    String spelled = isText ? makeString(object, '.', StringView(key)) : makeString(object, '[', StringView(key), ']');
    String example = found
        ? makeString(spelled, " = f or spyOn("_s, object, ", "_s, isText ? "\""_s : ""_s, StringView(key), isText ? "\""_s : ""_s, ')')
        : makeString(spelled, " = value"_s);
    return makeString(change, member, " because built-in objects are read-only in compiled code, and the build didn't see a test "_s, does, " this one. Spell out the object and the name where the test does it, as in "_s, example, pageOnCompiledTests);
}

// `new Function("Buffer.isEncoding = f")`: the text has the origin of the file that has it evaluated. Code from disk has its own.
static bool textOnTopIsOfFileOfProduct(VM& vm)
{
    std::optional<bool> answer;
    StackVisitor::visit(vm.topCallFrame, vm, [&](auto& visitor) {
        if constexpr (std::remove_reference_t<decltype(visitor)>::visitsCompiledFrames) {
            if (visitor->isCompiledFrame())
                return IterationStatus::Done;
        }
        CodeBlock* codeBlock = visitor->codeBlock();
        if (!codeBlock)
            return IterationStatus::Continue;
        const URL& origin = codeBlock->ownerExecutable()->sourceOrigin().url();
        std::optional<ModuleID> file = origin.protocolIsFile() ? findModule(origin.fileSystemPath()) : std::nullopt;
        if (file)
            answer = findsBuiltinsReadOnly(*file);
        return IterationStatus::Done;
    });
    return answer.value_or(false);
}

bool isWrittenByFileOfProduct(VM& vm)
{
    // Compiled code writes itself, or calls a built-in function that does. Code from disk writes for itself: no compiled frame is on top then.
    std::optional<bool> answer;
    forEachShownFrameOfRunAtTop(vm, [&](const TraceFrame& frame) {
        std::optional<SourcePosition> position = positionOf(frame);
        // Bun's own code writes for whoever has called it.
        if (position && position->source)
            answer = findsBuiltinsReadOnly(position->source);
        return answer ? IterationStatus::Done : IterationStatus::Continue;
    });
    return answer ? *answer : vm.topCallFrame && textOnTopIsOfFileOfProduct(vm);
}

String assignmentToLockedBuiltinMessage(VM& vm, BuiltinID holder, UniquedStringImpl* key, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return refusalInTestRun(vm, "Cannot assign to "_s, holder, key);
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinName name = table.nameOf(vm, key);
    BuiltinID found = name ? table.findOwnMember(holder, name) : BuiltinID();
    // A new name that would shadow a member inherited from a prototype is refused however the assignment is written.
    bool isNewName = !key->isSymbol() && !parseIndex(*key) && !found && (!name || table.findMember(holder, name).outcome != BuiltinMemberLookup::Outcome::Found);
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

String spyOnExportRefusedMessage(UniquedStringImpl* name, const String& path, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return makeString("Cannot spy on '"_s, StringView(name), "' of \""_s, path, "\" because the build didn't see a test spy on it. Spell out the module and the name where the spy is made: import * as module from \"<path>\"; spyOn(module, \""_s, StringView(name), "\")"_s, pageOnCompiledTests);
    return makeString("Cannot spy on '"_s, StringView(name), "' of \""_s, path, "\" because module exports can't be replaced in a compiled executable. Spy on it in a test, under bun test"_s);
}

String mockOfModuleRefusedMessage(const String& path, LockedIn lockedIn)
{
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
