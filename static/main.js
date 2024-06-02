// From https://github.com/lyngklip/structjs/blob/da47a27914dc3964db87e77a3ff7edde06b2dbd1/struct.mjs
const rechk = /^([<>])?(([1-9]\d*)?([xcbB?hHiIfdsp]))*$/
const refmt = /([1-9]\d*)?([xcbB?hHiIfdsp])/g
const str = (v,o,c) => String.fromCharCode(
	...new Uint8Array(v.buffer, v.byteOffset + o, c))
const rts = (v,o,c,s) => new Uint8Array(v.buffer, v.byteOffset + o, c)
	.set(s.split('').map(str => str.charCodeAt(0)))
const pst = (v,o,c) => str(v, o + 1, Math.min(v.getUint8(o), c - 1))
const tsp = (v,o,c,s) => { v.setUint8(o, s.length); rts(v, o + 1, c - 1, s) }
const lut = le => ({
	x: c=>[1,c,0],
	c: c=>[c,1,o=>({u:v=>str(v, o, 1)      , p:(v,c)=>rts(v, o, 1, c)     })],
	'?': c=>[c,1,o=>({u:v=>Boolean(v.getUint8(o)),p:(v,B)=>v.setUint8(o,B)})],
	b: c=>[c,1,o=>({u:v=>v.getInt8(   o   ), p:(v,b)=>v.setInt8(   o,b   )})],
	B: c=>[c,1,o=>({u:v=>v.getUint8(  o   ), p:(v,B)=>v.setUint8(  o,B   )})],
	h: c=>[c,2,o=>({u:v=>v.getInt16(  o,le), p:(v,h)=>v.setInt16(  o,h,le)})],
	H: c=>[c,2,o=>({u:v=>v.getUint16( o,le), p:(v,H)=>v.setUint16( o,H,le)})],
	i: c=>[c,4,o=>({u:v=>v.getInt32(  o,le), p:(v,i)=>v.setInt32(  o,i,le)})],
	I: c=>[c,4,o=>({u:v=>v.getUint32( o,le), p:(v,I)=>v.setUint32( o,I,le)})],
	f: c=>[c,4,o=>({u:v=>v.getFloat32(o,le), p:(v,f)=>v.setFloat32(o,f,le)})],
	d: c=>[c,8,o=>({u:v=>v.getFloat64(o,le), p:(v,d)=>v.setFloat64(o,d,le)})],
	s: c=>[1,c,o=>({u:v=>str(v,o,c), p:(v,s)=>rts(v,o,c,s.slice(0,c    ) )})],
	p: c=>[1,c,o=>({u:v=>pst(v,o,c), p:(v,s)=>tsp(v,o,c,s.slice(0,c - 1) )})]
})
const errbuf = new RangeError("Structure larger than remaining buffer")
const errval = new RangeError("Not enough values for structure")
function struct(format) {
	let fns = [], size = 0, m = rechk.exec(format)
	if (!m) { throw new RangeError("Invalid format string") }
	const t = lut('<' === m[1]), lu = (n, c) => t[c](n ? parseInt(n, 10) : 1)
	while ((m = refmt.exec(format))) { ((r, s, f) => {
	for (let i = 0; i < r; ++i, size += s) { if (f) {fns.push(f(size))} }
	})(...lu(...m.slice(1)))}
	const unpack_from = (arrb, offs) => {
		if (arrb.byteLength < (offs|0) + size) { throw errbuf }
		let v = new DataView(arrb, offs|0)
		return fns.map(f => f.u(v))
	}
	const pack_into = (arrb, offs, ...values) => {
		if (values.length < fns.length) { throw errval }
		if (arrb.byteLength < offs + size) { throw errbuf }
		const v = new DataView(arrb, offs)
		new Uint8Array(arrb, offs, size).fill(0)
		fns.forEach((f, i) => f.p(v, values[i]))
	}
	const pack = (...values) => {
		let b = new ArrayBuffer(size)
		pack_into(b, 0, ...values)
		return b
	}
	const unpack = arrb => unpack_from(arrb, 0)
	function* iter_unpack(arrb) { 
		for (let offs = 0; offs + size <= arrb.byteLength; offs += size) {
			yield unpack_from(arrb, offs);
		}
	}
	return Object.freeze({
	unpack, pack, unpack_from, pack_into, iter_unpack, format, size})
}

