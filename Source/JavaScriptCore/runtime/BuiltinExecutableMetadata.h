#pragma once

namespace JSC {

class BuiltinExecutableMetadata {
public:
    BuiltinExecutableMetadata(const unsigned char* data, unsigned length);
    BuiltinExecutableMetadata() = default;
    BuiltinExecutableMetadata(unsigned startColumn, unsigned endColumn, int functionKeywordStart, int functionNameStart, unsigned parametersStart, bool isInStrictContext, bool isArrowFunctionBodyExpression, bool isAsyncFunction, unsigned asyncOffset, unsigned parameterCount, unsigned lineCount, unsigned offsetOfLastNewline, unsigned positionBeforeLastNewlineLineStartOffset, unsigned closeBraceOffsetFromEnd)
        : startColumn(startColumn)
        , endColumn(endColumn)
        , functionKeywordStart(functionKeywordStart)
        , functionNameStart(functionNameStart)
        , parametersStart(parametersStart)
        , isInStrictContext(isInStrictContext)
        , isArrowFunctionBodyExpression(isArrowFunctionBodyExpression)
        , isAsyncFunction(isAsyncFunction)
        , asyncOffset(asyncOffset)
        , parameterCount(parameterCount)
        , lineCount(lineCount)
        , offsetOfLastNewline(offsetOfLastNewline)
        , positionBeforeLastNewlineLineStartOffset(positionBeforeLastNewlineLineStartOffset)
        , closeBraceOffsetFromEnd(closeBraceOffsetFromEnd)
    {
    }

    unsigned startColumn;
    unsigned endColumn;

    int functionKeywordStart;
    int functionNameStart;
    unsigned parametersStart;
    bool isInStrictContext;
    bool isArrowFunctionBodyExpression;
    bool isAsyncFunction;

    unsigned asyncOffset;
    unsigned parameterCount;

    unsigned lineCount;
    unsigned offsetOfLastNewline;
    unsigned positionBeforeLastNewlineLineStartOffset;
    unsigned closeBraceOffsetFromEnd;
};

} // namespace JSC