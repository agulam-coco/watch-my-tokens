const AgentPagerSerial = require('./serial');
const StatsTracker = require('./stats');
const createServer = require('./server');

const PORT_PATH = '/dev/tty.usbmodem101';
const HTTP_PORT = 4545;

const serial = new AgentPagerSerial(PORT_PATH);

serial.on('open', () => console.log('[serial] connected on', PORT_PATH));
serial.on('error', (err) => console.error('[serial] error:', err.message));
serial.on('close', () => console.log('[serial] port closed'));
serial.on('line', (line) => console.log('[serial] <-', line));
serial.on('approve', () => console.log('[serial] >> APPROVE received'));
serial.on('deny', () => console.log('[serial] >> DENY received'));

const stats = new StatsTracker();
stats.start(({ agents, pct, costUSD }) => {
    console.log(`[stats] agents=${agents} pct=${pct}% cost=$${costUSD.toFixed(2)}`);
    serial.send(`STATS:${agents}:${pct}:${costUSD.toFixed(2)}`);
});

setInterval(() => {
    stats._emitSummary();
}, 10000);

const app = createServer(serial);
app.listen(HTTP_PORT, () => {
    console.log(`[http] approval server listening on :${HTTP_PORT}`);
});