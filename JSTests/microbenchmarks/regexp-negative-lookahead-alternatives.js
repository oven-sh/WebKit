let paths = [
    "docs/guides/getting-started/installation",
    "packages/runtime/src/internal/streams/readable",
    "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0 and more",
    "releases/download/v1/archive_linux_x64_baseline",
];
let re = /^(?:(?![ \xa0]|[\0-\x1f\x7f-\x9f]|[><|]|[()[\]{}.,"'?!;]).)+/;
let result = 0;
for (let i = 0; i < 1e5; ++i) {
    for (let path of paths)
        result += re.exec(path)[0].length;
}
if (result !== 173 * 1e5)
    throw new Error("Bad result: " + result);
