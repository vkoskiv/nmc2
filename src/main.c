// Copyright (c) 2022 Valtteri Koskivuori (vkoskiv). All rights reserved.

#include "vendored/mongoose.h"
#include "vendored/cJSON.h"
#include <uuid/uuid.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#include <signal.h>

struct color {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
	uint16_t color_id;
};


// We default to the canvas size found in an existing db.
// If the db is new, we fall back to this value and create
// it based on that.
#define NEW_DB_CANVAS_SIZE 512

// These come from the original implementation here:
// https://github.com/vkoskiv/NoMansCanvas
static struct color g_color_list[] = {
	{255, 255, 255,  3},
	{221, 221, 221, 10},
	{117, 117, 117, 11},
	{  0,   0,   0,  4},
	{219,   0,   5,  0},
	{252, 145, 199,  8},
	{142,  87,  51, 12},
	{189, 161, 113, 16},
	{255, 153,  51,  7},
	{255, 255,   0,  9},
	{133, 222,  53,  1},
	{ 24, 181,   4,  6},
	{  0,   0, 255,  2},
	{ 13, 109, 187, 13},
	{ 26, 203, 213,  5},
	{195,  80, 222, 14},
	{110,   0, 108, 15},
};

#define COLOR_AMOUNT (sizeof(g_color_list) / sizeof(struct color))

// q&d linked list

struct list_elem {
	struct list_elem *next;
	size_t thing_size;
	void *thing;
};

struct list {
	struct list_elem *first;
};

#define LIST_INITIALIZER (struct list){ .first = NULL }

struct list_elem *_list_find_head(struct list *list) {
	if (!list->first) return NULL;
	struct list_elem *head = list->first;
	while (head->next) head = head->next;
	return head;
}

bool list_empty(struct list *list) {
	return !_list_find_head(list);
}

size_t list_elems(struct list *list) {
	size_t elems = 0;
	struct list_elem *head = list->first;
	while (head) {
		elems++;
		head = head->next;
	}
	return elems;
}

struct list_elem *list_new_elem(const void *thing, size_t thing_size) {
	struct list_elem *elem = calloc(1, sizeof(*elem));
	elem->thing_size = thing_size;
	elem->thing = calloc(1, elem->thing_size);
	memcpy(elem->thing, thing, elem->thing_size);
	return elem;
}

struct list_elem *_list_append(struct list *list, const void *thing, size_t thing_size) {
	if (!list) return NULL;
	if (!thing) return NULL;
	if (!thing_size) return NULL;
	if (!list->first) {
		list->first = list_new_elem(thing, thing_size);
		return list->first;
	}
	struct list_elem *head = _list_find_head(list);
	head->next = list_new_elem(thing, thing_size);
	return head->next;
}

void _list_remove(struct list *list, bool (*check_cb)(void *elem)) {
	struct list_elem *current = list->first;
	struct list_elem *prev = current;
	while (current) {
		if (check_cb(current->thing)) {
			prev->next = current->next;
			if (current == list->first) {
				list->first = current->next;
			}
			if (current->thing) free(current->thing);
			free(current);
			return;
		}
		prev = current;
		current = current->next;
	}
}

