// bridge/serial.js
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const { EventEmitter } = require('events');

class AgentPagerSerial extends EventEmitter {
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

    send(msg) {
        if (!this.port.isOpen) {
            console.warn('[serial] port not open, dropping message:', msg);
            return;
        }
        this.port.write(msg + '\n');
    }

    close() {
        if (this.port.isOpen) this.port.close();
    }
}

module.exports = AgentPagerSerial;