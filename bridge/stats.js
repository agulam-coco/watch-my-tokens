/**
 * @file bridge/stats.js
 * @description Tails Claude Code's session transcripts
 * (~/.claude/projects/**\/*.jsonl) and turns the token usage recorded on each
 * assistant message into a small summary for the device: active agents,
 * context fill %, and estimated dollar cost.
 *
 * Accuracy notes (see "Cost & usage accuracy" in the README):
 *  - Every message is priced at Claude Sonnet 5 rates, whatever model the
 *    session actually used (the model is in `entry.message.model`).
 *  - Claude Code writes one transcript line per content block, and each line
 *    repeats the same `message.usage`. Lines are not de-duplicated by
 *    `message.id`, so a message's cost is counted once per content block.
 *  - All cache writes are priced at the 5-minute TTL rate. Claude Code's
 *    writes are usually 1-hour TTL, which costs 2× input rather than 1.25×.
 */
const chokidar = require('chokidar');
const fs = require('fs');
const os = require('os');
const path = require('path');

/** Root directory where Claude Code stores one folder of transcripts per project. */
const PROJECTS_DIR = path.join(os.homedir(), '.claude', 'projects');
/** A session counts as active if it produced an assistant message within this window. */
const ACTIVE_WINDOW_MS = 60 * 1000;
/**
 * Token count treated as "100 %" on the device's ring. Claude Sonnet 5 (and
 * Opus 5.5 / Fable 5.1) actually have a 1,000,000-token context window;
 * 200,000 is kept as the display scale, so the ring fills 5× faster than
 * the real window.
 */
const SONNET_CONTEXT_WINDOW = 200000;

// Claude Sonnet 5 first-party API pricing, in USD per token (the published
// rate per million tokens divided by 1,000,000).
/** Uncached input: $2.00 / MTok. */
const PRICE_INPUT = 2.00 / 1_000_000;
/** Output: $10.00 / MTok. */
const PRICE_OUTPUT = 10.00 / 1_000_000;
/** Cache write, 5-minute TTL: 1.25 × input = $2.50 / MTok. (1-hour TTL is 2 × input = $4.00 / MTok.) */
const PRICE_CACHE_WRITE = 2.50 / 1_000_000;
/** Cache read: 0.1 × input = $0.20 / MTok. */
const PRICE_CACHE_READ = 0.20 / 1_000_000;

/**
 * Tracks per-session cost and context size from Claude Code transcripts.
 */
class StatsTracker {
    constructor() {
        /**
         * Per-session running state, keyed by Claude Code sessionId.
         * @type {Map<string, { costUSD: number, lastSeen: number, lastContextTokens: number }>}
         */
        this.sessions = new Map();
        /**
         * Byte offset already read from each transcript, so only new lines are parsed.
         * @type {Map<string, number>}
         */
        this.fileOffsets = new Map();
    }

    /**
     * Starts watching every transcript. Existing files are read in full once
     * (ignoreInitial: false), then only appended bytes are read on each change.
     *
     * @param {(summary: { agents: number, pct: number, costUSD: number }) => void} onUpdate
     *   Called with a fresh summary after each processed file change and on
     *   every manual `_emitSummary()` call.
     * @returns {void}
     */
    start(onUpdate) {
        this._onUpdate = onUpdate;
        const watcher = chokidar.watch(path.join(PROJECTS_DIR, '**/*.jsonl'), {
            persistent: true,
            ignoreInitial: false,
        });
        watcher.on('add', (filePath) => this._scan(filePath));
        watcher.on('change', (filePath) => this._scan(filePath));
    }

    /**
     * Reads the bytes appended to a transcript since the last scan, processes
     * each complete JSON line, and emits a new summary. If the file shrank
     * (rotated or truncated) it is re-read from the start. Read errors, such
     * as a file deleted mid-read, are ignored.
     *
     * @param {string} filePath - Absolute path to a *.jsonl transcript.
     * @returns {void}
     * @private
     */
    _scan(filePath) {
        try {
            const stat = fs.statSync(filePath);
            let startOffset = this.fileOffsets.get(filePath) || 0;
            if (stat.size < startOffset) startOffset = 0;

            const length = stat.size - startOffset;
            if (length <= 0) {
                this.fileOffsets.set(filePath, stat.size);
                return;
            }

            const fd = fs.openSync(filePath, 'r');
            const buffer = Buffer.alloc(length);
            fs.readSync(fd, buffer, 0, length, startOffset);
            fs.closeSync(fd);
            this.fileOffsets.set(filePath, stat.size);

            const lines = buffer.toString('utf8').split('\n').filter(Boolean);
            for (const line of lines) this._processLine(line);
            this._emitSummary();
        } catch (err) {
            // File may have been deleted or rotated mid-read; skip this change.
        }
    }

    /**
     * Parses one transcript line and, if it is an assistant message with
     * usage data, adds its cost to the session and records its context size.
     * Non-JSON lines and non-assistant entries are ignored.
     *
     * @param {string} line - One raw JSONL line from a transcript.
     * @returns {void}
     * @private
     */
    _processLine(line) {
        let entry;
        try {
            entry = JSON.parse(line);
        } catch (_) {
            return;
        }
        if (entry.type !== 'assistant') return;

        const sessionId = entry.sessionId;
        const usage = entry.message && entry.message.usage;
        if (!sessionId || !usage) return;

        const inputTokens = usage.input_tokens || 0;
        const outputTokens = usage.output_tokens || 0;
        const cacheWriteTokens = usage.cache_creation_input_tokens || 0;
        const cacheReadTokens = usage.cache_read_input_tokens || 0;

        // Estimated cost of this turn at Sonnet 5 rates (see file header for caveats).
        const turnCostUSD =
            inputTokens * PRICE_INPUT +
            outputTokens * PRICE_OUTPUT +
            cacheWriteTokens * PRICE_CACHE_WRITE +
            cacheReadTokens * PRICE_CACHE_READ;

        // Size of the prompt for this turn, i.e. how much of the context window
        // the session is using right now. This is a snapshot, not a running total.
        const contextTokens = inputTokens + cacheWriteTokens + cacheReadTokens;

        const existing = this.sessions.get(sessionId) || { costUSD: 0, lastSeen: 0, lastContextTokens: 0 };
        existing.costUSD += turnCostUSD;
        existing.lastSeen = Date.now();
        existing.lastContextTokens = contextTokens;
        this.sessions.set(sessionId, existing);
    }

    /**
     * Computes the summary over active sessions (seen within ACTIVE_WINDOW_MS)
     * and passes it to the `onUpdate` callback given to `start()`.
     *
     *  - agents:  number of active sessions.
     *  - pct:     context fill of the busiest active session, as a whole
     *             percentage of SONNET_CONTEXT_WINDOW, capped at 100.
     *  - costUSD: summed estimated cost of the active sessions since the bridge
     *             started (not a calendar-day total).
     *
     * @returns {void}
     */
    _emitSummary() {
        const now = Date.now();
        const active = [...this.sessions.values()].filter((s) => now - s.lastSeen < ACTIVE_WINDOW_MS);

        const agents = active.length;
        const totalCostUSD = active.reduce((sum, s) => sum + s.costUSD, 0);

        const busiestContextTokens = active.reduce((max, s) => Math.max(max, s.lastContextTokens), 0);
        const pct = Math.min(100, Math.round((busiestContextTokens / SONNET_CONTEXT_WINDOW) * 100));

        this._onUpdate({ agents, pct, costUSD: totalCostUSD });
    }
}

module.exports = StatsTracker;