#define list_remove(list, ...) \
	bool check_##item(void *arg) __VA_ARGS__\
	_list_remove(&list, check_##item)

#define list_append(list, thing) _list_append(&list, &thing, sizeof(thing))
#define list_foreach(element, list) for (element = list.first; element; element = element->next)

// end linked list

struct user {
	char *user_name;
	char uuid[UUID_STR_LEN + 1];
	struct mg_connection *socket;
	struct mg_timer tile_increment_timer;
	bool is_authenticated;
	bool is_shadow_banned;
	
	uint32_t remaining_tiles;
	uint32_t max_tiles;
	uint32_t tile_regen_seconds;
	uint32_t total_tiles_placed;
	uint32_t tiles_to_next_level;
	uint32_t current_level_progress;
	uint32_t level;
	uint64_t last_connected_unix;
};

struct tile {
	uint32_t x;
	uint32_t y;
	uint32_t color_id;
	uint64_t place_time_unix;
};

struct canvas {
	struct list connected_users;
	uint8_t *tiles;
	uint32_t edge_length;
	struct mg_timer ws_ping_timer;
	sqlite3 *backing_db; // For persistence
};

// timing

void sleep_ms(int ms) {
	usleep(ms * 1000);
}

// end timing

static struct canvas g_canvas;
static bool g_running = true;

static const char *s_listen_on = "ws://0.0.0.0:3001";
static const char *s_web_root = ".";

void generate_uuid(char *buf) {
	if (!buf) return;
	uuid_t uuid;
	uuid_generate(uuid);
	uuid_unparse_upper(uuid, buf);
}

bool str_eq(const char *s1, const char *s2) {
	return strcmp(s1, s2) == 0;
}

char *str_cpy(const char *source) {
	char *copy = malloc(strlen(source) + 1);
	strcpy(copy, source);
	return copy;
}

void send_json(const cJSON *payload, struct user *user) {
	char *str = cJSON_PrintUnformatted(payload);
	if (!str) return;
	mg_ws_send(user->socket, str, strlen(str), WEBSOCKET_OP_TEXT);
	free(str);
}

void broadcast(const cJSON *payload) {
	//FIXME: Make list_foreach more ergonomic
	struct list_elem *elem = NULL;
	list_foreach(elem, g_canvas.connected_users) {
		struct user *user = (struct user *)elem->thing;
		send_json(payload, user);
	}
}

cJSON *base_response(const char *type) {
	cJSON *payload = cJSON_CreateObject();
	cJSON_AddStringToObject(payload, "responseType", type);
	return payload;
}

cJSON *error_response(char *error_message) {
	cJSON *error = cJSON_CreateObject();
	cJSON_AddStringToObject(error, "responseType", "error");
	cJSON_AddStringToObject(error, "errorMessage", error_message);
	return error;
}

void save_user(const struct user *user) {
	const char *sql = "UPDATE users SET username = ?, remainingTiles = ?, tileRegenSeconds = ?, totalTilesPlaced = ?, lastConnected = ?, level = ?, hasSetUsername = ?, isShadowBanned = ?, maxTiles = ?, tilesToNextLevel = ?, levelProgress = ? WHERE uuid = ?";
	printf("Saving with remainingTiles: %u\n", user->remaining_tiles);
	sqlite3_stmt *query;
	int ret = sqlite3_prepare_v2(g_canvas.backing_db, sql, -1, &query, 0);
	if (ret != SQLITE_OK) {
		printf("Failed to prepare user save query: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		sqlite3_finalize(query);
		sqlite3_close(g_canvas.backing_db);
		exit(-1);
	}
	int idx = 1;
	ret = sqlite3_bind_text(query, idx++, user->user_name, strlen(user->user_name), NULL);
	ret = sqlite3_bind_int(query, idx++, user->remaining_tiles);
	ret = sqlite3_bind_int(query, idx++, user->tile_regen_seconds);
	ret = sqlite3_bind_int(query, idx++, user->total_tiles_placed);
	ret = sqlite3_bind_int64(query, idx++, user->last_connected_unix);
	ret = sqlite3_bind_int(query, idx++, user->level);
	ret = sqlite3_bind_int(query, idx++, str_eq(user->user_name, "Anonymous") ? 1 : 0);
	ret = sqlite3_bind_int(query, idx++, user->is_shadow_banned);
	ret = sqlite3_bind_int(query, idx++, user->max_tiles);
	ret = sqlite3_bind_int(query, idx++, user->tiles_to_next_level);
	ret = sqlite3_bind_int(query, idx++, user->current_level_progress);
	ret = sqlite3_bind_text(query, idx++, user->uuid, strlen(user->uuid), NULL);

	ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to update user: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		sqlite3_finalize(query);
		sqlite3_close(g_canvas.backing_db);
		exit(-1);
	}
	sqlite3_finalize(query);
	printf("Saved user %s\n", user->uuid);
}

static void user_tile_increment_fn(void *arg) {
	struct user *user = (struct user *)arg;

	if (user->remaining_tiles < user->max_tiles) user->remaining_tiles++;
	save_user(user);
	cJSON *payload_wrapper = cJSON_CreateArray();
	cJSON *payload = base_response("incrementTileCount");
	cJSON_AddNumberToObject(payload, "amount", 1);
	cJSON_InsertItemInArray(payload_wrapper, 0, payload);
	send_json(payload_wrapper, user);
	cJSON_Delete(payload_wrapper);

	user->tile_increment_timer.period_ms = user->tile_regen_seconds * 1000;
}

void start_user_timer(struct user *user, struct mg_mgr *mgr) {
	printf("Starting timer for %s of %is\n", user->uuid, user->tile_regen_seconds);
	mg_timer_init(&user->tile_increment_timer, user->tile_regen_seconds * 1000, MG_TIMER_REPEAT, user_tile_increment_fn, user);
}

void add_user(const struct user *user) {
	sqlite3_stmt *query;
	const char *sql =
		"INSERT INTO users (username, uuid, remainingTiles, tileRegenSeconds, totalTilesPlaced, lastConnected, availableColors, level, hasSetUsername, isShadowBanned, maxTiles, tilesToNextLevel, levelProgress)"
		" VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
	int ret = sqlite3_prepare_v2(g_canvas.backing_db, sql, -1, &query, 0);
	if (ret != SQLITE_OK) {
		printf("Failed to prepare user insert query: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		sqlite3_finalize(query);
		sqlite3_close(g_canvas.backing_db);
		exit(-1);
	}
	int idx = 1;
	sqlite3_bind_text(query, idx++, user->user_name, strlen(user->user_name), NULL);
	sqlite3_bind_text(query, idx++, user->uuid, strlen(user->uuid), NULL);
	sqlite3_bind_int(query, idx++, user->remaining_tiles);
	sqlite3_bind_int(query, idx++, user->tile_regen_seconds);
	sqlite3_bind_int(query, idx++, user->total_tiles_placed);
	sqlite3_bind_int64(query, idx++, user->last_connected_unix);
	sqlite3_bind_text(query, idx++, "", 1, NULL); // Not sure if this is okay to do
	sqlite3_bind_int(query, idx++, user->level);
	sqlite3_bind_int(query, idx++, str_eq(user->user_name, "Anonymous") ? 1 : 0);
	sqlite3_bind_int(query, idx++, user->is_shadow_banned);
	sqlite3_bind_int(query, idx++, user->max_tiles);
	sqlite3_bind_int(query, idx++, user->tiles_to_next_level);
	sqlite3_bind_int(query, idx++, user->current_level_progress);

	ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to insert user: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		sqlite3_finalize(query);
		sqlite3_close(g_canvas.backing_db);
		exit(-1);
	}
	printf("Inserted user %s\n", user->uuid);
}

struct user *try_load_user(const char *uuid) {
	sqlite3_stmt *query;
	int ret = sqlite3_prepare_v2(g_canvas.backing_db, "SELECT * FROM users WHERE uuid = ?", -1, &query, 0);
	if (ret != SQLITE_OK) return NULL;
	ret = sqlite3_bind_text(query, 1, uuid, strlen(uuid), NULL);
	if (ret != SQLITE_OK) return NULL;
	int step = sqlite3_step(query);
	struct user *user = NULL;
	if (step == SQLITE_ROW) {
		size_t i = 1;
		user = calloc(1, sizeof(*user));
		const unsigned char *user_name = sqlite3_column_text(query, i++);
		user->user_name = str_cpy(user_name);
		const unsigned char *uuid = sqlite3_column_text(query, i++);
		strncpy(user->uuid, uuid, UUID_STR_LEN);
		user->remaining_tiles = sqlite3_column_int(query, i++);
		user->tile_regen_seconds = sqlite3_column_int(query, i++);
		user->total_tiles_placed = sqlite3_column_int(query, i++);
		user->last_connected_unix = sqlite3_column_int64(query, i += 2);
		user->level = sqlite3_column_int(query, i += 2);
		user->is_shadow_banned = sqlite3_column_int(query, i++);
		user->max_tiles = sqlite3_column_int(query, i++);
		user->tiles_to_next_level = sqlite3_column_int(query, i++);
		user->current_level_progress = sqlite3_column_int(query, i++);
	}
	sqlite3_finalize(query);
	return user;
}

struct user *check_and_fetch_user(const char *uuid) {
	struct list_elem *head = NULL;
	list_foreach(head, g_canvas.connected_users) {
		struct user *user = (struct user *)head->thing;
		if (str_eq(user->uuid, uuid)) return user;
	}
	return NULL;
}

void level_up(struct user *user) {
	user->level++;
	user->max_tiles += 100;
	user->tiles_to_next_level += 150;
	user->current_level_progress = 0;
	user->remaining_tiles = user->max_tiles;
	if (user->tile_regen_seconds > 10) {
		user->tile_regen_seconds--;
	}

	//Tell the client the good news :^)
	cJSON *payload_array = cJSON_CreateArray();
	cJSON *response = base_response("levelUp");
	cJSON_AddNumberToObject(response, "level", user->level);
	cJSON_AddNumberToObject(response, "maxTiles", user->max_tiles);
	cJSON_AddNumberToObject(response, "tilesToNextLevel", user->tiles_to_next_level);
	cJSON_AddNumberToObject(response, "levelProgress", user->current_level_progress);
	cJSON_AddNumberToObject(response, "remainingTiles", user->remaining_tiles);
	cJSON_InsertItemInArray(payload_array, 0, response);
	send_json(payload_array, user);
	cJSON_Delete(payload_array);
}

void dump_connected_users(void) {
	struct list_elem *head = NULL;
	printf("Connected users:\n");
	list_foreach(head, g_canvas.connected_users) {
		struct user *user = (struct user *)head->thing;
		printf("\t%s\n", user->uuid);
	}
	printf("\n");
}

cJSON *handle_initial_auth(struct mg_connection *socket) {
	struct user user = {
		.user_name = str_cpy("Anonymous"),
		.socket = socket,
		.is_authenticated = true,
		.remaining_tiles = 60,
		.max_tiles = 250,
		.tile_regen_seconds = 10,
		.total_tiles_placed = 0,
		.tiles_to_next_level = 100,
		.current_level_progress = 0,
		.level = 1,
		.last_connected_unix = 0,
	};
	generate_uuid(user.uuid);
	//FIXME: Do rate limiting
	struct user *uptr = list_append(g_canvas.connected_users, user)->thing;
	add_user(uptr);
	dump_connected_users();

	start_user_timer(uptr, socket->mgr);

	// Again, a weird API because I didn't know what I was doing in 2017.
	// An array with a single object that contains the response
	cJSON *response_array = cJSON_CreateArray();
	cJSON *response = base_response("authSuccessful");
	cJSON_AddStringToObject(response, "uuid", uptr->uuid);
	cJSON_AddNumberToObject(response, "remainingTiles", uptr->remaining_tiles);
	cJSON_AddNumberToObject(response, "level", uptr->level);
	cJSON_AddNumberToObject(response, "maxTiles", uptr->max_tiles);
	cJSON_AddNumberToObject(response, "tilesToNextLevel", uptr->tiles_to_next_level);
	cJSON_AddNumberToObject(response, "levelProgress", uptr->current_level_progress);
	cJSON_InsertItemInArray(response_array, 0, response);
	return response_array;
}

cJSON *handle_auth(const cJSON *user_id, struct mg_connection *socket) {
	if (!cJSON_IsString(user_id)) return error_response("Invalid userID");
	if (strlen(user_id->valuestring) > UUID_STR_LEN) return error_response("Invalid userID");

	//FIXME: Awkward copy & free
	struct user *user = try_load_user(user_id->valuestring);
	if (!user) return error_response("Invalid userID");
	struct user *uptr = list_append(g_canvas.connected_users, *user)->thing;
	free(user);
	dump_connected_users();
	uptr->socket = socket;

	// Kinda pointless flag. If this thing is in connected_users list, it's valid.
	uptr->is_authenticated = true;

	start_user_timer(uptr, socket->mgr);

	size_t sec_since_last_connected = (unsigned)time(NULL) - uptr->last_connected_unix;
	size_t tiles_to_add = sec_since_last_connected / uptr->tile_regen_seconds;
	// This is how it was in the original, might want to check
	uptr->remaining_tiles += tiles_to_add > uptr->max_tiles ? uptr->max_tiles - uptr->remaining_tiles : tiles_to_add;

	save_user(uptr);

	// Again, a weird API because I didn't know what I was doing in 2017.
	// An array with a single object that contains the response
	cJSON *response_array = cJSON_CreateArray();
	cJSON *response = base_response("reAuthSuccessful");
	cJSON_AddNumberToObject(response, "remainingTiles", uptr->remaining_tiles);
	cJSON_AddNumberToObject(response, "level", uptr->level);
	cJSON_AddNumberToObject(response, "maxTiles", uptr->max_tiles);
	cJSON_AddNumberToObject(response, "tilesToNextLevel", uptr->tiles_to_next_level);
	cJSON_AddNumberToObject(response, "levelProgress", uptr->current_level_progress);
	cJSON_InsertItemInArray(response_array, 0, response);
	return response_array;
}

cJSON *handle_get_canvas(const cJSON *user_id) {
	(void)user_id; //TODO: Validate user
	cJSON *canvas_array = cJSON_CreateArray();
	cJSON *response = base_response("fullCanvas");
	cJSON_InsertItemInArray(canvas_array, 0, response);
	size_t tiles = g_canvas.edge_length * g_canvas.edge_length;
	for (size_t i = 0; i < tiles; ++i) {
		cJSON_AddItemToArray(canvas_array, cJSON_CreateNumber(g_canvas.tiles[i]));
	}
	return canvas_array;
}

cJSON *new_tile_update(size_t x, size_t y, size_t color_id) {
	cJSON *payload_array = cJSON_CreateArray();
	cJSON *payload = base_response("tileUpdate");
	cJSON_AddNumberToObject(payload, "X", x);
	cJSON_AddNumberToObject(payload, "Y", y);
	cJSON_AddNumberToObject(payload, "colorID", color_id);
	cJSON_InsertItemInArray(payload_array, 0, payload);
	return payload_array;
}

void persist_tile_state(size_t x, size_t y, size_t color_id, const char *placer) {
	const char *sql = "UPDATE tiles SET colorID = ?, lastModifier = ?, placeTime = ? WHERE X = ? AND Y = ?";
	if (!sqlite3_get_autocommit(g_canvas.backing_db)) {
		printf("HOX! Not in autocommit mode!\n");
	}
	printf("Persisting %lu, %lu, id %lu\n", x, y, color_id);
	sqlite3_stmt *query;
	int ret = sqlite3_prepare_v2(g_canvas.backing_db, sql, -1, &query, NULL);
	if (ret != SQLITE_OK) {
		printf("Failed to prepare tile save query: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		sqlite3_finalize(query);
		sqlite3_close(g_canvas.backing_db);
		exit(-1);
	}
	int idx = 1;
	ret = sqlite3_bind_int(query, idx++, color_id);
	ret = sqlite3_bind_text(query, idx++, placer, strlen(placer), NULL);
	ret = sqlite3_bind_int(query, idx++, (unsigned)time(NULL));
	ret = sqlite3_bind_int(query, idx++, x);
	ret = sqlite3_bind_int(query, idx++, y);
	
	printf("expanded: %s\n", sqlite3_expanded_sql(query));
	
	ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to persist tile: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		sqlite3_finalize(query);
		sqlite3_close(g_canvas.backing_db);
		exit(-1);
	}
	sqlite3_finalize(query);
	printf("Persisted tile, affected %i rows.\n", sqlite3_changes(g_canvas.backing_db));
}

cJSON *handle_post_tile(const cJSON *user_id, const cJSON *x_param, const cJSON *y_param, const cJSON *color_id_param) {
	if (!cJSON_IsString(user_id)) return error_response("Invalid userID");
	if (!cJSON_IsNumber(x_param)) return error_response("X coordinate not a number");
	if (!cJSON_IsNumber(y_param)) return error_response("Y coordinate not a number");
	if (!cJSON_IsString(color_id_param)) return error_response("colorID not a string");

	//Another ugly detail, the client sends the colorID number as a string...
	uintmax_t num = strtoumax(color_id_param->valuestring, NULL, 10);
	if (num == UINTMAX_MAX && errno == ERANGE) return error_response("colorID not a valid number in a string");

	size_t color_id = num;
	size_t x = x_param->valueint;
	size_t y = y_param->valueint;

	if (x > g_canvas.edge_length - 1) return error_response("Invalid X coordinate");
	if (y > g_canvas.edge_length - 1) return error_response("Invalid Y coordinate");
	if (color_id > COLOR_AMOUNT - 1) return error_response("Invalid colorID");
	
	struct user *user = check_and_fetch_user(user_id->valuestring);
	if (!user) return error_response("Invalid userID");
	if (user->remaining_tiles < 1) return error_response("No tiles remaining");
	
	user->remaining_tiles--;
	user->total_tiles_placed++;
	user->current_level_progress++;
	if (user->current_level_progress >= user->tiles_to_next_level) {
		level_up(user);
	}
	save_user(user);

	g_canvas.tiles[x + y * g_canvas.edge_length] = color_id;
	persist_tile_state(x, y, color_id, user->uuid);
	cJSON *update = new_tile_update(x, y, color_id);
	broadcast(update);
	cJSON_Delete(update);
	return NULL; // The broadcast takes care of this
}

cJSON *color_to_json(struct color color) {
	cJSON *c = cJSON_CreateObject();
	cJSON_AddNumberToObject(c, "R", color.red);
	cJSON_AddNumberToObject(c, "G", color.green);
	cJSON_AddNumberToObject(c, "B", color.blue);
	cJSON_AddNumberToObject(c, "ID", color.color_id);
	return c;
}

// The API is a bit weird. Instead of having a responsetype and an array in an object
// we have an array where the first object is an object containing the responsetype, rest
// are objects containing colors. *shrug*
cJSON *handle_get_colors(const cJSON *user_id) {
	(void)user_id; //TODO: Validate
	//TODO: Might as well cache this color list instead of building it every time.
	cJSON *color_list = cJSON_CreateArray();
	cJSON *response_object = base_response("colorList");
	cJSON_InsertItemInArray(color_list, 0, response_object);
	for (size_t i = 0; i < COLOR_AMOUNT; ++i) {
		cJSON_InsertItemInArray(color_list, i + 1, color_to_json(g_color_list[i]));
	}
	return color_list;
}

cJSON *handle_command(const char *cmd, size_t len, struct mg_connection *connection) {
	cJSON *command = cJSON_ParseWithLength(cmd, len);
	if (!command) return error_response("No command provided");
	const cJSON *request_type = cJSON_GetObjectItem(command, "requestType");
	if (!cJSON_IsString(request_type)) return error_response("No requestType provided");
	printf("%d Received request: %s\n", (unsigned)time(NULL), cmd);
	
	const cJSON *user_id = cJSON_GetObjectItem(command, "userID");
	const cJSON *x = cJSON_GetObjectItem(command, "X");
	const cJSON *y = cJSON_GetObjectItem(command, "Y");
	const cJSON *color_id = cJSON_GetObjectItem(command, "colorID");

	char *reqstr = request_type->valuestring;

	cJSON *response = NULL;
	if (str_eq(reqstr, "initialAuth")) {
		response = handle_initial_auth(connection);
	} else if (str_eq(reqstr, "auth")) {
		response = handle_auth(user_id, connection);
	} else if (str_eq(reqstr, "getCanvas")) {
		response = handle_get_canvas(user_id);
	} else if (str_eq(reqstr, "postTile")) {
		response = handle_post_tile(user_id, x, y, color_id);
	} else if (str_eq(reqstr, "getColors")) {
		response = handle_get_colors(user_id);
	} else {
		response = error_response("Unknown requestType");
	}

	cJSON_Delete(command);
	return response;
}

void dump_client_conns(void) {
	struct list_elem *elem = NULL;
	list_foreach(elem, g_canvas.connected_users) {
		struct user *user = (struct user *)elem->thing;
		printf("\t%s\n", user->uuid);
	}
}

void drop_user_with_connection(struct mg_connection *c) {
	struct list_elem *elem = NULL;
	list_foreach(elem, g_canvas.connected_users) {
		struct user *user = (struct user *)elem->thing;
		printf("%p\n", (void*)c);
		if (user->socket != c) continue;
		printf("!!!!! User %s disconnected.\n", user->uuid);
		mg_timer_free(&user->tile_increment_timer);
		free(user->user_name);
		list_remove(g_canvas.connected_users, {
			struct user *list_user = (struct user *)arg;
			return str_eq(list_user->uuid, user->uuid);
		});
		break;
	}
}

static void callback_fn(struct mg_connection *c, int event_type, void *event_data, void *arg) {
	(void)arg; // TODO: Pass around a canvas instead of having that global up there
	if (event_type == MG_EV_HTTP_MSG) {
		struct mg_http_message *msg = (struct mg_http_message *)event_data;
		if (mg_http_match_uri(msg, "/ws")) {
			mg_ws_upgrade(c, msg, NULL);
		} else if (mg_http_match_uri(msg, "/canvas")) {
			//TODO: Return canvas encoded as a PNG
			mg_http_reply(c, 200, "", "Unimplemented");
		} else if (mg_http_match_uri(msg, "/message")) {
			//TODO: Handle message
			mg_http_reply(c, 200, "", "Unimplemented");
		} else if (mg_http_match_uri(msg, "/shutdown")) {
			//TODO: Handle shutdown
			mg_http_reply(c, 200, "", "Unimplemented");
		}
	} else if (event_type == MG_EV_WS_MSG) {
		struct mg_ws_message *wm = (struct mg_ws_message *)event_data;
		cJSON *response = handle_command(wm->data.ptr, wm->data.len, c);
		char *response_str = cJSON_PrintUnformatted(response);
		if (response_str) {
			mg_ws_send(c, response_str, strlen(response_str), WEBSOCKET_OP_TEXT);
			free(response_str);
		}
		cJSON_Delete(response);
	} else if (event_type == MG_EV_CLOSE) {
		drop_user_with_connection(c);
	}
}

static void ping_timer_fn(void *arg) {
	struct mg_mgr *mgr = (struct mg_mgr *)arg;
	for (struct mg_connection *c = mgr->conns; c != NULL && c->is_websocket; c = c->next) {
		//printf("%d Sending PING to connection %lu with label %s\n", (unsigned)time(NULL), c->id, c->label);
		mg_ws_send(c, NULL, 0, WEBSOCKET_OP_PING);
	}
}

void ensure_tiles_table(sqlite3 *db) {
	sqlite3_stmt *query;
	char *schema =
		"CREATE TABLE IF NOT EXISTS `tiles` ("
		"`id` integer  NOT NULL PRIMARY KEY AUTOINCREMENT"
		",  `X` integer NOT NULL"
		",  `Y` integer NOT NULL"
		",  `colorID` integer NOT NULL"
		",  `lastModifier` varchar(255) NOT NULL"
		",  `placeTime` integer NOT NULL"
		")";

	sqlite3_prepare_v2(db, schema, -1, &query, NULL);
	int ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to create tiles table\n");
		sqlite3_finalize(query);
		sqlite3_close(db);
		exit(-1);
	}
	sqlite3_finalize(query);
	// Next, ensure we've got tiles in there.
	sqlite3_stmt *count;
	ret = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tiles", -1, &count, NULL);
	if (ret != SQLITE_OK) {
		printf("Failed to create count query\n");
		sqlite3_finalize(count);
		sqlite3_close(db);
		exit(-1);
	}
	ret = sqlite3_step(count);
	if (ret != SQLITE_ROW) {
		printf("No rows in tile init COUNT(*)\n");
		sqlite3_finalize(count);
		sqlite3_close(db);
		exit(-1);
	}
	size_t rows = sqlite3_column_int(count, 0);
	sqlite3_finalize(count);

	sqlite3_stmt *bt;
	sqlite3_prepare_v2(db, "BEGIN TRANSACTION", -1, &bt, NULL);
	ret = sqlite3_step(bt);
	if (ret != SQLITE_DONE) {
		printf("Failed to begin transaction\n");
		sqlite3_finalize(bt);
		sqlite3_close(db);
		exit(-1);
	}
	sqlite3_finalize(bt);

	if (rows > 0) return;

	printf("%u Running initial tile db init...\n", (unsigned)time(NULL));
	sqlite3_stmt *insert;
	ret = sqlite3_prepare_v2(db, "INSERT INTO tiles (X, Y, colorID, lastModifier, placeTime) VALUES (?, ?, 3, \"\", 0)", -1, &insert, NULL);
	if (ret != SQLITE_OK) printf("Failed to prepare tile insert: %s\n", sqlite3_errmsg(db));

	for (size_t y = 0; y < NEW_DB_CANVAS_SIZE; ++y) {
		for (size_t x = 0; x < NEW_DB_CANVAS_SIZE; ++x) {
			ret = sqlite3_bind_int(insert, 1, x);
			if (ret != SQLITE_OK) printf("Failed to bind x: %s\n", sqlite3_errmsg(db));
			ret = sqlite3_bind_int(insert, 2, y);
			if (ret != SQLITE_OK) printf("Failed to bind y: %s\n", sqlite3_errmsg(db));
			int res = sqlite3_step(insert);
			if (res != SQLITE_DONE) {
				printf("Failed to insert for x = %lu, y = %lu\n", x, y);
				sqlite3_finalize(insert);
				sqlite3_close(db);
				exit(-1);
			}
			sqlite3_clear_bindings(insert);
			sqlite3_reset(insert);
		}
	}
	sqlite3_finalize(insert);

	sqlite3_stmt *et;
	sqlite3_prepare_v2(db, "COMMIT", -1, &et, NULL);
	ret = sqlite3_step(et);
	if (ret != SQLITE_DONE) {
		printf("Failed to commit transaction\n");
		sqlite3_finalize(et);
		sqlite3_close(db);
		exit(-1);
	}
	sqlite3_finalize(et);

	printf("%u db init done.\n", (unsigned)time(NULL));
}

