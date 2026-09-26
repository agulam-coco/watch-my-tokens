// bridge/server.js
const express = require('express');

const APPROVAL_TIMEOUT_MS = 25 * 1000;

function createServer(serial) {
    const app = express();
    app.use(express.json());

    let pending = null; // { command, resolve }

    app.post('/approval', (req, res) => {
        const command = req.body.command || '(unknown command)';

        if (pending) {
            // Already have one in flight — reject the new one immediately
            // rather than silently overwriting it.
            return res.json({ decision: 'ask' });
        }

        let settled = false;
        const timer = setTimeout(() => {
            if (settled) return;
            settled = true;
            pending = null;
            res.json({ decision: 'ask' }); // no tap in time -> fall back to terminal
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
    });

    serial.on('deny', () => {
        if (pending) pending.resolve('deny');
    });

    return app;
}

module.exports = createServer;