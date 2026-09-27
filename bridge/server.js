// bridge/server.js
require('dotenv').config();
const express = require('express');
const { exec } = require('child_process');

const APPROVAL_TIMEOUT_MS = 25 * 1000;
let approvedCount = 0;
let deniedCount = 0;

async function speakAlert(text) {
    try {
        const response = await fetch('https://api.elevenlabs.io/v1/text-to-speech/EXAVITQu4vr4xnSDxMaL', {
            method: 'POST',
            headers: {
                'xi-api-key': process.env.ELEVENLABS_API_KEY,
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({
                text,
                model_id: 'eleven_turbo_v2',
            }),
        });
        if (!response.ok) throw new Error(`ElevenLabs error: ${response.status}`);
        const buffer = Buffer.from(await response.arrayBuffer());
        const fs = require('fs');
        const path = require('path');
        const tmpFile = path.join('/tmp', 'agentpager-alert.mp3');
        fs.writeFileSync(tmpFile, buffer);
        exec(`afplay ${tmpFile}`); // macOS built-in player, since you're on a Mac
    } catch (err) {
        console.error('[elevenlabs] failed:', err.message);
    }
}

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
        speakAlert(`Claude wants to run: ${command}`);
        serial.send(`ALERT:${command}`);
    });
    app.post('/debug/log', (req, res) => {
        const { approved = approvedCount, denied = deniedCount } = req.body;
        serial.send(`SCREEN:LOG:${approved}:${denied}`);
        res.json({ sent: true, approved, denied });
    });

    function handleApprove() {
        if (pending) pending.resolve('allow');
        approvedCount++;
        serial.send('SCREEN:HOME');
    }

    function handleDeny() {
        if (pending) pending.resolve('deny');
        deniedCount++;
        serial.send('SCREEN:HOME');
    }

    serial.on('approve', handleApprove);
    serial.on('deny', handleDeny);

    return app;
}

module.exports = createServer;