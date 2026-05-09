// main.c – Graph Visualizer with Raylib + HTTP API + SQLite (C version)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sqlite3.h>
#include <microhttpd.h>
#include <cjson/cJSON.h>
#include <uthash.h>
#include "raylib.h"    // <-- Add this line back

// ----------------------------- Configuration ---------------------------------
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720
#define FPS_TARGET    60

#define REPULSION_STRENGTH      2500.0f
#define ATTRACTION_STRENGTH     0.005f
#define DESIRED_EDGE_LENGTH     150.0f
#define DAMPING                 0.85f
#define ITERATIONS_PER_FRAME    5
#define MAX_FORCE               30.0f
#define TIME_STEP               0.1f

#define NODE_RADIUS             12
#define SELECTED_NODE_RADIUS    16
#define MIN_DISTANCE            32.0f

#define EDGE_COLOR              LIGHTGRAY
#define NODE_COLOR              SKYBLUE
#define SELECTED_NODE_COLOR     GOLD
#define UI_BG_COLOR             (Color){30, 30, 40, 200}
#define SOLARIZED_BASE03        (Color){0, 43, 54}

// ----------------------------- Uthash structures ----------------------------
typedef struct Node {
    char id[64];
    char label[128];
    float x, y;
    float fx, fy;
    char *metadata;
    UT_hash_handle hh;
} Node;

typedef struct EdgeSet {
    char key[128];
    UT_hash_handle hh;
} EdgeSet;

typedef struct TagList {
    char **tags;
    int count;
    int capacity;
} TagList;

typedef struct NodeTags {
    char node_id[64];
    TagList tags;
    UT_hash_handle hh;
} NodeTags;

Node *nodes = NULL;
EdgeSet *edges_set = NULL;
NodeTags *tags_hash = NULL;

typedef struct Edge {
    char src[64];
    char tgt[64];
} Edge;
Edge *edges_list = NULL;
int edges_count = 0;
int edges_capacity = 0;

// ----------------------------- Database & Queue ------------------------------
static const char *DB_URI = "file::memory:?cache=shared";
sqlite3 *db = NULL;

typedef enum {
    MSG_ADD_NODE,
    MSG_ADD_EDGE,
    MSG_ADD_TAGS,
    MSG_DELETE_NODE,
    MSG_DELETE_EDGE
} MsgType;

typedef struct QueueMsg {
    MsgType type;
    char id1[64];
    char id2[64];
    char *metadata;
    char *tags_str;
    struct QueueMsg *next;
} QueueMsg;

QueueMsg *queue_head = NULL, *queue_tail = NULL;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

void enqueue_msg(QueueMsg *msg) {
    pthread_mutex_lock(&queue_mutex);
    msg->next = NULL;
    if (queue_tail) {
        queue_tail->next = msg;
        queue_tail = msg;
    } else {
        queue_head = queue_tail = msg;
    }
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
}

QueueMsg* dequeue_msg_nonblock(void) {
    pthread_mutex_lock(&queue_mutex);
    QueueMsg *msg = queue_head;
    if (msg) {
        queue_head = msg->next;
        if (!queue_head) queue_tail = NULL;
    }
    pthread_mutex_unlock(&queue_mutex);
    return msg;
}

