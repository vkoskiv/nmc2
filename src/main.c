// Copyright (c) 2022 Valtteri Koskivuori (vkoskiv). All rights reserved.

// Some platforms just don't have this for whatever reason
#ifndef UUID_STR_LEN
#define UUID_STR_LEN 37
#endif

#include "vendored/mongoose.h"
#include "vendored/cJSON.h"
#include "base64.h"
#include "linked_list.h"
#include "logging.h"
#include "fileio.h"
#include <uuid/uuid.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <zlib.h>

struct color {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
	uint16_t color_id;
};

// Compile-time constants
#define MAX_NICK_LEN 64

// Useful for testing, but be careful with this
//#define DISABLE_RATE_LIMITING

struct rate_limiter {
	struct timeval last_event_time;
	float current_allowance;
	float max_rate;
	float per_seconds;
};

struct user {
	char user_name[MAX_NICK_LEN];
	char uuid[UUID_STR_LEN + 1];
	struct mg_connection *socket;
	struct mg_timer tile_increment_timer;
	bool is_authenticated;
	bool is_shadow_banned;

	struct rate_limiter canvas_limiter;
	struct rate_limiter tile_limiter;
	
	uint32_t remaining_tiles;
	uint32_t max_tiles;
	uint32_t tile_regen_seconds;
	uint32_t total_tiles_placed;
	uint32_t tiles_to_next_level;
	uint32_t current_level_progress;
	uint32_t level;
	uint64_t last_connected_unix;
	uint64_t last_event_unix;
};

struct tile {
	uint32_t color_id;
	uint64_t place_time_unix;
	char last_modifier[UUID_STR_LEN];
};

struct params {
	size_t new_db_canvas_size;
	float getcanvas_max_rate;
	float getcanvas_per_seconds;
	float setpixel_max_rate;
	float setpixel_per_seconds;
	size_t max_users_per_ip;
	size_t canvas_save_interval_sec;
	size_t websocket_ping_interval_sec;
	size_t users_save_interval_sec;
	size_t kick_inactive_after_sec;
	char listen_url[128];
	char dbase_file[PATH_MAX];
};

struct color_list {
	struct color *colors;
	size_t amount;
};

struct tile_placement {
	size_t x;
	size_t y;
	struct tile tile;
};

struct administrator {
	char uuid[UUID_STR_LEN + 1];
	bool can_shutdown;
	bool can_announce;
	bool can_shadowban;
	bool can_banclick;
};

struct canvas {
	struct list connected_users;
	struct list connected_hosts;
	struct list administrators;
	struct list delta;
	struct tile *tiles;
	bool dirty;
	uint32_t edge_length;
	struct mg_timer ws_ping_timer;
	struct mg_timer canvas_save_timer;
	struct mg_timer users_save_timer;
	sqlite3 *backing_db; // For persistence
	struct params settings;
	struct color_list color_list;
	char *color_response_cache;
};

// rate limiting

struct remote_host {
	struct mg_addr addr;
	size_t total_accounts;
};

long get_ms_delta(struct timeval timer) {
	struct timeval tmr2;
	gettimeofday(&tmr2, NULL);
	return 1000 * (tmr2.tv_sec - timer.tv_sec) + ((tmr2.tv_usec - timer.tv_usec) / 1000);
}

// 'Token bucket' algorithm
// This particular implementation is adapted from this SO answer:
// https://stackoverflow.com/a/668327
bool is_within_rate_limit(struct rate_limiter *limiter) {
#ifdef DISABLE_RATE_LIMITING
	(void)limiter;
	return true;
#else
	bool is_within_limit = true;
	long ms_since_last_tile = get_ms_delta(limiter->last_event_time);
	gettimeofday(&limiter->last_event_time, NULL);
	float secs_since_last = (float)ms_since_last_tile / 1000.0f;
	
	limiter->current_allowance += secs_since_last * (limiter->max_rate / limiter->per_seconds);
	if (limiter->current_allowance > limiter->max_rate) limiter->current_allowance = limiter->max_rate;
	if (limiter->current_allowance < 1.0f) {
		is_within_limit = false;
	} else {
		limiter->current_allowance -= 1.0f;
	}

	return is_within_limit;
#endif
}

// end rate limiting

// timing

void sleep_ms(int ms) {
	usleep(ms * 1000);
}

// end timing

static struct canvas g_canvas;
static bool g_running = true;

cJSON *color_to_json(struct color color) {
	cJSON *c = cJSON_CreateObject();
	cJSON_AddNumberToObject(c, "R", color.red);
	cJSON_AddNumberToObject(c, "G", color.green);
	cJSON_AddNumberToObject(c, "B", color.blue);
	cJSON_AddNumberToObject(c, "ID", color.color_id);
	return c;
}

cJSON *base_response(const char *type) {
	cJSON *payload = cJSON_CreateObject();
	cJSON_AddStringToObject(payload, "rt", type);
	return payload;
}

void update_color_response_cache(struct canvas *c) {
	if (c->color_response_cache) free(c->color_response_cache);

	cJSON *response_object = base_response("colorList");
	cJSON *color_list = cJSON_CreateArray();
	for (size_t i = 0; i < c->color_list.amount; ++i) {
		cJSON_InsertItemInArray(color_list, i + 1, color_to_json(c->color_list.colors[i]));
	}
	cJSON_AddItemToObject(response_object, "colors", color_list);
	c->color_response_cache = cJSON_PrintUnformatted(response_object);
	cJSON_Delete(response_object);
}

