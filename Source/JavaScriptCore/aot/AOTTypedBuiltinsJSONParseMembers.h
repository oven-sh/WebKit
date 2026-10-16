/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

    const Value& constantJSONParserOperand(const Value&, unsigned index) const;
    bool isJSONParserTest(const Value&) const;
    void branchOnJSONParserTest(const Value&, LBasicBlock holds, LBasicBlock fails);
    Integer skipJSONWhitespace(Integer cursor);
