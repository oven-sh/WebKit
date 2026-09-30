// What is done about a signal can be a function of either language, and the signal can come while either is running.
import * as m from "./signals.py";

function show(label, f) {
    try {
        print(label, "=>", String(f()));
    } catch (e) {
        print(label, "=>", "JavaScript caught", String(e));
    }
}

m.set_handler(m.USR1, m.handler);
show("raised from JavaScript, with no Python on the stack: there is no frame", () => (m.raise_signal(m.USR1), m.took()));
show("with Python further out, it is that frame", () => (m.call(() => m.raise_signal(m.USR1)), m.took()));

const calls = [];
function inJavaScript(number, frame) { calls.push([m.name(number), frame === undefined ? "no frame" : frame.f_code.co_name]); }
show("a function of JavaScript's", () => (m.set_handler(m.USR1, inJavaScript) === m.handler));
show("is what there is for it", () => m.get_handler(m.USR1) === inJavaScript);
show("and is called", () => (m.raise_signal(m.USR1), m.call(m.raise_signal, m.USR1), JSON.stringify(calls.splice(0))));
show("when the signal comes to Python that is going round", () => (m.kill(), m.spin_until(() => calls.length), JSON.stringify(calls.splice(0).map(call => call[0]))));

m.set_handler(m.USR1, () => { throw new RangeError("from JavaScript's function"); });
show("what it throws, JavaScript catches", () => m.raise_signal(m.USR1));
show("or Python does", () => m.catching(() => m.raise_signal(m.USR1)));
m.set_handler(m.USR1, m.raises);
show("what Python's raises, JavaScript catches", () => m.raise_signal(m.USR1));
show("KeyboardInterrupt too", () => m.raise_signal(m.INT));
show("or Python does", () => m.catching(() => m.raise_signal(m.INT)));

// It is Python code that looks whether a signal has come. JavaScript that is going round does not, so it is seen to when Python is next run.
m.set_handler(m.USR1, inJavaScript);
show("while JavaScript is going round nothing is done", () => {
    m.kill();
    calls.length = 0;
    for (let i = 0; i < 1e6; ++i);
    return calls.length;
});
show("what will not do", () => m.set_handler(m.USR1, {}));
show("nor this", () => m.set_handler(m.USR1, "text"));
show("an arrow function, a bound function and a class will", () => [() => {}, function () {}.bind(null), class {}].map(f => (m.set_handler(m.USR1, f), m.get_handler(m.USR1) === f)));
