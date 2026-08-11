// Micro/macro RegExp benchmarks: jsc bench.js  (or node bench.js). Prints "name<TAB>ms".
const P = typeof print !== "undefined" ? print : console.log;
const RF = typeof readFile !== "undefined" ? readFile : (p) => require("fs").readFileSync(p, "utf8");
const now = typeof preciseTime !== "undefined" ? () => preciseTime() * 1000 : () => performance.now();
const HERE = (typeof arguments !== "undefined" && arguments[0]) || (typeof process !== "undefined" && process.argv[2]) || ".";
const isbot = JSON.parse(RF(HERE + "/isbot-pattern.json"));

function bench(name, fn, iters, warm) {
    for (let i = 0; i < (warm ?? Math.max(1, iters / 10 | 0)); i++) fn(i);
    const t0 = now();
    let sink = 0;
    for (let i = 0; i < iters; i++) sink += fn(i) ? 1 : 0;
    const ms = now() - t0;
    P(name + "\t" + ms.toFixed(2) + "\t" + sink);
}

const UAS = [
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36",
    "Mozilla/5.0 (compatible; Googlebot/2.1; +http://www.google.com/bot.html)",
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Mobile/15E148 Safari/604.1",
    "curl/8.7.1",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36 Edg/125.0.0.0",
    "Slackbot-LinkExpanding 1.0 (+https://api.slack.com/robots)",
    "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.6478.71 Mobile Safari/537.36",
    "python-requests/2.32.3",
];
const isbotRe = new RegExp(isbot.source, isbot.flags);
bench("isbot.test x200k", (i) => isbotRe.test(UAS[i & 7]), 200000, 2000);

const kw = /\b(?:break|case|catch|class|const|continue|debugger|default|delete|do|else|export|extends|finally|for|function|if|import|in|instanceof|let|new|return|super|switch|this|throw|try|typeof|var|void|while|with|yield|async|await)\b/g;
let code = ""; for (let i = 0; i < 3000; i++) code += "function f" + i + "(a,b){ if (a) { return b + " + i + "; } else { for (let k=0;k<a;k++) b+=k; } var x = new Thing(); }\n";
bench("36 keywords /g over " + (code.length >> 10) + "KB x20", () => { kw.lastIndex = 0; let n = 0; while (kw.exec(code)) n++; return n; }, 20, 3);

const camel = /([a-z0-9])([A-Z])/g;
let ident = ""; for (let i = 0; i < 40000; i++) ident += "someCamelCaseIdentifier" + (i % 97) + " anotherOneHere ";
bench("camel->snake replace 1.3MB x5", () => ident.replace(camel, "$1_$2").length, 5, 1);

const lb = /(?<=X[^"]*)!/u;
const lbText = "X" + " wörd".repeat(1000) + "!";
bench("/(?<=X[^\"]*)!/u 5KB x2000", () => lb.test(lbText), 2000, 100);

const small = [
    ["bool", /^(?:true|false|yes|no|on|off)$/i, ["true", "off", "maybe", "YES"]],
    ["http-method", /^(?:GET|POST|PUT|HEAD|DELETE|OPTIONS|PATCH) /, ["GET /", "PATCH /x", "BREW /", "POST /a"]],
    ["ext", /\.(?:js|ts|jsx|tsx|mjs|cjs)$/, ["a.js", "b.tsx", "c.txt", "d.cjs"]],
    ["month-i", /(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) \d+/i, ["jan 5", "Oct 31", "Foo 1", "DEC 25"]],
    ["semver-tag", /-(?:alpha|beta|rc|canary|next|dev)\.?\d*$/, ["1.0.0-alpha.1", "2.0.0", "3.1.0-rc2", "1.2.3-dev"]],
    ["log-level", /\b(?:TRACE|DEBUG|INFO|WARN|ERROR|FATAL)\b/, ["[INFO] x", "warn", "xx ERROR yy", "none"]],
    ["tokens/g", /(?:=>|==|!=|<=|>=|&&|\|\||\+\+|--)/g, ["a => b == c", "x++ && y--", "plain", "a<=b>=c"]],
    ["unit", /^(-?[\d.]+)(?:px|em|rem|vh|vw|%)$/, ["10px", "-1.5rem", "12pt", "100%"]],
    ["alt8", /ab|cd|ef|gh|ij|kl|mn|op/, ["xxop", "zzzz", "abcd", "mnop"]],
];
for (const [name, re, subs] of small)
    bench("small:" + name + " x2M", (i) => { re.lastIndex = 0; return re.test(subs[i & 3]); }, 2000000, 20000);

// Wide alternation with shared prefixes (factoring + dispatch), exec with captures.
const words = []; for (let i = 0; i < 120; i++) words.push("pre" + (i % 7) + "fix" + i.toString(36) + "tail");
const wide = new RegExp("(" + words.join("|") + ")=(\\d+)");
const wideText = "zzz ".repeat(200) + words[93] + "=42";
bench("120-alt shared-prefix exec x20k", () => wide.exec(wideText) !== null, 20000, 1000);

// Boyer-Moore: rare literal near the end of a long subject, 8-bit and 16-bit.
const hay8 = "the quick brown fox jumps over the lazy dog ".repeat(3000) + "NEEDLE42";
const hay16 = "thé quick brown fox jumps over the lazy dog ".repeat(3000) + "NEEDLE42";
bench("BM /NEEDLE\\d+/ 130KB 8-bit x300", () => /NEEDLE\d+/.test(hay8), 300, 30);
bench("BM /NEEDLE\\d+/ 130KB 16-bit x300", () => /NEEDLE\d+/.test(hay16), 300, 30);
bench("BM /zalpha|qbravo|xcharlie/ 130KB x300", () => /zalpha|qbravo|xcharlie/.test(hay8), 300, 30);
