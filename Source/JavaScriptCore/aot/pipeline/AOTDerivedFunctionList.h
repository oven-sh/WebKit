/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#define FOR_EACH_AOT_FUNCTION_ROLE_DERIVED_ON_REQUEST(v) \
    v(GenericEntrypoint, buildGenericEntrypoint, Function, GenericEntrypoints) \
    v(AllocatedThisVersion, buildAllocatedThisVersion, Function, None) \
    v(BoundFunction, buildBoundFunction, Function, BoundFunctions) \
    v(ResultWorker, buildResultWorker, Function, FreshResultsSplit) \
    v(ChainStep, buildChainStep, Function, None) \
    v(Serializer, buildSerializer, Layout, SerializersCreated) \
    v(Parser, buildParser, Type, ParsersCreated) \
    v(ValidatingParser, buildValidatingParser, ValidationNode, ParsersCreated) \
    v(Validator, buildValidator, ValidationNode, ValidatorsCreated)

#define FOR_EACH_AOT_FUNCTION_ROLE_DERIVED_BY_PROGRAM_PASS(v) \
    v(GroupEntrypoint, Function) \
    v(GenericVersion, Function) \
    v(CoroutineBody, Function)

#endif // ENABLE(AOT)
