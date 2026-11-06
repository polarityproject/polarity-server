// Tests for the rooms API Worker without Cloudflare: Node 18+ (fetch's
// Request and Response), a stand-in R2 bucket, no cache.
//   node test.mjs
import assert from "node:assert/strict";
import worker from "./src/worker.js";

const store = new Map();
const env = {
  PUSH_SECRET: "s3cret",
  ROOMS: {
    async put(key, value) { store.set(key, value); },
    async get(key) { return store.has(key) ? { text: async () => store.get(key) } : null; },
  },
};
const call = (path, init = {}) => worker.fetch(new Request("https://rooms.example" + path, init), env, { waitUntil() {} });
const push = (body, secret = "s3cret") =>
  call("/v1/push", { method: "POST", headers: { Authorization: "Bearer " + secret }, body: typeof body === "string" ? body : JSON.stringify(body) });

const doc = {
  version: 1,
  updated: Date.now() - 5000,
  totals: { lobby: 3, rooms: 2, public_rooms: 1, players: 5, tables: 3, tables_playing: 1 },
  rooms: [{ code: "ABC123", preset: "Casual sets", owner: "Laptop", members: 2, capacity: 16, tables: 2, open_seats: 1, rules: "r1 ft2 bo3 t99 ws3", faces: ["RYU"] }],
};

let r = await call("/v1/rooms");
assert.equal(r.status, 503, "no list yet");
assert.equal((await push(doc, "wrong")).status, 401, "wrong secret");
assert.equal((await push(doc, "s3cret-and-more")).status, 401, "longer secret");
assert.equal((await push("not json")).status, 400);
assert.equal((await push({ version: 2, rooms: [] })).status, 400);
assert.equal((await push("x".repeat(600 * 1024))).status, 413);
assert.equal((await push(doc)).status, 204);

r = await call("/v1/rooms");
assert.equal(r.status, 200);
assert.equal(r.headers.get("Access-Control-Allow-Origin"), "*");
assert.match(r.headers.get("Cache-Control"), /max-age=10/);
let body = await r.json();
assert.equal(body.stale, false);
assert.equal(body.rooms[0].code, "ABC123");
assert.equal(body.totals, undefined);
assert.ok(body.age >= 4 && body.age <= 7);

body = await (await call("/v1/summary/")).json();
assert.equal(body.totals.players, 5);
assert.equal(body.rooms, undefined);

await push({ ...doc, updated: Date.now() - 120000 });
assert.equal((await (await call("/v1/rooms")).json()).stale, true, "two minutes old");

assert.equal((await call("/v1/push")).status, 405);
assert.equal((await call("/v1/rooms", { method: "POST", body: "{}" })).status, 405);
assert.equal((await call("/v1/nope")).status, 404);
assert.equal((await call("/v1/rooms", { method: "OPTIONS" })).status, 204);
assert.equal(await (await call("/v1/health")).text(), "ok");
console.log("rooms API: all checks passed");