// ----------------------------- SQLite helpers --------------------------------
sqlite3* get_db_conn(void) {
    sqlite3 *conn;
    sqlite3_open(DB_URI, &conn);
    sqlite3_exec(conn, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    return conn;
}

void init_database(void) {
    sqlite3_open(DB_URI, &db);
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    const char *sql =
        "CREATE TABLE IF NOT EXISTS nodes ("
        "  id TEXT PRIMARY KEY,"
        "  label TEXT,"
        "  x REAL,"
        "  y REAL,"
        "  metadata TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS edges ("
        "  source TEXT,"
        "  target TEXT,"
        "  PRIMARY KEY (source, target)"
        ");"
        "CREATE TABLE IF NOT EXISTS node_tags ("
        "  node_id TEXT,"
        "  tag TEXT,"
        "  PRIMARY KEY (node_id, tag),"
        "  FOREIGN KEY (node_id) REFERENCES nodes(id) ON DELETE CASCADE"
        ");";
    char *errmsg = NULL;
    sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (errmsg) { fprintf(stderr, "SQL error: %s\n", errmsg); sqlite3_free(errmsg); }
}

void db_insert_node(const char *id, const char *label, float x, float y, const char *metadata) {
    sqlite3 *conn = get_db_conn();
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(conn, "INSERT OR REPLACE INTO nodes (id, label, x, y, metadata) VALUES (?,?,?,?,?)", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, label, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, x);
    sqlite3_bind_double(stmt, 4, y);
    sqlite3_bind_text(stmt, 5, metadata, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(conn);
}

void db_insert_edge(const char *src, const char *tgt) {
    sqlite3 *conn = get_db_conn();
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(conn, "INSERT OR IGNORE INTO edges (source, target) VALUES (?,?)", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, src, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, tgt, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(conn);
}

void db_insert_tag(const char *node_id, const char *tag) {
    sqlite3 *conn = get_db_conn();
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(conn, "INSERT OR IGNORE INTO node_tags (node_id, tag) VALUES (?,?)", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, node_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, tag, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(conn);
}

void db_delete_node(const char *node_id) {
    sqlite3 *conn = get_db_conn();
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(conn, "DELETE FROM nodes WHERE id = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, node_id, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(conn);
}

void db_delete_edge(const char *src, const char *tgt) {
    sqlite3 *conn = get_db_conn();
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(conn, "DELETE FROM edges WHERE source = ? AND target = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, src, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, tgt, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(conn);
}

// ----------------------------- In-memory helpers ----------------------------
void mem_add_node(const char *id, const char *label, float x, float y, const char *metadata, const char **tags, int tag_count) {
    Node *node = malloc(sizeof(Node));
    strncpy(node->id, id, sizeof(node->id)-1);
    node->id[sizeof(node->id)-1] = '\0';
    strncpy(node->label, label, sizeof(node->label)-1);
    node->label[sizeof(node->label)-1] = '\0';
    node->x = x; node->y = y;
    node->fx = node->fy = 0.0f;
    node->metadata = malloc(strlen(metadata)+1);
    strcpy(node->metadata, metadata);
    HASH_ADD_STR(nodes, id, node);

    NodeTags *nt = NULL;
    HASH_FIND_STR(tags_hash, id, nt);
    if (!nt) {
        nt = malloc(sizeof(NodeTags));
        strncpy(nt->node_id, id, sizeof(nt->node_id)-1);
        nt->node_id[sizeof(nt->node_id)-1] = '\0';
        nt->tags.count = 0;
        nt->tags.capacity = 4;
        nt->tags.tags = malloc(nt->tags.capacity * sizeof(char*));
        HASH_ADD_STR(tags_hash, node_id, nt);
    }
    for (int i = 0; i < tag_count; i++) {
        if (nt->tags.count >= nt->tags.capacity) {
            nt->tags.capacity *= 2;
            nt->tags.tags = realloc(nt->tags.tags, nt->tags.capacity * sizeof(char*));
        }
        nt->tags.tags[nt->tags.count] = malloc(strlen(tags[i])+1);
        strcpy(nt->tags.tags[nt->tags.count], tags[i]);
        nt->tags.count++;
    }
}

void mem_add_edge(const char *src, const char *tgt) {
    EdgeSet *e = malloc(sizeof(EdgeSet));
    snprintf(e->key, sizeof(e->key), "%s|%s", src, tgt);
    HASH_ADD_STR(edges_set, key, e);

    if (edges_count >= edges_capacity) {
        edges_capacity = edges_capacity ? edges_capacity*2 : 16;
        edges_list = realloc(edges_list, edges_capacity * sizeof(Edge));
    }
    memset(&edges_list[edges_count], 0, sizeof(Edge));
    strncpy(edges_list[edges_count].src, src, sizeof(edges_list[edges_count].src)-1);
    strncpy(edges_list[edges_count].tgt, tgt, sizeof(edges_list[edges_count].tgt)-1);
    edges_count++;
}

void mem_add_tags(const char *node_id, const char **tags, int tag_count) {
    NodeTags *nt = NULL;
    HASH_FIND_STR(tags_hash, node_id, nt);
    if (!nt) {
        nt = malloc(sizeof(NodeTags));
        strncpy(nt->node_id, node_id, sizeof(nt->node_id)-1);
        nt->node_id[sizeof(nt->node_id)-1] = '\0';
        nt->tags.count = 0;
        nt->tags.capacity = 4;
        nt->tags.tags = malloc(nt->tags.capacity * sizeof(char*));
        HASH_ADD_STR(tags_hash, node_id, nt);
    }
    for (int i = 0; i < tag_count; i++) {
        if (nt->tags.count >= nt->tags.capacity) {
            nt->tags.capacity *= 2;
            nt->tags.tags = realloc(nt->tags.tags, nt->tags.capacity * sizeof(char*));
        }
        nt->tags.tags[nt->tags.count] = malloc(strlen(tags[i])+1);
        strcpy(nt->tags.tags[nt->tags.count], tags[i]);
        nt->tags.count++;
    }
}

void rebuild_edge_list(void) {
    free(edges_list);
    edges_count = 0;
    edges_capacity = 0;
    edges_list = NULL;
    EdgeSet *e, *tmp;
    HASH_ITER(hh, edges_set, e, tmp) {
        if (edges_count >= edges_capacity) {
            edges_capacity = edges_capacity ? edges_capacity*2 : 16;
            edges_list = realloc(edges_list, edges_capacity * sizeof(Edge));
        }
        memset(&edges_list[edges_count], 0, sizeof(Edge));
        sscanf(e->key, "%[^|]|%[^|]", edges_list[edges_count].src, edges_list[edges_count].tgt);
        edges_count++;
    }
}

void mem_delete_node(const char *node_id) {
    Node *node = NULL;
    HASH_FIND_STR(nodes, node_id, node);
    if (node) {
        HASH_DEL(nodes, node);
        free(node->metadata);
        free(node);
    }
    NodeTags *nt = NULL;
    HASH_FIND_STR(tags_hash, node_id, nt);
    if (nt) {
        HASH_DEL(tags_hash, nt);
        for (int i = 0; i < nt->tags.count; i++) free(nt->tags.tags[i]);
        free(nt->tags.tags);
        free(nt);
    }
    EdgeSet *e, *tmp;
    HASH_ITER(hh, edges_set, e, tmp) {
        char src[64], tgt[64];
        sscanf(e->key, "%[^|]|%[^|]", src, tgt);
        if (strcmp(src, node_id) == 0 || strcmp(tgt, node_id) == 0) {
            HASH_DEL(edges_set, e);
            free(e);
        }
    }
    rebuild_edge_list();
}

void mem_delete_edge(const char *src, const char *tgt) {
    char key[128];
    snprintf(key, sizeof(key), "%s|%s", src, tgt);
    EdgeSet *e = NULL;
    HASH_FIND_STR(edges_set, key, e);
    if (e) {
        HASH_DEL(edges_set, e);
        free(e);
    }
    rebuild_edge_list();
}

void load_initial_data(void) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT id, label, x, y, metadata FROM nodes", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *id = (const char*)sqlite3_column_text(stmt, 0);
        const char *label = (const char*)sqlite3_column_text(stmt, 1);
        float x = (float)sqlite3_column_double(stmt, 2);
        float y = (float)sqlite3_column_double(stmt, 3);
        const char *metadata = (const char*)sqlite3_column_text(stmt, 4) ?: "{}";
        mem_add_node(id, label, x, y, metadata, NULL, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(db, "SELECT source, target FROM edges", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *src = (const char*)sqlite3_column_text(stmt, 0);
        const char *tgt = (const char*)sqlite3_column_text(stmt, 1);
        mem_add_edge(src, tgt);
    }
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(db, "SELECT node_id, tag FROM node_tags", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *node_id = (const char*)sqlite3_column_text(stmt, 0);
        const char *tag = (const char*)sqlite3_column_text(stmt, 1);
        mem_add_tags(node_id, &tag, 1);
    }
    sqlite3_finalize(stmt);
}

void add_sample_nodes(void) {
    if (HASH_COUNT(nodes) == 0) {
        const char *meta = "{}";
        const char *tags1[] = {"sample"};
        mem_add_node("sample1", "Node Alpha", -150.0f, -80.0f, meta, tags1, 1);
        mem_add_node("sample2", "Node Beta", 180.0f, 90.0f, meta, tags1, 1);
        mem_add_edge("sample1", "sample2");
        db_insert_node("sample1", "Node Alpha", -150.0f, -80.0f, meta);
        db_insert_node("sample2", "Node Beta", 180.0f, 90.0f, meta);
        db_insert_edge("sample1", "sample2");
        db_insert_tag("sample1", "sample");
        db_insert_tag("sample2", "sample");
    }
}

// ----------------------------- Force layout --------------------------------
void resolve_collisions(void) {
    Node *n1, *n2;
    for (n1 = nodes; n1 != NULL; n1 = n1->hh.next) {
        for (n2 = n1->hh.next; n2 != NULL; n2 = n2->hh.next) {
            float dx = n1->x - n2->x;
            float dy = n1->y - n2->y;
            float dist = sqrtf(dx*dx + dy*dy);
            if (dist < MIN_DISTANCE && dist > 0.001f) {
                float overlap = MIN_DISTANCE - dist;
                float angle = atan2f(dy, dx);
                float push_x = cosf(angle) * overlap * 0.5f;
                float push_y = sinf(angle) * overlap * 0.5f;
                n1->x += push_x;
                n1->y += push_y;
                n2->x -= push_x;
                n2->y -= push_y;
            }
        }
    }
}

void update_layout(void) {
    if (HASH_COUNT(nodes) == 0) return;
    Node *n1, *n2;
    for (n1 = nodes; n1 != NULL; n1 = n1->hh.next) {
        n1->fx = 0.0f;
        n1->fy = 0.0f;
    }
    for (n1 = nodes; n1 != NULL; n1 = n1->hh.next) {
        for (n2 = n1->hh.next; n2 != NULL; n2 = n2->hh.next) {
            float dx = n1->x - n2->x;
            float dy = n1->y - n2->y;
            float dist_sq = dx*dx + dy*dy + 1e-5f;
            float dist = sqrtf(dist_sq);
            float force = REPULSION_STRENGTH / dist_sq;
            float fx = (dx / dist) * force;
            float fy = (dy / dist) * force;
            n1->fx += fx;
            n1->fy += fy;
            n2->fx -= fx;
            n2->fy -= fy;
        }
    }
    for (int i = 0; i < edges_count; i++) {
        Node *src = NULL, *tgt = NULL;
        HASH_FIND_STR(nodes, edges_list[i].src, src);
        HASH_FIND_STR(nodes, edges_list[i].tgt, tgt);
        if (src && tgt) {
            float dx = src->x - tgt->x;
            float dy = src->y - tgt->y;
            float dist = sqrtf(dx*dx + dy*dy) + 1e-5f;
            float force = ATTRACTION_STRENGTH * (dist - DESIRED_EDGE_LENGTH);
            float fx = (dx / dist) * force;
            float fy = (dy / dist) * force;
            src->fx -= fx;
            src->fy -= fy;
            tgt->fx += fx;
            tgt->fy += fy;
        }
    }
    for (n1 = nodes; n1 != NULL; n1 = n1->hh.next) {
        float fx = fmaxf(-MAX_FORCE, fminf(MAX_FORCE, n1->fx));
        float fy = fmaxf(-MAX_FORCE, fminf(MAX_FORCE, n1->fy));
        n1->x += fx * TIME_STEP;
        n1->y += fy * TIME_STEP;
        n1->fx *= DAMPING;
        n1->fy *= DAMPING;
        n1->x = fmaxf(50.0f, fminf(WINDOW_WIDTH - 50.0f, n1->x));
        n1->y = fmaxf(50.0f, fminf(WINDOW_HEIGHT - 50.0f, n1->y));
    }
    resolve_collisions();
}

// ----------------------------- HTTP Server ----------------------------------
static struct MHD_Daemon *http_daemon = NULL;

typedef struct {
    char *payload;
    size_t size;
} ReqState;

enum MHD_Result send_json_response(struct MHD_Connection *connection, int status_code, const char *json_str) {
    struct MHD_Response *resp = MHD_create_response_from_buffer(strlen(json_str), (void*)json_str, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    int ret = MHD_queue_response(connection, status_code, resp);
    MHD_destroy_response(resp);
    return ret;
}

static void request_completed(void *cls, struct MHD_Connection *connection,
                              void **con_cls, enum MHD_RequestTerminationCode toe) {
    if (*con_cls) {
        ReqState *state = (ReqState *)*con_cls;
        if (state->payload) free(state->payload);
        free(state);
        *con_cls = NULL;
    }
}

enum MHD_Result handle_request(void *cls, struct MHD_Connection *connection,
                               const char *url, const char *method,
                               const char *version, const char *upload_data,
                               size_t *upload_data_size, void **con_cls) {
    (void)cls; (void)version;

    if (strcmp(method, "POST") == 0) {
        ReqState *state = (ReqState *)*con_cls;
        if (!state) {
            state = calloc(1, sizeof(ReqState));
            *con_cls = state;
            return MHD_YES;
        }
        if (*upload_data_size > 0) {
            state->payload = realloc(state->payload, state->size + *upload_data_size + 1);
            memcpy(state->payload + state->size, upload_data, *upload_data_size);
            state->size += *upload_data_size;
            state->payload[state->size] = '\0';
            *upload_data_size = 0;
            return MHD_YES;
        }

        char *post = state->payload ? state->payload : strdup("");
        cJSON *json = cJSON_Parse(post);
        if (state->payload == NULL) free(post); // Free if it was strdup'd

        if (!json) return send_json_response(connection, 400, "{\"error\":\"Invalid JSON\"}");

        if (strcmp(url, "/nodes") == 0) {
            cJSON *id = cJSON_GetObjectItem(json, "id");
            if (!cJSON_IsString(id)) { cJSON_Delete(json); return send_json_response(connection, 400, "{\"error\":\"Missing id\"}"); }
            const char *id_str = id->valuestring;
            cJSON *label = cJSON_GetObjectItem(json, "label");
            const char *label_str = (label && cJSON_IsString(label)) ? label->valuestring : id_str;
            cJSON *metadata = cJSON_GetObjectItem(json, "metadata");
            char *meta_str = metadata ? cJSON_PrintUnformatted(metadata) : strdup("{}");

            cJSON *tags = cJSON_GetObjectItem(json, "tags");
            int actual_tag_cnt = 0;
            const char **tag_arr = NULL;

            if (cJSON_IsArray(tags)) {
                int total_cnt = cJSON_GetArraySize(tags);
                tag_arr = malloc(total_cnt * sizeof(const char*));
                for (int i = 0; i < total_cnt; i++) {
                    cJSON *t = cJSON_GetArrayItem(tags, i);
                    if (cJSON_IsString(t)) tag_arr[actual_tag_cnt++] = t->valuestring;
                }
            }

            float x = (float)(rand() % (WINDOW_WIDTH - 400) + 200);
            float y = (float)(rand() % (WINDOW_HEIGHT - 400) + 200);
            db_insert_node(id_str, label_str, x, y, meta_str);
            for (int i=0; i<actual_tag_cnt; i++) db_insert_tag(id_str, tag_arr[i]);

            QueueMsg *msg = calloc(1, sizeof(QueueMsg));
            msg->type = MSG_ADD_NODE;
            strncpy(msg->id1, id_str, sizeof(msg->id1)-1);
            strncpy(msg->id2, label_str, sizeof(msg->id2)-1);
            msg->metadata = meta_str;

            if (actual_tag_cnt > 0) {
                size_t buf_len = 1;
                for(int i=0; i<actual_tag_cnt; i++) buf_len += strlen(tag_arr[i]) + 1;
                char *tags_buf = calloc(1, buf_len);
                for (int i=0; i<actual_tag_cnt; i++) {
                    strcat(tags_buf, tag_arr[i]);
                    if (i<actual_tag_cnt-1) strcat(tags_buf, ",");
                }
                msg->tags_str = tags_buf;
            } else {
                msg->tags_str = strdup("");
            }

            enqueue_msg(msg);
            free((void*)tag_arr);
            cJSON_Delete(json);
            return send_json_response(connection, 201, "{\"status\":\"ok\"}");
        }
        else if (strcmp(url, "/edges") == 0) {
            cJSON *src = cJSON_GetObjectItem(json, "source");
            cJSON *tgt = cJSON_GetObjectItem(json, "target");
            if (!cJSON_IsString(src) || !cJSON_IsString(tgt)) { cJSON_Delete(json); return send_json_response(connection, 400, "{\"error\":\"Missing source/target\"}"); }
            db_insert_edge(src->valuestring, tgt->valuestring);
            QueueMsg *msg = calloc(1, sizeof(QueueMsg));
            msg->type = MSG_ADD_EDGE;
            strncpy(msg->id1, src->valuestring, sizeof(msg->id1)-1);
            strncpy(msg->id2, tgt->valuestring, sizeof(msg->id2)-1);
            enqueue_msg(msg);
            cJSON_Delete(json);
            return send_json_response(connection, 201, "{\"status\":\"ok\"}");
        }
        else if (strncmp(url, "/nodes/", 7) == 0 && strstr(url, "/tags")) {
            char node_id[64] = {0};
            sscanf(url, "/nodes/%63[^/]/tags", node_id);
            cJSON *tags = cJSON_GetObjectItem(json, "tags");
            if (!cJSON_IsArray(tags)) { cJSON_Delete(json); return send_json_response(connection, 400, "{\"error\":\"Missing tags array\"}"); }

            int total_cnt = cJSON_GetArraySize(tags);
            const char **tag_arr = malloc(total_cnt * sizeof(const char*));
            int actual_tag_cnt = 0;
            for (int i=0; i<total_cnt; i++) {
                cJSON *t = cJSON_GetArrayItem(tags, i);
                if (cJSON_IsString(t)) {
                    tag_arr[actual_tag_cnt++] = t->valuestring;
                    db_insert_tag(node_id, t->valuestring);
                }
            }

            QueueMsg *msg = calloc(1, sizeof(QueueMsg));
            msg->type = MSG_ADD_TAGS;
            strncpy(msg->id1, node_id, sizeof(msg->id1)-1);
            
            size_t buf_len = 1;
            for(int i=0; i<actual_tag_cnt; i++) buf_len += strlen(tag_arr[i]) + 1;
            char *tags_buf = calloc(1, buf_len);
            for (int i=0; i<actual_tag_cnt; i++) {
                strcat(tags_buf, tag_arr[i]);
                if (i<actual_tag_cnt-1) strcat(tags_buf, ",");
            }
            msg->tags_str = tags_buf;

            enqueue_msg(msg);
            free((void*)tag_arr);
            cJSON_Delete(json);
            return send_json_response(connection, 200, "{\"status\":\"ok\"}");
        }

        cJSON_Delete(json);
        return MHD_NO;
    }
    else if (strcmp(method, "GET") == 0) {
        if (strcmp(url, "/nodes") == 0) {
            const char *tag = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "tag");
            sqlite3 *conn = get_db_conn();
            sqlite3_stmt *stmt;
            if (tag) {
                sqlite3_prepare_v2(conn, "SELECT n.id, n.label, n.x, n.y, n.metadata FROM nodes n JOIN node_tags t ON n.id = t.node_id WHERE t.tag = ?", -1, &stmt, NULL);
                sqlite3_bind_text(stmt, 1, tag, -1, SQLITE_STATIC);
            } else {
                sqlite3_prepare_v2(conn, "SELECT id, label, x, y, metadata FROM nodes", -1, &stmt, NULL);
            }
            cJSON *arr = cJSON_CreateArray();
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                cJSON *obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "id", (const char*)sqlite3_column_text(stmt, 0));
                cJSON_AddStringToObject(obj, "label", (const char*)sqlite3_column_text(stmt, 1));
                cJSON_AddNumberToObject(obj, "x", sqlite3_column_double(stmt, 2));
                cJSON_AddNumberToObject(obj, "y", sqlite3_column_double(stmt, 3));
                const char *meta = (const char*)sqlite3_column_text(stmt, 4) ?: "{}";
                cJSON *meta_json = cJSON_Parse(meta);
                if (!meta_json) meta_json = cJSON_CreateObject();
                cJSON_AddItemToObject(obj, "metadata", meta_json);
                cJSON_AddItemToArray(arr, obj);
            }
            sqlite3_finalize(stmt);
            sqlite3_close(conn);
            char *json_str = cJSON_PrintUnformatted(arr);
            cJSON_Delete(arr);
            enum MHD_Result ret = send_json_response(connection, 200, json_str);
            free(json_str);
            return ret;
        }
        else if (strcmp(url, "/graph") == 0) {
            sqlite3 *conn = get_db_conn();
            sqlite3_stmt *stmt;
            cJSON *root = cJSON_CreateObject();
            cJSON *nodes_arr = cJSON_CreateArray();
            cJSON *edges_arr = cJSON_CreateArray();
            sqlite3_prepare_v2(conn, "SELECT id, label, x, y, metadata FROM nodes", -1, &stmt, NULL);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                cJSON *obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "id", (const char*)sqlite3_column_text(stmt, 0));
                cJSON_AddStringToObject(obj, "label", (const char*)sqlite3_column_text(stmt, 1));
                cJSON_AddNumberToObject(obj, "x", sqlite3_column_double(stmt, 2));
                cJSON_AddNumberToObject(obj, "y", sqlite3_column_double(stmt, 3));
                const char *meta = (const char*)sqlite3_column_text(stmt, 4) ?: "{}";
                cJSON *meta_json = cJSON_Parse(meta);
                if (!meta_json) meta_json = cJSON_CreateObject();
                cJSON_AddItemToObject(obj, "metadata", meta_json);
                cJSON_AddItemToArray(nodes_arr, obj);
            }
            sqlite3_finalize(stmt);
            sqlite3_prepare_v2(conn, "SELECT source, target FROM edges", -1, &stmt, NULL);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                cJSON *obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "source", (const char*)sqlite3_column_text(stmt, 0));
                cJSON_AddStringToObject(obj, "target", (const char*)sqlite3_column_text(stmt, 1));
                cJSON_AddItemToArray(edges_arr, obj);
            }
            sqlite3_finalize(stmt);
            sqlite3_close(conn);
            cJSON_AddItemToObject(root, "nodes", nodes_arr);
            cJSON_AddItemToObject(root, "edges", edges_arr);
            char *json_str = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            enum MHD_Result ret = send_json_response(connection, 200, json_str);
            free(json_str);
            return ret;
        }
    }
    else if (strcmp(method, "DELETE") == 0) {
        if (strncmp(url, "/nodes/", 7) == 0 && strlen(url) > 7) {
            char node_id[64] = {0};
            strncpy(node_id, url + 7, sizeof(node_id)-1);
            db_delete_node(node_id);
            QueueMsg *msg = calloc(1, sizeof(QueueMsg));
            msg->type = MSG_DELETE_NODE;
            strncpy(msg->id1, node_id, sizeof(msg->id1)-1);
            enqueue_msg(msg);
            return send_json_response(connection, 200, "{\"status\":\"ok\"}");
        }
        else if (strcmp(url, "/edges") == 0) {
            const char *src = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "source");
            const char *tgt = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "target");
            if (!src || !tgt) return send_json_response(connection, 400, "{\"error\":\"Missing source/target\"}");
            db_delete_edge(src, tgt);
            QueueMsg *msg = calloc(1, sizeof(QueueMsg));
            msg->type = MSG_DELETE_EDGE;
            strncpy(msg->id1, src, sizeof(msg->id1)-1);
            strncpy(msg->id2, tgt, sizeof(msg->id2)-1);
            enqueue_msg(msg);
            return send_json_response(connection, 200, "{\"status\":\"ok\"}");
        }
    }
    return MHD_NO;
}