//TODO: Restart timers when those change
void load_config(struct canvas *c) {
	size_t file_bytes;
	char *conf = load_file("params.json", &file_bytes);
	if (!conf) {
		logr("params.json not found, exiting.\n");
		goto bail;
	}
	cJSON *config = cJSON_ParseWithLength(conf, file_bytes);
	if (!config) {
		logr("Failed to load params.json contents, exiting.\n");
		logr("Error before: %s\n", cJSON_GetErrorPtr());
		goto bail;
	}
	const cJSON *canvas_size = cJSON_GetObjectItem(config, "new_db_canvas_size");
	if (!cJSON_IsNumber(canvas_size)) {
		logr("new_db_canvas_size not a number, exiting.\n");
		goto bail;
	}
	const cJSON *gc_maxrate  = cJSON_GetObjectItem(config, "getcanvas_max_rate");
	if (!cJSON_IsNumber(gc_maxrate)) {
		logr("getcanvas_max_rate not a number, exiting.\n");
		goto bail;
	}
	const cJSON *gc_persecs  = cJSON_GetObjectItem(config, "getcanvas_per_seconds");
	if (!cJSON_IsNumber(gc_persecs)) {
		logr("getcanvas_per_seconds not a number, exiting.\n");
		goto bail;
	}
	const cJSON *sp_maxrate  = cJSON_GetObjectItem(config, "setpixel_max_rate");
	if (!cJSON_IsNumber(sp_maxrate)) {
		logr("setpixel_max_rate not a number, exiting.\n");
		goto bail;
	}
	const cJSON *sp_persecs  = cJSON_GetObjectItem(config, "setpixel_per_seconds");
	if (!cJSON_IsNumber(sp_persecs)) {
		logr("setpixel_per_seconds not a number, exiting.\n");
		goto bail;
	}
	const cJSON *max_users   = cJSON_GetObjectItem(config, "max_users_per_ip");
	if (!cJSON_IsNumber(max_users)) {
		logr("max_users_per_ip not a number, exiting.\n");
		goto bail;
	}
	const cJSON *cs_interval = cJSON_GetObjectItem(config, "canvas_save_interval_sec");
	if (!cJSON_IsNumber(cs_interval)) {
		logr("canvas_save_interval_sec not a number, exiting.\n");
		goto bail;
	}
	const cJSON *wp_interval = cJSON_GetObjectItem(config, "websocket_ping_interval_sec");
	if (!cJSON_IsNumber(wp_interval)) {
		logr("websocket_ping_interval_sec not a number, exiting.\n");
		goto bail;
	}
	const cJSON *us_interval = cJSON_GetObjectItem(config, "users_save_interval_sec");
	if (!cJSON_IsNumber(us_interval)) {
		logr("users_save_interval_sec not a number, exiting\n");
		goto bail;
	}
	const cJSON *kick_secs = cJSON_GetObjectItem(config, "kick_inactive_after_sec");
	if (!cJSON_IsNumber(kick_secs)) {
		logr("kick_inactive_after_sec not a number, exiting.\n");
		goto bail;
	}
	const cJSON *administrators  = cJSON_GetObjectItem(config, "administrators");
	if (!cJSON_IsArray(administrators)) {
		logr("administrators not an array, exiting.\n");
		goto bail;
	}
	const cJSON *listen_url  = cJSON_GetObjectItem(config, "listen_url");
	if (!cJSON_IsString(listen_url)) {
		logr("listen_url not a string, exiting.\n");
		goto bail;
	}
	const cJSON *dbase_file = cJSON_GetObjectItem(config, "dbase_file");
	if (!cJSON_IsString(dbase_file)) {
		logr("dbase_file not a string, exiting.\n");
		goto bail;
	}
	const cJSON *colors = cJSON_GetObjectItem(config, "colors");
	if (!cJSON_IsArray(colors)) {
		logr("colors not an array, exiting\n");
		goto bail;
	}

	c->settings.new_db_canvas_size = canvas_size->valueint;
	c->settings.getcanvas_max_rate = gc_maxrate->valuedouble;
	c->settings.getcanvas_per_seconds = gc_persecs->valuedouble;
	c->settings.setpixel_max_rate = sp_maxrate->valuedouble;
	c->settings.setpixel_per_seconds = sp_persecs->valuedouble;
	c->settings.max_users_per_ip = max_users->valueint;
	c->settings.canvas_save_interval_sec = cs_interval->valueint;
	c->settings.websocket_ping_interval_sec = wp_interval->valueint;
	c->settings.users_save_interval_sec = us_interval->valueint;
	c->settings.kick_inactive_after_sec = kick_secs->valueint;
	strncpy(c->settings.listen_url, listen_url->valuestring, sizeof(c->settings.listen_url) - 1);
	strncpy(c->settings.dbase_file, dbase_file->valuestring, sizeof(c->settings.dbase_file) - 1);

	// Load up administrator list
	list_destroy(&g_canvas.administrators);
	cJSON *admin = NULL;
	cJSON_ArrayForEach(admin, administrators) {
		if (!cJSON_IsObject(admin)) continue;
		cJSON *uuid = cJSON_GetObjectItem(admin, "uuid");
		cJSON *shutdown = cJSON_GetObjectItem(admin, "shutdown");
		cJSON *announce = cJSON_GetObjectItem(admin, "announce");
		cJSON *shadowban = cJSON_GetObjectItem(admin, "shadowban");
		cJSON *banclick = cJSON_GetObjectItem(admin, "banclick");
		if (!cJSON_IsString(uuid)) continue;
		if (!cJSON_IsBool(shutdown)) continue;
		if (!cJSON_IsBool(announce)) continue;
		if (!cJSON_IsBool(shadowban)) continue;
		if (!cJSON_IsBool(banclick)) continue;
		struct administrator a = (struct administrator){
			.can_shutdown = shutdown->valueint,
			.can_announce = announce->valueint,
			.can_shadowban = shadowban->valueint,
			.can_banclick = banclick->valueint
		};
		strncpy(a.uuid, uuid->valuestring, sizeof(a.uuid) - 1);
		list_append(g_canvas.administrators, a);
	}

	if (c->color_list.colors) free(c->color_list.colors);
	c->color_list.amount = cJSON_GetArraySize(colors);
	c->color_list.colors = calloc(c->color_list.amount, sizeof(struct color));
	for (size_t i = 0; i < c->color_list.amount; ++i) {
		cJSON *color = cJSON_GetArrayItem(colors, i);
		if (!cJSON_IsArray(color) || cJSON_GetArraySize(color) != 4) {
			logr("Color at index %lu not an array of format [R,G,B,id]\n", i);
			goto bail;
		}
		c->color_list.colors[i] = (struct color){
			cJSON_GetArrayItem(color, 0)->valueint,
			cJSON_GetArrayItem(color, 1)->valueint,
			cJSON_GetArrayItem(color, 2)->valueint,
			cJSON_GetArrayItem(color, 3)->valueint,
		};
	}

	update_color_response_cache(c);

	logr("Loaded conf:\n%s\n", conf);
	cJSON_Delete(config);
	free(conf);
	return;
bail:
	logr("params.json invalid, exiting.\n");
	if (c->backing_db) sqlite3_close(c->backing_db);
	exit(-1);
}

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

void send_json(const cJSON *payload, const struct user *user) {
	char *str = cJSON_PrintUnformatted(payload);
	if (!str) return;
	mg_ws_send(user->socket, str, strlen(str), WEBSOCKET_OP_TEXT);
	free(str);
}

void broadcast(const cJSON *payload) {
	//FIXME: Make list_foreach more ergonomic
	struct list_elem *elem = NULL;
	list_foreach_ro(elem, g_canvas.connected_users) {
		struct user *user = (struct user *)elem->thing;
		send_json(payload, user);
	}
}

cJSON *error_response(char *error_message) {
	cJSON *error = cJSON_CreateObject();
	cJSON_AddStringToObject(error, "responseType", "error");
	cJSON_AddStringToObject(error, "errorMessage", error_message);
	return error;
}

void save_host(const struct remote_host *host) {
	const char *sql = "UPDATE hosts SET total_accounts = ? WHERE ip_address = ?";
	sqlite3_stmt *query;
	int ret = sqlite3_prepare_v2(g_canvas.backing_db, sql, -1, &query, 0);
	if (ret != SQLITE_OK) {
		printf("Failed to prepare host save query: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		sqlite3_finalize(query);
		sqlite3_close(g_canvas.backing_db);
		exit(-1);
	}
	int idx = 1;
	char ipbuf[100];
	mg_ntoa(&host->addr, ipbuf, sizeof(ipbuf));
	ret = sqlite3_bind_int(query, idx++, host->total_accounts);
	ret = sqlite3_bind_text(query, idx++, ipbuf, strlen(ipbuf), NULL);

	ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to update host: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		sqlite3_finalize(query);
		sqlite3_close(g_canvas.backing_db);
		exit(-1);
	}
	sqlite3_finalize(query);
}

