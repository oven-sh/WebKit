// The arithmetic of decimal (runtime/PyDecimalNumber.h) by itself, without the engine and without Python. Reads lines
//     operation precision rounding Emin Emax clamp operands...
// and prints, for each, the result and the conditions it met: result|status (in hexadecimal). decimal_arithmetic_fuzz.py drives it.
//
//     R=../runtime; c++ -std=c++23 -O2 -DPY_DECIMAL_STANDALONE -I$R decimal_arithmetic.cpp $R/PyDecimalNumber.cpp $R/PyDecimalFunctions.cpp \
//         $R/PyDecimalText.cpp $R/PyDecimalTransform.cpp -o decimal_arithmetic
//
// With -fsanitize=address,undefined as well, and with -DPY_DECIMAL_WITHOUT_ASSEMBLY for the division that other processors than x86-64 get.
#include "PyDecimalNumber.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
using namespace JSC::Python::Decimal;

static std::string show(const Number& n, unsigned flags = TextScientific | TextUpperCase)
{
    Text t;
    if (!toText(t, n, flags)) return "<oom>";
    return std::string(t.data, t.length);
}

int main()
{
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string op;
        Context ctx;
        int rounding, clamp;
        in >> op >> ctx.precision >> rounding >> ctx.emin >> ctx.emax >> clamp;
        ctx.rounding = static_cast<Rounding>(rounding);
        ctx.clamp = clamp;
        std::vector<std::string> args;
        std::string a;
        while (in >> a) args.push_back(a);
        Number x[3];
        Context maxctx = Context::maximum();
        uint32_t st = 0;
        bool raw = op == "create" || op == "format";
        size_t nOperands = raw ? (op == "format" ? 1 : 0) : args.size();
        for (size_t i = 0; i < nOperands && i < 3; ++i) {
            uint32_t s2 = 0;
            fromString(x[i], args[i].c_str(), args[i].size(), maxctx, s2);
            if (s2 & (Inexact | Rounded | Clamped)) x[i].setSpecial(0, Number::QuietNaN);
        }
        Number r, r2;
        std::string out;
        bool done = false;
#define UN(name, fn) if (op == name) { fn(r, x[0], ctx, st); done = true; }
#define BIN(name, fn) if (op == name) { fn(r, x[0], x[1], ctx, st); done = true; }
        UN("abs", absolute) UN("plus", plus) UN("minus", minus) UN("sqrt", squareRoot) UN("exp", exponential) UN("ln", naturalLogarithm)
        UN("log10", commonLogarithm) UN("next_minus", nextMinus) UN("next_plus", nextPlus) UN("normalize", reduce)
        UN("to_integral_value", roundToIntegralValue) UN("to_integral_exact", roundToIntegralExact) UN("logical_invert", logicalInvert) UN("logb", logB)
        BIN("add", add) BIN("subtract", subtract) BIN("multiply", multiply) BIN("divide", divide) BIN("divide_int", divideInteger)
        BIN("remainder", remainder) BIN("remainder_near", remainderNear) BIN("max", maximum) BIN("max_mag", maximumMagnitude)
        BIN("min", minimum) BIN("min_mag", minimumMagnitude) BIN("next_toward", nextToward) BIN("quantize", quantize)
        BIN("logical_and", logicalAnd) BIN("logical_or", logicalOr) BIN("logical_xor", logicalXor) BIN("scaleb", scaleB)
        BIN("shift", shift) BIN("rotate", rotate) BIN("compare", compare) BIN("compare_signal", compareSignal) BIN("power", power)
        if (op == "compare_total") { compareTotal(r, x[0], x[1]); done = true; }
        if (op == "compare_total_mag") { compareTotalMagnitude(r, x[0], x[1]); done = true; }
        if (op == "copy_abs") { copyAbsolute(r, x[0], st); done = true; }
        if (op == "copy_negate") { copyNegated(r, x[0], st); done = true; }
        if (op == "copy_sign") { copySign(r, x[0], x[1], st); done = true; }
        if (op == "fma") { fusedMultiplyAdd(r, x[0], x[1], x[2], ctx, st); done = true; }
        if (op == "powmod") { powerModulo(r, x[0], x[1], x[2], ctx, st); done = true; }
        if (op == "create") { fromString(r, args[0].c_str(), args[0].size(), ctx, st); done = true; }
        if (done) out = show(r);
        if (op == "divmod") { divideWithRemainder(r, r2, x[0], x[1], ctx, st); out = show(r) + "," + show(r2); done = true; }
        if (op == "same_quantum") { out = haveSameQuantum(x[0], x[1]) ? "True" : "False"; done = true; }
        if (op == "number_class") { out = classOf(x[0], ctx); done = true; }
        if (op == "to_eng_string") { out = show(x[0], TextEngineering | TextUpperCase); done = true; }
        if (op == "format") {
            FormatSpec spec; bool loc; Text t;
            std::string f = args.size() > 1 ? args[1] : "";
            if (!parseFormatSpec(spec, f.c_str(), true, loc)) out = "<badspec>";
            else if (!format(t, x[0], spec, ctx, st)) out = "<fail>";
            else out = std::string(t.data, t.length);
            done = true;
        }
        if (op == "int64") { int64_t v = toInt64(x[0], st); out = std::to_string(v); done = true; }
        if (!done) out = "<unknown op>";
        printf("%s|%x\n", out.c_str(), st);
    }
    return 0;
}
