/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/AOTBuiltinIDTypes.h>
#include <JavaScriptCore/AOTID.h>
#include <JavaScriptCore/AOTSchemaEnums.h>
#include <type_traits>

namespace JSC { namespace AOT {

using Typed::FunctionID;
using Typed::ModuleFailureID;
using Typed::ModuleID;
using Typed::RegExpRecordID;
using Typed::StringID;
using Typed::TypeSiteID;

using Typed::AsyncGeneratorStepStatus;
using Typed::CellTestKind;
using Typed::ClassSide;
using Typed::CommonJsParameter;
using Typed::CoroutineKind;
using Typed::EvalBindingKind;
using Typed::EvaluationState;
using Typed::ExternalKind;
using Typed::GeneratorStepCheck;
using Typed::GeneratorStepStatus;
using Typed::ImportPhase;
using Typed::JSONMemberSearch;
using Typed::JSONOutcome;
using Typed::JSONPosition;
using Typed::LayoutKind;
using Typed::MethodKind;
using Typed::ModuleExportKind;
using Typed::ModuleKind;
using Typed::NameClass;
using Typed::ObjectMutation;
using Typed::PrimitiveSet;
using Typed::PropertyKeyKind;
using Typed::PutMode;
using Typed::RegExpMatchForm;
using Typed::RequireResult;
using Typed::ResumeMode;
using Typed::RuntimeErrorKind;
using Typed::SlotRepresentation;
using Typed::StepGeneratorResult;
using Typed::StubExit;
using Typed::SyntheticExportRule;
using Typed::TypeSiteKind;

template<typename T>
inline constexpr bool isImageRecord = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> && std::has_unique_object_representations_v<T>;

// A number under 2^24 in a table of the image.
struct Number24 {
    static constexpr uint32_t max = (1u << 24) - 1;

    constexpr Number24() = default;
    explicit constexpr Number24(uint32_t number)
        : bytes { static_cast<uint8_t>(number), static_cast<uint8_t>(number >> 8), static_cast<uint8_t>(number >> 16) }
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(number <= max);
    }

    constexpr operator uint32_t() const { return bytes[0] | bytes[1] << 8 | bytes[2] << 16; }

    uint8_t bytes[3] { };
};
static_assert(sizeof(Number24) == 3 && alignof(Number24) == 1 && isImageRecord<Number24>);
static_assert(Number24(0xabcdef) == 0xabcdefu && Number24(Number24::max) == Number24::max && !Number24());

static_assert(sizeof(FunctionID) == sizeof(uint32_t));
static_assert(sizeof(StringID) == sizeof(uint32_t));
static_assert(sizeof(ModuleID) == sizeof(uint32_t));
static_assert(sizeof(LayoutKind) == sizeof(uint8_t));
static_assert(sizeof(SlotRepresentation) == sizeof(uint8_t));
static_assert(sizeof(MethodKind) == sizeof(uint8_t));
static_assert(sizeof(CellTestKind) == sizeof(uint8_t));
static_assert(sizeof(PrimitiveSet) == sizeof(uint16_t));
static_assert(sizeof(ObjectMutation) == sizeof(uint16_t));
static_assert(EvaluationState { } == EvaluationState::SeenTwice);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
