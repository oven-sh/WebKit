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

// The validation nodes of aot/ir/schema/types.def, as the runtime holds them.
class ValidationPlan : public ThreadSafeRefCounted<ValidationPlan> {
public:
    static constexpr uint32_t none = UINT32_MAX;

    enum class Shape : uint8_t { Unknown, Never, Undefined, Null, AnyObject, String, Number, Boolean, BigInt, BooleanLiteral, NumberLiteral, StringLiteral, Array, Tuple, Dictionary, Object, Union };
    enum class Undeclared : uint8_t { Allowed, Rejected, Validated };
    enum class RuleKind : uint8_t {
        Integer, Minimum, Maximum, ExclusiveMinimum, ExclusiveMaximum, MultipleOf, MinimumLength, MaximumLength, MinimumMembers, MaximumMembers,
        Pattern, Format, Url, Template, UniqueItems, Contains, PatternMembers, MemberNames, DependentMembers, DependentNode, Not, If, ExactlyOne, Every,
    };
    enum class Literal : uint8_t { None, Null, Boolean, Number, String };

    // A flag of MultipleOf.
    static constexpr uint16_t compareAsDecimals = 1;

    struct Rule {
        RuleKind kind;
        uint16_t flags { 0 };
        uint32_t count { 0 };
        uint32_t maxCount { none };
        double number { 0 };
        String text;
        Vector<String> texts;
        Vector<uint32_t> nodes;
    };

    struct Member {
        AtomString key;
        uint32_t node;
        bool isOptional;
        bool mustBeOwn { false };
    };

    struct Node {
        Shape shape { Shape::Unknown };
        Undeclared undeclared { Undeclared::Allowed };
        bool boolean { false };
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
    // JSON Schema: a required member that holds undefined is missing.
    bool treatsUndefinedAsMissing { false };

private:
    ValidationPlan() = default;
};

// Divides the shortest decimal spellings of both numbers, so 0.07 is a multiple of 0.01.
JS_EXPORT_PRIVATE bool isDecimalMultipleOf(double number, double divisor);

// Scans a value at every depth without running user code.
enum class JSONScanResult : uint8_t { OnlyJSON, HasNonJSON, NeedsUserCode };
JS_EXPORT_PRIVATE JSONScanResult scanForNonJSON(JSGlobalObject*, JSValue);

using ValidationFormatChecker = bool (*)(VM&, unsigned format, StringView);
JS_EXPORT_PRIVATE void setValidationFormatChecker(ValidationFormatChecker);

// Parses the text that aot/passes/AOTValidationPlanText.cpp writes. Returns null for anything else.
JS_EXPORT_PRIVATE RefPtr<ValidationPlan> parseValidationPlan(JSGlobalObject*, const String& text);


// The three functions below never run user code. They return null if the plan has no bytecode.

// (value) => boolean
JS_EXPORT_PRIVATE JSFunction* createCheckFunction(VM&, JSGlobalObject*, Ref<ValidationPlan>&&);

// (value) => copy. Calls onRefused(value) for plain data that fails, and onBailout(value) for anything it cannot read without running user code.
JS_EXPORT_PRIVATE JSFunction* createValidateFunction(VM&, JSGlobalObject*, Ref<ValidationPlan>&&, JSObject* onRefused, JSObject* onBailout);

// (text) => value. Both handlers get the parsed value. Any other arguments go to onOtherArguments unchanged.
JS_EXPORT_PRIVATE JSFunction* createParseFunction(VM&, JSGlobalObject*, Ref<ValidationPlan>&&, JSObject* onRefused, JSObject* onBailout, JSObject* onOtherArguments);

enum class ValidatorKind : uint8_t { Check, Validate, Parse };

JS_EXPORT_PRIVATE JSFunction* createValidatorFromPlanText(JSGlobalObject*, ValidatorKind, const String& planText, JSObject* onRefused, JSObject* onBailout, JSObject* onOtherArguments);

} // namespace JSC
