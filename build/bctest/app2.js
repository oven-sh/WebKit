const fs = require("node:fs");
const stream = require("node:stream");
const path = require("node:path");
const util = require("node:util");
console.log(JSON.stringify({
  fs: typeof fs.readFileSync,
  stream: typeof stream.Readable,
  path: typeof path.join,
  util: typeof util.inspect,
}));
