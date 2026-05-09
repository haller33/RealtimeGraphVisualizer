// main.c – Graph Visualizer with Raylib + HTTP API + SQLite (C version)
// Multi‑threaded with background layout using message passing (no shared graph).
// UI thread owns one graph, layout thread owns another, synchronized via messages.
// Compile: gcc -o graph_visualizer main.c -lraylib -lsqlite3 -lmicrohttpd -lcjson -lpthread -lm
// Usage: ./graph_visualizer [--db <file>] [--spatial] [--background-layout] [--layout-mode=force|tree|grid] [--log-http] [--log-db] [--log-ui] [--ui-batch-size N]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <getopt.h>
#include <sqlite3.h>
#include <microhttpd.h>
#include <cjson/cJSON.h>
#include <uthash.h>
#include <float.h>
#include "raylib.h"

// ----------------------------- Configuration ---------------------------------
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720
#define FPS_TARGET    60
#define DEFAULT_UI_BATCH_SIZE 5

// Force layout parameters
#define REPULSION_STRENGTH      2500.0f
#define ATTRACTION_STRENGTH     0.005f
#define DESIRED_EDGE_LENGTH     150.0f
#define DAMPING                 0.85f
#define ITERATIONS_PER_FRAME    5
#define MAX_FORCE               30.0f
#define TIME_STEP               0.1f

// Grid acceleration
#define CELL_SIZE               (DESIRED_EDGE_LENGTH * 2.0f)

#define NODE_RADIUS             12
#define SELECTED_NODE_RADIUS    16
#define MIN_DISTANCE            32.0f

#define EDGE_COLOR              LIGHTGRAY
#define NODE_COLOR              SKYBLUE
#define SELECTED_NODE_COLOR     GOLD
#define UI_BG_COLOR             (Color){30, 30, 40, 200}
#define SOLARIZED_BASE03        (Color){0, 43, 54}

// Global UI Flags
bool layout_enabled = true;
bool use_cage = false;
bool use_collisions = true;
bool use_spatial_accel = false;
bool use_background_layout = false;

// ----------------------------- Layout mode ----------------------------------
typedef enum {
    LAYOUT_FORCE = 0,
    LAYOUT_TREE,
    LAYOUT_GRID
} LayoutMode;
LayoutMode layout_mode = LAYOUT_FORCE;

// ----------------------------- Logging flags ---------------------------------
bool log_http = false;
bool log_db = false;
bool log_ui = false;
int ui_batch_size = DEFAULT_UI_BATCH_SIZE;

// ----------------------------- Graph structures (shared type definitions) ---
typedef struct Node {
    char id[64];
    char label[128];
    float x, y;
    float fx, fy;        // force vectors (only used by layout thread)
    char *metadata;
    UT_hash_handle hh;
} Node;