void* run_http_server(void *arg) {
    (void)arg;
    http_daemon = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION, 5000, NULL, NULL, &handle_request, NULL, 
                                   MHD_OPTION_NOTIFY_COMPLETED, &request_completed, NULL, MHD_OPTION_END);
    if (!http_daemon) { fprintf(stderr, "Failed to start HTTP server\n"); exit(1); }
    printf("API server running on http://localhost:5000\n");
    while (1) sleep(1);
    return NULL;
}

// ----------------------------- Main (Raylib) --------------------------------
int main(void) {
    srand((unsigned)time(NULL));
    init_database();
    load_initial_data();
    add_sample_nodes();

    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Graph Visualizer (C)");
    SetTargetFPS(FPS_TARGET);

    Camera2D camera = { 0 };
    camera.offset = (Vector2){ WINDOW_WIDTH/2.0f, WINDOW_HEIGHT/2.0f };
    camera.target = (Vector2){ 0, 0 };
    camera.zoom = 1.0f;

    bool layout_enabled = true;
    bool search_active = false;
    char search_text[64] = {0};
    Node *selected_node = NULL;

    pthread_t http_thread;
    pthread_create(&http_thread, NULL, run_http_server, NULL);

    while (!WindowShouldClose()) {
        QueueMsg *msg;
        while ((msg = dequeue_msg_nonblock()) != NULL) {
            switch (msg->type) {
                case MSG_ADD_NODE: {
                    int tag_cnt = 0;
                    const char **tags = NULL;
                    if (msg->tags_str && strlen(msg->tags_str) > 0) {
                        char *copy = strdup(msg->tags_str);
                        char *token = strtok(copy, ",");
                        while (token) {
                            tags = realloc(tags, (tag_cnt+1)*sizeof(const char*));
                            tags[tag_cnt++] = strdup(token);
                            token = strtok(NULL, ",");
                        }
                        free(copy);
                    }
                    float x = (float)(rand() % (WINDOW_WIDTH-400) + 200);
                    float y = (float)(rand() % (WINDOW_HEIGHT-400) + 200);
                    mem_add_node(msg->id1, msg->id2, x, y, msg->metadata, tags, tag_cnt);
                    for (int i=0; i<tag_cnt; i++) free((void*)tags[i]);
                    free(tags);
                    free(msg->metadata);
                    free(msg->tags_str);
                    break;
                }
                case MSG_ADD_EDGE:
                    mem_add_edge(msg->id1, msg->id2);
                    break;
                case MSG_ADD_TAGS: {
                    char *copy = strdup(msg->tags_str);
                    char *token = strtok(copy, ",");
                    const char **tags = NULL;
                    int cnt = 0;
                    while (token) {
                        tags = realloc(tags, (cnt+1)*sizeof(const char*));
                        tags[cnt++] = strdup(token);
                        token = strtok(NULL, ",");
                    }
                    mem_add_tags(msg->id1, tags, cnt);
                    for (int i=0; i<cnt; i++) free((void*)tags[i]);
                    free(tags);
                    free(copy);
                    free(msg->tags_str);
                    break;
                }
                case MSG_DELETE_NODE:
                    mem_delete_node(msg->id1);
                    break;
                case MSG_DELETE_EDGE:
                    mem_delete_edge(msg->id1, msg->id2);
                    break;
            }
            free(msg);
        }

        // Input
        if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            Vector2 delta = GetMouseDelta();
            camera.target.x -= delta.x / camera.zoom;
            camera.target.y -= delta.y / camera.zoom;
        }
        float scroll = GetMouseWheelMove();
        if (scroll != 0) {
            camera.zoom += scroll * 0.1f;
            if (camera.zoom < 0.2f) camera.zoom = 0.2f;
            if (camera.zoom > 3.0f) camera.zoom = 3.0f;
        }
        if (IsKeyPressed(KEY_R)) {
            if (HASH_COUNT(nodes) > 0) {
                float avg_x = 0, avg_y = 0;
                Node *n; int count = 0;
                for (n = nodes; n; n = n->hh.next) { avg_x += n->x; avg_y += n->y; count++; }
                avg_x /= count; avg_y /= count;
                camera.target = (Vector2){ avg_x, avg_y };
            } else camera.target = (Vector2){ 0, 0 };
            camera.zoom = 1.0f;
        }
        if (IsKeyPressed(KEY_L)) layout_enabled = !layout_enabled;
        if (IsKeyPressed(KEY_S)) {
            search_active = !search_active;
            memset(search_text, 0, sizeof(search_text));
        }
        if (search_active) {
            int key = GetCharPressed();
            while (key > 0) {
                if (key >= 32 && key <= 126) {
                    size_t len = strlen(search_text);
                    if (len < sizeof(search_text)-1) {
                        search_text[len] = (char)key;
                        search_text[len+1] = '\0';
                    }
                }
                key = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && strlen(search_text) > 0)
                search_text[strlen(search_text)-1] = '\0';
            if (IsKeyPressed(KEY_ENTER)) search_active = false;
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            Vector2 mouse = GetMousePosition();
            Vector2 world = GetScreenToWorld2D(mouse, camera);
            float best_dist = NODE_RADIUS * 2;
            Node *best = NULL;
            for (Node *n = nodes; n; n = n->hh.next) {
                float dist = sqrtf((n->x - world.x)*(n->x - world.x) + (n->y - world.y)*(n->y - world.y));
                if (dist < best_dist) { best_dist = dist; best = n; }
            }
            selected_node = best;
        }

        for (int i = 0; i < ITERATIONS_PER_FRAME && layout_enabled; i++)
            update_layout();

        BeginDrawing();
        ClearBackground(SOLARIZED_BASE03);
        BeginMode2D(camera);

        for (int i = 0; i < edges_count; i++) {
            Node *src = NULL, *tgt = NULL;
            HASH_FIND_STR(nodes, edges_list[i].src, src);
            HASH_FIND_STR(nodes, edges_list[i].tgt, tgt);
            if (src && tgt)
                DrawLineEx((Vector2){ src->x, src->y }, (Vector2){ tgt->x, tgt->y }, 2.0f, EDGE_COLOR);
        }

        bool has_search = (strlen(search_text) > 0);
        for (Node *n = nodes; n; n = n->hh.next) {
            bool highlight = false;
            if (has_search) {
                NodeTags *nt = NULL;
                HASH_FIND_STR(tags_hash, n->id, nt);
                if (nt) {
                    for (int i=0; i<nt->tags.count; i++)
                        if (strcmp(nt->tags.tags[i], search_text) == 0) { highlight = true; break; }
                }
            }
            Color color = NODE_COLOR;
            if (selected_node == n) color = SELECTED_NODE_COLOR;
            else if (highlight) color = PURPLE;
            float radius = (selected_node == n) ? SELECTED_NODE_RADIUS : NODE_RADIUS;
            DrawCircleV((Vector2){ n->x, n->y }, radius, color);
            DrawCircleLines((int)n->x, (int)n->y, radius, DARKBLUE);
            int font_size = 14;
            int text_w = MeasureText(n->label, font_size);
            int text_x = (int)n->x - text_w/2;
            int text_y = (int)n->y - radius - 16;
            DrawText(n->label, text_x+1, text_y+1, font_size, BLACK);
            DrawText(n->label, text_x, text_y, font_size, WHITE);
        }

        EndMode2D();

        DrawText(TextFormat("FPS: %d", GetFPS()), 10, 10, 20, WHITE);
        DrawText(layout_enabled ? "Layout ON" : "Layout OFF", 10, 40, 16, layout_enabled ? LIME : RED);
        DrawText("[L] Layout | [S] Search tags | [R] Reset cam | Middle drag | Scroll zoom", 10, WINDOW_HEIGHT-30, 16, WHITE);

        if (search_active) {
            Rectangle bar = { 10, 70, 300, 30 };
            DrawRectangleRec(bar, UI_BG_COLOR);
            DrawRectangleLines(bar.x, bar.y, bar.width, bar.height, WHITE);
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "Search tag: %s_", search_text);
            DrawText(prompt, 15, 75, 20, WHITE);
        }

        if (selected_node) {
            int panel_w = 280, panel_h = 150;
            int panel_x = WINDOW_WIDTH - panel_w - 10;
            int panel_y = 10;
            DrawRectangle(panel_x, panel_y, panel_w, panel_h, UI_BG_COLOR);
            DrawRectangleLines(panel_x, panel_y, panel_w, panel_h, WHITE);
            DrawText(TextFormat("ID: %s", selected_node->id), panel_x+5, panel_y+5, 14, WHITE);
            DrawText(TextFormat("Label: %s", selected_node->label), panel_x+5, panel_y+25, 14, WHITE);
            char meta_short[128];
            strncpy(meta_short, selected_node->metadata, 40);
            meta_short[40] = '\0';
            DrawText(TextFormat("Meta: %s", meta_short), panel_x+5, panel_y+45, 12, LIGHTGRAY);
            NodeTags *nt = NULL;
            HASH_FIND_STR(tags_hash, selected_node->id, nt);
            char tags_str[256] = "Tags: ";
            if (nt && nt->tags.count > 0) {
                for (int i=0; i<nt->tags.count; i++) {
                    strcat(tags_str, nt->tags.tags[i]);
                    if (i<nt->tags.count-1) strcat(tags_str, ", ");
                }
            } else strcat(tags_str, "none");
            DrawText(tags_str, panel_x+5, panel_y+65, 12, LIGHTGRAY);
        }

        EndDrawing();
    }

    MHD_stop_daemon(http_daemon);
    sqlite3_close(db);
    CloseWindow();
    return 0;
}
