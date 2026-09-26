// bridge/server.js
const express = require('express');

const APPROVAL_TIMEOUT_MS = 25 * 1000;

function createServer(serial) {
    const app = express();
    app.use(express.json());

    let pending = null; // { command, resolve }

    app.post('/approval', (req, res) => {
        const command = req.body.command || '(unknown command)';
        if (pending) { return res.json({ decision: 'ask' }); }
        let settled = false;
        const timer = setTimeout(() => {
            if (settled) return;
            settled = true;
            pending = null;
            res.json({ decision: 'ask' });
            serial.send('SCREEN:HOME');
        }, APPROVAL_TIMEOUT_MS);
        pending = {
            command,
            resolve: (decision) => {
                if (settled) return;
                settled = true;
                clearTimeout(timer);
                pending = null;
                res.json({ decision });
            },
        };
        serial.send(`ALERT:${command}`);
    });

    serial.on('approve', () => {
        if (pending) pending.resolve('allow');
        serial.send('SCREEN:HOME');
    });
    serial.on('deny', () => {
        if (pending) pending.resolve('deny');
        serial.send('SCREEN:HOME');
    });

    return app;
}

module.exports = createServer;