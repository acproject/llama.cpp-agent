import net from "node:net";
function encodeBulk(s) {
    const b = Buffer.from(s, "utf8");
    return Buffer.concat([Buffer.from(`$${b.length}\r\n`, "utf8"), b, Buffer.from("\r\n", "utf8")]);
}
function encodeCommand(args) {
    const parts = [Buffer.from(`*${args.length}\r\n`, "utf8")];
    for (const a of args) {
        parts.push(encodeBulk(a));
    }
    return Buffer.concat(parts);
}
function indexOfCrlf(buf, start) {
    for (let i = start; i + 1 < buf.length; i++) {
        if (buf[i] === 13 && buf[i + 1] === 10)
            return i;
    }
    return -1;
}
function parseIntLine(buf, start) {
    const end = indexOfCrlf(buf, start);
    if (end < 0)
        return null;
    const s = buf.subarray(start, end).toString("utf8");
    const n = Number(s);
    if (!Number.isFinite(n))
        return null;
    return { n: Math.trunc(n), next: end + 2 };
}
function parseStringLine(buf, start) {
    const end = indexOfCrlf(buf, start);
    if (end < 0)
        return null;
    const s = buf.subarray(start, end).toString("utf8");
    return { s, next: end + 2 };
}
function parseResp(buf, start = 0) {
    if (start >= buf.length)
        return null;
    const lead = buf[start];
    if (lead === 43) {
        const line = parseStringLine(buf, start + 1);
        if (!line)
            return null;
        return { value: { type: "simple", value: line.s }, next: line.next };
    }
    if (lead === 45) {
        const line = parseStringLine(buf, start + 1);
        if (!line)
            return null;
        return { value: { type: "error", value: line.s }, next: line.next };
    }
    if (lead === 58) {
        const line = parseIntLine(buf, start + 1);
        if (!line)
            return null;
        return { value: { type: "int", value: line.n }, next: line.next };
    }
    if (lead === 36) {
        const lenLine = parseIntLine(buf, start + 1);
        if (!lenLine)
            return null;
        const len = lenLine.n;
        if (len === -1)
            return { value: { type: "bulk", value: null }, next: lenLine.next };
        const end = lenLine.next + len;
        if (end + 2 > buf.length)
            return null;
        const s = buf.subarray(lenLine.next, end).toString("utf8");
        if (buf[end] !== 13 || buf[end + 1] !== 10)
            return null;
        return { value: { type: "bulk", value: s }, next: end + 2 };
    }
    if (lead === 42) {
        const countLine = parseIntLine(buf, start + 1);
        if (!countLine)
            return null;
        const count = countLine.n;
        if (count === -1)
            return { value: { type: "array", value: null }, next: countLine.next };
        const items = [];
        let pos = countLine.next;
        for (let i = 0; i < count; i++) {
            const parsed = parseResp(buf, pos);
            if (!parsed)
                return null;
            items.push(parsed.value);
            pos = parsed.next;
        }
        return { value: { type: "array", value: items }, next: pos };
    }
    return null;
}
export function respToJson(v) {
    if (v.type === "simple" || v.type === "error")
        return v.value;
    if (v.type === "int")
        return v.value;
    if (v.type === "bulk")
        return v.value;
    if (v.type === "array")
        return v.value ? v.value.map(respToJson) : null;
    return null;
}
export class MiniMemoryClient {
    host;
    port;
    password;
    socket;
    buffer = Buffer.alloc(0);
    pending = [];
    constructor(opts) {
        this.host = opts.host;
        this.port = opts.port;
        this.password = opts.password;
    }
    async connect() {
        if (this.socket)
            return;
        const sock = net.createConnection({ host: this.host, port: this.port });
        this.socket = sock;
        sock.on("data", (chunk) => this.onData(chunk));
        sock.on("error", (err) => this.onError(err));
        sock.on("close", () => this.onClose());
        await new Promise((resolve, reject) => {
            sock.once("connect", resolve);
            sock.once("error", reject);
        });
        if (this.password && this.password.length > 0) {
            const r = await this.command(["AUTH", this.password]);
            if (r.type === "error") {
                throw new Error(`AUTH failed: ${r.value}`);
            }
        }
    }
    close() {
        if (this.socket) {
            this.socket.destroy();
            this.socket = undefined;
        }
        this.buffer = Buffer.alloc(0);
        while (this.pending.length > 0) {
            const p = this.pending.shift();
            if (p)
                p.reject(new Error("socket closed"));
        }
    }
    onData(chunk) {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        while (this.pending.length > 0) {
            const parsed = parseResp(this.buffer, 0);
            if (!parsed)
                break;
            this.buffer = this.buffer.subarray(parsed.next);
            const p = this.pending.shift();
            if (p)
                p.resolve(parsed.value);
        }
    }
    onError(err) {
        const e = new Error(err.message);
        while (this.pending.length > 0) {
            const p = this.pending.shift();
            if (p)
                p.reject(e);
        }
    }
    onClose() {
        while (this.pending.length > 0) {
            const p = this.pending.shift();
            if (p)
                p.reject(new Error("socket closed"));
        }
    }
    async command(args) {
        await this.connect();
        const sock = this.socket;
        if (!sock)
            throw new Error("not connected");
        const payload = encodeCommand(args);
        const p = new Promise((resolve, reject) => this.pending.push({ resolve, reject }));
        sock.write(payload);
        return p;
    }
}
