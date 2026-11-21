// Recording Renamer - Stream Deck plugin
// "Add word" button: sends its word to the OBS Recording Renamer window via
// obs-websocket (CallVendorRequest -> recording-renamer / add_word).

const WebSocket = require("ws");
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

// ------------------------------------------------------------ logging
const LOG_FILE = path.join(__dirname, "..", "logs", "plugin.log");
function log(...args) {
	try {
		fs.mkdirSync(path.dirname(LOG_FILE), { recursive: true });
		fs.appendFileSync(LOG_FILE, `[${new Date().toISOString()}] ${args.join(" ")}\n`);
	} catch (_) {}
}

// ------------------------------------------------------------ Stream Deck connection
const argv = process.argv;
const arg = (name) => argv[argv.indexOf(name) + 1];
const SD_PORT = arg("-port");
const PLUGIN_UUID = arg("-pluginUUID");
const REGISTER_EVENT = arg("-registerEvent");

let globalSettings = { port: "4455", password: "" };
const contexts = new Map(); // context -> settings
const ACTION_ADD = "com.zolig1.recordingrenamer.addword";
const ACTION_REMOVE = "com.zolig1.recordingrenamer.removeword";
const ACTION_DELETE = "com.zolig1.recordingrenamer.delete";

const sd = new WebSocket(`ws://127.0.0.1:${SD_PORT}`);
const sdSend = (obj) => sd.readyState === WebSocket.OPEN && sd.send(JSON.stringify(obj));

sd.on("open", () => {
	sdSend({ event: REGISTER_EVENT, uuid: PLUGIN_UUID });
	sdSend({ event: "getGlobalSettings", context: PLUGIN_UUID });
	log("Registered with Stream Deck");
});

sd.on("close", () => process.exit(0));

sd.on("message", async (raw) => {
	let msg;
	try {
		msg = JSON.parse(raw);
	} catch (_) {
		return;
	}
	const { event, context, payload, action } = msg;

	switch (event) {
		case "didReceiveGlobalSettings": {
			const s = (payload && payload.settings) || {};
			const changed = s.port !== globalSettings.port || s.password !== globalSettings.password;
			globalSettings = { port: s.port || "4455", password: s.password || "" };
			if (changed) obs.reconnect();
			break;
		}
		case "willAppear":
		case "didReceiveSettings":
			contexts.set(context, (payload && payload.settings) || {});
			if (action === ACTION_ADD) showWord(context);
			break;
		case "willDisappear":
			contexts.delete(context);
			break;
		case "keyDown":
			if (action === ACTION_REMOVE) await sendToObs(context, "remove_word", {});
			else if (action === ACTION_DELETE) await sendToObs(context, "delete_recording", {});
			else await onKeyDown(context, (payload && payload.settings) || contexts.get(context) || {});
			break;
	}
});

function showWord(context) {
	const s = contexts.get(context) || {};
	// Only shows when the user hasn't typed their own title in Stream Deck.
	sdSend({ event: "setTitle", context, payload: { title: s.word || "", target: 0 } });
}

async function onKeyDown(context, settings) {
	const word = (settings.word || "").trim();
	if (!word) {
		sdSend({ event: "showAlert", context });
		return;
	}
	await sendToObs(context, "add_word", { word });
}

async function sendToObs(context, requestType, requestData) {
	try {
		const res = await obs.call("CallVendorRequest", {
			vendorName: "recording-renamer",
			requestType,
			requestData,
		});
		const ok = res && res.responseData && res.responseData.ok;
		sdSend({ event: ok ? "showOk" : "showAlert", context });
		if (!ok) log("OBS answered:", JSON.stringify(res));
	} catch (e) {
		log("Request failed:", e.message || e);
		sdSend({ event: "showAlert", context });
	}
}

// ------------------------------------------------------------ OBS websocket (v5)
const obs = {
	ws: null,
	ready: false,
	waiters: [],
	pending: new Map(),
	nextId: 1,
	retryTimer: null,

	connect() {
		clearTimeout(this.retryTimer);
		const url = `ws://127.0.0.1:${globalSettings.port || "4455"}`;
		const ws = new WebSocket(url);
		this.ws = ws;
		this.ready = false;

		ws.on("message", (raw) => {
			let msg;
			try {
				msg = JSON.parse(raw);
			} catch (_) {
				return;
			}
			if (msg.op === 0) {
				const ident = { rpcVersion: 1, eventSubscriptions: 0 };
				const auth = msg.d.authentication;
				if (auth) {
					const secret = crypto
						.createHash("sha256")
						.update(globalSettings.password + auth.salt)
						.digest("base64");
					ident.authentication = crypto
						.createHash("sha256")
						.update(secret + auth.challenge)
						.digest("base64");
				}
				ws.send(JSON.stringify({ op: 1, d: ident }));
			} else if (msg.op === 2) {
				this.ready = true;
				log("Connected to OBS on", url);
				this.waiters.splice(0).forEach((w) => w.resolve());
			} else if (msg.op === 7) {
				const p = this.pending.get(msg.d.requestId);
				if (!p) return;
				this.pending.delete(msg.d.requestId);
				if (msg.d.requestStatus.result) p.resolve(msg.d.responseData);
				else p.reject(new Error(msg.d.requestStatus.comment || `code ${msg.d.requestStatus.code}`));
			}
		});

		ws.on("error", () => {});
		ws.on("close", (code) => {
			if (this.ws !== ws) return;
			this.ready = false;
			if (code === 4009) log("OBS rejected the WebSocket password");
			const err = new Error(code === 4009 ? "Wrong OBS WebSocket password" : "Not connected to OBS");
			this.waiters.splice(0).forEach((w) => w.reject(err));
			this.pending.forEach((p) => p.reject(err));
			this.pending.clear();
			this.retryTimer = setTimeout(() => this.connect(), 3000);
		});
	},

	reconnect() {
		const old = this.ws;
		this.ws = null;
		if (old) old.terminate();
		this.connect();
	},

	waitReady(timeoutMs) {
		if (this.ready) return Promise.resolve();
		return new Promise((resolve, reject) => {
			const w = { resolve, reject };
			this.waiters.push(w);
			setTimeout(() => {
				const i = this.waiters.indexOf(w);
				if (i >= 0) {
					this.waiters.splice(i, 1);
					reject(new Error("Timed out connecting to OBS"));
				}
			}, timeoutMs);
		});
	},

	async call(requestType, requestData) {
		await this.waitReady(2000);
		const requestId = String(this.nextId++);
		return new Promise((resolve, reject) => {
			this.pending.set(requestId, { resolve, reject });
			this.ws.send(JSON.stringify({ op: 6, d: { requestType, requestId, requestData } }));
			setTimeout(() => {
				if (this.pending.delete(requestId)) reject(new Error("OBS did not answer"));
			}, 3000);
		});
	},
};

obs.connect();