void save_user(const struct user *user) {
	const char *sql = "UPDATE users SET username = ?, remainingTiles = ?, tileRegenSeconds = ?, totalTilesPlaced = ?, lastConnected = ?, level = ?, hasSetUsername = ?, isShadowBanned = ?, maxTiles = ?, tilesToNextLevel = ?, levelProgress = ?, cl_last_event_sec = ?, cl_last_event_usec = ?, cl_current_allowance = ?, cl_max_rate = ?, cl_per_seconds = ?, tl_last_event_sec = ?, tl_last_event_usec = ?, tl_current_allowance = ?, tl_max_rate = ?, tl_per_seconds = ? WHERE uuid = ?";
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
	ret = sqlite3_bind_int(query, idx++, user->canvas_limiter.last_event_time.tv_sec);
	ret = sqlite3_bind_int64(query, idx++, user->canvas_limiter.last_event_time.tv_usec);
	ret = sqlite3_bind_double(query, idx++, user->canvas_limiter.current_allowance);
	ret = sqlite3_bind_double(query, idx++, user->canvas_limiter.max_rate);
	ret = sqlite3_bind_double(query, idx++, user->canvas_limiter.per_seconds);
	ret = sqlite3_bind_int(query, idx++, user->tile_limiter.last_event_time.tv_sec);
	ret = sqlite3_bind_int64(query, idx++, user->tile_limiter.last_event_time.tv_usec);
	ret = sqlite3_bind_double(query, idx++, user->tile_limiter.current_allowance);
	ret = sqlite3_bind_double(query, idx++, user->tile_limiter.max_rate);
	ret = sqlite3_bind_double(query, idx++, user->tile_limiter.per_seconds);
	ret = sqlite3_bind_text(query, idx++, user->uuid, strlen(user->uuid), NULL);

	ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to update user: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		sqlite3_finalize(query);
		sqlite3_close(g_canvas.backing_db);
		exit(-1);
	}
	sqlite3_finalize(query);
}

static void user_tile_increment_fn(void *arg) {
	struct user *user = (struct user *)arg;
	// tile_regen_seconds may change in level_up(), so keep it updated here.
	user->tile_increment_timer.period_ms = user->tile_regen_seconds * 1000;
	if (user->remaining_tiles == user->max_tiles) return;
	user->remaining_tiles++;
	cJSON *response = base_response("itc");
	cJSON_AddNumberToObject(response, "a", 1);
	send_json(response, user);
	cJSON_Delete(response);
}

void start_user_timer(struct user *user, struct mg_mgr *mgr) {
	(void)mgr; // Probably not needed
	mg_timer_init(&user->tile_increment_timer, user->tile_regen_seconds * 1000, MG_TIMER_REPEAT, user_tile_increment_fn, user);
}

void send_user_count(void) {
	cJSON *response = base_response("userCount");
	cJSON_AddNumberToObject(response, "count", list_elems(&g_canvas.connected_users));
	broadcast(response);
	cJSON_Delete(response);
}

void add_host(const struct remote_host *host) {
	sqlite3_stmt *query;
	const char *sql = "INSERT INTO hosts (ip_address, total_accounts) VALUES (?, ?)";
	int ret = sqlite3_prepare_v2(g_canvas.backing_db, sql, -1, &query, 0);
	if (ret != SQLITE_OK) {
		printf("Failed to prepare host insert query: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		sqlite3_finalize(query);
		sqlite3_close(g_canvas.backing_db);
		exit(-1);
	}
	int idx = 1;
	char ipbuf[100];
	mg_ntoa(&host->addr, ipbuf, sizeof(ipbuf));
	ret = sqlite3_bind_text(query, idx++, ipbuf, strlen(ipbuf), NULL);
	ret = sqlite3_bind_int(query, idx++, host->total_accounts);

	ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to insert host: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		sqlite3_finalize(query);
		sqlite3_close(g_canvas.backing_db);
		exit(-1);
	}

	char ip_buf[50];
	mg_ntoa(&host->addr, ip_buf, sizeof(ip_buf));
	logr("Adding new host %s\n", ip_buf);
	sqlite3_finalize(query);
}

void add_user(const struct user *user) {
	sqlite3_stmt *query;
	const char *sql =
		"INSERT INTO users (username, uuid, remainingTiles, tileRegenSeconds, totalTilesPlaced, lastConnected, availableColors, level, hasSetUsername, isShadowBanned, maxTiles, tilesToNextLevel, levelProgress, cl_last_event_sec, cl_last_event_usec, cl_current_allowance, cl_max_rate, cl_per_seconds, tl_last_event_sec, tl_last_event_usec, tl_current_allowance, tl_max_rate, tl_per_seconds)"
		" VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
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
	sqlite3_bind_int(query, idx++, user->canvas_limiter.last_event_time.tv_sec);
	sqlite3_bind_int64(query, idx++, user->canvas_limiter.last_event_time.tv_usec);
	sqlite3_bind_double(query, idx++, user->canvas_limiter.current_allowance);
	sqlite3_bind_double(query, idx++, user->canvas_limiter.max_rate);
	sqlite3_bind_double(query, idx++, user->canvas_limiter.per_seconds);
	sqlite3_bind_int(query, idx++, user->tile_limiter.last_event_time.tv_sec);
	sqlite3_bind_int64(query, idx++, user->tile_limiter.last_event_time.tv_usec);
	sqlite3_bind_double(query, idx++, user->tile_limiter.current_allowance);
	sqlite3_bind_double(query, idx++, user->tile_limiter.max_rate);
	sqlite3_bind_double(query, idx++, user->tile_limiter.per_seconds);

	ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to insert user: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		sqlite3_finalize(query);
		sqlite3_close(g_canvas.backing_db);
		exit(-1);
	}
	sqlite3_finalize(query);
}

struct remote_host *try_load_host(struct mg_addr addr) {
	sqlite3_stmt *query;
	int ret = sqlite3_prepare_v2(g_canvas.backing_db, "SELECT * FROM hosts WHERE ip_address = ?", -1, &query, 0);
	if (ret != SQLITE_OK) {
		logr("Failed to prepare host load query: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		return NULL;
	}
	char ipbuf[100];
	mg_ntoa(&addr, ipbuf, sizeof(ipbuf));
	ret = sqlite3_bind_text(query, 1, ipbuf, strlen(ipbuf), NULL);
	if (ret != SQLITE_OK) {
		logr("Failed to bind ip to host load query: %s\n", sqlite3_errmsg(g_canvas.backing_db));
		return NULL;
	}
	int step = sqlite3_step(query);

	struct remote_host *host = NULL;
	if (step != SQLITE_ROW) {
		return NULL;
	}

	size_t i = 1;
	host = calloc(1, sizeof(*host));

	const char *user_name = (const char *)sqlite3_column_text(query, i++);
	mg_aton(mg_str(user_name), &host->addr);
	host->total_accounts = sqlite3_column_int(query, i++);

	sqlite3_finalize(query);

	return host;
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
		const char *user_name = (const char *)sqlite3_column_text(query, i++);
		strncpy(user->user_name, user_name, sizeof(user->user_name) - 1);
		const char *uuid = (const char *)sqlite3_column_text(query, i++);
		strncpy(user->uuid, uuid, UUID_STR_LEN);
		user->remaining_tiles = sqlite3_column_int(query, i++);
		user->tile_regen_seconds = sqlite3_column_int(query, i++);
		user->total_tiles_placed = sqlite3_column_int(query, i++);
		user->last_connected_unix = sqlite3_column_int64(query, i);
		i += 2;
		user->level = sqlite3_column_int(query, i);
		i += 2;
		user->is_shadow_banned = sqlite3_column_int(query, i++);
		user->max_tiles = sqlite3_column_int(query, i++);
		user->tiles_to_next_level = sqlite3_column_int(query, i++);
		user->current_level_progress = sqlite3_column_int(query, i++);

		user->canvas_limiter.last_event_time.tv_sec = sqlite3_column_int(query, i++);
		user->canvas_limiter.last_event_time.tv_usec = sqlite3_column_int64(query, i++);
		user->canvas_limiter.current_allowance = sqlite3_column_double(query, i++);
		user->canvas_limiter.max_rate = sqlite3_column_double(query, i++);
		user->canvas_limiter.per_seconds = sqlite3_column_double(query, i++);
		user->tile_limiter.last_event_time.tv_sec = sqlite3_column_int(query, i++);
		user->tile_limiter.last_event_time.tv_usec = sqlite3_column_int64(query, i++);
		user->tile_limiter.current_allowance = sqlite3_column_double(query, i++);
		user->tile_limiter.max_rate = sqlite3_column_double(query, i++);
		user->tile_limiter.per_seconds = sqlite3_column_double(query, i++);
	}
	sqlite3_finalize(query);
	return user;
}

