/* ============================================================================
   AquaControl — real-time telemetry backend
   ENG SCI 1050 Aquaponics Project · Western University

   A small, dependency-light Node server that turns the AquaControl dashboard
   into a full-stack system:

     - Ingests sensor readings from the ESP32 over plain HTTP
           POST /api/telemetry   {"ph":7.2,"temp":24.1,"level":1048,"tan":8.4}
     - Persists every reading to SQLite (built-in node:sqlite — no native build)
     - Broadcasts each new reading to all dashboards in real time over WebSocket
     - Serves historical data for trend charts
           GET  /api/history?hours=24
           GET  /api/latest
     - Serves the dashboard itself (the repo's index.html), so the whole thing
       is a single deploy.

   Run:   npm install && npm start        (defaults to http://localhost:8080)
   Env:   PORT          listen port               (default 8080)
          INGEST_KEY    if set, POST /api/telemetry requires header
                        x-api-key: <INGEST_KEY>   (keeps randos out of your DB)
          DB_PATH       SQLite file path          (default ./aquacontrol.db)
   ============================================================================ */

import express from 'express';
import { WebSocketServer } from 'ws';
import { DatabaseSync } from 'node:sqlite';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const PORT       = process.env.PORT || 8080;
const INGEST_KEY = process.env.INGEST_KEY || '';        // optional write auth
const DB_PATH    = process.env.DB_PATH || path.join(__dirname, 'aquacontrol.db');

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------
const db = new DatabaseSync(DB_PATH);
db.exec(`
  CREATE TABLE IF NOT EXISTS readings (
    id     INTEGER PRIMARY KEY AUTOINCREMENT,
    ts     INTEGER NOT NULL,            -- unix ms
    device TEXT,
    ph     REAL,
    temp   REAL,
    level  REAL,
    tan    REAL
  );
  CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings (ts);
`);

const insertStmt = db.prepare(
  `INSERT INTO readings (ts, device, ph, temp, level, tan) VALUES (?, ?, ?, ?, ?, ?)`
);
const latestStmt = db.prepare(`SELECT * FROM readings ORDER BY ts DESC LIMIT 1`);

function numOrNull(v) {
  const n = typeof v === 'string' ? parseFloat(v) : v;
  return typeof n === 'number' && isFinite(n) ? n : null;
}

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------
const app = express();
app.use(express.json({ limit: '16kb' }));

// CORS — let a dashboard hosted elsewhere (e.g. GitHub Pages) read this API.
app.use((req, res, next) => {
  res.header('Access-Control-Allow-Origin', '*');
  res.header('Access-Control-Allow-Headers', 'Content-Type, x-api-key');
  res.header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  if (req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});

app.get('/api/health', (req, res) => {
  const row = db.prepare(`SELECT COUNT(*) AS n FROM readings`).get();
  res.json({ ok: true, readings: row.n, uptime_s: Math.round(process.uptime()) });
});

// --- ingest -----------------------------------------------------------------
app.post('/api/telemetry', (req, res) => {
  if (INGEST_KEY && req.get('x-api-key') !== INGEST_KEY) {
    return res.status(401).json({ ok: false, error: 'bad api key' });
  }
  const b = req.body || {};
  const reading = {
    ts:     Date.now(),
    device: typeof b.device === 'string' ? b.device.slice(0, 64) : null,
    ph:     numOrNull(b.ph),
    temp:   numOrNull(b.temp),
    level:  numOrNull(b.level),
    tan:    numOrNull(b.tan),
  };
  if (reading.ph == null && reading.temp == null && reading.level == null && reading.tan == null) {
    return res.status(400).json({ ok: false, error: 'no numeric fields' });
  }
  insertStmt.run(reading.ts, reading.device, reading.ph, reading.temp, reading.level, reading.tan);
  broadcast(reading);
  res.json({ ok: true });
});

// --- history ----------------------------------------------------------------
app.get('/api/history', (req, res) => {
  const hours = Math.min(Math.max(parseFloat(req.query.hours) || 24, 0.01), 24 * 30);
  const max   = Math.min(Math.max(parseInt(req.query.max)   || 1500, 1), 20000);
  const since = Date.now() - hours * 3600 * 1000;
  const rows = db.prepare(
    `SELECT ts, device, ph, temp, level, tan FROM readings WHERE ts >= ? ORDER BY ts ASC`
  ).all(since);
  // Downsample to <= max points so the chart stays light over long windows.
  const step = Math.ceil(rows.length / max) || 1;
  const out = step > 1 ? rows.filter((_, i) => i % step === 0) : rows;
  res.json({ hours, count: out.length, total: rows.length, readings: out });
});

app.get('/api/latest', (req, res) => {
  const row = latestStmt.get();
  res.json(row || {});
});

// --- static dashboard (serves the repo's index.html, one folder up) ---------
app.use(express.static(path.join(__dirname, '..')));

// ---------------------------------------------------------------------------
// HTTP + WebSocket
// ---------------------------------------------------------------------------
const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: '/ws' });

function broadcast(reading) {
  const msg = JSON.stringify({ type: 'reading', ...reading });
  for (const client of wss.clients) {
    if (client.readyState === 1) client.send(msg);
  }
}

wss.on('connection', (ws) => {
  // Greet the new client with the most recent reading so cards aren't blank.
  const row = latestStmt.get();
  if (row) ws.send(JSON.stringify({ type: 'reading', ...row }));
  ws.send(JSON.stringify({ type: 'hello', server: 'aquacontrol', clients: wss.clients.size }));
});

server.listen(PORT, () => {
  console.log(`AquaControl backend listening on http://localhost:${PORT}`);
  console.log(`  POST  /api/telemetry   ingest readings${INGEST_KEY ? ' (x-api-key required)' : ''}`);
  console.log(`  GET   /api/history     trend data`);
  console.log(`  WS    /ws              live stream`);
  console.log(`  DB    ${DB_PATH}`);
});
