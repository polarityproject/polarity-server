// SFVR rooms API (Cloudflare Worker): the public room list the rooms VPS's
// publisher pushes (docker/publisher), served to sites, bots and overlays
// from Cloudflare, so readers never reach the VPS.
//
//   POST /v1/push     the publisher only (Authorization: Bearer <PUSH_SECRET>):
//                     the keeper's status document, stored in R2
//   GET  /v1/rooms    {version, updated, age, stale, attribution, rooms: [...]}
//   GET  /v1/summary  {version, updated, age, stale, totals: {online, in_public_rooms, public_rooms}}
//   GET  /v1/health   "ok"
//
// A room: {code, title, title_id, title_characters, owner, sfvr_version, members,
// capacity, tables, open_seats, rules, faces, players: [{name, country,
// face}]}. 	itle: the English of SFV's Battle Lounge comment the owner
// picked (null: none, show the owner's name). country is the rooms
// server's lookup of the player's address in DB-IP's free database (null
// when unknown); whoever shows it credits DB-IP (ttribution, CC BY 4.0). `stale`: the list is older than 60 s (the VPS, the keeper
// or the publisher is down); readers should say so rather than show it as
// live. Any site may read it (CORS *); answers are cached 10 s. The full
// specification: docs/rooms-api.md.

const OBJECT = "status.json";
const MAX_BYTES = 512 * 1024;
const STALE_MS = 60_000;
const CACHE_SECONDS = 10;
// DB-IP's licence (CC BY 4.0) for the players' countries: shown with them.
const ATTRIBUTION = "IP Geolocation by DB-IP (https://db-ip.com), CC BY 4.0";

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type",
  "Access-Control-Max-Age": "86400",
};

function json(body, status = 200, extra = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8", ...CORS, ...extra },
  });
}

// Constant-time comparison of two strings (the push secret).
function sameSecret(a, b) {
  const x = new TextEncoder().encode(a);
  const y = new TextEncoder().encode(b);
  let diff = x.length ^ y.length;
  for (let i = 0; i < Math.max(x.length, y.length); i++) diff |= (x[i] ?? 0) ^ (y[i] ?? 0);
  return diff === 0;
}

function validDocument(doc) {
  if (!doc || typeof doc !== "object" || doc.version !== 1) return false;
  if (!Number.isFinite(doc.updated) || !doc.totals || typeof doc.totals !== "object") return false;
  if (!Array.isArray(doc.rooms) || doc.rooms.length > 2000) return false;
  return doc.rooms.every(
    (r) => r && typeof r.code === "string" && r.code.length <= 16 && typeof r.owner === "string" && Number.isFinite(r.members),
  );
}

async function handlePush(request, env) {
  const auth = request.headers.get("Authorization") || "";
  if (!env.PUSH_SECRET || !sameSecret(auth, "Bearer " + env.PUSH_SECRET)) return json({ error: "unauthorized" }, 401);
  const length = Number(request.headers.get("Content-Length") || 0);
  if (length > MAX_BYTES) return json({ error: "too large" }, 413);
  const text = await request.text();
  if (text.length > MAX_BYTES) return json({ error: "too large" }, 413);
  let doc;
  try {
    doc = JSON.parse(text);
  } catch {
    return json({ error: "not JSON" }, 400);
  }
  if (!validDocument(doc)) return json({ error: "not a room list" }, 400);
  await env.ROOMS.put(OBJECT, JSON.stringify(doc), {
    httpMetadata: { contentType: "application/json" },
    customMetadata: { received: String(Date.now()) },
  });
  return new Response(null, { status: 204 });
}

async function readDocument(env) {
  const object = await env.ROOMS.get(OBJECT);
  if (!object) return null;
  try {
    return JSON.parse(await object.text());
  } catch {
    return null;
  }
}

async function handleRead(path, env, now) {
  const doc = await readDocument(env);
  if (!doc) return json({ error: "no room list yet" }, 503, { "Cache-Control": "no-store" });
  const age = Math.max(0, now - doc.updated);
  const head = { version: 1, updated: doc.updated, age: Math.round(age / 1000), stale: age > STALE_MS };
  const body = path === "/v1/rooms" ? { ...head, attribution: ATTRIBUTION, rooms: doc.rooms } : { ...head, totals: doc.totals };
  return json(body, 200, { "Cache-Control": `public, max-age=${CACHE_SECONDS}` });
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const path = url.pathname.replace(/\/+$/, "") || "/";
    if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });
    if (path === "/v1/push") {
      if (request.method !== "POST") return json({ error: "POST only" }, 405);
      return handlePush(request, env);
    }
    if (request.method !== "GET" && request.method !== "HEAD") return json({ error: "GET only" }, 405);
    if (path === "/v1/health") return new Response("ok", { headers: { "Content-Type": "text/plain", ...CORS } });
    if (path !== "/v1/rooms" && path !== "/v1/summary") {
      return json({ error: "not found", see: ["/v1/rooms", "/v1/summary"] }, 404);
    }
    // Cloudflare's cache in front of R2 (per data centre, 10 s): many
    // readers, one R2 read.
    const cache = typeof caches !== "undefined" ? caches.default : null;
    const key = new Request(url.origin + path, { method: "GET" });
    if (cache) {
      const hit = await cache.match(key);
      if (hit) return hit;
    }
    const response = await handleRead(path, env, Date.now());
    if (cache && response.status === 200 && ctx && ctx.waitUntil) ctx.waitUntil(cache.put(key, response.clone()));
    return response;
  },
};
