// Account setup the end-to-end tests need and meshctrl can't do: turning on
// two-factor authentication (an authenticator app secret) for an account,
// and creating a login token. Runs with Node.js against the test server;
// the "ws" module comes from the MeshCentral installation (NODE_PATH).
//
//   node meshcentral-setup.js totp  <wss://host:port> <user> <password>
//   node meshcentral-setup.js token <wss://host:port> <user> <password> <name>
//
// "totp" prints nothing on success; "token" prints {"user":…,"pass":…}.

'use strict';

const crypto = require('crypto');
const WebSocket = require('ws');

function fail(text) {
    console.error('meshcentral-setup: ' + text);
    process.exit(1);
}

// RFC 6238 one-time password: HMAC-SHA1 over the 30-second counter.
function totp(base32secret) {
    const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
    let bits = '';
    for (const c of base32secret.replace(/=+$/, '').toUpperCase()) {
        const v = alphabet.indexOf(c);
        if (v < 0) fail('the secret is not base32');
        bits += v.toString(2).padStart(5, '0');
    }
    const key = Buffer.alloc(Math.floor(bits.length / 8));
    for (let i = 0; i < key.length; i++) key[i] = parseInt(bits.substr(i * 8, 8), 2);
    const counter = Buffer.alloc(8);
    counter.writeBigUInt64BE(BigInt(Math.floor(Date.now() / 30000)));
    const mac = crypto.createHmac('sha1', key).update(counter).digest();
    const offset = mac[mac.length - 1] & 15;
    const code = (mac.readUInt32BE(offset) & 0x7fffffff) % 1000000;
    return code.toString().padStart(6, '0');
}

function login(url, user, pass, onMessage) {
    const b64 = (s) => Buffer.from(s).toString('base64');
    const ws = new WebSocket(url.replace(/\/$/, '') + '/control.ashx', {
        rejectUnauthorized: false, // the test server's own certificate
        headers: { 'x-meshauth': b64(user) + ',' + b64(pass) },
    });
    ws.on('message', (data) => {
        const msg = JSON.parse(data.toString());
        if (msg.action === 'close') fail('the server refused the login: ' + JSON.stringify(msg));
        onMessage(ws, msg);
    });
    ws.on('error', (e) => fail(e.message));
    setTimeout(() => fail('timed out'), 20000);
    return ws;
}

const [command, url, user, pass, name] = process.argv.slice(2);
if (!command || !url || !user || !pass) fail('usage: meshcentral-setup.js totp|token <url> <user> <password> [name]');

if (command === 'totp') {
    login(url, user, pass, (ws, msg) => {
        if (msg.action === 'serverinfo') ws.send(JSON.stringify({ action: 'otpauth-request' }));
        if (msg.action === 'otpauth-request') {
            if (!msg.secret) fail('the server does not offer authenticator apps: ' + JSON.stringify(msg));
            ws.send(JSON.stringify({ action: 'otpauth-setup', secret: msg.secret, token: totp(msg.secret) }));
        }
        if (msg.action === 'otpauth-setup') {
            if (msg.success !== true) fail('the server did not accept the one-time password');
            process.exit(0);
        }
    });
} else if (command === 'token') {
    if (!name) fail('a token needs a name');
    login(url, user, pass, (ws, msg) => {
        if (msg.action === 'serverinfo') ws.send(JSON.stringify({ action: 'createLoginToken', name: name, expire: 0 }));
        if (msg.action === 'createLoginToken') {
            if (!msg.tokenUser || !msg.tokenPass) fail('no token: ' + JSON.stringify(msg));
            console.log(JSON.stringify({ user: msg.tokenUser, pass: msg.tokenPass }));
            process.exit(0);
        }
    });
} else {
    fail('unknown command ' + command);
}
