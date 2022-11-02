// Copyright (c) 2022 Valtteri Koskivuori (vkoskiv). All rights reserved.

// Some platforms just don't have this for whatever reason
#ifndef UUID_STR_LEN
#define UUID_STR_LEN 37
#endif

#include "vendored/mongoose.h"
#include "vendored/cJSON.h"
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
#include <pthread.h>

struct color {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
	uint8_t color_id;
};

// Compile-time constants
#define MAX_NICK_LEN 64

#define UNUSED_VAL 41414141

// Useful for testing, but be careful with this
//#define DISABLE_RATE_LIMITING

struct rate_limiter {
	struct timeval last_event_time;
	float current_allowance;
	float *max_rate;
	float *per_seconds;
};

struct user {
	char user_name[MAX_NICK_LEN];
	char uuid[UUID_STR_LEN + 1];
	struct mg_connection *socket;
	struct mg_timer *tile_increment_timer;
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
	uint8_t color_id;
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
	size_t max_concurrent_users;
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
	bool can_cleanup;
};

struct canvas {
	struct mg_mgr mgr;
	struct list connected_users;
	size_t connected_user_count;
	struct list connected_hosts;
	struct list administrators;
	struct list delta;
	struct tile *tiles;
	bool dirty;
	uint32_t edge_length;
	sqlite3 *backing_db; // For persistence
	struct params settings;
	struct color_list color_list;
	char *color_response_cache;
	pthread_t canvas_worker_thread;
	pthread_mutex_t canvas_cache_lock;
	uint8_t *canvas_cache;
	size_t canvas_cache_len;
	float canvas_cache_compression_ratio;
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
	if (!limiter->max_rate) {
		logr("WHOA! Rate limiter has no max_rate set!\n");
		return false;
	}
	if (!limiter->per_seconds) {
		logr("WHOA! Rate limiter has no per_seconds set!\n");
		return false;
	}
	bool is_within_limit = true;
	long ms_since_last_event = get_ms_delta(limiter->last_event_time);
	gettimeofday(&limiter->last_event_time, NULL);
	float secs_since_last = (float)ms_since_last_event / 1000.0f;
	limiter->current_allowance += secs_since_last * (*limiter->max_rate / *limiter->per_seconds);
	if (limiter->current_allowance > *limiter->max_rate) limiter->current_allowance = *limiter->max_rate;
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

static bool g_running = true;
// These two below are triggered from a signal handler and polled in main runloop
static bool g_reload_config = false;
static bool g_do_db_backup = false;

// common request handling logic

void start_user_timer(struct user *user, struct mg_mgr *mgr);
void send_user_count(const struct canvas *c);

struct tile_update {
	uint8_t resp_type;
	uint8_t color_id;
	//2 bytes of padding :(
	uint32_t i;
};

enum response_id {
	RES_AUTH_SUCCESS = 0,
	RES_CANVAS,
	RES_TILE_INFO,
	RES_TILE_UPDATE,
	RES_COLOR_LIST,
	RES_USERNAME_SET_SUCCESS,
	RES_TILE_INCREMENT,
	RES_LEVEL_UP,
	RES_USER_COUNT,
	ERR_INVALID_UUID = 128,
	ERR_OUT_OF_TILES,
	ERR_RATE_LIMIT_EXCEEDED,
};

void bin_broadcast(const struct canvas *c, const char *payload, size_t len) {
	struct list_elem *elem = NULL;
	list_foreach_ro(elem, c->connected_users) {
		struct user *user = (struct user *)elem->thing;
		mg_ws_send(user->socket, payload, len, WEBSOCKET_OP_BINARY);
	}
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

cJSON *base_response(const char *type) {
	cJSON *payload = cJSON_CreateObject();
	cJSON_AddStringToObject(payload, "rt", type);
	return payload;
}

void send_json(const cJSON *payload, const struct user *user) {
	char *str = cJSON_PrintUnformatted(payload);
	if (!str) return;
	mg_ws_send(user->socket, str, strlen(str), WEBSOCKET_OP_TEXT);
	free(str);
}

void broadcast(const struct canvas *c, const cJSON *payload) {
	//FIXME: Make list_foreach more ergonomic
	struct list_elem *elem = NULL;
	list_foreach_ro(elem, c->connected_users) {
		struct user *user = (struct user *)elem->thing;
		send_json(payload, user);
	}
}

struct remote_host *try_load_host(struct canvas *c, struct mg_addr addr) {
	sqlite3_stmt *query;
	int ret = sqlite3_prepare_v2(c->backing_db, "SELECT * FROM hosts WHERE ip_address = ?", -1, &query, 0);
	if (ret != SQLITE_OK) {
		logr("Failed to prepare host load query: %s\n", sqlite3_errmsg(c->backing_db));
		return NULL;
	}
	char ipbuf[100];
	mg_ntoa(&addr, ipbuf, sizeof(ipbuf));
	ret = sqlite3_bind_text(query, 1, ipbuf, strlen(ipbuf), NULL);
	if (ret != SQLITE_OK) {
		logr("Failed to bind ip to host load query: %s\n", sqlite3_errmsg(c->backing_db));
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

void add_host(struct canvas *c, const struct remote_host *host) {
	sqlite3_stmt *query;
	const char *sql = "INSERT INTO hosts (ip_address, total_accounts) VALUES (?, ?)";
	int ret = sqlite3_prepare_v2(c->backing_db, sql, -1, &query, 0);
	if (ret != SQLITE_OK) {
		printf("Failed to prepare host insert query: %s\n", sqlite3_errmsg(c->backing_db));
		sqlite3_finalize(query);
		sqlite3_close(c->backing_db);
		exit(-1);
	}
	int idx = 1;
	char ipbuf[100];
	mg_ntoa(&host->addr, ipbuf, sizeof(ipbuf));
	ret = sqlite3_bind_text(query, idx++, ipbuf, strlen(ipbuf), NULL);
	ret = sqlite3_bind_int(query, idx++, host->total_accounts);

	ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to insert host: %s\n", sqlite3_errmsg(c->backing_db));
		sqlite3_finalize(query);
		sqlite3_close(c->backing_db);
		exit(-1);
	}

	char ip_buf[50];
	mg_ntoa(&host->addr, ip_buf, sizeof(ip_buf));
	logr("Adding new host %s\n", ip_buf);
	sqlite3_finalize(query);
}

void save_host(struct canvas *c, const struct remote_host *host) {
	const char *sql = "UPDATE hosts SET total_accounts = ? WHERE ip_address = ?";
	sqlite3_stmt *query;
	int ret = sqlite3_prepare_v2(c->backing_db, sql, -1, &query, 0);
	if (ret != SQLITE_OK) {
		printf("Failed to prepare host save query: %s\n", sqlite3_errmsg(c->backing_db));
		sqlite3_finalize(query);
		sqlite3_close(c->backing_db);
		exit(-1);
	}
	int idx = 1;
	char ipbuf[100];
	mg_ntoa(&host->addr, ipbuf, sizeof(ipbuf));
	ret = sqlite3_bind_int(query, idx++, host->total_accounts);
	ret = sqlite3_bind_text(query, idx++, ipbuf, strlen(ipbuf), NULL);

	ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to update host: %s\n", sqlite3_errmsg(c->backing_db));
		sqlite3_finalize(query);
		sqlite3_close(c->backing_db);
		exit(-1);
	}
	sqlite3_finalize(query);
}

bool mg_addr_eq(struct mg_addr a, struct mg_addr b) {
	return memcmp(&a, &b, sizeof(a)) == 0;
}

struct remote_host *find_host(struct canvas *c, struct mg_addr addr) {
	struct list_elem *elem = NULL;
	list_foreach_ro(elem, c->connected_hosts) {
		struct remote_host *host = (struct remote_host *)elem->thing;
		if (mg_addr_eq(host->addr, addr)) return host;
	}
	struct remote_host *host = try_load_host(c, addr);
	if (host) {
		struct remote_host *hptr = list_append(c->connected_hosts, *host)->thing;
		free(host);
		return hptr;
	}

	struct remote_host new = {
		.addr = addr,
	};
	host = list_append(c->connected_hosts, new)->thing;
	add_host(c, host);

	return host;
}

struct remote_host *extract_host(struct canvas *c, struct mg_connection *socket) {
	if (strlen(socket->label)) { //TODO: Needed?
		struct mg_addr remote_addr;
		if (mg_aton(mg_str(socket->label), &remote_addr)) {
			return find_host(c, remote_addr);
		} else {
			logr("Failed to convert peer address\n");
		}
	} else {
		logr("No peer address\n");
	}
	return NULL;
}

struct user *find_in_connected_users(const struct canvas *c, const char *uuid) {
	struct list_elem *head = NULL;
	list_foreach_ro(head, c->connected_users) {
		struct user *user = (struct user *)head->thing;
		if (strncmp(user->uuid, uuid, UUID_STR_LEN) == 0) return user;
	}
	return NULL;
}

void assign_rate_limiter_limit(struct rate_limiter *limiter, float *max_rate, float *per_seconds) {
	if (!max_rate || !per_seconds) {
		logr("WHOA! Trying to init a rate limiter with invalid params\n");
		return;
	}
	limiter->max_rate = max_rate;
	limiter->per_seconds = per_seconds;
}

void save_user(struct canvas *c, const struct user *user) {
	const char *sql = "UPDATE users SET username = ?, remainingTiles = ?, tileRegenSeconds = ?, totalTilesPlaced = ?, lastConnected = ?, level = ?, hasSetUsername = ?, isShadowBanned = ?, maxTiles = ?, tilesToNextLevel = ?, levelProgress = ?, cl_last_event_sec = ?, cl_last_event_usec = ?, cl_current_allowance = ?, cl_max_rate = ?, cl_per_seconds = ?, tl_last_event_sec = ?, tl_last_event_usec = ?, tl_current_allowance = ?, tl_max_rate = ?, tl_per_seconds = ? WHERE uuid = ?";
	sqlite3_stmt *query;
	int ret = sqlite3_prepare_v2(c->backing_db, sql, -1, &query, 0);
	if (ret != SQLITE_OK) {
		printf("Failed to prepare user save query: %s\n", sqlite3_errmsg(c->backing_db));
		sqlite3_finalize(query);
		sqlite3_close(c->backing_db);
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
	ret = sqlite3_bind_double(query, idx++, UNUSED_VAL);
	ret = sqlite3_bind_double(query, idx++, UNUSED_VAL);
	ret = sqlite3_bind_int(query, idx++, user->tile_limiter.last_event_time.tv_sec);
	ret = sqlite3_bind_int64(query, idx++, user->tile_limiter.last_event_time.tv_usec);
	ret = sqlite3_bind_double(query, idx++, user->tile_limiter.current_allowance);
	ret = sqlite3_bind_double(query, idx++, UNUSED_VAL);
	ret = sqlite3_bind_double(query, idx++, UNUSED_VAL);
	ret = sqlite3_bind_text(query, idx++, user->uuid, strlen(user->uuid), NULL);

	ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to update user: %s\n", sqlite3_errmsg(c->backing_db));
		sqlite3_finalize(query);
		sqlite3_close(c->backing_db);
		exit(-1);
	}
	sqlite3_finalize(query);
}

struct user *try_load_user(struct canvas *c, const char *uuid) {
	sqlite3_stmt *query;
	int ret = sqlite3_prepare_v2(c->backing_db, "SELECT * FROM users WHERE uuid = ?", -1, &query, 0);
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
		i += 2;
		user->tile_limiter.last_event_time.tv_sec = sqlite3_column_int(query, i++);
		user->tile_limiter.last_event_time.tv_usec = sqlite3_column_int64(query, i++);
		user->tile_limiter.current_allowance = sqlite3_column_double(query, i++);
		i += 2;
	}
	sqlite3_finalize(query);
	return user;
}

void add_user(struct canvas *c, const struct user *user) {
	sqlite3_stmt *query;
	const char *sql =
		"INSERT INTO users (username, uuid, remainingTiles, tileRegenSeconds, totalTilesPlaced, lastConnected, availableColors, level, hasSetUsername, isShadowBanned, maxTiles, tilesToNextLevel, levelProgress, cl_last_event_sec, cl_last_event_usec, cl_current_allowance, cl_max_rate, cl_per_seconds, tl_last_event_sec, tl_last_event_usec, tl_current_allowance, tl_max_rate, tl_per_seconds)"
		" VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
	int ret = sqlite3_prepare_v2(c->backing_db, sql, -1, &query, 0);
	if (ret != SQLITE_OK) {
		printf("Failed to prepare user insert query: %s\n", sqlite3_errmsg(c->backing_db));
		sqlite3_finalize(query);
		sqlite3_close(c->backing_db);
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
	sqlite3_bind_double(query, idx++, UNUSED_VAL);
	sqlite3_bind_double(query, idx++, UNUSED_VAL);
	sqlite3_bind_int(query, idx++, user->tile_limiter.last_event_time.tv_sec);
	sqlite3_bind_int64(query, idx++, user->tile_limiter.last_event_time.tv_usec);
	sqlite3_bind_double(query, idx++, user->tile_limiter.current_allowance);
	sqlite3_bind_double(query, idx++, UNUSED_VAL);
	sqlite3_bind_double(query, idx++, UNUSED_VAL);

	ret = sqlite3_step(query);
	if (ret != SQLITE_DONE) {
		printf("Failed to insert user: %s\n", sqlite3_errmsg(c->backing_db));
		sqlite3_finalize(query);
		sqlite3_close(c->backing_db);
		exit(-1);
	}
	sqlite3_finalize(query);
}

void drop_user_with_connection(struct canvas *c, struct mg_connection *connection) {
	list_foreach(c->connected_users, {
		struct user *user = (struct user *)arg;
		if (user->socket != connection) return;
		c->connected_user_count--;
		logr("User %s disconnected. (%4lu)\n", user->uuid, c->connected_user_count);
		user->last_connected_unix = (unsigned)time(NULL);
		save_user(c, user);
		mg_timer_free(&c->mgr.timers, user->tile_increment_timer);
		user->socket->is_draining = 1;
		list_remove(c->connected_users, {
			const struct user *list_user = (struct user *)arg;
			return list_user->socket == user->socket;
		});
	});
	send_user_count(c);
}

// end common request handling logic

// json response handling

cJSON *error_response(char *error_message) {
	cJSON *error = cJSON_CreateObject();
	cJSON_AddStringToObject(error, "rt", "error");
	cJSON_AddStringToObject(error, "msg", error_message);
	return error;
}

cJSON *color_to_json(struct color color) {
	cJSON *c = cJSON_CreateObject();
	cJSON_AddNumberToObject(c, "R", color.red);
	cJSON_AddNumberToObject(c, "G", color.green);
	cJSON_AddNumberToObject(c, "B", color.blue);
	cJSON_AddNumberToObject(c, "ID", color.color_id);
	return c;
}

struct administrator *find_in_admins(struct canvas *c, const char *uuid) {
	struct list_elem *elem = NULL;
	list_foreach_ro(elem, c->administrators) {
		struct administrator *admin = (struct administrator *)elem->thing;
		if (str_eq(admin->uuid, uuid)) return admin;
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

cJSON *handle_get_tile_info(struct canvas *c, const cJSON *user_id, const cJSON *x_param, const cJSON *y_param) {
	if (!cJSON_IsString(user_id)) return error_response("Invalid userID");
	if (!cJSON_IsNumber(x_param)) return error_response("X coordinate not a number");
	if (!cJSON_IsNumber(y_param)) return error_response("Y coordinate not a number");

	struct user *user = find_in_connected_users(c, user_id->valuestring);
	if (!user) return error_response("Not authenticated");

	if (!is_within_rate_limit(&user->tile_limiter)) {
		return NULL;
	}

	size_t x = x_param->valueint;
	size_t y = y_param->valueint;

	if (x > c->edge_length - 1) return error_response("Invalid X coordinate");
	if (y > c->edge_length - 1) return error_response("Invalid Y coordinate");

	struct tile *tile = &c->tiles[x + y * c->edge_length];
	struct user *queried_user = find_in_connected_users(c, tile->last_modifier);
	if (!queried_user) user = try_load_user(c, tile->last_modifier);
	if (!queried_user) return error_response("Couldn't find a user who modified that tile.");

	cJSON *response = base_response("ti");
	cJSON_AddStringToObject(response, "un", queried_user->user_name);
	cJSON_AddNumberToObject(response, "pt", tile->place_time_unix);

	return response;
}

cJSON *handle_get_colors(struct canvas *c, const cJSON *user_id) {
	if (!cJSON_IsString(user_id)) return error_response("No userID provided");
	struct user *user = find_in_connected_users(c, user_id->valuestring);
	if (!user) return error_response("Not authenticated");
	user->last_event_unix = (unsigned)time(NULL);
	return cJSON_Parse(c->color_response_cache);
}

cJSON *handle_set_nickname(struct canvas *c, const cJSON *user_id, const cJSON *name) {
	if (!cJSON_IsString(user_id)) return error_response("No userID provided");
	if (!cJSON_IsString(name)) return error_response("No nickname provided");
	struct user *user = find_in_connected_users(c, user_id->valuestring);
	if (!user) return error_response("Not authenticated");
	if (strlen(name->valuestring) > sizeof(user->user_name)) return error_response("Nickname too long");
	logr("User %s set their username to %s\n", user_id->valuestring, name->valuestring);
	strncpy(user->user_name, name->valuestring, sizeof(user->user_name) - 1);
	user->last_event_unix = (unsigned)time(NULL);
	return base_response("nameSetSuccess");
}

cJSON *broadcast_announcement(struct canvas *c, const char *message) {
	cJSON *response = base_response("announcement");
	cJSON_AddStringToObject(response, "message", message);
	broadcast(c, response);
	cJSON_Delete(response);
	return base_response("Success");
}

void kick_with_message(struct canvas *c, const struct user *user, const char *message, const char *reconnect_btn_text) {
	cJSON *response = base_response("kicked");
	cJSON_AddStringToObject(response, "message", message ? message : "Kicked");
	cJSON_AddStringToObject(response, "btn_text", reconnect_btn_text ? reconnect_btn_text : "Reconnect");
	send_json(response, user);
	cJSON_Delete(response);
	drop_user_with_connection(c, user->socket);
}

void drop_all_connections(struct canvas *c) {
	list_foreach(c->connected_users, {
		struct user *user = (struct user *)arg;
		user->last_connected_unix = (unsigned)time(NULL);
		c->connected_user_count--;
		save_user(c, user);
		mg_timer_free(&c->mgr.timers, user->tile_increment_timer);
		user->socket->is_draining = 1;
		list_remove(c->connected_users, {
			const struct user *list_user = (struct user *)arg;
			return list_user->socket == user->socket;
		});
		send_user_count(c);
	});
}

cJSON *shut_down_server(void) {
	g_running = false;
	return NULL;
}

cJSON *toggle_shadow_ban(struct canvas *c, const char *uuid) {
	struct user *user = find_in_connected_users(c, uuid);
	if (!user) user = try_load_user(c, uuid);
	if (!user) return error_response("No user found with that uuid");
	logr("Toggling is_shadow_banned to %s for user %s\n", !user->is_shadow_banned ? "true " : "false", uuid);
	user->is_shadow_banned = !user->is_shadow_banned;
	save_user(c, user);
	return base_response("Success");
}

cJSON *shadow_ban_user(struct canvas *c, const char *uuid) {
	struct user *user = find_in_connected_users(c, uuid);
	if (!user) user = try_load_user(c, uuid);
	if (!user) return error_response("No user found with that uuid");
	logr("Setting is_shadow_banned to true for user %s\n", uuid);
	user->is_shadow_banned = true;
	save_user(c, user);
	return base_response("Success");
}

cJSON *handle_ban_click(struct canvas *c, const cJSON *coordinates) {
	if (!cJSON_IsArray(coordinates)) return error_response("No valid coordinates provided");
	if (cJSON_GetArraySize(coordinates) < 2) return error_response("No valid coordinates provided");
	cJSON *x_param = cJSON_GetArrayItem(coordinates, 0);
	cJSON *y_param = cJSON_GetArrayItem(coordinates, 1);
	if (!cJSON_IsNumber(x_param)) return error_response("X coordinate not a number");
	if (!cJSON_IsNumber(y_param)) return error_response("Y coordinate not a number");

	size_t x = x_param->valueint;
	size_t y = y_param->valueint;

	if (x > c->edge_length - 1) return error_response("Invalid X coordinate");
	if (y > c->edge_length - 1) return error_response("Invalid Y coordinate");

	struct tile *tile = &c->tiles[x + y * c->edge_length];
	struct user *user = find_in_connected_users(c, tile->last_modifier);
	if (!user) user = try_load_user(c, tile->last_modifier);
	if (!user) return error_response("Couldn't find a user who modified that tile.");
	if (user->is_shadow_banned) return error_response("Already shadowbanned from there");
	// Just in case...
	struct administrator *admin = find_in_admins(c, user->uuid);
	if (admin) return error_response("Refusing to shadowban an administrator");
	logr("User %s shadowbanned from (%4lu,%4lu)\n", user->uuid, x, y);
	user->is_shadow_banned = true;
	save_user(c, user);

	return base_response("ban_click_success");
}

static void admin_place_tile(struct canvas *c, int x, int y, uint8_t color_id, const char *uuid) {
	if ((size_t)x > c->edge_length - 1) return;
	if ((size_t)y > c->edge_length - 1) return;
	if (x < 0) return;
	if (y < 0) return;

	struct tile *tile = &c->tiles[x + y * c->edge_length];
	if (tile->color_id == color_id) return;
	tile->color_id = color_id;
	tile->place_time_unix = (unsigned)time(NULL);
	memcpy(tile->last_modifier, uuid, sizeof(tile->last_modifier));

	// This print is for compatibility with https://github.com/zouppen/pikselipeli-parser
	logr("Received request: {\"requestType\":\"postTile\",\"userID\":\"%s\",\"X\":%i,\"Y\":%i,\"colorID\":\"%u\"}\n", uuid, x, y, color_id);

	// Record delta for persistence. These get flushed to disk every canvas_save_interval_sec seconds.
	struct tile_placement placement = {
		.x = x,
		.y = y,
		.tile = *tile
	};
	list_append(c->delta, placement);

	c->dirty = true;

	struct tile_update response = {
		.resp_type = RES_TILE_UPDATE,
		.color_id = color_id,
		.i = htonl(x + y * c->edge_length),
	};
	bin_broadcast(c, (const char *)&response, sizeof(response));
}

cJSON *handle_admin_brush(struct canvas *c, const cJSON *coordinates, const cJSON *colorID, const char *uuid) {
	if (!cJSON_IsArray(coordinates)) return error_response("No valid coordinates provided");
	if (!cJSON_IsNumber(colorID)) return error_response("colorID not a number");
	if (cJSON_GetArraySize(coordinates) < 2) return error_response("No valid coordinates provided");
	cJSON *x_param = cJSON_GetArrayItem(coordinates, 0);
	cJSON *y_param = cJSON_GetArrayItem(coordinates, 1);
	if (!cJSON_IsNumber(x_param)) return error_response("X coordinate not a number");
	if (!cJSON_IsNumber(y_param)) return error_response("Y coordinate not a number");

	uint8_t color_id = colorID->valueint;
	size_t x = x_param->valueint;
	size_t y = y_param->valueint;

	if (color_id > c->color_list.amount - 1) return error_response("Invalid colorID");

	for (int diffX = -3; diffX < 4; ++diffX) {
		for (int diffY = -3; diffY < 4; ++diffY) {
			admin_place_tile(c, x + diffX, y + diffY, color_id, uuid);
		}
	}

	return NULL;
}

cJSON *handle_admin_command(struct canvas *c, const cJSON *user_id, const cJSON *command) {
	if (!cJSON_IsString(user_id)) return error_response("No valid userID provided");
	struct administrator *admin = find_in_admins(c, user_id->valuestring);
	if (!admin) {
		logr("Rejecting admin command for unknown user %s. Naughty naughty!\n", user_id->valuestring);
		return error_response("Invalid admin userID");	
	}
	if (!cJSON_IsObject(command)) return error_response("No valid command object provided");
	const cJSON *action = cJSON_GetObjectItem(command, "action");	
	const cJSON *message = cJSON_GetObjectItem(command, "message");
	const cJSON *coordinates = cJSON_GetObjectItem(command, "coords");
	const cJSON *colorID = cJSON_GetObjectItem(command, "colorID");
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
			return broadcast_announcement(c, message->valuestring);
		} else {
			return error_response("You don't have announce permission");
		}
	}
	if (str_eq(action->valuestring, "toggle_shadowban")) {
		if (admin->can_shadowban) {
			return toggle_shadow_ban(c, message->valuestring);
		} else {
			return error_response("You don't have shadowban permission");
		}
	}
	if (str_eq(action->valuestring, "banclick")) {
		if (admin->can_banclick) {
			return handle_ban_click(c, coordinates);
		} else {
			return error_response("You don't have banclick permission");
		}
	}
	if (str_eq(action->valuestring, "brush")) {
		return handle_admin_brush(c, coordinates, colorID, admin->uuid);
	}
	return error_response("Unknown admin action invoked");
}

cJSON *handle_auth(struct canvas *c, const cJSON *user_id, struct mg_connection *socket) {
	if (!cJSON_IsString(user_id)) return error_response("Invalid userID");
	if (strlen(user_id->valuestring) > UUID_STR_LEN) return error_response("Invalid userID");

	// Kick old user if the user opens in more than one browser tab at once.
	struct user *user = find_in_connected_users(c, user_id->valuestring);
	if (user) {
		logr("Kicking %s, they opened a new session\n", user->uuid);
		kick_with_message(c, user, "It looks like you opened another tab?", "Reconnect here");
	}

	user = try_load_user(c, user_id->valuestring);
	if (!user) return error_response("Invalid userID");
	struct user *uptr = list_append(c->connected_users, *user)->thing;
	free(user);
	uptr->socket = socket;

	c->connected_user_count++;
	if (c->connected_user_count > c->settings.max_concurrent_users) {
		logr("Kicking %s. Server full. (Sad!)\n", uptr->uuid);
		kick_with_message(c, uptr, "Sorry, the server is full :(\n Try again later!", "Try again");
		return NULL;
	}

	assign_rate_limiter_limit(&uptr->canvas_limiter, &c->settings.getcanvas_max_rate, &c->settings.getcanvas_per_seconds);
	assign_rate_limiter_limit(&uptr->tile_limiter, &c->settings.setpixel_max_rate, &c->settings.setpixel_per_seconds);
	// Kinda pointless flag. If this thing is in connected_users list, it's valid.
	uptr->is_authenticated = true;

	logr("User %s connected. (%4lu)\n", uptr->uuid, c->connected_user_count);
	start_user_timer(uptr, socket->mgr);
	send_user_count(c);

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
	struct administrator *admin = find_in_admins(c, uptr->uuid);
	if (admin) {
		cJSON_AddBoolToObject(response, "showBanBtn", admin->can_banclick);
		cJSON_AddBoolToObject(response, "showCleanupBtn", admin->can_cleanup);
	}
	return response;
}

cJSON *handle_initial_auth(struct canvas *c, struct mg_connection *socket, struct remote_host *host) {
	if (host) {
		logr("Received initialAuth from %s\n", socket->label);
		host->total_accounts++;
		save_host(c, host);
		if (host->total_accounts > c->settings.max_users_per_ip) {
			char ip_buf[50];
			mg_ntoa(&host->addr, ip_buf, sizeof(ip_buf));
			logr("Rejecting initialAuth from %s, reached maximum of %li users\n", ip_buf, c->settings.max_users_per_ip);
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
	struct user *uptr = list_append(c->connected_users, user)->thing;
	add_user(c, uptr);
	uptr->socket = socket;

	// Set up rate limiting
	assign_rate_limiter_limit(&uptr->canvas_limiter, &c->settings.getcanvas_max_rate, &c->settings.getcanvas_per_seconds);
	assign_rate_limiter_limit(&uptr->tile_limiter, &c->settings.setpixel_max_rate, &c->settings.setpixel_per_seconds);
	uptr->canvas_limiter.current_allowance = c->settings.getcanvas_max_rate;
	uptr->tile_limiter.current_allowance = c->settings.setpixel_max_rate;
	gettimeofday(&uptr->tile_limiter.last_event_time, NULL);
	gettimeofday(&uptr->canvas_limiter.last_event_time, NULL);

	c->connected_user_count++;
	if (c->connected_user_count > c->settings.max_concurrent_users) {
		logr("Kicking %s. Server full. (Sad!)\n", uptr->uuid);
		kick_with_message(c, uptr, "Sorry, the server is full :(", "Try again");
		return NULL;
	}

	logr("User %s connected. (%4lu)\n", uptr->uuid, c->connected_user_count);
	start_user_timer(uptr, socket->mgr);
	send_user_count(c);

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

cJSON *handle_command(struct canvas *c, const char *cmd, size_t len, struct mg_connection *connection) {
	// cmd is not necessarily null-terminated. Trust len.
	cJSON *command = cJSON_ParseWithLength(cmd, len);
	if (!command) return error_response("No command provided");
	const cJSON *request_type = cJSON_GetObjectItem(command, "requestType");
	if (!cJSON_IsString(request_type)) return error_response("No requestType provided");

	const cJSON *user_id   = cJSON_GetObjectItem(command, "userID");
	const cJSON *name      = cJSON_GetObjectItem(command, "name");
	const cJSON *x         = cJSON_GetObjectItem(command, "X");
	const cJSON *y         = cJSON_GetObjectItem(command, "Y");
	const cJSON *admin_cmd = cJSON_GetObjectItem(command, "cmd");
	char *reqstr = request_type->valuestring;

	cJSON *response = NULL;
	if (str_eq(reqstr, "initialAuth")) {
		struct remote_host *host = extract_host(c, connection);
		response = handle_initial_auth(c, connection, host);
	} else if (str_eq(reqstr, "auth")) {
		response = handle_auth(c, user_id, connection);
	} else if (str_eq(reqstr, "gti")) {
		response = handle_get_tile_info(c, user_id, x, y);
	} else if (str_eq(reqstr, "getColors")) {
		response = handle_get_colors(c, user_id);
	} else if (str_eq(reqstr, "setUsername")) {
		response = handle_set_nickname(c, user_id, name);
	} else if (str_eq(reqstr, "admin_cmd")) {
		response = handle_admin_command(c, user_id, admin_cmd);
	} else {
		response = error_response("Unknown requestType");
	}

	cJSON_Delete(command);
	return response;
}

// end json response handling

// binary response handling

enum request_type {
	REQ_INITIAL_AUTH = 0,
	REQ_AUTH,
	REQ_GET_CANVAS,
	REQ_GET_TILE_INFO,
	REQ_POST_TILE,
	REQ_GET_COLORS,
	REQ_SET_USERNAME,
};

char *ack(enum response_id e) {
	char *response = malloc(1);
	*response = (char)e;
	return response;
}

char *error(enum response_id e) {
	return ack(e);
}

struct user_count {
	uint8_t type;
	// 1 byte padding :(
	uint16_t count;
};

void send_user_count(const struct canvas *c) {
	struct user_count resp = {
		.type = RES_USER_COUNT,
		.count = htons(c->connected_user_count),
	};
	bin_broadcast(c, (const char *)&resp, sizeof(resp));
}

struct request {
	uint8_t request_type;
	char uuid[UUID_STR_LEN];
	uint16_t x;
	uint16_t y;
	union {
		uint16_t color_id;
		uint16_t data_len;
	};
	char data[];
};

char *handle_req_auth(struct canvas *c, const struct request *req, struct mg_connection *connection, size_t *response_len) {
	(void)req;
	(void)c;
	(void)response_len;
	(void)connection;
	//TODO
	logr("TODO: req_auth\n");
	return NULL;
}

char *handle_req_get_canvas(struct canvas *c, const struct request *req, struct mg_connection *connection, size_t *response_len) {
	(void)c;
	(void)response_len;
	(void)connection;
	struct user *user = find_in_connected_users(c, req->uuid);
	if (!user) return error(ERR_INVALID_UUID);

	bool within_limit = is_within_rate_limit(&user->canvas_limiter);
	if (!within_limit) {
		logr("%s exceeded canvas rate limit\n", user->uuid);
		return error(ERR_RATE_LIMIT_EXCEEDED);
	}

	user->last_event_unix = (unsigned)time(NULL);

	struct timeval tmr;
	gettimeofday(&tmr, NULL);

	// nab a copy of the data, avoid blocking updates for others.
	uint8_t *copy;
	size_t copy_len;
	float copy_ratio;
	//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!
	pthread_mutex_lock(&c->canvas_cache_lock);
	copy_len = c->canvas_cache_len;
	copy_ratio = c->canvas_cache_compression_ratio;
	copy = malloc(copy_len + 1);
	memcpy(copy + 1, c->canvas_cache, copy_len);
	pthread_mutex_unlock(&c->canvas_cache_lock);
	//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!
	copy[0] = RES_CANVAS;
	long ms = get_ms_delta(tmr);
	char buf[64];
	human_file_size(copy_len, buf);
	logr("Sending zlib'd canvas to %s. (%.2f%%, %s, %lums)\n", user->uuid, copy_ratio, buf, ms);
	mg_ws_send(user->socket, (char *)copy, copy_len + 1, WEBSOCKET_OP_BINARY);
	free(copy);
	return NULL;
}

char *handle_req_get_tile_info(struct canvas *c, const struct request *req, struct mg_connection *connection, size_t *response_len) {
	//TODO
	(void)req;
	(void)c;
	(void)response_len;
	(void)connection;
	logr("TODO: req_get_tile_info\n");
	return NULL;
}

void dump_req(const struct request *req) {
	printf("request_type: %i\n", req->request_type);
	printf("uuid        : %.*s\n", UUID_STR_LEN, req->uuid);
	printf("x           : %i\n", req->x);
	printf("y           : %i\n", req->y);
	printf("cld/len     : %i\n", req->color_id);
}

char *handle_req_post_tile(struct canvas *c, const struct request *req, struct mg_connection *connection, size_t *response_len) {
	(void)c;
	(void)response_len;
	(void)connection;
	struct user *user = find_in_connected_users(c, req->uuid);

	if (!user) return error(ERR_INVALID_UUID);
	if (user->remaining_tiles < 1) return NULL;

	if (!is_within_rate_limit(&user->tile_limiter)) {
		return NULL;
	}

	uint8_t color_id = req->color_id;
	size_t x = req->x;
	size_t y = req->y;

	if (x > c->edge_length - 1) return NULL;
	if (y > c->edge_length - 1) return NULL;
	if (color_id > c->color_list.amount - 1) return NULL;

	user->remaining_tiles--;
	user->total_tiles_placed++;
	user->current_level_progress++;
	if (user->current_level_progress >= user->tiles_to_next_level) {
		level_up(user);
	}
	user->last_event_unix = (unsigned)time(NULL);

	if (user->is_shadow_banned) {
		logr("Rejecting request from shadowbanned user: {\"requestType\":\"postTile\",\"userID\":\"%s\",\"X\":%li,\"Y\":%li,\"colorID\":\"%u\"}\n", user->uuid, x, y, color_id);
		struct tile_update response = {
			.resp_type = RES_TILE_UPDATE,
			.color_id = color_id,
			.i = htonl(x + y * c->edge_length),
		};
		mg_ws_send(user->socket, (const char *)&response, sizeof(response), WEBSOCKET_OP_BINARY);
		return NULL;
	}

	// This print is for compatibility with https://github.com/zouppen/pikselipeli-parser
	logr("Received request: {\"requestType\":\"postTile\",\"userID\":\"%s\",\"X\":%li,\"Y\":%li,\"colorID\":\"%u\"}\n", user->uuid, x, y, color_id);

	struct tile *tile = &c->tiles[x + y * c->edge_length];
	tile->color_id = color_id;
	tile->place_time_unix = user->last_event_unix;
	memcpy(tile->last_modifier, user->uuid, sizeof(tile->last_modifier));

	// Record delta for persistence. These get flushed to disk every canvas_save_interval_sec seconds.
	struct tile_placement placement = {
		.x = x,
		.y = y,
		.tile = *tile
	};
	list_append(c->delta, placement);

	c->dirty = true;

	struct tile_update response = {
		.resp_type = RES_TILE_UPDATE,
		.color_id = color_id,
		.i = htonl(x + y * c->edge_length),
	};
	bin_broadcast(c, (const char *)&response, sizeof(response));
	return NULL; // The broadcast takes care of this
}

char *handle_req_get_colors(struct canvas *c, const struct request *req, struct mg_connection *connection, size_t *response_len) {
	(void)c;
	(void)connection;
	(void)response_len;
	struct user *user = find_in_connected_users(c, req->uuid);
	if (!user) return error(ERR_INVALID_UUID);

	user->last_event_unix = (unsigned)time(NULL);
	//TODO: cache as well maybe

	char *response = malloc(1 + c->color_list.amount * sizeof(struct color));
	response[0] = RES_COLOR_LIST;
	struct color *list = (struct color *)response + 1;
	for (size_t i = 0; i < c->color_list.amount; ++i) {
		list[i] = c->color_list.colors[i];
	}
	return response;
}

char *handle_req_set_username(struct canvas *canvas, const struct request *req, struct mg_connection *c, size_t *response_len) {
	//TODO
	logr("TODO: req_set_username\n");
	(void)canvas;
	(void)req;
	(void)c;
	(void)response_len;
	return NULL;
}

struct initial_auth_response {
	uint8_t response_type;
	uint32_t remaining_tiles;
	uint32_t max_tiles;
	uint32_t tiles_to_next_level;
	uint32_t current_level_progress;
	uint8_t level;
	char uuid[UUID_STR_LEN];
};

char *handle_req_initial_auth(struct canvas *c, const struct request *req, struct mg_connection *socket, size_t *response_len, struct remote_host *host) {
	(void)c;
	(void)req;
	(void)socket;
	(void)response_len;
	(void)host;
	return NULL;
	/*
	if (host) {
		logr("Received initialAuth from %s\n", socket->label);
		host->total_accounts++;
		save_host(c, host);
		if (host->total_accounts > c->settings.max_users_per_ip) {
			char ip_buf[50];
			mg_ntoa(&host->addr, ip_buf, sizeof(ip_buf));
			logr("Rejecting initialAuth from %s, reached maximum of %li users\n", ip_buf, c->settings.max_users_per_ip);
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
	struct user *uptr = list_append(c->connected_users, user)->thing;
	add_user(c, uptr);
	uptr->socket = socket;

	// Set up rate limiting
	assign_rate_limiter_limit(&uptr->canvas_limiter, &c->settings.getcanvas_max_rate, &g_canvas.settings.getcanvas_per_seconds);
	assign_rate_limiter_limit(&uptr->tile_limiter, &c->settings.setpixel_max_rate, &g_canvas.settings.setpixel_per_seconds);
	uptr->canvas_limiter.current_allowance = c->settings.getcanvas_max_rate;
	uptr->tile_limiter.current_allowance = c->settings.setpixel_max_rate;
	gettimeofday(&uptr->tile_limiter.last_event_time, NULL);
	gettimeofday(&uptr->canvas_limiter.last_event_time, NULL);

	c->connected_user_count++;
	if (c->connected_user_count > g_canvas.settings.max_concurrent_users) {
		logr("Kicking %s. Server full. (Sad!)\n", uptr->uuid);
		kick_with_message(c, uptr, "Sorry, the server is full :(", "Try again");
		return NULL;
	}

	logr("User %s connected. (%4lu)\n", uptr->uuid, c->connected_user_count);
	start_user_timer(uptr, socket->mgr);
	send_user_count(c);

	uptr->last_event_unix = (unsigned)time(NULL);

	char *res = malloc(1 +
					   UUID_STR_LEN +
					   sizeof(uptr->remaining_tiles) +
					   sizeof(uptr->level) +
					   sizeof(uptr->max_tiles) +
					   sizeof(uptr->tiles_to_next_level) +
					   sizeof(uptr->current_level_progress));

	struct initial_auth_response resp = {
		//.response_type = RES_AUTH_SUCCESS,
		//.remaining_tiles = htonl(uptr->remaining_tiles),
		.level = htonl(uptr->level),
		.max_tiles = htonl(uptr->max_tiles),
		.tiles_to_next_level = htonl(uptr->tiles_to_next_level),
		.current_level_progress = htonl(uptr->current_level_progress),
	};
	memcpy(&resp.uuid, uptr->uuid, UUID_STR_LEN);
	memcpy(res, &resp, sizeof(resp));

	return res;
	*/
}

char *handle_binary_command(struct canvas *c, const char *request, size_t len, struct mg_connection *connection, size_t *response_len) {
	if (!request || !len) return NULL;
	struct request *req = (struct request *)request;
	req->x = ntohs(req->x);
	req->y = ntohs(req->y);
	req->color_id = ntohs(req->color_id);

	enum request_type type = (enum request_type)req->request_type;
	switch (type) {
		case REQ_AUTH:          return handle_req_auth(c, req, connection, response_len);
		case REQ_GET_CANVAS:    return handle_req_get_canvas(c, req, connection, response_len);
		case REQ_GET_TILE_INFO: return handle_req_get_tile_info(c, req, connection, response_len);
		case REQ_POST_TILE:     return handle_req_post_tile(c, req, connection, response_len);
		case REQ_GET_COLORS:    return handle_req_get_colors(c, req, connection, response_len);
		case REQ_SET_USERNAME:  return handle_req_set_username(c, req, connection, response_len);
		case REQ_INITIAL_AUTH: {
			struct remote_host *host = extract_host(c, connection);
			return handle_req_initial_auth(c, req, connection, response_len, host);
		}
	}
	return NULL;
}

// end binary response handling

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

void do_db_backup(struct canvas *c) {
	const time_t cur_time = time(NULL);
	struct tm time = *localtime(&cur_time);
	char target[128];
	snprintf(target, sizeof(target), "backups/backup-%d-%02d-%02dT%02d:%02d:%02d.db",
	time.tm_year + 1900,
	time.tm_mon + 1,
	time.tm_mday,
	time.tm_hour,
	time.tm_min,
	time.tm_sec);
	logr("Backing up db to %s", target);
	struct timeval tmr;
	gettimeofday(&tmr, NULL);
	sqlite3 *target_db;
	if (sqlite3_open(target, &target_db) != SQLITE_OK) {
		logr("\nFailed to open db %s for backup.\n", target);
		return;
	}

	sqlite3_backup *backup = sqlite3_backup_init(target_db, "main", c->backing_db, "main");
	if (backup) {
		sqlite3_backup_step(backup, -1);
		sqlite3_backup_finish(backup);
	}

	sqlite3_close(target_db);

	long ms = get_ms_delta(tmr);
	printf(" (%lums)\n", ms);
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
	const cJSON *max_concurrent = cJSON_GetObjectItem(config, "max_concurrent_users");
	if (!cJSON_IsNumber(max_concurrent)) {
		logr("max_concurrent_users not a number, exiting.\n");
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
	c->settings.max_concurrent_users = max_concurrent->valueint;
	strncpy(c->settings.listen_url, listen_url->valuestring, sizeof(c->settings.listen_url) - 1);
	strncpy(c->settings.dbase_file, dbase_file->valuestring, sizeof(c->settings.dbase_file) - 1);

	// Load up administrator list
	list_destroy(&c->administrators);
	cJSON *admin = NULL;
	cJSON_ArrayForEach(admin, administrators) {
		if (!cJSON_IsObject(admin)) continue;
		cJSON *uuid = cJSON_GetObjectItem(admin, "uuid");
		cJSON *shutdown = cJSON_GetObjectItem(admin, "shutdown");
		cJSON *announce = cJSON_GetObjectItem(admin, "announce");
		cJSON *shadowban = cJSON_GetObjectItem(admin, "shadowban");
		cJSON *banclick = cJSON_GetObjectItem(admin, "banclick");
		cJSON *cleanup = cJSON_GetObjectItem(admin, "cleanup");
		if (!cJSON_IsString(uuid)) continue;
		if (!cJSON_IsBool(shutdown)) continue;
		if (!cJSON_IsBool(announce)) continue;
		if (!cJSON_IsBool(shadowban)) continue;
		if (!cJSON_IsBool(banclick)) continue;
		struct administrator a = (struct administrator){
			.can_shutdown  = cJSON_IsBool(shutdown)  ? shutdown->valueint  : false,
			.can_announce  = cJSON_IsBool(announce)  ? announce->valueint  : false,
			.can_shadowban = cJSON_IsBool(shadowban) ? shadowban->valueint : false,
			.can_banclick  = cJSON_IsBool(banclick)  ? banclick->valueint  : false,
			.can_cleanup   = cJSON_IsBool(cleanup)   ? cleanup->valueint   : false,
		};
		strncpy(a.uuid, uuid->valuestring, sizeof(a.uuid) - 1);
		list_append(c->administrators, a);
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

	logr("Loaded conf:\n");
	printf("%s\n", conf);
	cJSON_Delete(config);
	free(conf);
	return;
bail:
	logr("params.json invalid, exiting.\n");
	if (c->backing_db) sqlite3_close(c->backing_db);
	exit(-1);
}

static void user_tile_increment_fn(void *arg) {
	struct user *user = (struct user *)arg;
	// tile_regen_seconds may change in level_up(), so keep it updated here.
	user->tile_increment_timer->period_ms = user->tile_regen_seconds * 1000;
	if (user->remaining_tiles >= user->max_tiles) return;
	user->remaining_tiles++;
	char response[2];
	response[0] = RES_TILE_INCREMENT;
	response[1] = 1;
	mg_ws_send(user->socket, response, 2, WEBSOCKET_OP_BINARY);
}

void start_user_timer(struct user *user, struct mg_mgr *mgr) {
	user->tile_increment_timer = mg_timer_add(mgr, user->tile_regen_seconds * 1000, MG_TIMER_REPEAT, user_tile_increment_fn, user);
}

void update_getcanvas_cache(struct canvas *c) {
	size_t tilecount = c->edge_length * c->edge_length;

	uint8_t *pixels = malloc(tilecount);
	for (size_t i = 0; i < tilecount; ++i) {
		pixels[i] = c->tiles[i].color_id;
	}

	size_t compressed_len = compressBound(tilecount);
	float orig_len = compressed_len;
	uint8_t *compressed = malloc(compressed_len);
	int ret = compress(compressed, &compressed_len, pixels, tilecount);
	float compression_ratio = 100.0f * ((float)compressed_len / orig_len);
	free(pixels);
	if (ret != Z_OK) {
		if (ret == Z_MEM_ERROR) logr("Z_MEM_ERROR\n");
		if (ret == Z_BUF_ERROR) logr("Z_BUF_ERROR\n");
	}

	uint8_t *old = NULL;
	//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!
	pthread_mutex_lock(&c->canvas_cache_lock);
	old = c->canvas_cache;
	c->canvas_cache = compressed;
	c->canvas_cache_len = compressed_len;
	c->canvas_cache_compression_ratio = compression_ratio;
	pthread_mutex_unlock(&c->canvas_cache_lock);
	//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!//!
	free(old);
}

void *worker_thread(void *arg) {
	struct canvas *c = (struct canvas *)arg;
	while (true) {
		sleep_ms(1000); //TODO: configurable?
		// Compress and swap canvas cache data
		if (!c->dirty) continue;
		update_getcanvas_cache(c);
	}
	return NULL;
}

static void callback_fn(struct mg_connection *c, int event_type, void *event_data, void *arg) {
	struct canvas *canvas = (struct canvas *)arg;

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
				mg_ntoa(&c->rem, c->label, sizeof(c->label));
			}
			mg_ws_upgrade(c, msg, NULL);
		} else if (mg_http_match_uri(msg, "/brew_coffee")) {
			mg_http_reply(c, 418, "", "Sorry, can't do that. :(");
		}
	} else if (event_type == MG_EV_WS_MSG) {
		struct mg_ws_message *wm = (struct mg_ws_message *)event_data;
		uint8_t op = wm->flags & 15;
		if (op == WEBSOCKET_OP_BINARY) {
			size_t response_len = 0;
			char *response = handle_binary_command(canvas, wm->data.ptr, wm->data.len, c, &response_len);
			if (response) {
				mg_ws_send(c, response, response_len, WEBSOCKET_OP_BINARY);
				free(response);
			}
		} else if (op == WEBSOCKET_OP_TEXT) {
			cJSON *response = handle_command(canvas, wm->data.ptr, wm->data.len, c);
			char *response_str = cJSON_PrintUnformatted(response);
			if (response_str) {
				mg_ws_send(c, response_str, strlen(response_str), WEBSOCKET_OP_TEXT);
				free(response_str);
			}
			cJSON_Delete(response);
		}
	} else if (event_type == MG_EV_CLOSE) {
		drop_user_with_connection(canvas, c);
	}
}

static void ping_timer_fn(void *arg) {
	struct mg_mgr *mgr = (struct mg_mgr *)arg;
	for (struct mg_connection *c = mgr->conns; c != NULL && c->is_websocket; c = c->next) {
		mg_ws_send(c, NULL, 0, WEBSOCKET_OP_PING);
	}
}

void start_transaction(sqlite3 *db) {
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

void commit_transaction(sqlite3 *db) {
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
	struct canvas *canvas = (struct canvas *)arg;
	size_t users = list_elems(&canvas->connected_users);
	if (!users) return;
	start_transaction(canvas->backing_db);
	struct list_elem *elem = NULL;
	list_foreach_ro(elem, canvas->connected_users) {
		struct user *user = (struct user *)elem->thing;
		save_user(canvas, user);
	}
	commit_transaction(canvas->backing_db);

	// Check and kick inactive users
	uint64_t current_time_unix = (unsigned)time(NULL);
	list_foreach(canvas->connected_users, {
		struct user *user = (struct user *)arg;
		size_t sec_since_last_event = current_time_unix - user->last_event_unix;
		if (sec_since_last_event > canvas->settings.kick_inactive_after_sec) {
			logr("Kicking inactive user %s\n", user->uuid);
			kick_with_message(canvas, user, "You haven't drawn anything for a while, so you were disconnected.", "Reconnect");
		}
	});
}

static void canvas_save_timer_fn(void *arg) {
	struct canvas *canvas = (struct canvas *)arg;

	if (!canvas->dirty) return;

	struct timeval timer;
	gettimeofday(&timer, NULL);

	sqlite3 *db = canvas->backing_db;
	
	start_transaction(db);

	sqlite3_stmt *insert;
	int ret = sqlite3_prepare_v2(db, "UPDATE tiles SET colorID = ?, lastModifier = ?, placeTime = ? WHERE X = ? AND Y = ?", -1, &insert, NULL);
	if (ret != SQLITE_OK) {
		printf("Failed to prepare tile update: %s\n", sqlite3_errmsg(db));
		goto bail;
	}

	logr("Saving canvas to disk (%li events) ", list_elems(&canvas->delta));

	struct list_elem *elem = NULL;
	list_foreach_ro(elem, canvas->delta) {
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
	list_destroy(&canvas->delta);

bail:
	sqlite3_finalize(insert);

	commit_transaction(db);
	long ms = get_ms_delta(timer);
	printf("(%lims)\n", ms);
	canvas->dirty = false;
}

void ensure_tiles_table(sqlite3 *db, size_t edge_length) {
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

	start_transaction(db);

	logr("Running initial tile db init...\n");
	sqlite3_stmt *insert;
	ret = sqlite3_prepare_v2(db, "INSERT INTO tiles (X, Y, colorID, lastModifier, placeTime) VALUES (?, ?, 3, \"\", 0)", -1, &insert, NULL);
	if (ret != SQLITE_OK) printf("Failed to prepare tile insert: %s\n", sqlite3_errmsg(db));

	for (size_t y = 0; y < edge_length; ++y) {
		for (size_t x = 0; x < edge_length; ++x) {
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

	commit_transaction(db);

	logr("db init done.\n");
}

void ensure_valid_db(struct canvas *c) {
	char *schema = load_file("schema.sql", NULL);
	if (!schema) {
		exit(-1);
	}
	int ret = sqlite3_exec(c->backing_db, schema, NULL, NULL, NULL);
	if (ret == SQLITE_ABORT) {
		printf("Failed to create database, check schema file. Error: %s\n", sqlite3_errmsg(c->backing_db));
		sqlite3_close(c->backing_db);
		exit(-1);
	}
	free(schema);
	ensure_tiles_table(c->backing_db, c->settings.new_db_canvas_size);
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

	c->tiles = calloc(c->edge_length * c->edge_length, sizeof(struct tile));
	c->connected_users = LIST_INITIALIZER;
	c->connected_hosts = LIST_INITIALIZER;
	printf("Loading %ux%u canvas...\n", c->edge_length, c->edge_length);

	sqlite3_stmt *query;
	sqlite3_prepare_v2(c->backing_db, "select * from tiles", -1, &query, NULL);

	while (sqlite3_step(query) != SQLITE_DONE) {
		int idx = 1;
		size_t x = sqlite3_column_int(query, idx++);
		size_t y = sqlite3_column_int(query, idx++);
		c->tiles[x + y * c->edge_length].color_id = sqlite3_column_int(query, idx++);
		const char *last_modifier = (const char *)sqlite3_column_text(query, idx++);
		strncpy(c->tiles[x + y * c->edge_length].last_modifier, last_modifier, UUID_STR_LEN - 1);
		c->tiles[x + y * c->edge_length].place_time_unix = sqlite3_column_int64(query, idx++);
	}
	sqlite3_finalize(query);
	c->dirty = false;
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
	ensure_valid_db(c);
	load_tiles(c);

	return false;
}

// Intersect ^C to safely shut down
void (*signal(int signo, void (*func)(int)))(int);
typedef void sigfunc(int);
sigfunc *signal(int, sigfunc*);

void sig_handler(int sig) {
	switch (sig) {
		case SIGINT:
		case SIGTERM:
			printf("Received SIGTERM, stopping.\n");
			g_running = false;
			break;
		case SIGUSR1:
			printf("Received SIGUSR1, reloading config...\n");
			g_reload_config = true;
			break;
		case SIGUSR2:
			g_do_db_backup = true;
			break;
	}
}

void start_worker_thread(struct canvas *c) {
	pthread_attr_t attribs;
	pthread_attr_init(&attribs);
	pthread_attr_setdetachstate(&attribs, PTHREAD_CREATE_JOINABLE);
	pthread_setname_np(c->canvas_worker_thread, "CanvasCacheWorker");
	int ret = pthread_create(&c->canvas_worker_thread, &attribs, worker_thread, c);
	pthread_attr_destroy(&attribs);
	if (ret < 0) printf("Oops\n");
}

int main(void) {
	setbuf(stdout, NULL); // Disable output buffering

	struct canvas canvas = (struct canvas){ 0 };
	load_config(&canvas);

	struct list_elem *elem = NULL;
	list_foreach_ro(elem, canvas.administrators) {
		struct administrator *admin = (struct administrator *)elem->thing;

		if (str_eq(admin->uuid, "<Desired userID here>")) {
			logr("Warning - Admin UUID still at default, anyone can shut down this server.\n");
			logr("Substitute uuid in params.json admin list with your desired UUID before running.\n");
			exit(-1);
		}
	}

	if (signal(SIGINT, sig_handler) == SIG_ERR) {
		printf("Failed to register sigint handler\n");
		return -1;
	}
	if (signal(SIGTERM, sig_handler) == SIG_ERR) {
		printf("Failed to register sigterm handler\n");
		return -1;
	}
	if (signal(SIGUSR1, sig_handler) == SIG_ERR) {
		printf("Failed to register SIGUSR1 handler\n");
		return -1;
	}
	if (signal(SIGUSR2, sig_handler) == SIG_ERR) {
		printf("Failed to register SIGUSR2 handler\n");
		return -1;
	}
	printf("Using SQLite v%s\n", sqlite3_libversion());
	if (set_up_db(&canvas)) {
		printf("Failed to set up db\n");
		return -1;
	}

	mg_mgr_init(&canvas.mgr);
	//ws ping loop. TODO: Probably do this from the client side instead.
	mg_timer_add(&canvas.mgr, 1000 * canvas.settings.websocket_ping_interval_sec, MG_TIMER_REPEAT, ping_timer_fn, &canvas.mgr);
	mg_timer_add(&canvas.mgr, 1000 * canvas.settings.canvas_save_interval_sec, MG_TIMER_REPEAT, canvas_save_timer_fn, &canvas);
	mg_timer_add(&canvas.mgr, 1000 * canvas.settings.users_save_interval_sec, MG_TIMER_REPEAT, users_save_timer_fn, &canvas);
	printf("Starting WS listener on %s/ws\n", canvas.settings.listen_url);
	mg_http_listen(&canvas.mgr, canvas.settings.listen_url, callback_fn, &canvas);
	// Set up canvas cache and start a background worker to refresh it
	update_getcanvas_cache(&canvas);
	start_worker_thread(&canvas);
	while (g_running) {
		if (g_reload_config) {
			load_config(&canvas);
			g_reload_config = false;
		}
		if (g_do_db_backup) {
			do_db_backup(&canvas);
			g_do_db_backup = false;
		}
		mg_mgr_poll(&canvas.mgr, 1000);
	}

	cJSON *response = base_response("disconnecting");
	broadcast(&canvas, response);
	cJSON_Delete(response);
	drop_all_connections(&canvas);

	//FIXME: Hack. Just flush some events before closing
	for (size_t i = 0; i < 100; ++i) {
		mg_mgr_poll(&canvas.mgr, 1);
	}
	logr("Saving canvas one more time...\n");
	canvas_save_timer_fn(&canvas);
	logr("Saving users...\n");
	users_save_timer_fn(&canvas);

	printf("Closing db\n");
	mg_mgr_free(&canvas.mgr);
	free(canvas.tiles);
	free(canvas.color_list.colors);
	free(canvas.color_response_cache);
	list_destroy(&canvas.connected_users);
	list_destroy(&canvas.connected_hosts);
	list_destroy(&canvas.administrators);
	list_destroy(&canvas.delta);
	sqlite3_close(canvas.backing_db);
	return 0;
}
