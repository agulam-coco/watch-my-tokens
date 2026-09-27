/**
 * @file bridge/server.js
 * @description Express HTTP server that brokers Claude Code approval requests
 * to the device. The PreToolUse hook POSTs a command to /approval; the request
 * is held open until the user taps (allow) or holds (deny) the device button,
 * or until APPROVAL_TIMEOUT_MS elapses (ask). Each new request is also read
 * aloud with ElevenLabs text-to-speech.
 */
require('dotenv').config();
const express = require('express');
const { spawn } = require('child_process');

/** How long an approval request waits for a button press before answering "ask". */
const APPROVAL_TIMEOUT_MS = 25 * 1000;

/** Running totals of button decisions since the bridge started. */
let approvedCount = 0;
let deniedCount = 0;

/** The afplay child process that is currently speaking, or null when silent. */
let currentAudioProcess = null;
/** Cancels the in-flight ElevenLabs download, or null when none is running. */
let currentAbort = null;
/**
 * Incremented by every stopSpeaking() call. Each speakAlert() remembers the
 * value it started with; if it has changed by the time the audio arrives,
 * that alert was superseded or already answered and must not play.
 */
let speechGeneration = 0;

/**
 * Silences voice alerts: cancels any ElevenLabs download still in flight,
 * kills any audio that is playing, and marks every earlier speakAlert() call
 * as stale so that it never starts playing later.
 *
 * @returns {void}
 */
function stopSpeaking() {
    speechGeneration++;
    if (currentAbort) {
        currentAbort.abort();
        currentAbort = null;
    }
    if (currentAudioProcess) {
        currentAudioProcess.kill('SIGKILL');
        currentAudioProcess = null;
    }
}

/**
 * Converts text to speech with ElevenLabs and plays it through the macOS
 * `afplay` player. Any earlier alert is stopped first. If stopSpeaking() is
 * called while the audio is downloading, the download is cancelled and the
 * clip is never played. Failures (missing API key, network error, non-2xx
 * response) are logged and swallowed so they never block an approval.
 *
 * @param {string} text - The sentence to speak, e.g. "Claude wants to run: Bash: ls".
 * @returns {Promise<void>} Resolves once playback has *started* (not finished),
 *   or once the alert has been abandoned.
 */
async function speakAlert(text) {
    stopSpeaking();
    const myGeneration = speechGeneration;
    const abort = new AbortController();
    currentAbort = abort;

    try {
        const response = await fetch('https://api.elevenlabs.io/v1/text-to-speech/EXAVITQu4vr4xnSDxMaL', {
            method: 'POST',
            headers: { 'xi-api-key': process.env.ELEVENLABS_API_KEY, 'Content-Type': 'application/json' },
            body: JSON.stringify({ text, model_id: 'eleven_turbo_v2_5' }),
            signal: abort.signal,
        });
        if (!response.ok) throw new Error(`ElevenLabs error: ${response.status}`);
        const buffer = Buffer.from(await response.arrayBuffer());

        // A newer alert or a button decision happened while downloading.
        if (myGeneration !== speechGeneration) return;
        currentAbort = null;

        const fs = require('fs');
        const path = require('path');
        const tmpFile = path.join('/tmp', 'agentpager-alert.mp3');
        fs.writeFileSync(tmpFile, buffer);

        const proc = spawn('afplay', [tmpFile]);
        currentAudioProcess = proc;
        proc.on('exit', () => {
            // Only clear the handle if a newer clip hasn't replaced this one.
            if (currentAudioProcess === proc) currentAudioProcess = null;
        });
    } catch (err) {
        if (err.name === 'AbortError') return; // cancelled by stopSpeaking()
        if (currentAbort === abort) currentAbort = null;
        console.error('[elevenlabs] failed:', err.message);
    }
}

/**
 * Builds the Express app and subscribes to the device's button events.
 *
 * Routes:
 *   POST /approval   body { command: string }
 *                    → { decision: "allow" | "deny" | "ask" }
 *                    Holds the request open until a button press or the timeout.
 *                    Only one request can be pending; extras get "ask" at once.
 *   POST /debug/log  body { approved?: number, denied?: number }
 *                    → { sent: true, approved, denied }
 *                    Sends SCREEN:LOG to the device; defaults to the running totals.
 *
 * @param {import('./serial')} serial - An open AgentPagerSerial used to send
 *   screen commands and to receive `approve` / `deny` events.
 * @returns {import('express').Express} The configured app; the caller must `listen()`.
 */
function createServer(serial) {
    const app = express();
    app.use(express.json());

    /**
     * The approval request currently waiting on the device, or null.
     * @type {{ command: string, resolve: (decision: 'allow' | 'deny') => void } | null}
     */
    let pending = null;

    app.post('/approval', (req, res) => {
        const command = req.body.command || '(unknown command)';
        if (pending) { return res.json({ decision: 'ask' }); }

        // `settled` guards against answering the same HTTP request twice when a
        // button press and the timeout race each other.
        let settled = false;
        const timer = setTimeout(() => {
            if (settled) return;
            settled = true;
            pending = null;
            stopSpeaking();
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

    /**
     * Handles a BTN:APPROVE from the device: silences the voice alert, answers
     * the pending request with "allow", bumps the tally and returns the device
     * to the home screen.
     *
     * @returns {void}
     */
    function handleApprove() {
        stopSpeaking();
        if (pending) pending.resolve('allow');
        approvedCount++;
        serial.send('SCREEN:HOME');
    }

    /**
     * Handles a BTN:DENY from the device: silences the voice alert, answers
     * the pending request with "deny", bumps the tally and returns the device
     * to the home screen.
     *
     * @returns {void}
     */
    function handleDeny() {
        stopSpeaking();
        if (pending) pending.resolve('deny');
        deniedCount++;
        serial.send('SCREEN:HOME');
    }

    serial.on('approve', handleApprove);
    serial.on('deny', handleDeny);

    return app;
}

module.exports = createServer;