void ensure_users_table(sqlite3 *db) {
	sqlite3_stmt *query;
	char *schema =
				"CREATE TABLE IF NOT EXISTS `users` ("
  				"`id` integer  NOT NULL PRIMARY KEY AUTOINCREMENT"
				",  `username` varchar(255) NOT NULL"
				",  `uuid` varchar(255) NOT NULL"
				",  `remainingTiles` integer NOT NULL"
				",  `tileRegenSeconds` integer NOT NULL"
				",  `totalTilesPlaced` integer NOT NULL"
				",  `lastConnected` integer NOT NULL"
				",  `availableColors` varchar(255) NOT NULL"
				",  `level` integer NOT NULL"
				",  `hasSetUsername` integer  NOT NULL"
				",  `isShadowBanned` integer  NOT NULL"
				",  `maxTiles` integer NOT NULL"
				",  `tilesToNextLevel` integer NOT NULL"
				",  `levelProgress` integer NOT NULL"
				")";
	sqlite3_prepare_v2(db, schema, -1, &query, NULL);
	int ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to create users table: %s\n", sqlite3_errmsg(db));
		sqlite3_finalize(query);
		sqlite3_close(db);
		exit(-1);
	}
}

void ensure_valid_db(sqlite3 *db) {
	ensure_tiles_table(db);
	ensure_users_table(db);
}

