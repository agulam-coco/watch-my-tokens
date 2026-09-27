/**
 * @file bridge/index.js
 * @description Entry point for the bridge (`npm start`). Opens the serial
 * connection to the device, streams Claude Code usage stats to it, and starts
 * the HTTP approval server used by hooks/pretooluse.sh.
 */
const AgentPagerSerial = require('./serial');
const StatsTracker = require('./stats');
const createServer = require('./server');

/** Serial device path for the board. Check with `ls /dev/tty.usbmodem*`. */
const PORT_PATH = '/dev/tty.usbmodem101';
/** Port the approval server listens on; must match BRIDGE_URL in the hook. */
const HTTP_PORT = 4545;

const serial = new AgentPagerSerial(PORT_PATH);

serial.on('open', () => console.log('[serial] connected on', PORT_PATH));
serial.on('error', (err) => console.error('[serial] error:', err.message));
serial.on('close', () => console.log('[serial] port closed'));
serial.on('line', (line) => console.log('[serial] <-', line));
serial.on('approve', () => console.log('[serial] >> APPROVE received'));
serial.on('deny', () => console.log('[serial] >> DENY received'));

// Forward every usage summary to the device as STATS:<agents>:<pct>:<cost>.
const stats = new StatsTracker();
stats.start(({ agents, pct, costUSD }) => {
    console.log(`[stats] agents=${agents} pct=${pct}% cost=$${costUSD.toFixed(2)}`);
    serial.send(`STATS:${agents}:${pct}:${costUSD.toFixed(2)}`);
});

// Re-emit the summary every 10 s even when no transcript changes, so sessions
// that go idle (see ACTIVE_WINDOW_MS in stats.js) drop off the display.
setInterval(() => {
    stats._emitSummary();
}, 10000);

const app = createServer(serial);
app.listen(HTTP_PORT, () => {
    console.log(`[http] approval server listening on :${HTTP_PORT}`);
});