// Our own logic now

const url = 'ws://127.0.0.1:3001/ws';
const scale_factor = 0.1;
const min_zoom = 20.0;
const max_zoom = 0.5;
const drag_threshold = 15;
const zoom_factor = 1.1;
const zoom_mass = 40.0;

async function decompress(data) {
	const ds = new DecompressionStream('deflate');
	const stream = new ReadableStream({
		start(controller) {
			controller.enqueue(data);
			controller.close();
		}
	});
	const decompressed = stream.pipeThrough(ds);
	const reader = decompressed.getReader();
	const chunks = [];
	while (true) {
		const { done, value } = await reader.read();
		if (done) break;
		chunks.push(value);
	}
	const decomp_arr = new Uint8Array(chunks.reduce((acc, chunk) => acc + chunk.length, 0));
	let offset = 0;
	for (const chunk of chunks) {
		decomp_arr.set(chunk, offset);
		offset += chunk.length;
	}
	return Array.from(decomp_arr);
}

class ColorList {
	constructor(colors) {
		this.colors = [];
		this.selected_index = window.localStorage.getItem('selected_color') || 0;
		this.active_color = 0;

		for (var i = 0; i < colors.length; i++) {
			this.colors.push({ 'id': colors[i].ID, 'color': this.rgb_to_hex(colors[i].R, colors[i].G, colors[i].B) });
		}
		this.container = document.getElementById("color-container");
		this.container.innerHTML = '';
		this.colors.forEach((color, index) => {
			const color_square = document.createElement('div');
			color_square.className = 'color-square';
			color_square.style.backgroundColor = color.color;
			color_square.dataset.index = index;
			this.container.appendChild(color_square);
			color_square.addEventListener('click', () => {
				this.select_color(index);
			});
		});
		this.select_color(this.selected_index);
	}
	select_color(index) {
		// if (this.selected_index === index) return;
		if (this.selected_index !== -1) {
			console.log(this.container);
			this.container.children[this.selected_index].classList.remove('selected');
		}
		this.selected_index = index;
		this.container.children[this.selected_index].classList.add('selected');
		this.active_color = this.colors[this.selected_index].id;
		window.localStorage.setItem("selected_color", this.selected_index);
	}
	rgb_to_hex(r, g, b) {
		return "#" + ((1 << 24) + (r << 16) + (g << 8) + b).toString(16).slice(1);
	}
	get_color(id) {
		return this.colors.filter(function (c) { return c.id === id })[0].color;
	}
	selected_color() {
		return this.get_color(this.active_color);
	}
	selected_color_id() {
		return this.active_color;
	}
}