bool load_tiles(struct canvas *c) {
	//First figure out how many tiles there are to get the canvas size
	printf("Getting tile count\n");
	sqlite3_stmt *count_query;
	int ret = sqlite3_prepare_v2(c->backing_db, "SELECT COUNT(*) FROM tiles", -1, &count_query, NULL);
	if (ret != SQLITE_OK) {
		printf("Failed\n");
		sqlite3_finalize(count_query);
		return true;
	}
	ret = sqlite3_step(count_query);
	if (ret != SQLITE_ROW) {
		printf("No rows in COUNT(*)\n");
		sqlite3_finalize(count_query);
		return true;
	}
	size_t rows = sqlite3_column_int(count_query, 0);
	c->edge_length = sqrt(rows);
	sqlite3_finalize(count_query);

	g_canvas.tiles = calloc(g_canvas.edge_length * g_canvas.edge_length, 1);
	g_canvas.connected_users = LIST_INITIALIZER;
	printf("Loading %ux%u canvas...\n", c->edge_length, c->edge_length);

	sqlite3_stmt *query;
	sqlite3_prepare_v2(c->backing_db, "select * from tiles", -1, &query, NULL);

	while (sqlite3_step(query) != SQLITE_DONE) {
		size_t x = sqlite3_column_int(query, 1);
		size_t y = sqlite3_column_int(query, 2);
		size_t id = sqlite3_column_int(query, 3);
		//FIXME: Figure out if we care about these two anymore
		//char *last_modifier = sqlite3_column_text(query, 4);
		//uint64_t place_time = sqlite3_column_int64(query, 5);
		g_canvas.tiles[x + y * g_canvas.edge_length] = id;
	}
	sqlite3_finalize(query);
	return false;
}

