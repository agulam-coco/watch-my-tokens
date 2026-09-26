// bridge/stats.js
const chokidar = require('chokidar');
const fs = require('fs');
const os = require('os');
const path = require('path');

const PROJECTS_DIR = path.join(os.homedir(), '.claude', 'projects');
const ACTIVE_WINDOW_MS = 5 * 60 * 1000; // "active" = activity in the last 5 min

class StatsTracker {
    constructor() {
        this.sessions = new Map();     // sessionId -> { tokens, lastSeen }
        this.fileOffsets = new Map();  // filePath -> byte offset already read
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

            if (stat.size < startOffset) {
                startOffset = 0; // file was truncated/rotated
            }

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
            for (const line of lines) {
                this._processLine(line);
            }

            this._emitSummary();
        } catch (err) {
            // file may have been deleted/rotated mid-read; ignore and retry next event
        }
    }

    _processLine(line) {
        let entry;
        try {
            entry = JSON.parse(line);
        } catch (_) {
            return; // malformed/partial line
        }

        if (entry.type !== 'assistant') return;

        const sessionId = entry.sessionId;
        const usage = entry.message && entry.message.usage;
        if (!sessionId || !usage) return;

        const turnTokens =
            (usage.input_tokens || 0) +
            (usage.output_tokens || 0) +
            (usage.cache_creation_input_tokens || 0) +
            (usage.cache_read_input_tokens || 0);

        const existing = this.sessions.get(sessionId) || { tokens: 0, lastSeen: 0 };
        existing.tokens += turnTokens;
        existing.lastSeen = Date.now();
        this.sessions.set(sessionId, existing);
    }

    _emitSummary() {
        const now = Date.now();
        const active = [...this.sessions.values()].filter(
            (s) => now - s.lastSeen < ACTIVE_WINDOW_MS
        );
        const totalTokens = active.reduce((sum, s) => sum + s.tokens, 0);

        this._onUpdate({
            agents: active.length,
            totalTokens,
        });
    }
}

module.exports = StatsTracker;