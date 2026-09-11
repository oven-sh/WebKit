function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function shouldThrow(func, errorType) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }

    if (!(error instanceof errorType))
        throw new Error(`Expected ${errorType.name}!`);
}

if (typeof Intl.DurationFormat !== "undefined") {
    // A sub-second unit in "numeric" style is formatted as the decimal fraction of the
    // preceding unit. The "auto" display test of that preceding unit has to look at the
    // value with the fraction folded in, not at its own integer field.
    // https://tc39.es/ecma402/#sec-partitiondurationformatpattern step 4.h.ii-iii.
    {
        const df = new Intl.DurationFormat("en", { milliseconds: "numeric" });
        shouldBe(df.format({ milliseconds: 250 }), "0.25 sec");
        shouldBe(df.format({ hours: 3, milliseconds: 250 }), "3 hr, 0.25 sec");
        shouldBe(df.format({ seconds: 1, milliseconds: 500 }), "1.5 sec");
        shouldBe(df.format({ microseconds: 500 }), "0.0005 sec");
        shouldBe(df.format({ nanoseconds: 1 }), "0.000000001 sec");
        shouldBe(df.format({ milliseconds: 0 }), "");
        shouldBe(df.format({ hours: 3 }), "3 hr");

        shouldBe(df.format({ milliseconds: -250 }), "-0.25 sec");
        shouldBe(df.format({ hours: -3, milliseconds: -250 }), "-3 hr, 0.25 sec");
        shouldBe(df.format({ seconds: -1, milliseconds: -250 }), "-1.25 sec");

        shouldBe(JSON.stringify(df.formatToParts({ milliseconds: 999 })), JSON.stringify([
            { type: "integer", value: "0", unit: "second" },
            { type: "decimal", value: ".", unit: "second" },
            { type: "fraction", value: "999", unit: "second" },
            { type: "literal", value: " ", unit: "second" },
            { type: "unit", value: "sec", unit: "second" },
        ]));
        shouldBe(JSON.stringify(df.formatToParts({ milliseconds: -999 })), JSON.stringify([
            { type: "minusSign", value: "-", unit: "second" },
            { type: "integer", value: "0", unit: "second" },
            { type: "decimal", value: ".", unit: "second" },
            { type: "fraction", value: "999", unit: "second" },
            { type: "literal", value: " ", unit: "second" },
            { type: "unit", value: "sec", unit: "second" },
        ]));
    }

    // Every base style.
    {
        shouldBe(new Intl.DurationFormat("en", { style: "long", milliseconds: "numeric" }).format({ milliseconds: 250 }), "0.25 seconds");
        shouldBe(new Intl.DurationFormat("en", { style: "short", milliseconds: "numeric" }).format({ milliseconds: 250 }), "0.25 sec");
        shouldBe(new Intl.DurationFormat("en", { style: "narrow", milliseconds: "numeric" }).format({ milliseconds: 1 }), "0.001s");
        shouldBe(new Intl.DurationFormat("en", { style: "digital", milliseconds: "numeric" }).format({ milliseconds: 250 }), "0:00:00.25");
        shouldBe(new Intl.DurationFormat("en", { style: "long", milliseconds: "numeric" }).format({ minutes: 2, microseconds: 300 }), "2 minutes, 0.0003 seconds");
    }

    // Milliseconds and microseconds as the carrying unit.
    {
        const micro = new Intl.DurationFormat("en", { microseconds: "numeric" });
        shouldBe(micro.format({ microseconds: 55 }), "0.055 ms");
        shouldBe(micro.format({ seconds: 3, microseconds: 55 }), "3 sec, 0.055 ms");
        shouldBe(micro.format({ nanoseconds: 5 }), "0.000005 ms");
        shouldBe(micro.format({ microseconds: -55 }), "-0.055 ms");

        const nano = new Intl.DurationFormat("en", { nanoseconds: "numeric" });
        shouldBe(nano.format({ nanoseconds: 7 }), "0.007 μs");
        shouldBe(nano.format({ hours: 1, nanoseconds: 7 }), "1 hr, 0.007 μs");
        shouldBe(nano.format({ nanoseconds: -7 }), "-0.007 μs");
    }

    // fractionalDigits truncates the displayed fraction, but the display test is on the exact value.
    {
        const df = new Intl.DurationFormat("en", { milliseconds: "numeric", fractionalDigits: 2 });
        shouldBe(df.format({ milliseconds: 5 }), "0.00 sec");
        shouldBe(df.format({ milliseconds: 45 }), "0.04 sec");
        shouldBe(df.format({ seconds: 0 }), "");
    }

    // A numeric or 2-digit seconds unit with integer part 0 keeps the sign of the fraction.
    {
        shouldBe(new Intl.DurationFormat("en", { seconds: "numeric" }).format({ milliseconds: -250 }), "-0.25");
        shouldBe(new Intl.DurationFormat("en", { seconds: "numeric" }).format({ milliseconds: 250 }), "0.25");
        shouldBe(new Intl.DurationFormat("en", { seconds: "2-digit" }).format({ milliseconds: -250 }), "-00.25");
        shouldBe(new Intl.DurationFormat("en", { secondsDisplay: "always", milliseconds: "numeric" }).format({ milliseconds: -250 }), "-0.25 sec");
        shouldBe(new Intl.DurationFormat("en", { style: "digital" }).format({ milliseconds: -250 }), "-0:00:00.25");
        shouldBe(JSON.stringify(new Intl.DurationFormat("en", { seconds: "numeric" }).formatToParts({ milliseconds: -250 })), JSON.stringify([
            { type: "minusSign", value: "-", unit: "second" },
            { type: "integer", value: "0", unit: "second" },
            { type: "decimal", value: ".", unit: "second" },
            { type: "fraction", value: "25", unit: "second" },
        ]));
    }

    // https://tc39.es/ecma402/#sec-getdurationunitoptions step 4: "numeric" on a sub-second unit
    // is the "fractional" style, whose display default is "auto" even when the style was explicit.
    {
        const options = new Intl.DurationFormat("en", { milliseconds: "numeric" }).resolvedOptions();
        shouldBe(options.milliseconds, "numeric");
        shouldBe(options.millisecondsDisplay, "auto");
        shouldBe(options.microseconds, "numeric");
        shouldBe(options.microsecondsDisplay, "auto");
        shouldBe(options.nanoseconds, "numeric");
        shouldBe(options.nanosecondsDisplay, "auto");

        shouldBe(new Intl.DurationFormat("en", { microseconds: "numeric" }).resolvedOptions().microsecondsDisplay, "auto");
        shouldBe(new Intl.DurationFormat("en", { nanoseconds: "numeric" }).resolvedOptions().nanosecondsDisplay, "auto");

        // A numeric seconds unit is not fractional and keeps "always".
        shouldBe(new Intl.DurationFormat("en", { seconds: "numeric" }).resolvedOptions().secondsDisplay, "always");
        // A sub-second unit that is not fractional keeps "always" too.
        shouldBe(new Intl.DurationFormat("en", { milliseconds: "short" }).resolvedOptions().millisecondsDisplay, "always");
    }

    // https://tc39.es/ecma402/#sec-validatedurationunitstyle step 1: display "always" on a
    // fractional unit is a RangeError, whether the style was explicit or a default.
    {
        shouldThrow(() => new Intl.DurationFormat("en", { milliseconds: "numeric", millisecondsDisplay: "always" }), RangeError);
        shouldThrow(() => new Intl.DurationFormat("en", { microseconds: "numeric", microsecondsDisplay: "always" }), RangeError);
        shouldThrow(() => new Intl.DurationFormat("en", { nanoseconds: "numeric", nanosecondsDisplay: "always" }), RangeError);
        shouldThrow(() => new Intl.DurationFormat("en", { milliseconds: "numeric", nanosecondsDisplay: "always" }), RangeError);
        shouldThrow(() => new Intl.DurationFormat("en", { seconds: "numeric", millisecondsDisplay: "always" }), RangeError);
        shouldThrow(() => new Intl.DurationFormat("en", { style: "digital", millisecondsDisplay: "always" }), RangeError);
        shouldThrow(() => new Intl.DurationFormat("en", { style: "digital", microsecondsDisplay: "always" }), RangeError);

        // "auto" is accepted, and "always" is still accepted on a sub-second unit that is not fractional.
        new Intl.DurationFormat("en", { milliseconds: "numeric", millisecondsDisplay: "auto" });
        new Intl.DurationFormat("en", { style: "digital", nanosecondsDisplay: "auto" });
        shouldBe(new Intl.DurationFormat("en", { milliseconds: "short", millisecondsDisplay: "always" }).format({ seconds: 1 }), "1 sec, 0 ms");
    }
}
