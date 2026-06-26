/* Seed the database with ~24h of realistic readings so the history chart has
   something to draw on a fresh deploy / for the demo.
   Run:  npm run seed                                                           */
import { DatabaseSync } from 'node:sqlite';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const DB_PATH = process.env.DB_PATH || path.join(__dirname, 'aquacontrol.db');

const db = new DatabaseSync(DB_PATH);
db.exec(`CREATE TABLE IF NOT EXISTS readings (
  id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL, device TEXT,
  ph REAL, temp REAL, level REAL, tan REAL);
  CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings (ts);`);

const ins = db.prepare(`INSERT INTO readings (ts, device, ph, temp, level, tan) VALUES (?,?,?,?,?,?)`);

const HOURS = 24, STEP_MS = 60 * 1000;       // one point per minute
const N = (HOURS * 3600 * 1000) / STEP_MS;
const now = Date.now();

let ph = 7.2, temp = 24.1, level = 1048, tan = 8.4;
db.exec('BEGIN');
for (let i = N; i >= 0; i--) {
  const t = now - i * STEP_MS;
  const hourOfDay = new Date(t).getHours() + new Date(t).getMinutes() / 60;
  // gentle daily cycle on temperature + slow drift / evaporation on level
  temp  = 24 + 1.8 * Math.sin((hourOfDay / 24) * 2 * Math.PI) + (Math.random() - 0.5) * 0.3;
  ph    = Math.max(6.6, Math.min(8.3, ph + (Math.random() - 0.5) * 0.05));
  level = level - 0.012 + (Math.random() - 0.5) * 0.4;          // slow drop, top-ups
  if (level < 1010) level = 1058;                               // periodic top-up
  tan   = Math.max(6, Math.min(10.2, tan + (Math.random() - 0.5) * 0.08));
  ins.run(t, 'seed', +ph.toFixed(2), +temp.toFixed(2), Math.round(level), +tan.toFixed(2));
}
db.exec('COMMIT');

const n = db.prepare('SELECT COUNT(*) AS n FROM readings').get().n;
console.log(`Seeded. Database now holds ${n} readings (~${HOURS}h).`);
