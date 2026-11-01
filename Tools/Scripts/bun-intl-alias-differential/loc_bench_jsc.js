const n = +arguments[0], tag = arguments[1], mode = arguments[2];
let s = 0;
if (mode === "opt") { const o = { region: "CA" }; for (let i = 0; i < n; i++) s += new Intl.Locale(tag, o).toString().length; }
else if (mode === "number") { const d = 1234.5; for (let i = 0; i < n; i++) s += d.toLocaleString(tag).length; }
else if (mode === "canonical") { for (let i = 0; i < n; i++) s += Intl.getCanonicalLocales(tag)[0].length; }
else for (let i = 0; i < n; i++) s += new Intl.Locale(tag).toString().length;
if (s < 0) print(s);