class Canvas {
	constructor(client) {
		this.client = client;
		this.offset = { x: 0, y: 0 };
		this.scale = 1;
		this.prev_pix = { x: 0, y: 0, c: 0 };
		this.drag_start = { x: 0, y: 0 };
		this.mouse_start = { x: 0, y: 0 };
		this.mouse_down = false;
		this.is_dragging = false;
		this.elem = document.getElementById('canvas_container');
		this.canvas = document.getElementById('canvas');
		this.ctx = this.canvas.getContext('2d', { alpha: false });
		this.mouse_pos_pixel = { x: 0, y: 0 };
		this.pixel_info = {
			visible: false,
			// TODO
		};
		this.size = 0;
		this.client = client;
		this.pixels = [];

		document.body.style.mozUserSelect = document.body.style.webkitUserSelect = document.body.style.userSelect = 'none';
		// Mouse down event to start dragging
		this.elem.addEventListener('mousedown', (e) => {
			this.mouse_down = true;
			this.drag_start.x = e.clientX - this.offset.x;
			this.drag_start.y = e.clientY - this.offset.y;
			this.mouse_start.x = e.screenX;
			this.mouse_start.y = e.screenY;
		});

		// Mouse move event to handle dragging
		this.elem.addEventListener('mousemove', (e) => {
			if (this.mouse_down) {
				// Prevent accidental canvas drag when clicking
				const mouse_x = e.screenX - this.mouse_start.x;
				const mouse_y = e.screenY - this.mouse_start.y;
				if (mouse_x * mouse_x + mouse_y * mouse_y < drag_threshold * drag_threshold) return;
				this.is_dragging = true;

				this.offset.x = e.clientX - this.drag_start.x;
				this.offset.y = e.clientY - this.drag_start.y;
				this.elem.style.transform = `translateX(${this.offset.x}px) translateY(${this.offset.y}px) scale(${this.scale})`
			} else {
				const mouse_x = (e.pageX - this.offset.x) / this.scale;
				const mouse_y = (e.pageY - this.offset.y) / this.scale;
				const pix_x = Math.floor(mouse_x);
				const pix_y = Math.floor(mouse_y);
				if (pix_x === this.prev_pix.x && pix_y === this.prev_pix.y) return;
				const pix_idx = pix_y * this.size + pix_x;
				const color = this.pixels[pix_idx];
				this.ctx.fillStyle = this.color_list.selected_color();
				this.ctx.globalAlpha = 0.4;
				this.ctx.fillRect(pix_x, pix_y, 1, 1);
				this.ctx.globalAlpha = 1.0;
				if (this.prev_pix.c >= 0) {
					this.draw_pixel(this.prev_pix.x, this.prev_pix.y, this.prev_pix.c);
				}
				this.prev_pix.x = pix_x;
				this.prev_pix.y = pix_y;
				this.prev_pix.c = color;
			}
		});

		// Mouse up event to stop dragging
		this.elem.addEventListener('mouseup', (e) => {
			if (!this.is_dragging) {
				this.on_click(e);
			}
			this.mouse_down = false;
			this.is_dragging = false;
		});

		this.elem.addEventListener('mouseout', (e) => {
			this.mouse_down = false;
			this.is_dragging = false;
		})

		this.elem.addEventListener('wheel', (e) => {
			e.preventDefault();
			const mouseX = e.clientX - this.offset.x;
			const mouseY = e.clientY - this.offset.y;
			const delta = -e.deltaY > 0 ? 1 : -1;
			if ((delta > 0 && this.scale > min_zoom) || (delta < 0 && this.scale < max_zoom)) return;
			const zoom = Math.exp(delta * scale_factor);

			this.offset.x -= mouseX * (zoom - 1);
			this.offset.y -= mouseY * (zoom - 1);
			this.scale *= zoom;
			this.elem.style.transform = `translateX(${this.offset.x}px) translateY(${this.offset.y}px) scale(${this.scale})`
		});
		
	}

	on_click(e) {
		e.preventDefault();
		const mouse_x = (e.pageX - this.offset.x) / this.scale;
		const mouse_y = (e.pageY - this.offset.y) / this.scale;
		const pix_x = Math.floor(mouse_x);
		const pix_y = Math.floor(mouse_y);
		this.client.send_tile(pix_x, pix_y, this.color_list.selected_color_id());
		this.prev_pix.c = -1;
	}
	
	draw_pixel(x, y, c) {
		this.ctx.fillStyle = this.color_list.get_color(c);
		this.ctx.fillRect(x, y, 1, 1);
	}
	
	fill(data) {
		decompress(data).then(data => {
			this.size = Math.sqrt(data.length);
			this.canvas.width = this.size;
			this.canvas.height = this.size;
			this.pixels = data;
			
			console.log("Drawing " + this.size + "x" + this.size + " canvas");
			var counter = 0;
			for (var y = 0; y < this.size; y++) {
				for (var x = 0; x < this.size; x++) {
					this.draw_pixel(x, y, data[counter++]);
				}
			}
		});
	}
}

const req = {
	INITIAL_AUTH: 0,
	AUTH: 1,
	GET_CANVAS: 2,
	GET_TILE_INFO: 3,
	POST_TILE: 4,
	GET_COLORS: 5,
	SET_USERNAME: 6,
};