struct user *find_in_connected_users(const char *uuid) {
	struct list_elem *head = NULL;
	list_foreach_ro(head, g_canvas.connected_users) {
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
	cJSON *response = base_response("levelUp");
	cJSON_AddNumberToObject(response, "level", user->level);
	cJSON_AddNumberToObject(response, "maxTiles", user->max_tiles);
	cJSON_AddNumberToObject(response, "tilesToNextLevel", user->tiles_to_next_level);
	cJSON_AddNumberToObject(response, "levelProgress", user->current_level_progress);
	cJSON_AddNumberToObject(response, "remainingTiles", user->remaining_tiles);
	send_json(response, user);
	cJSON_Delete(response);
}

void init_rate_limiter(struct rate_limiter *limiter, float max_rate, float per_seconds) {
	*limiter = (struct rate_limiter){ .current_allowance = max_rate, .max_rate = max_rate, .per_seconds = per_seconds };
	gettimeofday(&limiter->last_event_time, NULL);
}

cJSON *handle_initial_auth(struct mg_connection *socket, struct remote_host *host) {
	if (host) {
		logr("Received initialAuth from %s\n", socket->label);
		host->total_accounts++;
		save_host(host);
		if (host->total_accounts > g_canvas.settings.max_users_per_ip) {
			char ip_buf[50];
			mg_ntoa(&host->addr, ip_buf, sizeof(ip_buf));
			logr("Rejecting initialAuth from %s, reached maximum of %li users\n", ip_buf, g_canvas.settings.max_users_per_ip);
			return error_response("Maximum users reached for this IP (contact vkoskiv if you think this is an issue)");
		}
	} else {
		logr("Warning: No host given to handle_initial_auth. Maybe fix this probably.\n");
	}

	struct user user = {
		.user_name = "Anonymous",
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
	struct user *uptr = list_append(g_canvas.connected_users, user)->thing;
	add_user(uptr);
	uptr->socket = socket;

	// Set up rate limiting
	init_rate_limiter(&uptr->canvas_limiter, g_canvas.settings.getcanvas_max_rate, g_canvas.settings.getcanvas_per_seconds);
	init_rate_limiter(&uptr->tile_limiter, g_canvas.settings.setpixel_max_rate, g_canvas.settings.setpixel_per_seconds);
	save_user(uptr);

	logr("User %s connected. (%4lu)\n", uptr->uuid, list_elems(&g_canvas.connected_users));
	start_user_timer(uptr, socket->mgr);
	send_user_count();

	uptr->last_event_unix = (unsigned)time(NULL);

	cJSON *response = base_response("authSuccessful");
	cJSON_AddStringToObject(response, "uuid", uptr->uuid);
	cJSON_AddNumberToObject(response, "remainingTiles", uptr->remaining_tiles);
	cJSON_AddNumberToObject(response, "level", uptr->level);
	cJSON_AddNumberToObject(response, "maxTiles", uptr->max_tiles);
	cJSON_AddNumberToObject(response, "tilesToNextLevel", uptr->tiles_to_next_level);
	cJSON_AddNumberToObject(response, "levelProgress", uptr->current_level_progress);
	return response;
}

void drop_user_with_connection(struct mg_connection *c) {
	list_foreach(g_canvas.connected_users, {
		struct user *user = (struct user *)arg;
		if (user->socket != c) return;
		logr("User %s disconnected. (%4lu)\n", user->uuid, list_elems(&g_canvas.connected_users) - 1);
		user->last_connected_unix = (unsigned)time(NULL);
		save_user(user);
		mg_timer_free(&user->tile_increment_timer);
		user->socket->is_draining = 1;
		list_remove(g_canvas.connected_users, {
			const struct user *list_user = (struct user *)arg;
			return list_user->socket == user->socket;
		});
	});
	send_user_count();
}

void kick_with_message(const struct user *user, const char *message, const char *reconnect_btn_text) {
	cJSON *response = base_response("kicked");
	cJSON_AddStringToObject(response, "message", message ? message : "Kicked");
	cJSON_AddStringToObject(response, "btn_text", reconnect_btn_text ? reconnect_btn_text : "Reconnect");
	send_json(response, user);
	cJSON_Delete(response);
	drop_user_with_connection(user->socket);
}

struct administrator *find_in_admins(const char *uuid) {
	struct list_elem *elem = NULL;
	list_foreach_ro(elem, g_canvas.administrators) {
		struct administrator *admin = (struct administrator *)elem->thing;
		if (str_eq(admin->uuid, uuid)) return admin;
	}
	return NULL;
}

cJSON *handle_auth(const cJSON *user_id, struct mg_connection *socket) {
	if (!cJSON_IsString(user_id)) return error_response("Invalid userID");
	if (strlen(user_id->valuestring) > UUID_STR_LEN) return error_response("Invalid userID");

	// Kick old user if the user opens in more than one browser tab at once.
	struct user *user = find_in_connected_users(user_id->valuestring);
	if (user) {
		logr("Kicking %s, they opened a new session\n", user->uuid);
		kick_with_message(user, "It looks like you opened another tab?", "Reconnect here");
	}

	user = try_load_user(user_id->valuestring);
	if (!user) return error_response("Invalid userID");
	struct user *uptr = list_append(g_canvas.connected_users, *user)->thing;
	free(user);
	uptr->socket = socket;

	// Kinda pointless flag. If this thing is in connected_users list, it's valid.
	uptr->is_authenticated = true;

	logr("User %s connected. (%4lu)\n", uptr->uuid, list_elems(&g_canvas.connected_users));
	start_user_timer(uptr, socket->mgr);
	send_user_count();

	uint64_t cur_time = (unsigned)time(NULL);
	size_t sec_since_last_connected = cur_time - uptr->last_connected_unix;
	size_t tiles_to_add = sec_since_last_connected / uptr->tile_regen_seconds;
	// This is how it was in the original, might want to check
	uptr->remaining_tiles += tiles_to_add > uptr->max_tiles ? uptr->max_tiles - uptr->remaining_tiles : tiles_to_add;
	uptr->last_event_unix = cur_time;

	cJSON *response = base_response("reAuthSuccessful");
	cJSON_AddNumberToObject(response, "remainingTiles", uptr->remaining_tiles);
	cJSON_AddNumberToObject(response, "level", uptr->level);
	cJSON_AddNumberToObject(response, "maxTiles", uptr->max_tiles);
	cJSON_AddNumberToObject(response, "tilesToNextLevel", uptr->tiles_to_next_level);
	cJSON_AddNumberToObject(response, "levelProgress", uptr->current_level_progress);
	struct administrator *admin = find_in_admins(uptr->uuid);
	cJSON_AddBoolToObject(response, "isAdministrator", admin != NULL);
	return response;
}

uint8_t *handle_get_canvas_bin(const cJSON *user_id, size_t *bin_length) {
	if (!cJSON_IsString(user_id)) return NULL;
	struct user *user = find_in_connected_users(user_id->valuestring);
	if (!user) return NULL;	
	
	bool within_limit = is_within_rate_limit(&user->canvas_limiter);
	if (!within_limit) {
		logr("CANVAS rate limit exceeded\n");
		return NULL;
	}
	size_t tilecount = g_canvas.edge_length * g_canvas.edge_length;
	struct timeval tmr;
	gettimeofday(&tmr, NULL);

	uint8_t *pixels = calloc(tilecount, 1);
	for (size_t i = 0; i < tilecount; ++i) {
		pixels[i] = g_canvas.tiles[i].color_id;
	}

	size_t compressed_len = compressBound(tilecount);
	uint8_t *compressed = malloc(compressed_len);
	logr("Compressing with initial: %lu...\n", compressed_len);
	int ret = compress(compressed, &compressed_len, pixels, tilecount);
	if (ret != Z_OK) {
		if (ret == Z_MEM_ERROR) logr("Z_MEM_ERROR\n");
		if (ret == Z_BUF_ERROR) logr("Z_BUF_ERROR\n");
	}
	/*
	uint8_t *uncompressed = calloc(tilecount, 1);
	size_t uncompressed_len = tilecount;
	logr("Uncompressing...\n");
	ret = uncompress(uncompressed, &uncompressed_len, compressed, compressed_len);
	if (ret != Z_OK) {
		if (ret == Z_MEM_ERROR) logr("Z_MEM_ERROR\n");
		if (ret == Z_BUF_ERROR) logr("Z_BUF_ERROR\n");
		if (ret == Z_DATA_ERROR) logr("Z_DATA_ERROR\n");
	}

	int bab = memcmp(pixels, uncompressed, tilecount);
	if (bab == 0) {
		logr("Match!\n");
	} else {
		logr("memcmp() returned %i\n", bab);
	}
	*/
	long ms = get_ms_delta(tmr);
	logr("gather, pack took %lums\n", ms);

	free(pixels);
	if (bin_length) *bin_length = compressed_len;
	return compressed;
}

cJSON *handle_get_canvas(const cJSON *user_id) {
	if (!cJSON_IsString(user_id)) return error_response("No userID provided");
	struct user *user = find_in_connected_users(user_id->valuestring);
	if (!user) return error_response("Not authenticated");

	bool within_limit = is_within_rate_limit(&user->canvas_limiter);
	if (!within_limit) {
		logr("CANVAS rate limit exceeded\n");
		return error_response("Rate limit exceeded");
	}
	user->last_event_unix = (unsigned)time(NULL);
	size_t tilecount = g_canvas.edge_length * g_canvas.edge_length;

	struct timeval tmr;
	gettimeofday(&tmr, NULL);
	uint8_t *pixels = calloc(tilecount, 1);
	for (size_t i = 0; i < tilecount; ++i) {
		pixels[i] = g_canvas.tiles[i].color_id;
	}

	size_t compressed_len = compressBound(tilecount);
	uint8_t *compressed = malloc(compressed_len);
	int ret = compress(compressed, &compressed_len, pixels, tilecount);
	if (ret != Z_OK) {
		if (ret == Z_MEM_ERROR) logr("Z_MEM_ERROR\n");
		if (ret == Z_BUF_ERROR) logr("Z_BUF_ERROR\n");
	}

	char *zlib_b64_encoded = b64encode(compressed, compressed_len);
	cJSON *response = base_response("fullCanvas");
	cJSON *content = cJSON_CreateString(zlib_b64_encoded);
	cJSON_AddItemToObject(response, "payload", content);
	free(pixels);
	free(zlib_b64_encoded);
	free(compressed);
	long ms = get_ms_delta(tmr);
	logr("Sending zlib'd canvas to %s. (%lums)\n", user->uuid, ms);
	return response;
}

cJSON *new_tile_update(size_t x, size_t y, size_t color_id) {
	size_t idx = x + y * g_canvas.edge_length;
	cJSON *response = base_response("tu");
	cJSON_AddNumberToObject(response, "i", idx);
	cJSON_AddNumberToObject(response, "c", color_id);
	return response;
}

cJSON *handle_post_tile(const cJSON *user_id, const cJSON *x_param, const cJSON *y_param, const cJSON *color_id_param, const char *raw_request, size_t raw_request_length) {
	if (!cJSON_IsString(user_id)) return error_response("Invalid userID");
	if (!cJSON_IsNumber(x_param)) return error_response("X coordinate not a number");
	if (!cJSON_IsNumber(y_param)) return error_response("Y coordinate not a number");
	if (!cJSON_IsString(color_id_param)) return error_response("colorID not a string");

	struct user *user = find_in_connected_users(user_id->valuestring);

	if (!user) return error_response("Not authenticated");
	if (user->remaining_tiles < 1) return error_response("No tiles remaining");

	if (!is_within_rate_limit(&user->tile_limiter)) {
		return NULL;
	}

	//Another ugly detail, the client sends the colorID number as a string...
	uintmax_t num = strtoumax(color_id_param->valuestring, NULL, 10);
	if (num == UINTMAX_MAX && errno == ERANGE) return error_response("colorID not a valid number in a string");

	size_t color_id = num;
	size_t x = x_param->valueint;
	size_t y = y_param->valueint;

	if (x > g_canvas.edge_length - 1) return error_response("Invalid X coordinate");
	if (y > g_canvas.edge_length - 1) return error_response("Invalid Y coordinate");
	if (color_id > g_canvas.color_list.amount - 1) return error_response("Invalid colorID");

	user->remaining_tiles--;
	user->total_tiles_placed++;
	user->current_level_progress++;
	if (user->current_level_progress >= user->tiles_to_next_level) {
		level_up(user);
	}
	user->last_event_unix = (unsigned)time(NULL);

	if (user->is_shadow_banned) {
		logr("Rejecting request from shadowbanned user: %.*s\n", (int)raw_request_length, raw_request);
		return new_tile_update(x, y, color_id);
	}

	// This print is for compatibility with https://github.com/zouppen/pikselipeli-parser
	logr("Received request: %.*s\n", (int)raw_request_length, raw_request);
	
	struct tile *tile = &g_canvas.tiles[x + y * g_canvas.edge_length];
	tile->color_id = color_id;
	tile->place_time_unix = user->last_event_unix;
	memcpy(tile->last_modifier, user->uuid, sizeof(tile->last_modifier));
	
	// Record delta for persistence. These get flushed to disk every canvas_save_interval_sec seconds.
	struct tile_placement placement = {
		.x = x,
		.y = y,
		.tile = *tile
	};
	list_append(g_canvas.delta, placement);

	g_canvas.dirty = true;

	cJSON *update = new_tile_update(x, y, color_id);
	broadcast(update);
	cJSON_Delete(update);
	return NULL; // The broadcast takes care of this
}

// The API is a bit weird. Instead of having a responsetype and an array in an object
// we have an array where the first object is an object containing the responsetype, rest
// are objects containing colors. *shrug*
cJSON *handle_get_colors(const cJSON *user_id) {
	if (!cJSON_IsString(user_id)) return error_response("No userID provided");
	struct user *user = find_in_connected_users(user_id->valuestring);
	if (!user) return error_response("Not authenticated");
	user->last_event_unix = (unsigned)time(NULL);
	return cJSON_Parse(g_canvas.color_response_cache);
}

cJSON *handle_set_nickname(const cJSON *user_id, const cJSON *name) {
	if (!cJSON_IsString(user_id)) return error_response("No userID provided");
	if (!cJSON_IsString(name)) return error_response("No nickname provided");
	struct user *user = find_in_connected_users(user_id->valuestring);
	if (!user) return error_response("Not authenticated");
	if (strlen(name->valuestring) > sizeof(user->user_name)) return error_response("Nickname too long");
	logr("User %s set their username to %s\n", user_id->valuestring, name->valuestring);
	strncpy(user->user_name, name->valuestring, sizeof(user->user_name) - 1);
	user->last_event_unix = (unsigned)time(NULL);
	return base_response("nameSetSuccess");
}

cJSON *broadcast_announcement(const char *message) {
	cJSON *response = base_response("announcement");
	cJSON_AddStringToObject(response, "message", message);
	broadcast(response);
	cJSON_Delete(response);
	return base_response("Success");
}

void drop_all_connections(void) {
	list_foreach(g_canvas.connected_users, {
		struct user *user = (struct user *)arg;
		user->last_connected_unix = (unsigned)time(NULL);
		save_user(user);
		mg_timer_free(&user->tile_increment_timer);
		user->socket->is_draining = 1;
		list_remove(g_canvas.connected_users, {
			const struct user *list_user = (struct user *)arg;
			return list_user->socket == user->socket;
		});
		send_user_count();
	});
}

cJSON *shut_down_server(void) {
	g_running = false;
	return NULL;
}

cJSON *toggle_shadow_ban(const char *uuid) {
	struct user *user = find_in_connected_users(uuid);
	if (!user) user = try_load_user(uuid);
	if (!user) return error_response("No user found with that uuid");
	logr("Toggling is_shadow_banned to %s for user %s\n", !user->is_shadow_banned ? "true " : "false", uuid);
	user->is_shadow_banned = !user->is_shadow_banned;
	save_user(user);
	return base_response("Success");
}

cJSON *shadow_ban_user(const char *uuid) {
	struct user *user = find_in_connected_users(uuid);
	if (!user) user = try_load_user(uuid);
	if (!user) return error_response("No user found with that uuid");
	logr("Setting is_shadow_banned to true for user %s\n", uuid);
	user->is_shadow_banned = true;
	save_user(user);
	return base_response("Success");
}

cJSON *handle_ban_click(const cJSON *coordinates) {
	if (!cJSON_IsArray(coordinates)) return error_response("No valid coordinates provided");
	if (cJSON_GetArraySize(coordinates) < 2) return error_response("No valid coordinates provided");
	cJSON *x_param = cJSON_GetArrayItem(coordinates, 0);
	cJSON *y_param = cJSON_GetArrayItem(coordinates, 1);
	if (!cJSON_IsNumber(x_param)) return error_response("X coordinate not a number");
	if (!cJSON_IsNumber(y_param)) return error_response("Y coordinate not a number");

	size_t x = x_param->valueint;
	size_t y = y_param->valueint;

	if (x > g_canvas.edge_length - 1) return error_response("Invalid X coordinate");
	if (y > g_canvas.edge_length - 1) return error_response("Invalid Y coordinate");

	struct tile *tile = &g_canvas.tiles[x + y * g_canvas.edge_length];
	struct user *user = find_in_connected_users(tile->last_modifier);
	if (!user) user = try_load_user(tile->last_modifier);
	if (!user) return error_response("Couldn't find a user who modified that tile.");
	if (user->is_shadow_banned) return error_response("Already shadowbanned from there");
	// Just in case...
	struct administrator *admin = find_in_admins(user->uuid);
	if (admin) return error_response("Refusing to shadowban an administrator");
	logr("User %s shadowbanned from (%4lu,%4lu)\n", user->uuid, x, y);
	user->is_shadow_banned = true;
	save_user(user);
	cJSON *wrapper = cJSON_CreateArray();
	cJSON *payload = base_response("ban_click_success");
	cJSON_InsertItemInArray(wrapper, 0, payload);
	return wrapper;
}

cJSON *handle_admin_command(const cJSON *user_id, const cJSON *command) {
	if (!cJSON_IsString(user_id)) return error_response("No valid userID provided");
	struct administrator *admin = find_in_admins(user_id->valuestring);
	if (!admin) {
		logr("Rejecting admin command for unknown user %s. Naughty naughty!\n", user_id->valuestring);
		return error_response("Invalid admin userID");	
	}
	if (!cJSON_IsObject(command)) return error_response("No valid command object provided");
	const cJSON *action = cJSON_GetObjectItem(command, "action");	
	const cJSON *message = cJSON_GetObjectItem(command, "message");
	const cJSON *coordinates = cJSON_GetObjectItem(command, "coords");
	if (!cJSON_IsString(action)) return error_response("Invalid command action");
	if (str_eq(action->valuestring, "shutdown")) {
		if (admin->can_shutdown) {
			return shut_down_server();
		} else {
			return error_response("You don't have shutdown permission");
		}
	}
	if (str_eq(action->valuestring, "message")) {
		if (admin->can_announce) {
			return broadcast_announcement(message->valuestring);
		} else {
			return error_response("You don't have announce permission");
		}
	}
	if (str_eq(action->valuestring, "toggle_shadowban")) {
		if (admin->can_shadowban) {
			return toggle_shadow_ban(message->valuestring);
		} else {
			return error_response("You don't have shadowban permission");
		}
	}
	if (str_eq(action->valuestring, "banclick")) {
		if (admin->can_banclick) {
			return handle_ban_click(coordinates);
		} else {
			return error_response("You don't have banclick permission");
		}
	}
	return error_response("Unknown admin action invoked");
}

bool mg_addr_eq(struct mg_addr a, struct mg_addr b) {
	return memcmp(&a, &b, sizeof(a)) == 0;
}

struct remote_host *find_host(struct mg_addr addr) {
	struct list_elem *elem = NULL;
	list_foreach_ro(elem, g_canvas.connected_hosts) {
		struct remote_host *host = (struct remote_host *)elem->thing;
		if (mg_addr_eq(host->addr, addr)) return host;
	}
	struct remote_host *host = try_load_host(addr);
	if (host) {
		struct remote_host *hptr = list_append(g_canvas.connected_hosts, *host)->thing;
		free(host);
		return hptr;
	}

	struct remote_host new = {
		.addr = addr,
	};
	host = list_append(g_canvas.connected_hosts, new)->thing;
	add_host(host);

	return host;
}

cJSON *handle_command(const char *cmd, size_t len, struct mg_connection *connection) {
	// cmd is not necessarily null-terminated. Trust len.
	cJSON *command = cJSON_ParseWithLength(cmd, len);
	if (!command) return error_response("No command provided");
	const cJSON *request_type = cJSON_GetObjectItem(command, "requestType");
	if (!cJSON_IsString(request_type)) return error_response("No requestType provided");

	const cJSON *user_id   = cJSON_GetObjectItem(command, "userID");
	const cJSON *name      = cJSON_GetObjectItem(command, "name");
	const cJSON *x         = cJSON_GetObjectItem(command, "X");
	const cJSON *y         = cJSON_GetObjectItem(command, "Y");
	const cJSON *color_id  = cJSON_GetObjectItem(command, "colorID");
	const cJSON *admin_cmd = cJSON_GetObjectItem(command, "cmd");
	char *reqstr = request_type->valuestring;

	cJSON *response = NULL;
	if (str_eq(reqstr, "initialAuth")) {
		if (strlen(connection->label)) {
			struct mg_addr remote_addr;
			if (mg_aton(mg_str(connection->label), &remote_addr)) {
				struct remote_host *host = find_host(remote_addr);
				response = handle_initial_auth(connection, host);
			} else {
				logr("Failed to convert peer address\n");
				response = handle_initial_auth(connection, NULL);
			}
		} else {
			logr("No peer address\n");
			response = handle_initial_auth(connection, NULL);
		}
	} else if (str_eq(reqstr, "auth")) {
		response = handle_auth(user_id, connection);
	} else if (str_eq(reqstr, "getCanvas")) {
		response = handle_get_canvas(user_id);
	} else if (str_eq(reqstr, "postTile")) {
		response = handle_post_tile(user_id, x, y, color_id, cmd, len);
	} else if (str_eq(reqstr, "getColors")) {
		response = handle_get_colors(user_id);
	} else if (str_eq(reqstr, "setUsername")) {
		response = handle_set_nickname(user_id, name);
	} else if (str_eq(reqstr, "admin_cmd")) {
		response = handle_admin_command(user_id, admin_cmd);
	} else {
		response = error_response("Unknown requestType");
	}

	cJSON_Delete(command);
	return response;
}

static void callback_fn(struct mg_connection *c, int event_type, void *event_data, void *arg) {
	(void)arg; // TODO: Pass around a canvas instead of having that global up there

	if (event_type == MG_EV_HTTP_MSG) {
		struct mg_http_message *msg = (struct mg_http_message *)event_data;
		if (mg_http_match_uri(msg, "/ws")) {
			// This block here grabs the client IP address and stores it in the
			// mg_connection.label convenience string provided by mongoose.
			// We only use IPs as an identifier to prevent abuse by limiting
			// how many accounts a single IP can generate.
			struct mg_str *fwd_header = mg_http_get_header(msg, "X-Forwarded-For");
			if (fwd_header) {
				struct mg_str copy = mg_strdup(*fwd_header);
				char *stash = (char *)copy.ptr;
				struct mg_str k, v;
				if (mg_commalist(&copy, &k, &v)) {
					// This grabs the first string of a comma-separated list into k
					// Which is the true client address, if proxies are to be trusted.
					strncpy(c->label, k.ptr, k.len);
				}
				// This is ugly, and I had to cast away a const as well.
				// Mongoose doesn't have a mg_str_free() and mg_commalist messes with this too :(
				free(stash);
			} else {
				mg_ntoa(&c->peer, c->label, sizeof(c->label));
			}
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
		} else if (mg_http_match_uri(msg, "/brew_coffee")) {
			mg_http_reply(c, 418, "", "Sorry, can't do that. :(");
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
		mg_ws_send(c, NULL, 0, WEBSOCKET_OP_PING);
	}
}

void start_transaction() {
	sqlite3 *db = g_canvas.backing_db;
	sqlite3_stmt *bt;
	sqlite3_prepare_v2(db, "BEGIN TRANSACTION", -1, &bt, NULL);
	int ret = sqlite3_step(bt);
	if (ret != SQLITE_DONE) {
		printf("Failed to begin transaction\n");
		sqlite3_finalize(bt);
		sqlite3_close(db);
		exit(-1);
	}
	sqlite3_finalize(bt);
}

void commit_transaction() {
	sqlite3 *db = g_canvas.backing_db;
	sqlite3_stmt *et;
	sqlite3_prepare_v2(db, "COMMIT", -1, &et, NULL);
	int ret = sqlite3_step(et);
	if (ret != SQLITE_DONE) {
		printf("Failed to commit transaction\n");
		sqlite3_finalize(et);
		sqlite3_close(db);
		exit(-1);
	}
	sqlite3_finalize(et);
}

static void users_save_timer_fn(void *arg) {
	(void)arg;
	struct timeval timer;
	gettimeofday(&timer, NULL);
	size_t users = list_elems(&g_canvas.connected_users);
	if (!users) return;
	logr("Saving %lu users", users);
	start_transaction();
	struct list_elem *elem = NULL;
	list_foreach_ro(elem, g_canvas.connected_users) {
		struct user *user = (struct user *)elem->thing;
		save_user(user);
	}
	commit_transaction();
	printf(" (%lums)\n", get_ms_delta(timer));

	// Check and kick inactive users
	uint64_t current_time_unix = (unsigned)time(NULL);
	list_foreach(g_canvas.connected_users, {
		struct user *user = (struct user *)arg;
		size_t sec_since_last_event = current_time_unix - user->last_event_unix;
		if (sec_since_last_event > g_canvas.settings.kick_inactive_after_sec) {
			logr("Kicking inactive user %s\n", user->uuid);
			kick_with_message(user, "You haven't drawn anything for a while, so you were disconnected.", "Reconnect");
		}
	});
}

static void canvas_save_timer_fn(void *arg) {
	//struct mg_mgr *mgr = (struct mg_mgr *)arg;
	(void)arg;

	if (!g_canvas.dirty) return;

	struct timeval timer;
	gettimeofday(&timer, NULL);

	sqlite3 *db = g_canvas.backing_db;
	
	sqlite3_stmt *bt;
	sqlite3_prepare_v2(db, "BEGIN TRANSACTION", -1, &bt, NULL);
	int ret = sqlite3_step(bt);
	if (ret != SQLITE_DONE) {
		printf("Failed to begin transaction\n");
		sqlite3_finalize(bt);
		sqlite3_close(db);
		exit(-1);
	}
	sqlite3_finalize(bt);

	sqlite3_stmt *insert;
	ret = sqlite3_prepare_v2(db, "UPDATE tiles SET colorID = ?, lastModifier = ?, placeTime = ? WHERE X = ? AND Y = ?", -1, &insert, NULL);
	if (ret != SQLITE_OK) {
		printf("Failed to prepare tile update: %s\n", sqlite3_errmsg(db));
		goto bail;
	}

	logr("Saving canvas to disk (%li events) ", list_elems(&g_canvas.delta));

	struct list_elem *elem = NULL;
	list_foreach_ro(elem, g_canvas.delta) {
		struct tile_placement *p = (struct tile_placement *)elem->thing;
		int idx = 1;
		struct tile *tile = &p->tile;
		ret = sqlite3_bind_int(insert, idx++, tile->color_id);
		if (ret != SQLITE_OK) printf("Failed to bind colorID: %s\n", sqlite3_errmsg(db));
		ret = sqlite3_bind_text(insert, idx++, tile->last_modifier, sizeof(tile->last_modifier), NULL);
		if (ret != SQLITE_OK) printf("Failed to bind lastModifier: %s\n", sqlite3_errmsg(db));
		ret = sqlite3_bind_int64(insert, idx++, tile->place_time_unix);
		if (ret != SQLITE_OK) printf("Failed to bind placeTime: %s\n", sqlite3_errmsg(db));
		ret = sqlite3_bind_int(insert, idx++, p->x);
		if (ret != SQLITE_OK) printf("Failed to bind X: %s\n", sqlite3_errmsg(db));
		ret = sqlite3_bind_int(insert, idx++, p->y);
		if (ret != SQLITE_OK) printf("Failed to bind Y: %s\n", sqlite3_errmsg(db));

		int res = sqlite3_step(insert);
		if (res != SQLITE_DONE) {
			printf("Failed to UPDATE for x = %lu, y = %lu\n", p->x, p->y);
			sqlite3_finalize(insert);
			sqlite3_close(db);
			exit(-1);
		}
		sqlite3_clear_bindings(insert);
		sqlite3_reset(insert);
	}
	list_destroy(&g_canvas.delta);

bail:
	sqlite3_finalize(insert);

	sqlite3_stmt *et;
	sqlite3_prepare_v2(db, "COMMIT", -1, &et, NULL);
	ret = sqlite3_step(et);
	if (ret != SQLITE_DONE) {
		printf("Failed to commit transaction\n");
		sqlite3_finalize(et);
		sqlite3_close(db);
		exit(-1);
	} else {
		long ms = get_ms_delta(timer);
		printf("(%lims)\n", ms);
		g_canvas.dirty = false;
	}
	sqlite3_finalize(et);
}

void ensure_tiles_table(sqlite3 *db) {
	// Next, ensure we've got tiles in there.
	sqlite3_stmt *count;
	int ret = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tiles", -1, &count, NULL);
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

	if (rows > 0) return;

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

	logr("Running initial tile db init...\n");
	sqlite3_stmt *insert;
	ret = sqlite3_prepare_v2(db, "INSERT INTO tiles (X, Y, colorID, lastModifier, placeTime) VALUES (?, ?, 3, \"\", 0)", -1, &insert, NULL);
	if (ret != SQLITE_OK) printf("Failed to prepare tile insert: %s\n", sqlite3_errmsg(db));

	for (size_t y = 0; y < g_canvas.settings.new_db_canvas_size; ++y) {
		for (size_t x = 0; x < g_canvas.settings.new_db_canvas_size; ++x) {
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

	logr("db init done.\n");
}

void ensure_valid_db(sqlite3 *db) {
	char *schema = load_file("nmc2.sql", NULL);
	if (!schema) {
		exit(-1);
	}
	int ret = sqlite3_exec(db, schema, NULL, NULL, NULL);
	if (ret == SQLITE_ABORT) {
		printf("Failed to create database, check schema file. Error: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		exit(-1);
	}
	free(schema);
	ensure_tiles_table(db); // Generate canvas if needed
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

	g_canvas.tiles = calloc(g_canvas.edge_length * g_canvas.edge_length, sizeof(struct tile));
	g_canvas.connected_users = LIST_INITIALIZER;
	g_canvas.connected_hosts = LIST_INITIALIZER;
	printf("Loading %ux%u canvas...\n", c->edge_length, c->edge_length);

	sqlite3_stmt *query;
	sqlite3_prepare_v2(c->backing_db, "select * from tiles", -1, &query, NULL);

	while (sqlite3_step(query) != SQLITE_DONE) {
		int idx = 1;
		size_t x = sqlite3_column_int(query, idx++);
		size_t y = sqlite3_column_int(query, idx++);
		g_canvas.tiles[x + y * g_canvas.edge_length].color_id = sqlite3_column_int(query, idx++);
		const char *last_modifier = (const char *)sqlite3_column_text(query, idx++);
		strncpy(g_canvas.tiles[x + y * g_canvas.edge_length].last_modifier, last_modifier, UUID_STR_LEN - 1);
		g_canvas.tiles[x + y * g_canvas.edge_length].place_time_unix = sqlite3_column_int64(query, idx++);
	}
	sqlite3_finalize(query);
	g_canvas.dirty = false;
	return false;
}

bool set_up_db(struct canvas *c) {
	int ret = 0;
	ret = sqlite3_open(c->settings.dbase_file, &c->backing_db);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(c->backing_db));
		sqlite3_close(c->backing_db);
		return true;
	}

	// I might mess with the db while it's in use.
	sqlite3_busy_timeout(c->backing_db, 2000);
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
	} else if (sig == 15) {
		printf("Received SIGTERM, stopping.\n");
		g_running = false;
	}
}

void sigusr1_handler(int sig) {
	if (sig == SIGUSR1) {
		printf("Reeived SIGUSR1, reloading config...\n");
		load_config(&g_canvas);
	}
}

int main(void) {
	setbuf(stdout, NULL); // Disable output buffering

	g_canvas = (struct canvas){ 0 };
	load_config(&g_canvas);

	struct list_elem *elem = NULL;
	list_foreach_ro(elem, g_canvas.administrators) {
		struct administrator *admin = (struct administrator *)elem->thing;

		if (str_eq(admin->uuid, "<Desired userID here>")) {
			logr("Warning - Admin UUID still at default, anyone can shut down this server.\n");
			logr("Substitute uuid in params.json admin list with your desired UUID before running.\n");
			exit(-1);
		}
	}

	if (signal(SIGINT, sigint_handler) == SIG_ERR) {
		printf("Failed to register sigint handler\n");
		return -1;
	}
	if (signal(SIGTERM, sigint_handler) == SIG_ERR) {
		printf("Failed to register sigterm handler\n");
		return -1;
	}
	if (signal(SIGUSR1, sigusr1_handler) == SIG_ERR) {
		printf("Failed to register SIGUSR1 handler\n");
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
	mg_timer_init(&g_canvas.ws_ping_timer, 1000 * g_canvas.settings.websocket_ping_interval_sec, MG_TIMER_REPEAT, ping_timer_fn, &mgr);
	mg_timer_init(&g_canvas.canvas_save_timer, 1000 * g_canvas.settings.canvas_save_interval_sec, MG_TIMER_REPEAT, canvas_save_timer_fn, &mgr);
	mg_timer_init(&g_canvas.users_save_timer, 1000 * g_canvas.settings.users_save_interval_sec, MG_TIMER_REPEAT, users_save_timer_fn, &mgr);
	printf("Starting WS listener on %s/ws\n", g_canvas.settings.listen_url);
	mg_http_listen(&mgr, g_canvas.settings.listen_url, callback_fn, NULL);
	while (g_running) mg_mgr_poll(&mgr, 1000);

	cJSON *response = base_response("disconnecting");
	broadcast(response);
	cJSON_Delete(response);
	drop_all_connections();

	//FIXME: Hack. Just flush some events before closing
	for (size_t i = 0; i < 100; ++i) {
		mg_mgr_poll(&mgr, 1);
	}
	logr("Saving canvas one more time...\n");
	canvas_save_timer_fn(NULL);
	logr("Saving users...\n");
	users_save_timer_fn(NULL);

	printf("Closing db\n");
	mg_mgr_free(&mgr);
	free(g_canvas.tiles);
	free(g_canvas.color_list.colors);
	free(g_canvas.color_response_cache);
	list_destroy(&g_canvas.connected_users);
	list_destroy(&g_canvas.connected_hosts);
	list_destroy(&g_canvas.administrators);
	list_destroy(&g_canvas.delta);
	sqlite3_close(g_canvas.backing_db);
	return 0;
}
