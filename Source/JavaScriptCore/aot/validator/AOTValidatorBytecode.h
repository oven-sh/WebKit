/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/JSCJSValue.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSFunction;
class JSGlobalObject;
class JSObject;
class VM;

// The list of validation nodes (aot/ir/schema/types.def) as values of the running program hold it.
class ValidationPlan : public ThreadSafeRefCounted<ValidationPlan> {
public:
    static constexpr uint32_t none = UINT32_MAX;

    enum class Shape : uint8_t { Unknown, Never, Undefined, Null, AnyObject, String, Number, Boolean, BigInt, BooleanLiteral, NumberLiteral, StringLiteral, Array, Tuple, Dictionary, Object, Union };
    enum class Undeclared : uint8_t { Allowed, Rejected, Validated };
    // Those in front of Pattern cost a few instructions.
    enum class RuleKind : uint8_t {
        Integer, Minimum, Maximum, ExclusiveMinimum, ExclusiveMaximum, MultipleOf, MinimumLength, MaximumLength, MinimumMembers, MaximumMembers,
        Pattern, Format, Url, Template, UniqueItems, Contains, PatternMembers, MemberNames, DependentMembers, DependentNode, Not, If, ExactlyOne, Every,
    };
    enum class Literal : uint8_t { None, Null, Boolean, Number, String };

    // MultipleOf: a divisor with a fraction divides the number as the two are written in decimals. Without it the remainder of the two doubles decides.
    static constexpr uint16_t byDecimals = 1;

    struct Rule {
        RuleKind kind;
        // Pattern, PatternMembers: the flags of the expressions.
        uint16_t flags { 0 };
        // A length, a number of members, a format. Contains: the fewest.
        uint32_t count { 0 };
        // Contains: the most.
        uint32_t most { none };
        double number { 0 };
        // Pattern: the source. Template: a letter for each placeholder. DependentMembers, DependentNode: the key.
        String text;
        // Template: what stands around the placeholders. PatternMembers: the sources. DependentMembers: the names.
        Vector<String> texts;
        Vector<uint32_t> nodes;
    };

    struct Member {
        AtomString key;
        uint32_t node;
        bool isOptional;
        // The node takes undefined, and the key has to be there all the same. Without it a member that is not optional and holds undefined is missing.
        bool mustBeOwn { false };
    };

    struct Node {
        Shape shape { Shape::Unknown };
        Undeclared undeclared { Undeclared::Allowed };
        bool boolean { false };
        // What stands in for undefined.
        Literal defaultKind { Literal::None };
        bool defaultBoolean { false };
        double number { 0 };
        double defaultNumber { 0 };
        String string;
        String defaultString;
        uint32_t child { none };
        uint32_t required { 0 };
        Vector<uint32_t> children;
        Vector<Member> members;
        Vector<Rule> rules;
    };

    static Ref<ValidationPlan> create() { return adoptRef(*new ValidationPlan); }

    Vector<Node> nodes;
    uint32_t root { 0 };

private:
    ValidationPlan() = default;
};

// Whether the number, as the shortest decimals that say it, is a whole multiple of the divisor, taken the same way: 0.07 is one of 0.01.
JS_EXPORT_PRIVATE bool isMultipleInDecimals(double number, double divisor);

// Whether a value, at any depth, holds only what a JSON text says. What no text says: a number that is not finite, undefined or a hole among elements, a bigint, a symbol, a function.
// No code of the value runs: NeedsCode stands for an accessor and a Proxy, and for an exception that is pending.
enum class ValidationLook : uint8_t { OnlyJSON, HoldsOther, NeedsCode };
JS_EXPORT_PRIVATE ValidationLook lookThroughForJSON(JSGlobalObject*, JSValue);

using ValidationFormatChecker = bool (*)(VM&, unsigned format, StringView);
JS_EXPORT_PRIVATE void setValidationFormatChecker(ValidationFormatChecker);

// The nodes that the text of a plan (aot/passes/AOTValidationPlanText.cpp) says. Null where the text is no plan, or holds a word that is not known here.
JS_EXPORT_PRIVATE RefPtr<ValidationPlan> readValidationPlan(JSGlobalObject*, const String& text);

// Each of the three is null when the plan holds what has no bytecode yet. No code of the value runs in any of them.

// function (value) -> boolean. False for a value whose answer could change without a write: a getter, a Proxy.
JS_EXPORT_PRIVATE JSFunction* createCheckFunction(VM&, JSGlobalObject*, Ref<ValidationPlan>&&);

// function (value) -> a copy of the objects and arrays that the plan describes. A value of plain data that the plan refuses goes to onRefused(value). A value that cannot be read
// without running code, or that the function leaves for any other reason, goes to onNotLookedAt(value) untouched: nothing is said of it. What the two answer is the answer.
JS_EXPORT_PRIVATE JSFunction* createValidateFunction(VM&, JSGlobalObject*, Ref<ValidationPlan>&&, JSObject* onRefused, JSObject* onNotLookedAt);

// function (text, reviver) -> the value that the text says, checked. The two handlers are handed that value. Anything but one argument that is a string goes to onOtherArguments with the arguments that came.
JS_EXPORT_PRIVATE JSFunction* createParseFunction(VM&, JSGlobalObject*, Ref<ValidationPlan>&&, JSObject* onRefused, JSObject* onNotLookedAt, JSObject* onOtherArguments);

enum class ValidatorKind : uint8_t { Check, Validate, Parse };

// The function of one call site, made of the text of its plan. Throws a TypeError where the text is no plan. Null without an exception: no bytecode yet.
JS_EXPORT_PRIVATE JSFunction* createValidatorOfPlanText(JSGlobalObject*, ValidatorKind, const String& planText, JSObject* onRefused, JSObject* onNotLookedAt, JSObject* onOtherArguments);

} // namespace JSC