typedef struct EdgeSet {
    char key[256];               // Increased buffer to avoid truncation (was 128)
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

typedef struct Edge {
    char src[64];
    char tgt[64];
} Edge;

// Forward declarations for graph helpers
void mem_add_node(Node **nodes, NodeTags **tags_hash, const char *id, const char *label, float x, float y, const char *metadata, const char **tags, int tag_count);
void mem_add_edge(EdgeSet **edges_set, Edge **edges_list, int *edges_count, int *edges_capacity, const char *src, const char *tgt);
void mem_add_tags(NodeTags **tags_hash, const char *node_id, const char **tags, int tag_count);
void mem_delete_node(Node **nodes, NodeTags **tags_hash, EdgeSet **edges_set, Edge **edges_list, int *edges_count, int *edges_capacity, const char *node_id);
void mem_delete_edge(EdgeSet **edges_set, Edge **edges_list, int *edges_count, int *edges_capacity, const char *src, const char *tgt);
void rebuild_edge_list(EdgeSet *edges_set, Edge **edges_list, int *edges_count, int *edges_capacity);
void compute_layout_iteration(Node *nodes, Edge *edges_list, int edges_count, bool use_spatial, bool use_cage_flag, bool use_collisions_flag);

// ----------------------------- UI thread’s graph ----------------------------
Node *nodes_ui = NULL;
EdgeSet *edges_set_ui = NULL;
NodeTags *tags_hash_ui = NULL;
Edge *edges_list_ui = NULL;
int edges_count_ui = 0, edges_capacity_ui = 0;

// ----------------------------- Layout thread’s graph ------------------------
Node *nodes_layout = NULL;
EdgeSet *edges_set_layout = NULL;
NodeTags *tags_hash_layout = NULL;
Edge *edges_list_layout = NULL;
int edges_count_layout = 0, edges_capacity_layout = 0;

// ----------------------------- Database -------------------------------------
static const char *db_uri = "file::memory:?cache=shared";   // default: in‑memory
sqlite3 *db = NULL;

// ----------------------------- Task queue for DB operations -----------------
typedef enum {
    TASK_ADD_NODE,
    TASK_ADD_EDGE,
    TASK_ADD_TAGS,
    TASK_DELETE_NODE,
    TASK_DELETE_EDGE,
    TASK_POSITION_UPDATE    // batched positions from layout thread
} TaskType;

typedef struct DBTask {
    TaskType type;
    char id1[64];
    char id2[64];
    char *metadata;
    char *tags_str;
    struct DBTask *next;
} DBTask;

DBTask *task_head = NULL, *task_tail = NULL;
pthread_mutex_t task_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t task_cond = PTHREAD_COND_INITIALIZER;

// ----------------------------- UI message queue -----------------------------
typedef struct UIMsg {
    TaskType type;
    char id1[64];
    char id2[64];
    char *metadata;
    char *tags_str;
    int pos_count;                 // for TASK_POSITION_UPDATE
    struct { char id[64]; float x, y; } *pos_updates; // flexible array
    struct UIMsg *next;
} UIMsg;

UIMsg *ui_head = NULL, *ui_tail = NULL;
pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;

// ----------------------------- Layout update queue (structure changes) ------
typedef struct LayoutUpdate {
    TaskType type;
    char id1[64];
    char id2[64];
    char *metadata;
    char *tags_str;
    float x, y;                     // coordinates for ADD_NODE
    struct LayoutUpdate *next;
} LayoutUpdate;

LayoutUpdate *layout_update_head = NULL, *layout_update_tail = NULL;
pthread_mutex_t layout_update_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t layout_update_cond = PTHREAD_COND_INITIALIZER;

// ----------------------------- Forward declarations -------------------------
void enqueue_task(DBTask *task);
DBTask* dequeue_task(void);
void enqueue_ui_msg(UIMsg *msg);
UIMsg* dequeue_ui_msg(void);
void free_ui_msg(UIMsg *msg);
void process_ui_messages(int max_count);
void enqueue_layout_update(LayoutUpdate *upd);
LayoutUpdate* dequeue_layout_update(void);
void free_layout_update(LayoutUpdate *upd);
void apply_layout_update_to_back(LayoutUpdate *upd);
void* layout_thread_func(void *arg);

// ----------------------------- Comparator for qsort -------------------------
typedef struct SortNodeDepth {
    Node *node;
    int depth;
} SortNodeDepth;

int cmp_sort_node_depth(const void *a, const void *b) {
    const SortNodeDepth *sa = (const SortNodeDepth*)a;
    const SortNodeDepth *sb = (const SortNodeDepth*)b;
    if (sa->depth != sb->depth) return sa->depth - sb->depth;
    return strcmp(sa->node->id, sb->node->id);
}

int cmp_node_id(const void *a, const void *b) {
    const Node *na = *(const Node**)a;
    const Node *nb = *(const Node**)b;
    return strcmp(na->id, nb->id);
}

// ----------------------------- Tree layout computation ----------------------
void compute_tree_layout(Node *nodes, Edge *edges_list, int edges_count) {
    if (!nodes) return;

    // Build in-degree hash
    struct InDegree { int count; char id[64]; UT_hash_handle hh; } *in_deg = NULL;
    for (Edge *e = edges_list; e < edges_list + edges_count; e++) {
        struct InDegree *entry = NULL;
        HASH_FIND_STR(in_deg, e->tgt, entry);
        if (!entry) {
            entry = malloc(sizeof(struct InDegree));
            strcpy(entry->id, e->tgt);
            entry->count = 0;
            HASH_ADD_STR(in_deg, id, entry);
        }
        entry->count++;
    }

    int n_nodes = HASH_COUNT(nodes);
    Node **queue = malloc(n_nodes * sizeof(Node*));
    int head = 0, tail = 0;
    struct Depth { int depth; char id[64]; UT_hash_handle hh; } *depth_map = NULL;
    struct Visited { char id[64]; UT_hash_handle hh; } *visited = NULL;

    // Helper macro to enqueue a node
    #define ENQUEUE(n, d) do { \
        struct Visited *v_ = NULL; \
        HASH_FIND_STR(visited, (n)->id, v_); \
        if (!v_) { \
            v_ = malloc(sizeof(struct Visited)); \
            strcpy(v_->id, (n)->id); \
            HASH_ADD_STR(visited, id, v_); \
            queue[tail++] = (n); \
            struct Depth *d_ = malloc(sizeof(struct Depth)); \
            strcpy(d_->id, (n)->id); \
            d_->depth = (d); \
            HASH_ADD_STR(depth_map, id, d_); \
        } \
    } while(0)

    // Initial roots (in-degree == 0)
    for (Node *n = nodes; n; n = n->hh.next) {
        struct InDegree *entry = NULL;
        HASH_FIND_STR(in_deg, n->id, entry);
        if (!entry || entry->count == 0) {
            ENQUEUE(n, 0);
        }
    }

    // BFS to assign depths
    while (head < tail) {
        Node *cur = queue[head++];
        struct Depth *cd = NULL;
        HASH_FIND_STR(depth_map, cur->id, cd);
        int cur_depth = cd ? cd->depth : 0;
        for (Edge *e = edges_list; e < edges_list + edges_count; e++) {
            if (strcmp(e->src, cur->id) == 0) {
                Node *child = NULL;
                HASH_FIND_STR(nodes, e->tgt, child);
                if (child) {
                    struct Visited *cv = NULL;
                    HASH_FIND_STR(visited, child->id, cv);
                    if (!cv) {
                        ENQUEUE(child, cur_depth + 1);
                    }
                }
            }
        }
    }

    // For any node not visited (cyclic components), assign depth 0 and continue BFS
    for (Node *n = nodes; n; n = n->hh.next) {
        struct Visited *v = NULL;
        HASH_FIND_STR(visited, n->id, v);
        if (!v) {
            ENQUEUE(n, 0);
            // Resume BFS from this artificial root
            while (head < tail) {
                Node *cur = queue[head++];
                struct Depth *cd = NULL;
                HASH_FIND_STR(depth_map, cur->id, cd);
                int cur_depth = cd ? cd->depth : 0;
                for (Edge *e = edges_list; e < edges_list + edges_count; e++) {
                    if (strcmp(e->src, cur->id) == 0) {
                        Node *child = NULL;
                        HASH_FIND_STR(nodes, e->tgt, child);
                        if (child) {
                            struct Visited *cv = NULL;
                            HASH_FIND_STR(visited, child->id, cv);
                            if (!cv) {
                                ENQUEUE(child, cur_depth + 1);
                            }
                        }
                    }
                }
            }
        }
    }

    #undef ENQUEUE

    // Sort nodes by depth then ID using qsort
    typedef struct { Node *node; int depth; } SortNodeDepth;
    SortNodeDepth *sort_arr = malloc(n_nodes * sizeof(SortNodeDepth));
    int idx = 0;
    for (Node *n = nodes; n; n = n->hh.next) {
        struct Depth *d = NULL;
        HASH_FIND_STR(depth_map, n->id, d);
        sort_arr[idx].node = n;
        sort_arr[idx].depth = d ? d->depth : 0;
        idx++;
    }
    qsort(sort_arr, n_nodes, sizeof(SortNodeDepth), cmp_sort_node_depth);

    float x_spacing = 200.0f, y_spacing = 150.0f;
    for (int i = 0; i < n_nodes; i++) {
        sort_arr[i].node->x = i * x_spacing;
        sort_arr[i].node->y = sort_arr[i].depth * y_spacing;
    }

    // Cleanup
    struct InDegree *cur_in, *tmp_in;
    HASH_ITER(hh, in_deg, cur_in, tmp_in) { HASH_DEL(in_deg, cur_in); free(cur_in); }
    struct Depth *cur_d, *tmp_d;
    HASH_ITER(hh, depth_map, cur_d, tmp_d) { HASH_DEL(depth_map, cur_d); free(cur_d); }
    struct Visited *cur_v, *tmp_v;
    HASH_ITER(hh, visited, cur_v, tmp_v) { HASH_DEL(visited, cur_v); free(cur_v); }
    free(queue);
    free(sort_arr);
}

// ----------------------------- Grid layout computation ----------------------
void compute_grid_layout(Node *nodes) {
    int n_nodes = HASH_COUNT(nodes);
    if (n_nodes == 0) return;
    Node **sorted = malloc(n_nodes * sizeof(Node*));
    int idx = 0;
    for (Node *n = nodes; n; n = n->hh.next) sorted[idx++] = n;
    qsort(sorted, n_nodes, sizeof(Node*), cmp_node_id);

    int cols = (int)sqrt(n_nodes) + 1;
    float cell_w = 200.0f, cell_h = 150.0f;
    for (int i = 0; i < n_nodes; i++) {
        int row = i / cols;
        int col = i % cols;
        sorted[i]->x = col * cell_w;
        sorted[i]->y = row * cell_h;
    }
    free(sorted);
}

// ----------------------------- Grid helpers (unchanged) ---------------------
typedef struct GridCell {
    Node **nodes;
    int count;
    int capacity;
} GridCell;

void update_grid_bounds_for_table(Node *nodes, float *min_x, float *min_y, float *max_x, float *max_y) {
    *min_x = FLT_MAX; *min_y = FLT_MAX;
    *max_x = -FLT_MAX; *max_y = -FLT_MAX;
    for (Node *n = nodes; n; n = n->hh.next) {
        if (n->x < *min_x) *min_x = n->x;
        if (n->x > *max_x) *max_x = n->x;
        if (n->y < *min_y) *min_y = n->y;
        if (n->y > *max_y) *max_y = n->y;
    }
    *min_x -= CELL_SIZE; *min_y -= CELL_SIZE;
    *max_x += CELL_SIZE; *max_y += CELL_SIZE;
}