const bin = {
	RES_AUTH_SUCCESS: 0,
	RES_CANVAS: 1,
	RES_TILE_INFO: 2,
	RES_TILE_UPDATE: 3,
	RES_COLOR_LIST: 4,
	RES_USERNAME_SET_SUCCESS: 5,
	RES_TILE_INCREMENT: 6,
	RES_LEVEL_UP: 7,
	RES_USER_COUNT: 8,
	ERR_INVALID_UUID: 128,
};

class PixelClient {
	connect() {
		try {
			this.ws = new WebSocket(url);
		} catch (e) {
			throw new Error(`Failed to open websocket connection to ${url}: ${e.message}`)
		}
		this.ws.binaryType = "arraybuffer";
		this.ws.onopen = this.on_open.bind(this);
		this.ws.onmessage = this.on_message.bind(this);
		this.ws.onclose = this.on_close.bind(this);
		this.ws.onerror = function(err) {
			// console.log('Socket error: ' + err.message);
			// this.ws.close();
		}
	}
	constructor(url) {
		if (!url) throw new Error('Missing websocket URL');
		this.state = {
			user_id: window.localStorage.getItem('userID'),
			remaining_tiles: 0,
			max_tiles: 0,
			user_count: 0, // TODO: Update UI for these somehow
			disconnected: false,
			url: url,
			admin_perms: {
				ban: false,
				cleanup: false,
				tile_info: false,
			},
		};
		this.connect();
		this.state.canvas = new Canvas(this);
	}
	on_open() {
		if (this.state.user_id !== null) {
			this.ws.send(JSON.stringify({ "requestType": "auth", "userID": this.state.user_id.toString() }));
		} else {
			this.ws.send(JSON.stringify({ "requestType": "initialAuth" }));
		}
	}
	on_close() {
		if (this.state.disconnected) {
			// Intentional disconnect
		} else {
			console.log("Lost socket, wait a bit and reconnect");
			setTimeout(this.connect.bind(this), 2000 * Math.random());
		}
	}
	on_message(m) {
		if (m.data instanceof ArrayBuffer) {
			this.on_binary_message(m);
		} else {
			this.on_text_message(m);
		}
	}

	on_binary_message(m) {
		const data = new Uint8Array(m.data);
		switch (data[0]) {
			case bin.RES_AUTH_SUCCESS:
				console.log('RES_AUTH_SUCCESS');
				return;
			case bin.RES_CANVAS:
				this.state.canvas.fill(data.slice(1));
				// actions.loadingScreenVisible(false);
				// actions.setMessageBoxVisibility(false);
				return;
			case bin.RES_TILE_INFO:
				console.log('RES_TILE_INFO');
				return;
			case bin.RES_TILE_UPDATE:
			{
				let s = struct('BBxxI');
				let [_, c, i] = s.unpack(m.data);
				const thing = {
					c: c,
					i: i,
				};
				const x = Math.round(i % this.state.canvas.size);
				const y = Math.floor(i / this.state.canvas.size);
				this.state.canvas.pixels[i] = c;
				this.state.canvas.draw_pixel(x, y, c);
				return;
			}
			case bin.RES_COLOR_LIST:
			{
				// (data_bytes - header_length) / sizeof(struct color)
				const color_amount = (m.data.byteLength - 1) / 4;
				var colors = [];
				for (let i = 0; i < color_amount; ++i) {
					let str = struct('BBBB');
					let view = m.data.slice((i * 4) + 1, (i * 4) + 5);
					let [r, g, b, id] = str.unpack(view);
					colors[i] = { 'R': r, 'G': g, 'B': b, 'ID': id };
				}
				this.state.canvas.color_list = new ColorList(colors);
				this.ws.send(struct('B37s').pack(req.GET_CANVAS, this.state.user_id));
				return;
			}
			case bin.RES_USERNAME_SET_SUCCESS:
				console.log('USERNAME_SET_SUCCESS');
				return;
			case bin.RES_TILE_INCREMENT:
			{
				let i = struct('BB');
				let [_, c] = i.unpack(m.data);
				this.state.remaining_tiles = c;
				return;
			}
			case bin.RES_LEVEL_UP:
				// console.log('RES_LEVEL_UP');
				return;
			case bin.RES_USER_COUNT:
			{
				let i = struct('BxH');
				let [_, c] = i.unpack(m.data);
				this.state.user_count = c;
				return;
			}
			case bin.ERR_INVALID_UUID:
				console.log('ERR_INVALID_UUID');
				return;
			default:
				console.log("Received unknown binary message with id " + data[0]);
				return;
		}
	}

