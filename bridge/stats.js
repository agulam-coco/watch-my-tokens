const chokidar = require('chokidar');
const fs = require('fs');
const os = require('os');
const path = require('path');

const PROJECTS_DIR = path.join(os.homedir(), '.claude', 'projects');
const ACTIVE_WINDOW_MS = 60 * 1000; // a session goes "inactive" after 60s of silence
const SONNET_CONTEXT_WINDOW = 200000; // tokens, Sonnet's real context limit

// Real Claude Sonnet 5 API pricing, $ per token (not per million)
const PRICE_INPUT = 2.00 / 1_000_000;
const PRICE_OUTPUT = 10.00 / 1_000_000;
const PRICE_CACHE_WRITE = 2.50 / 1_000_000;
const PRICE_CACHE_READ = 0.20 / 1_000_000;

class StatsTracker {
    constructor() {
        // sessionId -> { costUSD, lastSeen, lastContextTokens }
        this.sessions = new Map();
        this.fileOffsets = new Map();
    }

    start(onUpdate) {
        this._onUpdate = onUpdate;
        const watcher = chokidar.watch(path.join(PROJECTS_DIR, '**/*.jsonl'), {
            persistent: true,
            ignoreInitial: false,
        });
        watcher.on('add', (filePath) => this._scan(filePath));
        watcher.on('change', (filePath) => this._scan(filePath));
    }

    _scan(filePath) {
        try {
            const stat = fs.statSync(filePath);
            let startOffset = this.fileOffsets.get(filePath) || 0;
            if (stat.size < startOffset) startOffset = 0; // file rotated/truncated

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
            // file may have been deleted/rotated mid-read
        }
    }

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

        // Real dollar cost of this turn, priced per token type.
        const turnCostUSD =
            inputTokens * PRICE_INPUT +
            outputTokens * PRICE_OUTPUT +
            cacheWriteTokens * PRICE_CACHE_WRITE +
            cacheReadTokens * PRICE_CACHE_READ;

        // This turn's total context size (what's actually sitting in the
        // context window right now for this session) — not cumulative.
        const contextTokens = inputTokens + cacheWriteTokens + cacheReadTokens;

        const existing = this.sessions.get(sessionId) || { costUSD: 0, lastSeen: 0, lastContextTokens: 0 };
        existing.costUSD += turnCostUSD;
        existing.lastSeen = Date.now();
        existing.lastContextTokens = contextTokens;
        this.sessions.set(sessionId, existing);
    }

    _emitSummary() {
        const now = Date.now();
        const active = [...this.sessions.values()].filter((s) => now - s.lastSeen < ACTIVE_WINDOW_MS);

        const agents = active.length;
        const totalCostUSD = active.reduce((sum, s) => sum + s.costUSD, 0);

        // pct = how full the busiest active session's context window is right now
        const busiestContextTokens = active.reduce((max, s) => Math.max(max, s.lastContextTokens), 0);
        const pct = Math.min(100, Math.round((busiestContextTokens / SONNET_CONTEXT_WINDOW) * 100));

        this._onUpdate({ agents, pct, costUSD: totalCostUSD });
    }
}

module.exports = StatsTracker;