int coord_to_cell_x(float x, float min_x) { return (int)((x - min_x) / CELL_SIZE); }
int coord_to_cell_y(float y, float min_y) { return (int)((y - min_y) / CELL_SIZE); }

GridCell* rebuild_grid_for_table(Node *nodes, int *width, int *height, float *min_x, float *min_y, float *max_x, float *max_y) {
    update_grid_bounds_for_table(nodes, min_x, min_y, max_x, max_y);
    float span_x = *max_x - *min_x, span_y = *max_y - *min_y;
    *width = (int)(span_x / CELL_SIZE) + 2;
    *height = (int)(span_y / CELL_SIZE) + 2;
    if (*width <= 0) *width = 1;
    if (*height <= 0) *height = 1;

    GridCell *grid = calloc((*width) * (*height), sizeof(GridCell));
    if (!grid) return NULL;

    for (Node *n = nodes; n; n = n->hh.next) {
        int cx = coord_to_cell_x(n->x, *min_x);
        int cy = coord_to_cell_y(n->y, *min_y);
        if (cx >= 0 && cx < *width && cy >= 0 && cy < *height) {
            int idx = cy * (*width) + cx;
            if (grid[idx].count >= grid[idx].capacity) {
                grid[idx].capacity = grid[idx].capacity ? grid[idx].capacity * 2 : 8;
                grid[idx].nodes = realloc(grid[idx].nodes, grid[idx].capacity * sizeof(Node*));
            }
            grid[idx].nodes[grid[idx].count++] = n;
        }
    }
    return grid;
}

void free_grid(GridCell *grid, int width, int height) {
    if (!grid) return;
    for (int i = 0; i < width * height; i++)
        if (grid[i].nodes) free(grid[i].nodes);
    free(grid);
}

void get_neighbor_cells(int cx, int cy, int width, int height, int *neighbors, int *count) {
    *count = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = cx + dx, ny = cy + dy;
            if (nx >= 0 && nx < width && ny >= 0 && ny < height)
                neighbors[(*count)++] = ny * width + nx;
        }
}

// ----------------------------- Layout function (works on any graph) ---------
void compute_layout_iteration(Node *nodes, Edge *edges_list, int edges_count, bool use_spatial, bool use_cage_flag, bool use_collisions_flag) {
    if (!nodes) return;
    for (Node *n1 = nodes; n1; n1 = n1->hh.next) n1->fx = n1->fy = 0.0f;

    // Repulsion
    if (use_spatial) {
        float min_x, min_y, max_x, max_y;
        int gw, gh;
        GridCell *grid = rebuild_grid_for_table(nodes, &gw, &gh, &min_x, &min_y, &max_x, &max_y);
        if (grid) {
            int neighbors[9], neighbor_cnt;
            for (Node *n1 = nodes; n1; n1 = n1->hh.next) {
                int cx = coord_to_cell_x(n1->x, min_x);
                int cy = coord_to_cell_y(n1->y, min_y);
                if (cx < 0 || cx >= gw || cy < 0 || cy >= gh) continue;
                get_neighbor_cells(cx, cy, gw, gh, neighbors, &neighbor_cnt);
                for (int k = 0; k < neighbor_cnt; k++) {
                    GridCell *cell = &grid[neighbors[k]];
                    for (int i = 0; i < cell->count; i++) {
                        Node *n2 = cell->nodes[i];
                        if (n1 == n2) continue;
                        float dx = n1->x - n2->x, dy = n1->y - n2->y;
                        float dist_sq = dx*dx + dy*dy + 1e-5f;
                        float dist = sqrtf(dist_sq);
                        float force = REPULSION_STRENGTH / dist_sq;
                        float fx = (dx / dist) * force, fy = (dy / dist) * force;
                        n1->fx += fx; n2->fx -= fx;
                        n1->fy += fy; n2->fy -= fy;
                    }
                }
            }
            free_grid(grid, gw, gh);
        }
    } else {
        for (Node *n1 = nodes; n1; n1 = n1->hh.next)
            for (Node *n2 = n1->hh.next; n2; n2 = n2->hh.next) {
                float dx = n1->x - n2->x, dy = n1->y - n2->y;
                float dist_sq = dx*dx + dy*dy + 1e-5f, dist = sqrtf(dist_sq);
                float force = REPULSION_STRENGTH / dist_sq;
                float fx = (dx / dist) * force, fy = (dy / dist) * force;
                n1->fx += fx; n1->fy += fy;
                n2->fx -= fx; n2->fy -= fy;
            }
    }

    // Attraction (edges)
    for (int i = 0; i < edges_count; i++) {
        Node *src = NULL, *tgt = NULL;
        HASH_FIND_STR(nodes, edges_list[i].src, src);
        HASH_FIND_STR(nodes, edges_list[i].tgt, tgt);
        if (src && tgt) {
            float dx = src->x - tgt->x, dy = src->y - tgt->y;
            float dist = sqrtf(dx*dx + dy*dy) + 1e-5f;
            float force = ATTRACTION_STRENGTH * (dist - DESIRED_EDGE_LENGTH);
            float fx = (dx / dist) * force, fy = (dy / dist) * force;
            src->fx -= fx; src->fy -= fy;
            tgt->fx += fx; tgt->fy += fy;
        }
    }

    // Apply forces
    for (Node *n1 = nodes; n1; n1 = n1->hh.next) {
        float fx = fmaxf(-MAX_FORCE, fminf(MAX_FORCE, n1->fx));
        float fy = fmaxf(-MAX_FORCE, fminf(MAX_FORCE, n1->fy));
        n1->x += fx * TIME_STEP;
        n1->y += fy * TIME_STEP;
        n1->fx *= DAMPING; n1->fy *= DAMPING;
        if (use_cage_flag) {
            n1->x = fmaxf(50.0f, fminf(WINDOW_WIDTH - 50.0f, n1->x));
            n1->y = fmaxf(50.0f, fminf(WINDOW_HEIGHT - 50.0f, n1->y));
        }
    }

    // Collisions
    if (use_collisions_flag) {
        for (Node *n1 = nodes; n1; n1 = n1->hh.next)
            for (Node *n2 = n1->hh.next; n2; n2 = n2->hh.next) {
                float dx = n1->x - n2->x, dy = n1->y - n2->y;
                float dist = sqrtf(dx*dx + dy*dy);
                if (dist < MIN_DISTANCE && dist > 0.001f) {
                    float overlap = MIN_DISTANCE - dist;
                    float angle = atan2f(dy, dx);
                    float push_x = cosf(angle) * overlap * 0.5f;
                    float push_y = sinf(angle) * overlap * 0.5f;
                    n1->x += push_x; n1->y += push_y;
                    n2->x -= push_x; n2->y -= push_y;
                }
            }
    }
}

