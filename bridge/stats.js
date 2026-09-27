/**
 * @file bridge/stats.js
 * @description Tails Claude Code's session transcripts
 * (~/.claude/projects/**\/*.jsonl) and turns the token usage recorded on each
 * assistant message into a small summary for the device: active agents,
 * context fill %, and today's estimated dollar cost.
 *
 * How cost is estimated:
 *  - Claude Code writes one transcript line per content block, and each line
 *    repeats the same `message.usage`. Cost is therefore counted once per
 *    `message.id`; a later line for the same id replaces the earlier amount.
 *  - Each message is priced with the rates for its own `message.model`.
 *  - Cache writes are split by TTL using `usage.cache_creation`
 *    (5-minute writes cost 1.25 × input, 1-hour writes cost 2 × input).
 *  - "Today" is the local calendar day of this computer, based on each
 *    entry's `timestamp`.
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

/**
 * Claude API first-party prices in USD per million tokens (MTok).
 * Keys are model-id prefixes, so dated ids such as
 * "claude-haiku-4-5-20251001" match "claude-haiku-4-5".
 * Fast mode, Batch and Bedrock/Vertex pricing are not modelled.
 *
 * @type {Object<string, { input: number, output: number, cacheRead: number }>}
 */
const PRICING_PER_MTOK = {
    'claude-fable-5-1':  { input: 10.00, output: 50.00, cacheRead: 0.25 },
    'claude-mythos-5-1': { input: 10.00, output: 50.00, cacheRead: 0.25 },
    'claude-fable-5':    { input: 10.00, output: 50.00, cacheRead: 1.00 },
    'claude-mythos-5':   { input: 10.00, output: 50.00, cacheRead: 1.00 },
    'claude-opus-5-5':   { input:  4.00, output: 20.00, cacheRead: 0.20 },
    'claude-opus-5':     { input:  5.00, output: 25.00, cacheRead: 0.50 },
    'claude-opus-4-8':   { input:  5.00, output: 25.00, cacheRead: 0.50 },
    'claude-opus-4-7':   { input:  5.00, output: 25.00, cacheRead: 0.50 },
    'claude-opus-4-6':   { input:  5.00, output: 25.00, cacheRead: 0.50 },
    'claude-sonnet-5':   { input:  2.00, output: 10.00, cacheRead: 0.20 },
    'claude-sonnet-4-6': { input:  3.00, output: 15.00, cacheRead: 0.30 },
    'claude-haiku-4-5':  { input:  1.00, output:  5.00, cacheRead: 0.10 },
};
/** Rates used when a message's model is missing or not in PRICING_PER_MTOK. */
const DEFAULT_MODEL = 'claude-sonnet-5';
/** Cache-write price as a multiple of the input price, per cache TTL. */
const CACHE_WRITE_5M_MULTIPLIER = 1.25;
const CACHE_WRITE_1H_MULTIPLIER = 2.0;

/** Model ids already warned about, so each unknown model is logged only once. */
const warnedModels = new Set();

/**
 * Looks up the price table for a model id by longest matching prefix, so
 * "claude-opus-5-5" is not mistaken for "claude-opus-5".
 *
 * @param {string | undefined} model - The `message.model` value from a transcript.
 * @returns {{ input: number, output: number, cacheRead: number }} Prices in USD per MTok.
 *   Falls back to DEFAULT_MODEL's prices (with a one-time warning) if unknown.
 */
function ratesFor(model) {
    let best = null;
    for (const prefix of Object.keys(PRICING_PER_MTOK)) {
        if (model && model.startsWith(prefix) && (!best || prefix.length > best.length)) best = prefix;
    }
    if (best) return PRICING_PER_MTOK[best];

    // "<synthetic>" entries are Claude Code's own placeholder messages with no real usage.
    if (model && model !== '<synthetic>' && !warnedModels.has(model)) {
        warnedModels.add(model);
        console.warn(`[stats] no pricing for model "${model}", using ${DEFAULT_MODEL} rates`);
    }
    return PRICING_PER_MTOK[DEFAULT_MODEL];
}

/**
 * Computes the estimated USD cost of one API response.
 *
 * @param {object} usage - The `message.usage` object from a transcript entry
 *   (input_tokens, output_tokens, cache_read_input_tokens,
 *   cache_creation_input_tokens and, when present, cache_creation with
 *   ephemeral_5m_input_tokens / ephemeral_1h_input_tokens).
 * @param {string | undefined} model - The `message.model` value.
 * @returns {number} Cost in USD.
 */
