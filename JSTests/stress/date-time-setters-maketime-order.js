//@ runDefault

// setHours, setMinutes, setSeconds, setMilliseconds and their UTC forms build the new time value
// with MakeDate(Day(t), MakeTime(hour, min, sec, ms)): the day of the Date is kept, and each of
// the four time fields is the argument or, when it was not given, the field of the Date.
// MakeTime adds in doubles, in one order:
//     ((hour * msPerHour + min * msPerMinute) + sec * msPerSecond) + ms
// The order shows once a partial sum is beyond 2^53, where a double no longer holds every
// integer. So a time setter gives what Date.UTC and the Date constructor give for the same
// fields, to the millisecond.
// https://tc39.es/ecma262/#sec-maketime

function shouldBe(actual, expected, message) {
    if (!Object.is(actual, expected))
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

const msPerDay = 86400000;
const msPerHour = 3600000;
const msPerMinute = 60000;
const msPerSecond = 1000;
const maxTimeValue = 8.64e15;

// MakeTime, MakeDate and TimeClip as the specification writes them. Each * and + is one double
// operation.
function makeTime(hour, min, sec, ms) {
    if (!Number.isFinite(hour) || !Number.isFinite(min) || !Number.isFinite(sec) || !Number.isFinite(ms))
        return NaN;
    return ((Math.trunc(hour) * msPerHour + Math.trunc(min) * msPerMinute) + Math.trunc(sec) * msPerSecond) + Math.trunc(ms);
}

function makeDate(day, time) {
    if (!Number.isFinite(day) || !Number.isFinite(time))
        return NaN;
    let tv = day * msPerDay + time;
    return Number.isFinite(tv) ? tv : NaN;
}

function timeClip(time) {
    if (!Number.isFinite(time) || Math.abs(time) > maxTimeValue)
        return NaN;
    return Math.trunc(time) + 0;
}

// The four time setters: the name, and which of hour, minute, second, millisecond the first
// argument is.
const setters = [["Hours", 0], ["Minutes", 1], ["Seconds", 2], ["Milliseconds", 3]];

// What set<name> and setUTC<name> return for the arguments. The expectation of the UTC form is
// the specification's arithmetic on the time value. The expectation of the local form is the Date
// constructor with the local fields of the Date: MakeDay of the Date's own year, month and day
// is its day number. Each form runs on a Date that nothing has read yet, and on one that a getter
// has broken down into fields before.
function checkSetter(timeValue, name, firstField, args) {
    let call = `(${args.join(", ")}) on ${timeValue}`;

    let probe = new Date(timeValue);
    let day = Math.floor(timeValue / msPerDay);
    let timeInDay = timeValue - day * msPerDay;
    let fields = [Math.floor(timeInDay / msPerHour), Math.floor(timeInDay / msPerMinute) % 60, Math.floor(timeInDay / msPerSecond) % 60, timeInDay % 1000];
    shouldBe(probe.getUTCHours(), fields[0], `getUTCHours of ${timeValue}`);
    shouldBe(probe.getUTCMinutes(), fields[1], `getUTCMinutes of ${timeValue}`);
    shouldBe(probe.getUTCSeconds(), fields[2], `getUTCSeconds of ${timeValue}`);
    shouldBe(probe.getUTCMilliseconds(), fields[3], `getUTCMilliseconds of ${timeValue}`);
    for (let i = 0; i < args.length; ++i)
        fields[firstField + i] = args[i];
    let expected = timeClip(makeDate(day, makeTime(...fields)));
    let year = probe.getUTCFullYear();
    if (year < 0 || year > 99)
        shouldBe(Date.UTC(year, probe.getUTCMonth(), probe.getUTCDate(), ...fields), expected, `Date.UTC for setUTC${name}${call}`);
    for (let brokenDown of [false, true]) {
        let date = new Date(timeValue);
        if (brokenDown)
            date.getUTCDay();
        shouldBe(date[`setUTC${name}`](...args), expected, `setUTC${name}${call}`);
        shouldBe(date.getTime(), expected, `the time value after setUTC${name}${call}`);
    }

    year = probe.getFullYear();
    if (year >= 0 && year <= 99)
        return;
    fields = [probe.getHours(), probe.getMinutes(), probe.getSeconds(), probe.getMilliseconds()];
    for (let i = 0; i < args.length; ++i)
        fields[firstField + i] = args[i];
    expected = new Date(year, probe.getMonth(), probe.getDate(), ...fields).getTime();
    for (let brokenDown of [false, true]) {
        let date = new Date(timeValue);
        if (brokenDown)
            date.getDay();
        shouldBe(date[`set${name}`](...args), expected, `set${name}${call}`);
        shouldBe(date.getTime(), expected, `the time value after set${name}${call}`);
    }
}

// One argument worth just more than 2^53 ms. The fields the Date keeps bring the sum back under
// 2^53, so MakeTime is exact where adding the argument to the milliseconds first loses the last
// bit.
shouldBe(new Date(1875619980085361).setUTCMinutes(-150119987617), -7131579276994639, "setUTCMinutes");
shouldBe(new Date(2725612719655037).setUTCMinutes(-150119987734), -6281586545584963, "setUTCMinutes");
shouldBe(new Date(6000780672132969).setUTCHours(-2501999793), -3006418582667031, "setUTCHours");
shouldBe(new Date(4092517536431551).setUTCHours(-2501999793), -4914681725568449, "setUTCHours");
shouldBe(new Date(8288993643969297).setUTCSeconds(-9007199277021), -718205633060703, "setUTCSeconds");
shouldBe(new Date(4913242772966623).setUTCSeconds(-9007199315586), -4093956542645377, "setUTCSeconds");
shouldBe(new Date(maxTimeValue - 1).setUTCHours(-2501999793), -367199337600001, "setUTCHours");

// Two or three arguments that cancel. MakeTime rounds each partial sum to a double, so what the
// first additions lose is not in the result.
shouldBe(new Date(1633790731877088).setUTCSeconds(-418978581251620, 415150737421630500), -2194053098129472, "setUTCSeconds");
shouldBe(new Date(2096109278351068).setUTCSeconds(-1065194099809714, 1069059298030771700), 5961307499397632, "setUTCSeconds");
shouldBe(new Date(-8371246840953827).setUTCMinutes(-18917803988527, 290920603005747, 846386976741586400), -6131906406286592, "setUTCMinutes");
shouldBe(new Date(1329686253984).setUTCMinutes(-9007199254740992, 540435517599968260), 3563645193973720, "setUTCMinutes");
shouldBe(new Date(60000).setUTCSeconds(2 ** 53, -(2 ** 53) * 1000), 60416, "setUTCSeconds");
shouldBe(new Date(3600000).setUTCMinutes(2 ** 46, -(2 ** 46) * 60), 3599872, "setUTCMinutes");
shouldBe(Date.UTC(1970, 0, 1, 0, 1, 2 ** 53, -(2 ** 53) * 1000), 60416, "Date.UTC");

// The same calls, and the ones around them, for every setter in both forms.
for (let timeValue of [1875619980085361, 6000780672132969, 8288993643969297, -1875619980085361, 1633790731877088, -123456789]) {
    let sign = timeValue > 0 ? -1 : 1;
    for (let extra of [0, 1, 1000, 86399]) {
        checkSetter(timeValue, "Hours", 0, [sign * (2501999793 + Math.trunc(extra / 3600))]);
        checkSetter(timeValue, "Minutes", 1, [sign * (150119987579 + Math.trunc(extra / 60))]);
        checkSetter(timeValue, "Seconds", 2, [sign * (9007199254741 + extra)]);
        checkSetter(timeValue, "Milliseconds", 3, [sign * (2 ** 53 + 2 * extra)]);
    }
}

// Arguments of every size, for every setter and every argument count: in the range of the
// field, out of it, and worth far more than 2^53 ms with the last argument cancelling the
// others.
{
    let state = 0x9e3779b9;
    let random = () => {
        state ^= state << 13;
        state >>>= 0;
        state ^= state >>> 17;
        state ^= state << 5;
        state >>>= 0;
        return state / 4294967296;
    };
    let randomInteger = (low, high) => Math.floor(low + random() * (high - low + 1));
    const units = [msPerHour, msPerMinute, msPerSecond, 1];
    const maxima = [23, 59, 59, 999];
    for (let i = 0; i < testLoopCount / 10; ++i) {
        let [name, firstField] = setters[i & 3];
        let count = randomInteger(1, 4 - firstField);
        let timeValue;
        switch (randomInteger(0, 3)) {
        case 0:
            timeValue = Math.trunc((random() * 2 - 1) * maxTimeValue);
            break;
        case 1:
            timeValue = randomInteger(-20000, 20000) * msPerDay + randomInteger(-1, 1) * randomInteger(0, 1000);
            break;
        default:
            timeValue = randomInteger(-2000000000000, 4000000000000);
            break;
        }
        let args = [];
        let worth = 0;
        let kind = randomInteger(0, 3);
        for (let k = 0; k < count; ++k) {
            let unit = units[firstField + k];
            let value;
            if (!kind)
                value = randomInteger(0, maxima[firstField + k]);
            else if (kind === 1)
                value = randomInteger(-100000, 100000);
            else if (k === count - 1)
                value = Math.trunc((-worth + (random() * 2 - 1) * 8e15) / unit);
            else
                value = Math.trunc((random() * 2 - 1) * 2 ** randomInteger(50, 62) / unit);
            args.push(value);
            worth += value * unit;
        }
        checkSetter(timeValue, name, firstField, args);
    }
}

// The day and the fields a setter keeps, for time values where they are easy to get wrong: before
// 1970, where the day number is a floor and not a truncation, the first and the last millisecond
// of a day, and both ends of the range.
for (let timeValue of [0, 1, -1, 999, -999, 1000, -1000, 86399999, -86399999, 86400000, -86400000, 86400001, -86400001,
    -62198755200001, -62135596800000, -2208988800000, 951782400000, 1710064800000, 1730624400000, 4102444800000,
    maxTimeValue, maxTimeValue - 1, maxTimeValue - msPerDay, maxTimeValue - msPerDay - 1, -maxTimeValue, -maxTimeValue + 1, -maxTimeValue + msPerDay, -maxTimeValue + msPerDay + 1]) {
    for (let [name, firstField] of setters) {
        for (let value of [0, -1, 86400]) {
            checkSetter(timeValue, name, firstField, [value]);
            if (firstField < 3)
                checkSetter(timeValue, name, firstField, [value, value + 1]);
            if (firstField < 2)
                checkSetter(timeValue, name, firstField, [0, value, -value]);
            if (!firstField)
                checkSetter(timeValue, name, firstField, [value, -value, value, -value]);
        }
    }
}

// Every hour across the two daylight saving changes of 2024 in the time zone of the test
// (US/Pacific under run-jsc-stress-tests): a local setter converts to local time and back.
for (let start of [Date.UTC(2024, 2, 9, 20), Date.UTC(2024, 10, 2, 20)]) {
    for (let hour = 0; hour < 40; ++hour) {
        let timeValue = start + hour * msPerHour + 1805123;
        checkSetter(timeValue, "Hours", 0, [hour % 24]);
        checkSetter(timeValue, "Hours", 0, [2, 30]);
        checkSetter(timeValue, "Minutes", 1, [hour]);
        checkSetter(timeValue, "Seconds", 2, [hour]);
        checkSetter(timeValue, "Milliseconds", 3, [hour]);
    }
}

// A local time zone offset that is not a whole number of minutes (local mean time, as
// US/Pacific has before 1883): the seconds a local setter keeps are those of the local time.
for (let timeValue of [-2717640000000, -2717640000001, -5000000000000, -12219292800000, -62198755200001]) {
    for (let [name, firstField] of setters) {
        checkSetter(timeValue, name, firstField, [7]);
        checkSetter(timeValue, name, firstField, [-7]);
    }
}

// An argument that is not a whole number: MakeTime truncates it.
for (let [name, firstField] of setters) {
    for (let value of [0.9, -0.9, -0, 1.5, -1.5, 4294967296.5, "7", " 7.9 ", null, true, false, [3], 2 ** 53, -(2 ** 53)]) {
        let timeValue = 1700000000123;
        let timeInDay = timeValue % msPerDay;
        let fields = [Math.floor(timeInDay / msPerHour), Math.floor(timeInDay / msPerMinute) % 60, Math.floor(timeInDay / msPerSecond) % 60, timeInDay % 1000];
        fields[firstField] = Number(value);
        let expected = timeClip(makeDate(Math.floor(timeValue / msPerDay), makeTime(...fields)));
        let date = new Date(timeValue);
        shouldBe(date[`setUTC${name}`](value), expected, `setUTC${name}(${JSON.stringify(value)})`);
        shouldBe(date.getTime(), expected, `the time value after setUTC${name}(${JSON.stringify(value)})`);
    }
}

const float64 = new Float64Array(1);
const uint64 = new BigUint64Array(float64.buffer);
function bitsOf(number) {
    float64[0] = number;
    return uint64[0].toString(16);
}

// A result that is not a time value: an argument that is not finite, finite arguments whose
// products are not, and a finite sum outside the range. Each call returns NaN and leaves the Date
// invalid, on a Date that nothing has read yet and on one that getters have broken down. The NaN
// a time setter returns and stores has the bits of NaN itself: they do not depend on how the
// arithmetic arrived at it.
function checkInvalid(method, args, timeValue = 1700000000123) {
    for (let brokenDown of [false, true]) {
        let call = `${method}(${args.map(String).join(", ")}) on ${timeValue}${brokenDown ? " after a getter" : ""}`;
        let date = new Date(timeValue);
        if (brokenDown)
            shouldBe(date.getDay() + date.getUTCDay() >= 0, true, "getDay");
        let result = date[method](...args);
        shouldBe(result, NaN, call);
        shouldBe(bitsOf(result), bitsOf(NaN), `the bits of ${call}`);
        shouldBe(date.getTime(), NaN, `the time value after ${call}`);
        shouldBe(bitsOf(date.getTime()), bitsOf(NaN), `the bits of the time value after ${call}`);
        shouldBe(date.getHours(), NaN, `getHours after ${call}`);
        shouldBe(date.getUTCMilliseconds(), NaN, `getUTCMilliseconds after ${call}`);
        shouldBe(String(date), "Invalid Date", `the Date after ${call}`);
    }
}
for (let [name, firstField] of setters) {
    for (let form of ["set", "setUTC"]) {
        let method = `${form}${name}`;
        let count = 4 - firstField;
        for (let value of [NaN, -NaN, Infinity, -Infinity, undefined, "x", {}, 1e308 * (firstField < 3 ? 1 : 10)]) {
            // The value that is not finite in each argument position, after ordinary ones.
            for (let position = 0; position < count; ++position) {
                let args = [1, 2, 3, 4].slice(0, position);
                args.push(value);
                checkInvalid(method, args);
                if (position + 1 < count)
                    checkInvalid(method, [...args, 5]);
            }
        }
        // Finite arguments: a product that overflows, a sum that overflows, a sum past the range.
        checkInvalid(method, [firstField < 3 ? 1e306 : 1.7976931348623157e308]);
        checkInvalid(method, [1e17]);
        checkInvalid(method, [-1e17]);
        if (count > 1) {
            checkInvalid(method, [Infinity, -Infinity]);
            checkInvalid(method, [-Infinity, Infinity]);
            checkInvalid(method, [1e308, -1e308]);
            checkInvalid(method, [1.7976931348623157e308, 1.7976931348623157e308]);
            checkInvalid(method, [NaN, NaN]);
        }
    }
}
shouldBe(new Date(maxTimeValue).setUTCMilliseconds(1), NaN, "setUTCMilliseconds past the range");
shouldBe(new Date(-maxTimeValue).setUTCMilliseconds(-1), NaN, "setUTCMilliseconds before the range");
shouldBe(new Date(maxTimeValue).setUTCHours(0, 0, 0, 0), maxTimeValue, "setUTCHours at the end of the range");
shouldBe(new Date(-maxTimeValue).setUTCHours(0, 0, 0, 0), -maxTimeValue, "setUTCHours at the start of the range");

// The time value is read before the arguments are converted, every argument is converted, in
// order, and arguments past the ones the setter takes are not.
for (let [name, firstField] of setters) {
    for (let form of ["set", "setUTC"]) {
        let method = `${form}${name}`;
        let taken = 4 - firstField;
        let expected = new Date(1700000000123)[method](1, 2, 3, 4);
        let date;
        let log;
        let args;
        for (let brokenDown of [false, true]) {
            for (let newTimeValue of [1e12, NaN]) {
                date = new Date(1700000000123);
                if (brokenDown)
                    shouldBe(date.getDay() + date.getUTCDay() >= 0, true, "getDay");
                log = [];
                args = [1, 2, 3, 4, 5].map((value, index) => ({
                    valueOf() {
                        log.push(index);
                        date.setTime(index * newTimeValue);
                        return value;
                    }
                }));
                shouldBe(date[method](...args), expected, `${method} with arguments that set the time value to ${newTimeValue}`);
                shouldBe(date.getTime(), expected, `the time value after ${method} with arguments that set the time value to ${newTimeValue}`);
                shouldBe(log.join(), [0, 1, 2, 3].slice(0, taken).join(), `the arguments ${method} converts`);
            }
        }

        date = new Date(1700000000123);
        log = [];
        args = [NaN, 2, 3, 4].map((value, index) => ({
            valueOf() {
                log.push(index);
                return value;
            }
        }));
        shouldBe(date[method](...args), NaN, `${method} with a first argument that is NaN`);
        shouldBe(date.getTime(), NaN, `the time value after ${method} with a first argument that is NaN`);
        shouldBe(log.join(), [0, 1, 2, 3].slice(0, taken).join(), `the arguments ${method} converts after a NaN`);

        date = new Date(1700000000123);
        let thrown = false;
        try {
            date[method]({ valueOf() { throw new Error("valueOf"); } });
        } catch {
            thrown = true;
        }
        shouldBe(thrown, true, `${method} with an argument that throws`);
        shouldBe(date.getTime(), 1700000000123, `the time value after ${method} threw`);
    }
}