bool set_up_db(struct canvas *c) {
	int ret = 0;
	//ret = sqlite3_open("pikselit2022.db", &c->backing_db);
	//ret = sqlite3_open(":memory:", &c->backing_db);
	ret = sqlite3_open("asdf.db", &c->backing_db);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(c->backing_db));
		sqlite3_close(c->backing_db);
		return true;
	}

	ensure_valid_db(c->backing_db);
	load_tiles(c);

	return false;
}

// Intersect ^C to safely shut down
void (*signal(int signo, void (*func)(int)))(int);
typedef void sigfunc(int);
sigfunc *signal(int, sigfunc*);

void sigint_handler(int sig) {
	if (sig == 2) {
		printf("Received SIGINT, stopping.\n");
		g_running = false;
	}
}

int main(void) {
	if (signal(SIGINT, sigint_handler) == SIG_ERR) {
		printf("Failed to register sigint handler\n");
		return -1;
	}
	printf("Using SQLite v%s\n", sqlite3_libversion());
	if (set_up_db(&g_canvas)) {
		printf("Failed to set up db\n");
		return -1;
	}
	struct mg_mgr mgr;
	mg_mgr_init(&mgr);
	//ws ping loop. TODO: Probably do this from the client side instead.
	mg_timer_init(&g_canvas.ws_ping_timer, 25000, MG_TIMER_REPEAT, ping_timer_fn, &mgr);
	printf("Starting WS listener on %s/ws\n", s_listen_on);
	mg_http_listen(&mgr, s_listen_on, callback_fn, NULL);
	while (g_running) mg_mgr_poll(&mgr, 1000);
	printf("Closing db\n");
	mg_mgr_free(&mgr);
	free(g_canvas.tiles);
	sqlite3_close(g_canvas.backing_db);
	return 0;
}
