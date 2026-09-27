/**
 * @file bridge/serial.js
 * @description Line-based wrapper around the device's USB serial port.
 * Incoming newline-terminated lines are re-emitted as events; outgoing
 * messages get a trailing newline appended.
 */
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const { EventEmitter } = require('events');

/**
 * Serial connection to the watch-my-tokens (AgentPager) device.
 *
 * Events:
 *   open            - the port opened.
 *   close           - the port closed.
 *   error (Error)   - the port reported an error (e.g. path not found, busy).
 *   line  (string)  - any non-empty line received, trimmed. Includes ESP-IDF logs.
 *   approve         - the device sent BTN:APPROVE (short tap).
 *   deny            - the device sent BTN:DENY (1 s long press).
 *   ready           - the device sent READY (not sent by current firmware).
 *
 * @extends EventEmitter
 */
class AgentPagerSerial extends EventEmitter {
    /**
     * Opens the serial port right away.
     *
     * @param {string} path - Serial device path, e.g. "/dev/tty.usbmodem101".
     * @param {number} [baudRate=115200] - Baud rate; must match the firmware.
     */
    constructor(path, baudRate = 115200) {
        super();
        this.port = new SerialPort({ path, baudRate });
        this.parser = this.port.pipe(new ReadlineParser({ delimiter: '\n' }));

        this.port.on('open', () => this.emit('open'));
        this.port.on('error', (err) => this.emit('error', err));
        this.port.on('close', () => this.emit('close'));

        this.parser.on('data', (line) => {
            const trimmed = line.trim();
            if (!trimmed) return;
            this.emit('line', trimmed);

            if (trimmed === 'BTN:APPROVE') this.emit('approve');
            else if (trimmed === 'BTN:DENY') this.emit('deny');
            else if (trimmed === 'READY') this.emit('ready');
        });
    }

    /**
     * Sends one protocol line to the device. The message is dropped (with a
     * warning) if the port is not open yet.
     *
     * @param {string} msg - A protocol message without a trailing newline,
     *   e.g. "STATS:2:47:1.83" or "SCREEN:HOME".
     * @returns {void}
     */
    send(msg) {
        if (!this.port.isOpen) {
            console.warn('[serial] port not open, dropping message:', msg);
            return;
        }
        this.port.write(msg + '\n');
    }

    /**
     * Closes the port if it is open.
     *
     * @returns {void}
     */
    close() {
        if (this.port.isOpen) this.port.close();
    }
}

module.exports = AgentPagerSerial;
