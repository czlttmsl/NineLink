// NineLink error-report relay (Cloudflare Worker).
//
// POST /report  JSON from the launcher (see src/launcher/ErrorReporter.cs)
//   -> one GitHub issue per error signature in a PRIVATE repo (REPORTS_REPO),
//      later reports of the same signature become comments on that issue.
// The GitHub token lives only here (wrangler secret GITHUB_TOKEN), never in the launcher.

const MAX_BODY = 32 * 1024;
const MAX_COMMENTS_PER_ISSUE = 30; // after this, only the counter in the title goes up

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/health") return new Response("ok");
    if (url.pathname !== "/report" || request.method !== "POST")
      return new Response("not found", { status: 404 });

    // Per-IP rate limit (Cloudflare rate limiting binding, see wrangler.toml).
    const ip = request.headers.get("CF-Connecting-IP") || "unknown";
    if (env.LIMITER) {
      const { success } = await env.LIMITER.limit({ key: ip });
      if (!success) return json({ error: "rate limited" }, 429);
    }

    const raw = await request.text();
    if (raw.length > MAX_BODY) return json({ error: "too large" }, 413);
    let r;
    try { r = JSON.parse(raw); } catch { return json({ error: "bad json" }, 400); }
    const err = validate(r);
    if (err) return json({ error: err }, 400);

    const gh = github(env);
    const sig = await sha8(r.signature);
    const marker = `nl-sig:${sig}`;

    // Existing open issue for this signature?
    const q = encodeURIComponent(`repo:${env.REPORTS_REPO} is:issue is:open in:body "${marker}"`);
    const found = await gh("GET", `/search/issues?q=${q}&per_page=1`);
    const issue = found.items && found.items[0];

    if (!issue) {
      const created = await gh("POST", `/repos/${env.REPORTS_REPO}/issues`, {
        title: `[x1] ${clip(r.headline, 120)}`,
        labels: ["auto-report"],
        body: issueBody(r, marker),
      });
      return json({ ok: true, issue: created.number, new: true });
    }

    // Bump the counter in the title: "[x3] ..." -> "[x4] ..."
    const m = /^\[x(\d+)\]\s*/.exec(issue.title);
    const count = (m ? +m[1] : 1) + 1;
    const title = `[x${count}] ${issue.title.replace(/^\[x\d+\]\s*/, "")}`;
    await gh("PATCH", `/repos/${env.REPORTS_REPO}/issues/${issue.number}`, { title });
    if (issue.comments < MAX_COMMENTS_PER_ISSUE)
      await gh("POST", `/repos/${env.REPORTS_REPO}/issues/${issue.number}/comments`, { body: reportBlock(r) });
    return json({ ok: true, issue: issue.number, new: false });
  },
};

function validate(r) {
  if (!r || typeof r !== "object") return "not an object";
  for (const k of ["version", "signature", "headline"])
    if (typeof r[k] !== "string" || !r[k] || r[k].length > 400) return `bad ${k}`;
  if (!/^\d+\.\d+\.\d+$/.test(r.version)) return "bad version";
  if (!Array.isArray(r.errors) || r.errors.length === 0 || r.errors.length > 50) return "bad errors";
  if (r.errors.some((e) => typeof e !== "string" || e.length > 500)) return "bad error line";
  if (r.context !== undefined && (typeof r.context !== "string" || r.context.length > 16000)) return "bad context";
  return null;
}

function issueBody(r, marker) {
  return [
    `自动错误报告（NineLink 启动器）。同一类错误再次出现时会追加评论，标题里的 [xN] 是出现次数。`,
    ``,
    `**错误特征**：\`${clip(r.signature, 300)}\``,
    ``,
    reportBlock(r),
    ``,
    `<!-- ${marker} -->`,
  ].join("\n");
}

function reportBlock(r) {
  const meta = [
    `模组版本 ${r.version}`,
    r.gameBuild ? `游戏构建 ${clip(String(r.gameBuild), 40)}` : null,
    r.os ? `系统 ${clip(String(r.os), 60)}` : null,
    r.logName ? `日志 ${clip(String(r.logName), 60)}` : null,
  ].filter(Boolean).join(" · ");
  return [
    meta,
    "",
    "错误行：",
    "```",
    r.errors.map((e) => clip(e, 500)).join("\n").replaceAll("```", "'''"),
    "```",
    r.context ? "<details><summary>上下文日志</summary>\n\n```\n" + r.context.replaceAll("```", "'''") + "\n```\n</details>" : "",
  ].join("\n");
}

function github(env) {
  return async (method, path, body) => {
    const res = await fetch("https://api.github.com" + path, {
      method,
      headers: {
        Authorization: `Bearer ${env.GITHUB_TOKEN}`,
        Accept: "application/vnd.github+json",
        "User-Agent": "ninelink-reporter",
        "X-GitHub-Api-Version": "2022-11-28",
        ...(body ? { "Content-Type": "application/json" } : {}),
      },
      body: body ? JSON.stringify(body) : undefined,
    });
    if (!res.ok) throw new Error(`GitHub ${method} ${path}: ${res.status} ${await res.text()}`);
    return res.json();
  };
}

async function sha8(s) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return [...new Uint8Array(d)].slice(0, 4).map((b) => b.toString(16).padStart(2, "0")).join("");
}

const clip = (s, n) => (s.length > n ? s.slice(0, n) + "…" : s);
const json = (o, status = 200) =>
  new Response(JSON.stringify(o), { status, headers: { "Content-Type": "application/json" } });