// ----------------------------- SQLite helpers (with persistent connection) -
sqlite3* get_db_conn(void) {
    sqlite3 *conn;
    sqlite3_open(db_uri, &conn);
    sqlite3_exec(conn, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    return conn;
}

void db_insert_node_task(DBTask *task, sqlite3 *conn) {
    // Use the given persistent connection
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(conn, "INSERT OR REPLACE INTO nodes (id, label, x, y, metadata) VALUES (?,?,?,?,?)", -1, &stmt, NULL);
    float x = (float)((rand() % 1000) - 500);
    float y = (float)((rand() % 1000) - 500);
    sqlite3_bind_text(stmt, 1, task->id1, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, task->id2, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, x);
    sqlite3_bind_double(stmt, 4, y);
    sqlite3_bind_text(stmt, 5, task->metadata, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE && log_db)
        fprintf(stderr, "DB: insert node failed: %s\n", sqlite3_errmsg(conn));
    sqlite3_finalize(stmt);
    if (task->tags_str && strlen(task->tags_str) > 0) {
        char *copy = strdup(task->tags_str);
        char *token = strtok(copy, ",");
        while (token) {
            sqlite3_prepare_v2(conn, "INSERT OR IGNORE INTO node_tags (node_id, tag) VALUES (?,?)", -1, &stmt, NULL);
            sqlite3_bind_text(stmt, 1, task->id1, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, token, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            token = strtok(NULL, ",");
        }
        free(copy);
    }
}

void db_insert_edge_task(DBTask *task, sqlite3 *conn) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(conn, "INSERT OR IGNORE INTO edges (source, target) VALUES (?,?)", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, task->id1, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, task->id2, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_insert_tags_task(DBTask *task, sqlite3 *conn) {
    if (!task->tags_str || strlen(task->tags_str) == 0) return;
    char *copy = strdup(task->tags_str);
    char *token = strtok(copy, ",");
    sqlite3_stmt *stmt;
    while (token) {
        sqlite3_prepare_v2(conn, "INSERT OR IGNORE INTO node_tags (node_id, tag) VALUES (?,?)", -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, task->id1, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, token, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        token = strtok(NULL, ",");
    }
    free(copy);
}

void db_delete_node_task(DBTask *task, sqlite3 *conn) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(conn, "DELETE FROM nodes WHERE id = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, task->id1, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_delete_edge_task(DBTask *task, sqlite3 *conn) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(conn, "DELETE FROM edges WHERE source = ? AND target = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, task->id1, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, task->id2, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ----------------------------- Queue management -----------------------------
void enqueue_task(DBTask *task) {
    pthread_mutex_lock(&task_mutex);
    task->next = NULL;
    if (task_tail) task_tail->next = task;
    else task_head = task;
    task_tail = task;
    pthread_cond_signal(&task_cond);
    pthread_mutex_unlock(&task_mutex);
}

DBTask* dequeue_task(void) {
    pthread_mutex_lock(&task_mutex);
    while (!task_head) pthread_cond_wait(&task_cond, &task_mutex);
    DBTask *task = task_head;
    task_head = task->next;
    if (!task_head) task_tail = NULL;
    pthread_mutex_unlock(&task_mutex);
    return task;
}

void enqueue_ui_msg(UIMsg *msg) {
    pthread_mutex_lock(&ui_mutex);
    msg->next = NULL;
    if (ui_tail) ui_tail->next = msg;
    else ui_head = msg;
    ui_tail = msg;
    pthread_mutex_unlock(&ui_mutex);
}

UIMsg* dequeue_ui_msg(void) {
    pthread_mutex_lock(&ui_mutex);
    UIMsg *msg = ui_head;
    if (msg) {
        ui_head = msg->next;
        if (!ui_head) ui_tail = NULL;
    }
    pthread_mutex_unlock(&ui_mutex);
    return msg;
}

void free_ui_msg(UIMsg *msg) {
    if (msg->metadata) free(msg->metadata);
    if (msg->tags_str) free(msg->tags_str);
    if (msg->pos_updates) free(msg->pos_updates);
    free(msg);
}

void enqueue_layout_update(LayoutUpdate *upd) {
    pthread_mutex_lock(&layout_update_mutex);
    upd->next = NULL;
    if (layout_update_tail) layout_update_tail->next = upd;
    else layout_update_head = upd;
    layout_update_tail = upd;
    pthread_cond_signal(&layout_update_cond);
    pthread_mutex_unlock(&layout_update_mutex);
}

LayoutUpdate* dequeue_layout_update(void) {
    pthread_mutex_lock(&layout_update_mutex);
    LayoutUpdate *upd = layout_update_head;
    if (upd) {
        layout_update_head = upd->next;
        if (!layout_update_head) layout_update_tail = NULL;
    }
    pthread_mutex_unlock(&layout_update_mutex);
    return upd;
}

void free_layout_update(LayoutUpdate *upd) {
    if (upd->metadata) free(upd->metadata);
    if (upd->tags_str) free(upd->tags_str);
    free(upd);
}

void apply_layout_update_to_back(LayoutUpdate *upd) {
    switch (upd->type) {
        case TASK_ADD_NODE: {
            int tag_cnt = 0;
            const char **tags = NULL;
            if (upd->tags_str && strlen(upd->tags_str)) {
                char *copy = strdup(upd->tags_str);
                char *token = strtok(copy, ",");
                while (token) {
                    tags = realloc(tags, (tag_cnt+1)*sizeof(const char*));
                    tags[tag_cnt++] = strdup(token);
                    token = strtok(NULL, ",");
                }
                free(copy);
            }
            // Use the coordinates from the UI message instead of random
            float x = upd->x;
            float y = upd->y;
            mem_add_node(&nodes_layout, &tags_hash_layout, upd->id1, upd->id2, x, y, upd->metadata, tags, tag_cnt);
            for (int i=0; i<tag_cnt; i++) free((void*)tags[i]);
            free(tags);
            break;
        }
        case TASK_ADD_EDGE:
            mem_add_edge(&edges_set_layout, &edges_list_layout, &edges_count_layout, &edges_capacity_layout, upd->id1, upd->id2);
            break;
        case TASK_ADD_TAGS: {
            char *copy = strdup(upd->tags_str);
            char *token = strtok(copy, ",");
            const char **tags = NULL;
            int cnt = 0;
            while (token) {
                tags = realloc(tags, (cnt+1)*sizeof(const char*));
                tags[cnt++] = strdup(token);
                token = strtok(NULL, ",");
            }
            mem_add_tags(&tags_hash_layout, upd->id1, tags, cnt);
            for (int i=0; i<cnt; i++) free((void*)tags[i]);
            free(tags);
            free(copy);
            break;
        }
        case TASK_DELETE_NODE:
            mem_delete_node(&nodes_layout, &tags_hash_layout, &edges_set_layout, &edges_list_layout, &edges_count_layout, &edges_capacity_layout, upd->id1);
            break;
        case TASK_DELETE_EDGE:
            mem_delete_edge(&edges_set_layout, &edges_list_layout, &edges_count_layout, &edges_capacity_layout, upd->id1, upd->id2);
            break;
        default: break;
    }
}

// Process UI messages (called from main thread)
void process_ui_messages(int max_count) {
    int processed = 0;
    UIMsg *msg;
    while (processed < max_count && (msg = dequeue_ui_msg()) != NULL) {
        switch (msg->type) {
            case TASK_ADD_NODE: {
                int tag_cnt = 0;
                const char **tags = NULL;
                if (msg->tags_str && strlen(msg->tags_str)) {
                    char *copy = strdup(msg->tags_str);
                    char *token = strtok(copy, ",");
                    while (token) {
                        tags = realloc(tags, (tag_cnt+1)*sizeof(const char*));
                        tags[tag_cnt++] = strdup(token);
                        token = strtok(NULL, ",");
                    }
                    free(copy);
                }
                float x = (float)((rand() % 1000) - 500);
                float y = (float)((rand() % 1000) - 500);
                mem_add_node(&nodes_ui, &tags_hash_ui, msg->id1, msg->id2, x, y, msg->metadata, tags, tag_cnt);
                // Also send to layout thread, including the exact coordinates
                LayoutUpdate *upd = calloc(1, sizeof(LayoutUpdate));
                upd->type = TASK_ADD_NODE;
                strcpy(upd->id1, msg->id1); strcpy(upd->id2, msg->id2);
                if (msg->metadata) upd->metadata = strdup(msg->metadata);
                if (msg->tags_str) upd->tags_str = strdup(msg->tags_str);
                upd->x = x;
                upd->y = y;
                enqueue_layout_update(upd);
                for (int i=0; i<tag_cnt; i++) free((void*)tags[i]);
                free(tags);
                if (log_ui) printf("UI: added node %s\n", msg->id1);
                break;
            }
            case TASK_ADD_EDGE: {
                mem_add_edge(&edges_set_ui, &edges_list_ui, &edges_count_ui, &edges_capacity_ui, msg->id1, msg->id2);
                LayoutUpdate *upd = calloc(1, sizeof(LayoutUpdate));
                upd->type = TASK_ADD_EDGE;
                strcpy(upd->id1, msg->id1); strcpy(upd->id2, msg->id2);
                enqueue_layout_update(upd);
                if (log_ui) printf("UI: added edge %s->%s\n", msg->id1, msg->id2);
                break;
            }
            case TASK_ADD_TAGS: {
                char *copy = strdup(msg->tags_str);
                char *token = strtok(copy, ",");
                const char **tags = NULL;
                int cnt = 0;
                while (token) {
                    tags = realloc(tags, (cnt+1)*sizeof(const char*));
                    tags[cnt++] = strdup(token);
                    token = strtok(NULL, ",");
                }
                mem_add_tags(&tags_hash_ui, msg->id1, tags, cnt);
                LayoutUpdate *upd = calloc(1, sizeof(LayoutUpdate));
                upd->type = TASK_ADD_TAGS;
                strcpy(upd->id1, msg->id1);
                if (msg->tags_str) upd->tags_str = strdup(msg->tags_str);
                enqueue_layout_update(upd);
                for (int i=0; i<cnt; i++) free((void*)tags[i]);
                free(tags);
                free(copy);
                if (log_ui) printf("UI: added tags to %s\n", msg->id1);
                break;
            }
            case TASK_DELETE_NODE: {
                mem_delete_node(&nodes_ui, &tags_hash_ui, &edges_set_ui, &edges_list_ui, &edges_count_ui, &edges_capacity_ui, msg->id1);
                LayoutUpdate *upd = calloc(1, sizeof(LayoutUpdate));
                upd->type = TASK_DELETE_NODE;
                strcpy(upd->id1, msg->id1);
                enqueue_layout_update(upd);
                if (log_ui) printf("UI: deleted node %s\n", msg->id1);
                break;
            }
            case TASK_DELETE_EDGE: {
                mem_delete_edge(&edges_set_ui, &edges_list_ui, &edges_count_ui, &edges_capacity_ui, msg->id1, msg->id2);
                LayoutUpdate *upd = calloc(1, sizeof(LayoutUpdate));
                upd->type = TASK_DELETE_EDGE;
                strcpy(upd->id1, msg->id1); strcpy(upd->id2, msg->id2);
                enqueue_layout_update(upd);
                if (log_ui) printf("UI: deleted edge %s->%s\n", msg->id1, msg->id2);
                break;
            }
            case TASK_POSITION_UPDATE: {
                for (int i = 0; i < msg->pos_count; i++) {
                    Node *node = NULL;
                    HASH_FIND_STR(nodes_ui, msg->pos_updates[i].id, node);
                    if (node) {
                        node->x = msg->pos_updates[i].x;
                        node->y = msg->pos_updates[i].y;
                    }
                }
                if (log_ui) printf("UI: applied %d position updates\n", msg->pos_count);
                break;
            }
            default:
                if (log_ui) printf("UI: unknown message type %d\n", msg->type);
                break;
        }
        free_ui_msg(msg);
        processed++;
    }
}

// ----------------------------- Database thread (persistent connection) -----
void* db_thread_func(void *arg) {
    (void)arg;
    if (log_db) printf("DB thread started\n");
    // Open a single connection for the lifetime of this thread
    sqlite3 *conn = get_db_conn();
    while (1) {
        DBTask *task = dequeue_task();
        if (!task) continue;
        switch (task->type) {
            case TASK_ADD_NODE: db_insert_node_task(task, conn); break;
            case TASK_ADD_EDGE: db_insert_edge_task(task, conn); break;
            case TASK_ADD_TAGS: db_insert_tags_task(task, conn); break;
            case TASK_DELETE_NODE: db_delete_node_task(task, conn); break;
            case TASK_DELETE_EDGE: db_delete_edge_task(task, conn); break;
            default: if (log_db) printf("DB thread ignoring task type %d\n", task->type); break;
        }
        if (task->type != TASK_POSITION_UPDATE) {
            UIMsg *uimsg = calloc(1, sizeof(UIMsg));
            uimsg->type = task->type;
            strcpy(uimsg->id1, task->id1); strcpy(uimsg->id2, task->id2);
            if (task->metadata) uimsg->metadata = strdup(task->metadata);
            if (task->tags_str) uimsg->tags_str = strdup(task->tags_str);
            enqueue_ui_msg(uimsg);
        }
        if (task->metadata) free(task->metadata);
        if (task->tags_str) free(task->tags_str);
        free(task);
    }
    sqlite3_close(conn);
    return NULL;
}

// ----------------------------- Background layout thread ---------------------
void* layout_thread_func(void *arg) {
    (void)arg;
    if (log_ui) printf("Layout thread started\n");

    for (Node *n = nodes_ui; n; n = n->hh.next) {
        mem_add_node(&nodes_layout, &tags_hash_layout, n->id, n->label, n->x, n->y, n->metadata, NULL, 0);
    }
    for (int i = 0; i < edges_count_ui; i++) {
        mem_add_edge(&edges_set_layout, &edges_list_layout, &edges_count_layout, &edges_capacity_layout,
                     edges_list_ui[i].src, edges_list_ui[i].tgt);
    }

    while (1) {
        LayoutUpdate *upd;
        while ((upd = dequeue_layout_update()) != NULL) {
            apply_layout_update_to_back(upd);
            free_layout_update(upd);
        }

        if (layout_enabled && nodes_layout) {
            compute_layout_iteration(nodes_layout, edges_list_layout, edges_count_layout,
                                     use_spatial_accel, use_cage, use_collisions);
        }

        int node_count = HASH_COUNT(nodes_layout);
        if (node_count > 0) {
            UIMsg *pos_msg = calloc(1, sizeof(UIMsg));
            pos_msg->type = TASK_POSITION_UPDATE;
            pos_msg->pos_count = node_count;
            pos_msg->pos_updates = malloc(node_count * sizeof(*pos_msg->pos_updates));
            int idx = 0;
            for (Node *n = nodes_layout; n; n = n->hh.next) {
                strcpy(pos_msg->pos_updates[idx].id, n->id);
                pos_msg->pos_updates[idx].x = n->x;
                pos_msg->pos_updates[idx].y = n->y;
                idx++;
            }
            enqueue_ui_msg(pos_msg);
        }

        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

// ----------------------------- HTTP Server (unchanged) ----------------------
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
        if (state->payload == NULL) free(post);

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

            char *tags_buf = NULL;
            if (actual_tag_cnt > 0) {
                size_t buf_len = 1;
                for (int i=0; i<actual_tag_cnt; i++) buf_len += strlen(tag_arr[i]) + 1;
                tags_buf = calloc(1, buf_len);
                for (int i=0; i<actual_tag_cnt; i++) {
                    strcat(tags_buf, tag_arr[i]);
                    if (i<actual_tag_cnt-1) strcat(tags_buf, ",");
                }
            } else {
                tags_buf = strdup("");
            }

            DBTask *task = calloc(1, sizeof(DBTask));
            task->type = TASK_ADD_NODE;
            strncpy(task->id1, id_str, sizeof(task->id1)-1);
            strncpy(task->id2, label_str, sizeof(task->id2)-1);
            task->metadata = meta_str;
            task->tags_str = tags_buf;
            enqueue_task(task);

            free((void*)tag_arr);
            cJSON_Delete(json);
            if (log_http) printf("HTTP: queued ADD_NODE %s\n", id_str);
            return send_json_response(connection, 202, "{\"status\":\"accepted\"}");
        }
        else if (strcmp(url, "/edges") == 0) {
            cJSON *src = cJSON_GetObjectItem(json, "source");
            cJSON *tgt = cJSON_GetObjectItem(json, "target");
            if (!cJSON_IsString(src) || !cJSON_IsString(tgt)) { cJSON_Delete(json); return send_json_response(connection, 400, "{\"error\":\"Missing source/target\"}"); }
            DBTask *task = calloc(1, sizeof(DBTask));
            task->type = TASK_ADD_EDGE;
            strncpy(task->id1, src->valuestring, sizeof(task->id1)-1);
            strncpy(task->id2, tgt->valuestring, sizeof(task->id2)-1);
            enqueue_task(task);
            cJSON_Delete(json);
            if (log_http) printf("HTTP: queued ADD_EDGE %s->%s\n", src->valuestring, tgt->valuestring);
            return send_json_response(connection, 202, "{\"status\":\"accepted\"}");
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
                if (cJSON_IsString(t)) tag_arr[actual_tag_cnt++] = t->valuestring;
            }

            char *tags_buf = NULL;
            if (actual_tag_cnt > 0) {
                size_t buf_len = 1;
                for (int i=0; i<actual_tag_cnt; i++) buf_len += strlen(tag_arr[i]) + 1;
                tags_buf = calloc(1, buf_len);
                for (int i=0; i<actual_tag_cnt; i++) {
                    strcat(tags_buf, tag_arr[i]);
                    if (i<actual_tag_cnt-1) strcat(tags_buf, ",");
                }
            } else {
                tags_buf = strdup("");
            }

            DBTask *task = calloc(1, sizeof(DBTask));
            task->type = TASK_ADD_TAGS;
            strncpy(task->id1, node_id, sizeof(task->id1)-1);
            task->tags_str = tags_buf;
            enqueue_task(task);

            free((void*)tag_arr);
            cJSON_Delete(json);
            if (log_http) printf("HTTP: queued ADD_TAGS for %s\n", node_id);
            return send_json_response(connection, 202, "{\"status\":\"accepted\"}");
        }

        cJSON_Delete(json);
        return MHD_NO;
    }
    else if (strcmp(method, "DELETE") == 0) {
        if (strncmp(url, "/nodes/", 7) == 0 && strlen(url) > 7) {
            char node_id[64] = {0};
            strncpy(node_id, url + 7, sizeof(node_id)-1);
            DBTask *task = calloc(1, sizeof(DBTask));
            task->type = TASK_DELETE_NODE;
            strncpy(task->id1, node_id, sizeof(task->id1)-1);
            enqueue_task(task);
            if (log_http) printf("HTTP: queued DELETE_NODE %s\n", node_id);
            return send_json_response(connection, 202, "{\"status\":\"accepted\"}");
        }
        else if (strcmp(url, "/edges") == 0) {
            const char *src = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "source");
            const char *tgt = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "target");
            if (!src || !tgt) return send_json_response(connection, 400, "{\"error\":\"Missing source/target\"}");
            DBTask *task = calloc(1, sizeof(DBTask));
            task->type = TASK_DELETE_EDGE;
            strncpy(task->id1, src, sizeof(task->id1)-1);
            strncpy(task->id2, tgt, sizeof(task->id2)-1);
            enqueue_task(task);
            if (log_http) printf("HTTP: queued DELETE_EDGE %s->%s\n", src, tgt);
            return send_json_response(connection, 202, "{\"status\":\"accepted\"}");
        }
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
    return MHD_NO;
}

void* http_thread_func(void *arg) {
    (void)arg;
    http_daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, 5000, NULL, NULL, &handle_request, NULL,
                                   MHD_OPTION_NOTIFY_COMPLETED, &request_completed, NULL, MHD_OPTION_END);
    if (!http_daemon) { fprintf(stderr, "Failed to start HTTP server on 5000\n"); exit(1); }
    printf("API server running on http://localhost:5000\n");
    while (1) sleep(1);
    return NULL;
}

// ----------------------------- Graph helpers (definitions) ------------------
void mem_add_node(Node **nodes, NodeTags **tags_hash, const char *id, const char *label, float x, float y, const char *metadata, const char **tags, int tag_count) {
    Node *node = malloc(sizeof(Node));
    strncpy(node->id, id, sizeof(node->id)-1); node->id[sizeof(node->id)-1] = '\0';
    strncpy(node->label, label, sizeof(node->label)-1); node->label[sizeof(node->label)-1] = '\0';
    node->x = x; node->y = y;
    node->fx = node->fy = 0.0f;
    node->metadata = malloc(strlen(metadata)+1);
    strcpy(node->metadata, metadata);
    HASH_ADD_STR(*nodes, id, node);

    NodeTags *nt = NULL;
    HASH_FIND_STR(*tags_hash, id, nt);
    if (!nt) {
        nt = malloc(sizeof(NodeTags));
        strncpy(nt->node_id, id, sizeof(nt->node_id)-1);
        nt->node_id[sizeof(nt->node_id)-1] = '\0';
        nt->tags.count = 0; nt->tags.capacity = 4;
        nt->tags.tags = malloc(nt->tags.capacity * sizeof(char*));
        HASH_ADD_STR(*tags_hash, node_id, nt);
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

void mem_add_edge(EdgeSet **edges_set, Edge **edges_list, int *edges_count, int *edges_capacity, const char *src, const char *tgt) {
    EdgeSet *e = malloc(sizeof(EdgeSet));
    snprintf(e->key, sizeof(e->key), "%s|%s", src, tgt);
    HASH_ADD_STR(*edges_set, key, e);

    if (*edges_count >= *edges_capacity) {
        *edges_capacity = *edges_capacity ? (*edges_capacity)*2 : 16;
        *edges_list = realloc(*edges_list, (*edges_capacity) * sizeof(Edge));
    }
    memset(&(*edges_list)[*edges_count], 0, sizeof(Edge));
    strncpy((*edges_list)[*edges_count].src, src, sizeof((*edges_list)[*edges_count].src)-1);
    strncpy((*edges_list)[*edges_count].tgt, tgt, sizeof((*edges_list)[*edges_count].tgt)-1);
    (*edges_count)++;
}

void mem_add_tags(NodeTags **tags_hash, const char *node_id, const char **tags, int tag_count) {
    NodeTags *nt = NULL;
    HASH_FIND_STR(*tags_hash, node_id, nt);
    if (!nt) {
        nt = malloc(sizeof(NodeTags));
        strncpy(nt->node_id, node_id, sizeof(nt->node_id)-1);
        nt->node_id[sizeof(nt->node_id)-1] = '\0';
        nt->tags.count = 0; nt->tags.capacity = 4;
        nt->tags.tags = malloc(nt->tags.capacity * sizeof(char*));
        HASH_ADD_STR(*tags_hash, node_id, nt);
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

void rebuild_edge_list(EdgeSet *edges_set, Edge **edges_list, int *edges_count, int *edges_capacity) {
    free(*edges_list);
    *edges_count = 0; *edges_capacity = 0;
    *edges_list = NULL;
    EdgeSet *e, *tmp;
    HASH_ITER(hh, edges_set, e, tmp) {
        if (*edges_count >= *edges_capacity) {
            *edges_capacity = *edges_capacity ? (*edges_capacity)*2 : 16;
            *edges_list = realloc(*edges_list, (*edges_capacity) * sizeof(Edge));
        }
        memset(&(*edges_list)[*edges_count], 0, sizeof(Edge));
        sscanf(e->key, "%[^|]|%[^|]", (*edges_list)[*edges_count].src, (*edges_list)[*edges_count].tgt);
        (*edges_count)++;
    }
}

void mem_delete_node(Node **nodes, NodeTags **tags_hash, EdgeSet **edges_set, Edge **edges_list, int *edges_count, int *edges_capacity, const char *node_id) {
    Node *node = NULL;
    HASH_FIND_STR(*nodes, node_id, node);
    if (node) {
        HASH_DEL(*nodes, node);
        free(node->metadata);
        free(node);
    }
    NodeTags *nt = NULL;
    HASH_FIND_STR(*tags_hash, node_id, nt);
    if (nt) {
        HASH_DEL(*tags_hash, nt);
        for (int i = 0; i < nt->tags.count; i++) free(nt->tags.tags[i]);
        free(nt->tags.tags);
        free(nt);
    }
    EdgeSet *e, *tmp;
    HASH_ITER(hh, *edges_set, e, tmp) {
        char src[64], tgt[64];
        sscanf(e->key, "%[^|]|%[^|]", src, tgt);
        if (strcmp(src, node_id) == 0 || strcmp(tgt, node_id) == 0) {
            HASH_DEL(*edges_set, e);
            free(e);
        }
    }
    rebuild_edge_list(*edges_set, edges_list, edges_count, edges_capacity);
}

void mem_delete_edge(EdgeSet **edges_set, Edge **edges_list, int *edges_count, int *edges_capacity, const char *src, const char *tgt) {
    char key[256];  // Increased buffer
    snprintf(key, sizeof(key), "%s|%s", src, tgt);
    EdgeSet *e = NULL;
    HASH_FIND_STR(*edges_set, key, e);
    if (e) {
        HASH_DEL(*edges_set, e);
        free(e);
    }
    rebuild_edge_list(*edges_set, edges_list, edges_count, edges_capacity);
}

// ----------------------------- Initial data load ---------------------------
void load_initial_data(void) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT id, label, x, y, metadata FROM nodes", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *id = (const char*)sqlite3_column_text(stmt, 0);
        const char *label = (const char*)sqlite3_column_text(stmt, 1);
        float x = (float)sqlite3_column_double(stmt, 2);
        float y = (float)sqlite3_column_double(stmt, 3);
        const char *metadata = (const char*)sqlite3_column_text(stmt, 4) ?: "{}";
        mem_add_node(&nodes_ui, &tags_hash_ui, id, label, x, y, metadata, NULL, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(db, "SELECT source, target FROM edges", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *src = (const char*)sqlite3_column_text(stmt, 0);
        const char *tgt = (const char*)sqlite3_column_text(stmt, 1);
        mem_add_edge(&edges_set_ui, &edges_list_ui, &edges_count_ui, &edges_capacity_ui, src, tgt);
    }
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(db, "SELECT node_id, tag FROM node_tags", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *node_id = (const char*)sqlite3_column_text(stmt, 0);
        const char *tag = (const char*)sqlite3_column_text(stmt, 1);
        mem_add_tags(&tags_hash_ui, node_id, &tag, 1);
    }
    sqlite3_finalize(stmt);
}

void add_sample_nodes(void) {
    if (HASH_COUNT(nodes_ui) == 0) {
        const char *meta = "{}";
        const char *tags1[] = {"sample"};
        mem_add_node(&nodes_ui, &tags_hash_ui, "sample1", "Node Alpha", -150.0f, -80.0f, meta, tags1, 1);
        mem_add_node(&nodes_ui, &tags_hash_ui, "sample2", "Node Beta", 180.0f, 90.0f, meta, tags1, 1);
        mem_add_edge(&edges_set_ui, &edges_list_ui, &edges_count_ui, &edges_capacity_ui, "sample1", "sample2");
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO nodes (id, label, x, y, metadata) VALUES (?,?,?,?,?)", -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, "sample1", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, "Node Alpha", -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 3, -150.0);
        sqlite3_bind_double(stmt, 4, -80.0);
        sqlite3_bind_text(stmt, 5, meta, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO nodes (id, label, x, y, metadata) VALUES (?,?,?,?,?)", -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, "sample2", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, "Node Beta", -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 3, 180.0);
        sqlite3_bind_double(stmt, 4, 90.0);
        sqlite3_bind_text(stmt, 5, meta, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO edges (source, target) VALUES (?,?)", -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, "sample1", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, "sample2", -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO node_tags (node_id, tag) VALUES (?,?)", -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, "sample1", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, "sample", -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO node_tags (node_id, tag) VALUES (?,?)", -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, "sample2", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, "sample", -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// ----------------------------- Command line parsing -------------------------
void parse_args(int argc, char **argv) {
    static struct option long_options[] = {
        {"log-http", no_argument, 0, 'H'},
        {"log-db",   no_argument, 0, 'D'},
        {"log-ui",   no_argument, 0, 'U'},
        {"ui-batch-size", required_argument, 0, 'b'},
        {"spatial",  no_argument, 0, 's'},
        {"background-layout", no_argument, 0, 'B'},
        {"layout-mode", required_argument, 0, 'm'},
        {"db",       required_argument, 0, 'd'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "HDUb:sBm:d:", long_options, NULL)) != -1) {
        switch (c) {
            case 'H': log_http = true; break;
            case 'D': log_db = true; break;
            case 'U': log_ui = true; break;
            case 'b': ui_batch_size = atoi(optarg); if (ui_batch_size <= 0) ui_batch_size = DEFAULT_UI_BATCH_SIZE; break;
            case 's': use_spatial_accel = true; break;
            case 'B': use_background_layout = true; break;
            case 'm':
                if (strcmp(optarg, "tree") == 0) layout_mode = LAYOUT_TREE;
                else if (strcmp(optarg, "grid") == 0) layout_mode = LAYOUT_GRID;
                else layout_mode = LAYOUT_FORCE;
                break;
            case 'd':
                db_uri = optarg;
                break;
            default: break;
        }
    }
}

// ----------------------------- Main (UI thread) -----------------------------
int main(int argc, char **argv) {
    parse_args(argc, argv);
    srand((unsigned)time(NULL));

    sqlite3_open(db_uri, &db);
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

    load_initial_data();
    add_sample_nodes();

    if (layout_mode == LAYOUT_TREE) {
        compute_tree_layout(nodes_ui, edges_list_ui, edges_count_ui);
        layout_enabled = false;
        if (log_ui) printf("Applied tree layout, dynamic layout disabled\n");
    } else if (layout_mode == LAYOUT_GRID) {
        compute_grid_layout(nodes_ui);
        layout_enabled = false;
        if (log_ui) printf("Applied grid layout, dynamic layout disabled\n");
    }

    if (use_background_layout && layout_mode != LAYOUT_FORCE) {
        use_background_layout = false;
        if (log_ui) printf("Background layout disabled because non-force layout mode selected\n");
    }

    pthread_t db_thread;
    pthread_create(&db_thread, NULL, db_thread_func, NULL);

    pthread_t http_thread;
    pthread_create(&http_thread, NULL, http_thread_func, NULL);

    if (use_background_layout) {
        pthread_t layout_thread;
        pthread_create(&layout_thread, NULL, layout_thread_func, NULL);
        if (log_ui) printf("Background layout thread started\n");
    }

    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Graph Visualizer - Multi‑threaded");
    SetTargetFPS(FPS_TARGET);

    Camera2D camera = { 0 };
    camera.offset = (Vector2){ WINDOW_WIDTH/2.0f, WINDOW_HEIGHT/2.0f };
    camera.target = (Vector2){ 0, 0 };
    camera.zoom = 1.0f;

    bool search_active = false;
    char search_text[64] = {0};
    Node *selected_node = NULL;

    while (!WindowShouldClose()) {
        process_ui_messages(ui_batch_size);

        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            Vector2 delta = GetMouseDelta();
            camera.target.x -= delta.x / camera.zoom;
            camera.target.y -= delta.y / camera.zoom;
        }
        float scroll = GetMouseWheelMove();
        if (scroll != 0) {
            camera.zoom += scroll * 0.1f;
            if (camera.zoom < 0.1f) camera.zoom = 0.1f;
            if (camera.zoom > 5.0f) camera.zoom = 5.0f;
        }
        if (IsKeyPressed(KEY_R)) {
            if (HASH_COUNT(nodes_ui) > 0) {
                float avg_x = 0, avg_y = 0;
                int count = 0;
                for (Node *n = nodes_ui; n; n = n->hh.next) { avg_x += n->x; avg_y += n->y; count++; }
                avg_x /= count; avg_y /= count;
                camera.target = (Vector2){ avg_x, avg_y };
            } else camera.target = (Vector2){ 0, 0 };
            camera.zoom = 1.0f;
        }
        if (IsKeyPressed(KEY_L)) layout_enabled = !layout_enabled;
        if (IsKeyPressed(KEY_C)) use_cage = !use_cage;
        if (IsKeyPressed(KEY_X)) use_collisions = !use_collisions;
        if (IsKeyPressed(KEY_S)) {
            search_active = !search_active;
            memset(search_text, 0, sizeof(search_text));
        }
        if (search_active) {
            int key = GetCharPressed();
            while (key) {
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
            for (Node *n = nodes_ui; n; n = n->hh.next) {
                float dist = sqrtf((n->x - world.x)*(n->x - world.x) + (n->y - world.y)*(n->y - world.y));
                if (dist < best_dist) { best_dist = dist; best = n; }
            }
            selected_node = best;
        }

        if (!use_background_layout && layout_enabled) {
            for (int i = 0; i < ITERATIONS_PER_FRAME; i++)
                compute_layout_iteration(nodes_ui, edges_list_ui, edges_count_ui, use_spatial_accel, use_cage, use_collisions);
        }

        BeginDrawing();
        ClearBackground(SOLARIZED_BASE03);
        BeginMode2D(camera);

        for (int i = 0; i < edges_count_ui; i++) {
            Node *src = NULL, *tgt = NULL;
            HASH_FIND_STR(nodes_ui, edges_list_ui[i].src, src);
            HASH_FIND_STR(nodes_ui, edges_list_ui[i].tgt, tgt);
            if (src && tgt)
                DrawLineEx((Vector2){ src->x, src->y }, (Vector2){ tgt->x, tgt->y }, 2.0f, EDGE_COLOR);
        }

        bool has_search = (strlen(search_text) > 0);
        for (Node *n = nodes_ui; n; n = n->hh.next) {
            bool highlight = false;
            if (has_search) {
                NodeTags *nt = NULL;
                HASH_FIND_STR(tags_hash_ui, n->id, nt);
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
        int y_off = 40;
        DrawText(TextFormat("Nodes: %d", HASH_COUNT(nodes_ui)), 10, y_off, 16, WHITE); y_off += 20;
        if (!use_background_layout) {
            DrawText(layout_enabled ? "[L] Layout: ON" : "[L] Layout: OFF", 10, y_off, 16, layout_enabled ? LIME : RED);
        } else {
            DrawText(layout_enabled ? "[L] Bg Layout: ON" : "[L] Bg Layout: OFF", 10, y_off, 16, layout_enabled ? LIME : RED);
        }
        y_off += 20;
        DrawText(use_cage ? "[C] Cage: ON" : "[C] Cage: OFF (Infinity)", 10, y_off, 16, use_cage ? LIME : SKYBLUE); y_off += 20;
        DrawText(use_collisions ? "[X] Collisions: ON" : "[X] Collisions: OFF", 10, y_off, 16, use_collisions ? LIME : ORANGE);
        if (use_spatial_accel) {
            DrawText("[Spatial Accel: ON]", 10, y_off+20, 16, GREEN);
            y_off += 20;
        }
        DrawText("[S] Search | [R] Reset | Right‑Mouse Pan | Scroll Zoom", 10, WINDOW_HEIGHT-30, 16, LIGHTGRAY);

        if (search_active) {
            DrawRectangle(10, 140, 300, 30, UI_BG_COLOR);
            DrawRectangleLines(10, 140, 300, 30, WHITE);
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "Search tag: %s_", search_text);
            DrawText(prompt, 15, 145, 20, WHITE);
        }

        if (selected_node) {
            int panel_w = 280, panel_h = 150;
            int panel_x = WINDOW_WIDTH - panel_w - 10, panel_y = 10;
            DrawRectangle(panel_x, panel_y, panel_w, panel_h, UI_BG_COLOR);
            DrawRectangleLines(panel_x, panel_y, panel_w, panel_h, WHITE);
            DrawText(TextFormat("ID: %s", selected_node->id), panel_x+5, panel_y+5, 14, WHITE);
            DrawText(TextFormat("Label: %s", selected_node->label), panel_x+5, panel_y+25, 14, WHITE);
            char meta_short[128];
            strncpy(meta_short, selected_node->metadata, 40);
            meta_short[40] = '\0';
            DrawText(TextFormat("Meta: %s", meta_short), panel_x+5, panel_y+45, 12, LIGHTGRAY);
            NodeTags *nt = NULL;
            HASH_FIND_STR(tags_hash_ui, selected_node->id, nt);
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