function costOf(usage, model) {
    const rates = ratesFor(model);

    // Split cache writes by TTL. Older transcripts only have the total, which
    // is treated as 5-minute writes.
    const split = usage.cache_creation;
    const write1h = (split && split.ephemeral_1h_input_tokens) || 0;
    const write5m = split
        ? split.ephemeral_5m_input_tokens || 0
        : usage.cache_creation_input_tokens || 0;

    const perMTok =
        (usage.input_tokens || 0) * rates.input +
        (usage.output_tokens || 0) * rates.output +
        (usage.cache_read_input_tokens || 0) * rates.cacheRead +
        write5m * rates.input * CACHE_WRITE_5M_MULTIPLIER +
        write1h * rates.input * CACHE_WRITE_1H_MULTIPLIER;
    return perMTok / 1_000_000;
}

/**
 * Formats a time as a local calendar-day key.
 *
 * @param {number} ms - Milliseconds since the epoch.
 * @returns {string} The local date as "YYYY-MM-DD".
 */
function localDayKey(ms) {
    const d = new Date(ms);
    const pad = (n) => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}

/**
 * Tracks today's cost and each session's activity and context size from
 * Claude Code transcripts.
 */
class StatsTracker {
    constructor() {
        /**
         * Per-session activity, keyed by Claude Code sessionId.
         * @type {Map<string, { lastSeen: number, lastContextTokens: number }>}
         */
        this.sessions = new Map();
        /**
         * Byte offset already read from each transcript, so only new lines are parsed.
         * @type {Map<string, number>}
         */
        this.fileOffsets = new Map();
        /** Local day that costTodayUSD and messageCosts belong to ("YYYY-MM-DD"). */
        this.costDayKey = localDayKey(Date.now());
        /** Estimated spend so far today across all sessions, in USD. */
        this.costTodayUSD = 0;
        /**
         * Cost already counted for each of today's messages, keyed by message.id,
         * so repeated transcript lines for the same message are not double-counted.
         * @type {Map<string, number>}
         */
        this.messageCosts = new Map();
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
     * Resets today's cost when the local date has changed since it was last
     * checked, so the device's "$X today" starts again at midnight.
     *
     * @param {number} now - Current time in milliseconds since the epoch.
     * @returns {void}
     * @private
     */
    _rolloverIfNewDay(now) {
        const key = localDayKey(now);
        if (key === this.costDayKey) return;
        this.costDayKey = key;
        this.costTodayUSD = 0;
        this.messageCosts.clear();
    }

    /**
     * Parses one transcript line and, if it is an assistant message with
     * usage data, updates the session's activity and context size and adds
     * the message's cost to today's total (once per message.id). Non-JSON
     * lines and non-assistant entries are ignored.
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
        const message = entry.message;
        const usage = message && message.usage;
        if (!sessionId || !usage) return;

        // Use the entry's own time, not the time it was read, so old transcripts
        // loaded at startup don't look active and don't count toward today.
        const entryTime = Date.parse(entry.timestamp) || Date.now();

        // Size of the prompt for this turn, i.e. how much of the context window
        // the session is using right now. This is a snapshot, not a running total.
        const contextTokens =
            (usage.input_tokens || 0) +
            (usage.cache_creation_input_tokens || 0) +
            (usage.cache_read_input_tokens || 0);

        const session = this.sessions.get(sessionId) || { lastSeen: 0, lastContextTokens: 0 };
        if (entryTime >= session.lastSeen) {
            session.lastSeen = entryTime;
            session.lastContextTokens = contextTokens;
        }
        this.sessions.set(sessionId, session);

        this._rolloverIfNewDay(Date.now());
        if (localDayKey(entryTime) !== this.costDayKey) return;

        const messageId = message.id || entry.uuid;
        const cost = costOf(usage, message.model);
        const alreadyCounted = this.messageCosts.get(messageId) || 0;
        this.costTodayUSD += cost - alreadyCounted;
        this.messageCosts.set(messageId, cost);
    }

    /**
     * Computes the summary and passes it to the `onUpdate` callback given to
     * `start()`.
     *
     *  - agents:  number of sessions with an assistant message in the last
     *             ACTIVE_WINDOW_MS.
     *  - pct:     context fill of the busiest active session, as a whole
     *             percentage of SONNET_CONTEXT_WINDOW, capped at 100.
     *  - costUSD: estimated spend across all sessions today (local time).
     *
     * @returns {void}
     */
    _emitSummary() {
        const now = Date.now();
        this._rolloverIfNewDay(now);
        const active = [...this.sessions.values()].filter((s) => now - s.lastSeen < ACTIVE_WINDOW_MS);

        const agents = active.length;

        const busiestContextTokens = active.reduce((max, s) => Math.max(max, s.lastContextTokens), 0);
        const pct = Math.min(100, Math.round((busiestContextTokens / SONNET_CONTEXT_WINDOW) * 100));

        this._onUpdate({ agents, pct, costUSD: this.costTodayUSD });
    }
}

module.exports = StatsTracker;
