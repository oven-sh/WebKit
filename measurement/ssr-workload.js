// App-shaped workload: React server rendering of a component tree with mixed prop shapes.
// Usage: bun ssr-workload.js [renders]
const React = require("react");
const { renderToString } = require("react-dom/server");
const h = React.createElement;
const renders = parseInt(process.argv[2] || "300", 10);

function Badge({ kind, label, count }) {
  return h("span", { className: "badge badge-" + kind, title: label }, label, count === undefined ? null : h("b", null, count));
}
function Avatar({ user }) {
  return h("img", { src: user.avatar || "/default.png", alt: user.name, width: 32, height: 32 });
}
function Comment({ comment, depth }) {
  return h(
    "li",
    { className: "comment depth-" + depth },
    h(Avatar, { user: comment.author }),
    h("p", null, comment.text),
    comment.tags ? h("div", null, comment.tags.map((t, i) => h(Badge, { key: i, kind: t.kind, label: t.label, count: t.count }))) : null,
    comment.replies && comment.replies.length ? h("ul", null, comment.replies.map(r => h(Comment, { key: r.id, comment: r, depth: depth + 1 }))) : null,
  );
}
function Post({ post }) {
  return h(
    "article",
    { id: "post-" + post.id },
    h("h2", null, post.title),
    h("div", { className: "meta" }, h(Avatar, { user: post.author }), h("time", { dateTime: post.date }, post.date)),
    h("section", null, post.body.map((block, i) => renderBlock(block, i))),
    h("ul", null, post.comments.map(c => h(Comment, { key: c.id, comment: c, depth: 0 }))),
  );
}
function renderBlock(block, key) {
  switch (block.type) {
    case "paragraph":
      return h("p", { key }, block.text);
    case "heading":
      return h("h" + block.level, { key }, block.text);
    case "code":
      return h("pre", { key }, h("code", { className: "lang-" + block.lang }, block.source));
    case "list":
      return h(block.ordered ? "ol" : "ul", { key }, block.items.map((item, i) => h("li", { key: i }, item)));
    case "image":
      return h("figure", { key }, h("img", { src: block.src, alt: block.alt }), block.caption ? h("figcaption", null, block.caption) : null);
    case "quote":
      return h("blockquote", { key, cite: block.cite }, block.text);
    default:
      return null;
  }
}
function App({ posts, user }) {
  return h(
    "html",
    null,
    h("head", null, h("title", null, "Feed")),
    h("body", null, h("header", null, user ? h(Avatar, { user }) : h("a", { href: "/login" }, "Log in")), posts.map(p => h(Post, { key: p.id, post: p }))),
  );
}

let seed = 12345;
const rand = n => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) % n;
const users = Array.from({ length: 20 }, (_, i) => (i % 3 ? { name: "user" + i, avatar: "/a/" + i + ".png" } : i % 2 ? { name: "user" + i } : { id: i, name: "user" + i, admin: true }));
function makeComment(id, depth) {
  const c = { id, author: users[rand(users.length)], text: "comment " + id };
  if (rand(3) === 0) c.tags = Array.from({ length: 1 + rand(3) }, (_, i) => (rand(2) ? { kind: "info", label: "tag" + i } : { kind: "warn", label: "tag" + i, count: rand(9) }));
  if (depth < 3 && rand(2)) c.replies = Array.from({ length: 1 + rand(3) }, (_, i) => makeComment(id * 10 + i, depth + 1));
  return c;
}
function makeBlock(i) {
  switch (rand(6)) {
    case 0:
      return { type: "paragraph", text: "paragraph " + i };
    case 1:
      return { type: "heading", level: 2 + rand(3), text: "heading " + i };
    case 2:
      return { type: "code", lang: "js", source: "let x = " + i + ";" };
    case 3:
      return { type: "list", ordered: !!rand(2), items: ["a", "b", "c"] };
    case 4:
      return rand(2) ? { type: "image", src: "/i/" + i, alt: "image" } : { type: "image", src: "/i/" + i, alt: "image", caption: "caption " + i };
    default:
      return { type: "quote", text: "quote " + i, cite: "/q/" + i };
  }
}
const posts = Array.from({ length: 30 }, (_, i) => ({
  id: i,
  title: "Post " + i,
  author: users[rand(users.length)],
  date: "2026-01-" + (1 + (i % 28)),
  body: Array.from({ length: 5 + rand(10) }, (_, j) => makeBlock(j)),
  comments: Array.from({ length: rand(6) }, (_, j) => makeComment(i * 100 + j, 0)),
}));

let bytes = 0;
const start = performance.now();
for (let i = 0; i < renders; ++i) bytes += renderToString(h(App, { posts, user: i % 2 ? users[i % users.length] : null })).length;
console.log(renders + " renders, " + bytes + " bytes, " + (performance.now() - start).toFixed(0) + " ms");
