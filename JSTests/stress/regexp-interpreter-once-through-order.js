//@ runDefault("--useRegExpJIT=true")
//@ runNoJIT("--useRegExpJIT=false")

function same(a, b) {
  if (Array.isArray(a) || Array.isArray(b))
    return Array.isArray(a) && Array.isArray(b) && a.length === b.length && a.every((v, k) => same(v, b[k]));
  return Object.is(a, b);
}

function show(v) {
  return JSON.stringify(v, (k, x) => (x === undefined ? "<undefined>" : x));
}

function t(re, s, expected, lastIndex) {
  if (lastIndex !== undefined)
    re.lastIndex = lastIndex;
  let m = re.exec(s);
  let actual = m ? [m.index, [...m]] : null;
  if (!same(actual, expected))
    throw new Error(re + " on " + show(s) + (lastIndex === undefined ? "" : " at " + lastIndex) + ": expected " + show(expected) + " but got " + show(actual));
}

t(/a|^x|aa/, "caa", [1, ["a"]]);
t(/(?:^|[^g])x6|ar?/, "gax6", [1, ["ax6"]]);
t(/(^|[^g])hz+|w/, "uwhz", [1, ["whz", "w"]]);
t(/(?:^|[^g])a|a|1/i, "gaA", [1, ["aA"]]);
t(/a(?<!q)|^x|aa/, "caa", [1, ["a"]]);
t(/^x|^y|a|aa/, "caa", [1, ["a"]]);
t(/^x|^y|^z|aa|a/, "caa", [1, ["aa"]]);
t(/^x|b|^y|a/, "cab", [1, ["a"]]);
t(/^c|a$|aa/, "caa", [0, ["c"]]);
t(/^q|a$|aa/, "caa", [1, ["aa"]]);
t(/^q|^r|a$|aa/, "xcaa", [2, ["aa"]]);
t(/^q|aa|^r|a/, "xcaa", [2, ["aa"]]);
t(/(?:^a|^b|c)|cc/, "xcc", [1, ["c"]]);
t(/^a|^b|c|cc/m, "x\ncc", [2, ["c"]]);
t(/^a|^b|cc|c/m, "x\ncc", [2, ["cc"]]);
t(/^x|^y|(?<=a)a|aa/, "caa", [1, ["aa"]]);
t(/a|^x|^y|aa/, "caa", [1, ["a"]]);
t(/^x|a|^y|aa/g, "caacaa", [2, ["a"]], 2);
t(/^x|^y|a|aa/y, "caa", [1, ["a"]], 1);
t(/^x|^y|a|aa/y, "caa", null, 0);
t(/^a|^b/, "xab", null);
t(/^a|^b/, "ab", [0, ["a"]]);
t(/^a|^b/, "b", [0, ["b"]]);
t(/^abc|^abd/, "xabd", null);
t(/^a|b|^c|bb/g, "abbcbb", [0, ["a"]], 0);
t(/^a|b|^c|bb/g, "abbcbb", [1, ["b"]], 1);
t(/^a|b|^c|bb/g, "abbcbb", [2, ["b"]], 2);
t(/^a|b|^c|bb/g, "abbcbb", [4, ["b"]], 3);
t(/^a|b|^c|bb/g, "abbcbb", [4, ["b"]], 4);
t(/^a|b|^c|bb/g, "abbcbb", [5, ["b"]], 5);
t(/^a|b|^c|bb/g, "abbcbb", null, 6);
t(/^a|b|^c|bb/y, "abbcbb", [0, ["a"]], 0);
t(/^a|b|^c|bb/y, "abbcbb", [1, ["b"]], 1);
t(/^a|b|^c|bb/y, "abbcbb", [2, ["b"]], 2);
t(/^a|b|^c|bb/y, "abbcbb", null, 3);
t(/^a|b|^c|bb/y, "abbcbb", [4, ["b"]], 4);
t(/^a|b|^c|bb/y, "abbcbb", [5, ["b"]], 5);
t(/^a|b|^c|bb/y, "abbcbb", null, 6);
t(/^x|^y|c|cc/, "", null);
t(/^x|^y/, "", null);
t(/^|a/, "ba", [0, [""]]);
t(/a|^/, "ba", [0, [""]]);
t(/^$|^a|b$|b/, "abb", [0, ["a"]]);
t(/(?:^-|^\+)?d|dd/, "xdd", [1, ["d"]]);
t(/^\d|^-|(a)|(aa)/, "baa", [1, ["a", "a", undefined]]);
t(/^b|^a|(?=a)a|aa/, "baa", [0, ["b"]]);
t(/^\b|\ba|aa/, "b aa", [0, [""]]);