	on_text_message(m) {
		console.log(m.data);
		const data = JSON.parse(m.data);
		switch (data.rt) {
			case "nameSetSuccess":
				// console.log("Name was set successfully")
				// actions.setMessageBoxText("Nickname was set successfully!")
				// actions.setMessageBoxVisibility(true)
				break;
			case "disconnecting":
				console.log("Server sent SHUTDOWN");
				// Change state message box text to default warning message
				// actions.setMessageBoxText("The server restarted, reconnecting...")
				// Change state message box visiblity
				// NOTE: Later this could be implemented with only state text
				// actions.setMessageBoxVisibility(true)
				break;

			case "announcement":
				// Change state message box text
				// actions.setMessageBoxText(data.message)
				// Change state message box visiblity
				// NOTE: Later this could be implemented with only state text
				// actions.setMessageBoxVisibility(true)
				break;

			case "authSuccessful":
				this.state.user_id = data.uuid;
				window.localStorage.setItem("userID", data.uuid);
				this.state.max_tiles = data.maxTiles;
				this.state.remaining_tiles = data.remainingTiles;
				// actions.setLevel(data.level)
				// actions.setUserRequiredExp(data.tilesToNextLevel)
				// actions.setUserExp(data.levelProgress)
				this.ws.send(struct('B37s').pack(req.GET_COLORS, this.state.user_id));
				break;

			case "userCount":
				this.state.user_count = data.count;
				break;

			case "levelUp":
				this.state.max_tiles = data.maxTiles;
				// actions.setLevel(data.level)
				this.state.remaining_tiles = data.remaining_tiles;
				// actions.setUserRequiredExp(data.tilesToNextLevel)
				// actions.setUserExp(data.levelProgress)
				break;

			case "ti":
				console.log("TODO: ti");
				// const date = new Date(data.pt * 1000);
				// let placer_info = data.un + ' on ' + date.toISOString();
				// actions.setPlacerInfo(placer_info);
				break;

			case "error":
				if (data.msg === "Invalid userID") {
					this.ws.send(JSON.stringify({ "requestType": "initialAuth" }));
				}
				console.log(JSON.stringify(data));
				// actions.setMessageBoxText(data.msg);
				// actions.setMessageBoxVisibility(true);
				break;

			case "reAuthSuccessful":
				this.ws.send(struct('B37s').pack(req.GET_COLORS, this.state.user_id));
				this.state.max_tiles = data.maxTiles;
				this.state.remaining_tiles = data.remainingTiles;
				this.state.admin_perms.ban = data.showBanBtn || false;
				this.state.admin_perms.cleanup = data.showCleanupBtn || false;
				this.state.admin_perms.tile_info = data.tileInfoAvailable || false;
				console.log(this.state.admin_perms);
				// actions.setLevel(data.level)
				// actions.setUserRequiredExp(data.tilesToNextLevel)
				// actions.setUserExp(data.levelProgress)
				// actions.setShowBanBtn(data.showBanBtn)
				// actions.setShowCleanupBtn(data.showCleanupBtn)
				// actions.setTileInfoAvailable(data.tileInfoAvailable)
				break;
			case "ban_click_success":
				// actions.toggleBanMode()
				break;
			case "kicked":
				this.state.disconnected = true;
				this.ws.close();
				// g_disconnected = true;
				// g_socket.close()
				// actions.setKickDialogText(data.message)
				// actions.setKickDialogButtonText(data.btn_text)
				// actions.setKickDialogVisibility(true)
				break;

			default:
				console.log("Unknown response received: " + JSON.stringify(data));
		}
		return false;
	}

	send_tile(x, y, c) {
		this.ws.send(struct('B37sHHH').pack(req.POST_TILE, this.state.user_id, x, y, c));
	}
}

try {
	const client = new PixelClient(url);
} catch (e) {
	console.error(e.message);
}
