#!/usr/bin/env python3
"""
Graph Visualizer with Raylib + HTTP API + SQLite.
- Non‑blocking queue between API and drawing.
- In‑memory hash tables for fast rendering.
- Improved force layout to prevent node clustering.
"""

import json
import math
import random
import sqlite3
import threading
import queue
from typing import Dict, Set, Tuple

import pyray as pr
from pyray import Camera2D, Vector2
from flask import Flask, request, jsonify

# ----------------------------- Configuration ---------------------------------
WINDOW_WIDTH = 1280
WINDOW_HEIGHT = 720
FPS_TARGET = 60

# Force-directed layout parameters (adjusted for better spacing)
REPULSION_STRENGTH = 2500.0      # much stronger repulsion (was 800)
ATTRACTION_STRENGTH = 0.005      # weaker attraction (was 0.01)
DESIRED_EDGE_LENGTH = 150.0
DAMPING = 0.85
ITERATIONS_PER_FRAME = 5
MAX_FORCE = 30.0
TIME_STEP = 0.1

# Minimum distance between any two nodes (prevents overlap)
NODE_RADIUS = 12
SELECTED_NODE_RADIUS = 16
MIN_DISTANCE = NODE_RADIUS * 2 + 8   # ~32 pixels

# Visual
EDGE_COLOR = pr.LIGHTGRAY
NODE_COLOR = pr.SKYBLUE
SELECTED_NODE_COLOR = pr.GOLD
UI_BG_COLOR = pr.Color(30, 30, 40, 200)
SOLARIZED_BASE03 = pr.Color(0, 43, 54)

# ----------------------------- Database & Queue ------------------------------
DB_URI = "file::memory:?cache=shared"
update_queue = queue.Queue()

def init_database() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_URI, uri=True, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()
    cur.executescript("""
        CREATE TABLE IF NOT EXISTS nodes (
            id TEXT PRIMARY KEY,
            label TEXT,
            x REAL,
            y REAL,
            metadata TEXT
        );
        CREATE TABLE IF NOT EXISTS edges (
            source TEXT,
            target TEXT,
            PRIMARY KEY (source, target)
        );
        CREATE TABLE IF NOT EXISTS node_tags (
            node_id TEXT,
            tag TEXT,
            PRIMARY KEY (node_id, tag),
            FOREIGN KEY (node_id) REFERENCES nodes(id) ON DELETE CASCADE
        );
    """)
    conn.commit()
    return conn

# ----------------------------- HTTP API Server ------------------------------
app = Flask(__name__)

def get_db_conn():
    return sqlite3.connect(DB_URI, uri=True, check_same_thread=False)

@app.route("/nodes", methods=["POST"])
def add_node():
    data = request.json
    node_id = str(data["id"])
    label = data.get("label", node_id)
    metadata = json.dumps(data.get("metadata", {}))
    tags = data.get("tags", [])
    # Place new nodes in a random area around the centre of the screen
    x = random.uniform(200, WINDOW_WIDTH - 200)
    y = random.uniform(200, WINDOW_HEIGHT - 200)

    conn = get_db_conn()
    cur = conn.cursor()
    try:
        cur.execute(
            "INSERT OR REPLACE INTO nodes (id, label, x, y, metadata) VALUES (?, ?, ?, ?, ?)",
            (node_id, label, x, y, metadata)
        )
        for tag in tags:
            cur.execute("INSERT OR IGNORE INTO node_tags (node_id, tag) VALUES (?, ?)",
                        (node_id, tag))
        conn.commit()
        update_queue.put(("add_node", node_id, label, x, y, metadata, tags))
        return jsonify({"status": "ok"}), 201
    except Exception as e:
        return jsonify({"error": str(e)}), 400
    finally:
        conn.close()

@app.route("/edges", methods=["POST"])
def add_edge():
    data = request.json
    source = str(data["source"])
    target = str(data["target"])
    conn = get_db_conn()
    cur = conn.cursor()
    try:
        cur.execute("INSERT OR IGNORE INTO edges (source, target) VALUES (?, ?)",
                    (source, target))
        conn.commit()
        update_queue.put(("add_edge", source, target))
        return jsonify({"status": "ok"}), 201
    except Exception as e:
        return jsonify({"error": str(e)}), 400
    finally:
        conn.close()

@app.route("/nodes/<node_id>/tags", methods=["POST"])
def add_tags(node_id):
    data = request.json
    tags = data.get("tags", [])
    conn = get_db_conn()
    cur = conn.cursor()
    try:
        for tag in tags:
            cur.execute("INSERT OR IGNORE INTO node_tags (node_id, tag) VALUES (?, ?)",
                        (node_id, tag))
        conn.commit()
        update_queue.put(("add_tags", node_id, tags))
        return jsonify({"status": "ok"}), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 400
    finally:
        conn.close()

@app.route("/nodes", methods=["GET"])
def get_nodes_by_tag():
    tag = request.args.get("tag")
    conn = get_db_conn()
    cur = conn.cursor()
    if tag:
        cur.execute("""
            SELECT n.id, n.label, n.x, n.y, n.metadata
            FROM nodes n JOIN node_tags t ON n.id = t.node_id
            WHERE t.tag = ?
        """, (tag,))
    else:
        cur.execute("SELECT id, label, x, y, metadata FROM nodes")
    nodes = [dict(row) for row in cur.fetchall()]
    for node in nodes:
        node["metadata"] = json.loads(node["metadata"])
    conn.close()
    return jsonify(nodes)

@app.route("/graph", methods=["GET"])
def get_graph():
    conn = get_db_conn()
    cur = conn.cursor()
    cur.execute("SELECT id, label, x, y, metadata FROM nodes")
    nodes = [dict(row) for row in cur.fetchall()]
    for node in nodes:
        node["metadata"] = json.loads(node["metadata"])
    cur.execute("SELECT source, target FROM edges")
    edges = [dict(row) for row in cur.fetchall()]
    conn.close()
    return jsonify({"nodes": nodes, "edges": edges})

@app.route("/nodes/<node_id>", methods=["DELETE"])
def delete_node(node_id):
    conn = get_db_conn()
    cur = conn.cursor()
    try:
        cur.execute("DELETE FROM nodes WHERE id = ?", (node_id,))
        cur.execute("DELETE FROM edges WHERE source = ? OR target = ?", (node_id, node_id))
        conn.commit()
        update_queue.put(("delete_node", node_id))
        return jsonify({"status": "ok"}), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 400
    finally:
        conn.close()

@app.route("/edges", methods=["DELETE"])
def delete_edge():
    source = request.args.get("source")
    target = request.args.get("target")
    if not source or not target:
        return jsonify({"error": "Missing source or target"}), 400
    conn = get_db_conn()
    cur = conn.cursor()
    cur.execute("DELETE FROM edges WHERE source = ? AND target = ?", (source, target))
    conn.commit()
    conn.close()
    update_queue.put(("delete_edge", source, target))
    return jsonify({"status": "ok"}), 200

def run_api_server():
    app.run(host="0.0.0.0", port=5000, threaded=True, debug=False)

# ----------------------------- Raylib Visualization -------------------------
def draw_text_outline(text, x, y, font_size, color, outline_color=pr.BLACK):
    pr.draw_text(text, x+1, y+1, font_size, outline_color)
    pr.draw_text(text, x, y, font_size, color)

def main():
    pr.init_window(WINDOW_WIDTH, WINDOW_HEIGHT, b"Graph Visualizer")
    pr.set_target_fps(FPS_TARGET)

    camera = Camera2D()
    camera.offset = Vector2(WINDOW_WIDTH // 2, WINDOW_HEIGHT // 2)
    camera.target = Vector2(0, 0)
    camera.zoom = 1.0

    # In‑memory structures
    nodes: Dict[str, Dict] = {}
    edges: Set[Tuple[str, str]] = set()
    node_tags: Dict[str, Set[str]] = {}

    # Load initial data from SQLite
    conn = init_database()
    cur = conn.cursor()
    cur.execute("SELECT id, label, x, y, metadata FROM nodes")
    for row in cur.fetchall():
        nid = row["id"]
        nodes[nid] = {
            "label": row["label"],
            "x": row["x"],
            "y": row["y"],
            "metadata": json.loads(row["metadata"]),
            "fx": 0.0, "fy": 0.0
        }
        node_tags[nid] = set()
    cur.execute("SELECT source, target FROM edges")
    for row in cur.fetchall():
        edges.add((row["source"], row["target"]))
    cur.execute("SELECT node_id, tag FROM node_tags")
    for row in cur.fetchall():
        node_tags.setdefault(row["node_id"], set()).add(row["tag"])

    # Sample nodes if empty
    if not nodes:
        nodes["sample1"] = {"label": "Node Alpha", "x": -150.0, "y": -80.0,
                            "metadata": {}, "fx": 0.0, "fy": 0.0}
        nodes["sample2"] = {"label": "Node Beta", "x": 180.0, "y": 90.0,
                            "metadata": {}, "fx": 0.0, "fy": 0.0}
        edges.add(("sample1", "sample2"))
        node_tags["sample1"] = set()
        node_tags["sample2"] = set()
        cur.execute("INSERT OR IGNORE INTO nodes (id, label, x, y, metadata) VALUES (?, ?, ?, ?, ?)",
                    ("sample1", "Node Alpha", -150.0, -80.0, "{}"))
        cur.execute("INSERT OR IGNORE INTO nodes (id, label, x, y, metadata) VALUES (?, ?, ?, ?, ?)",
                    ("sample2", "Node Beta", 180.0, 90.0, "{}"))
        cur.execute("INSERT OR IGNORE INTO edges (source, target) VALUES (?, ?)", ("sample1", "sample2"))
        conn.commit()
        camera.target = Vector2(15.0, 5.0)

    search_active = False
    search_text = ""
    selected_node_id = None
    layout_enabled = True

    api_thread = threading.Thread(target=run_api_server, daemon=True)
    api_thread.start()
    print("API server running on http://localhost:5000")

    def get_node_details(node_id):
        if node_id not in nodes:
            return None
        n = nodes[node_id]
        return {
            "id": node_id,
            "label": n["label"],
            "x": n["x"],
            "y": n["y"],
            "metadata": n["metadata"],
            "tags": list(node_tags.get(node_id, set())),
        }

    def get_nodes_by_tag(tag):
        result = set()
        for nid, tags in node_tags.items():
            if tag in tags:
                result.add(nid)
        return result

    # ----------------------- Collision resolution helper -----------------------
    def resolve_collisions():
        """Separate any two nodes that are closer than MIN_DISTANCE."""
        node_list = list(nodes.items())
        n = len(node_list)
        for i in range(n):
            id1, p1 = node_list[i]
            for j in range(i+1, n):
                id2, p2 = node_list[j]
                dx = p1["x"] - p2["x"]
                dy = p1["y"] - p2["y"]
                dist = math.hypot(dx, dy)
                if dist < MIN_DISTANCE and dist > 0:
                    overlap = MIN_DISTANCE - dist
                    angle = math.atan2(dy, dx)
                    push_x = math.cos(angle) * overlap * 0.5
                    push_y = math.sin(angle) * overlap * 0.5
                    p1["x"] += push_x
                    p1["y"] += push_y
                    p2["x"] -= push_x
                    p2["y"] -= push_y

    # Main loop
    while not pr.window_should_close():
        # Process queue messages (same as before)
        while True:
            try:
                msg = update_queue.get_nowait()
            except queue.Empty:
                break
            if msg[0] == "add_node":
                _, nid, label, x, y, metadata_str, tags = msg
                nodes[nid] = {
                    "label": label,
                    "x": x,
                    "y": y,
                    "metadata": json.loads(metadata_str),
                    "fx": 0.0, "fy": 0.0
                }
                node_tags[nid] = set(tags)
            elif msg[0] == "add_edge":
                _, src, tgt = msg
                edges.add((src, tgt))
            elif msg[0] == "add_tags":
                _, nid, tags = msg
                node_tags.setdefault(nid, set()).update(tags)
            elif msg[0] == "delete_node":
                _, nid = msg
                if nid in nodes:
                    del nodes[nid]
                if nid in node_tags:
                    del node_tags[nid]
                edges = {e for e in edges if e[0] != nid and e[1] != nid}
            elif msg[0] == "delete_edge":
                _, src, tgt = msg
                edges.discard((src, tgt))

        # Input handling (unchanged)
        if pr.is_mouse_button_down(pr.MOUSE_BUTTON_MIDDLE):
            delta = pr.get_mouse_delta()
            camera.target.x -= delta.x / camera.zoom
            camera.target.y -= delta.y / camera.zoom
        scroll = pr.get_mouse_wheel_move()
        if scroll != 0:
            camera.zoom += scroll * 0.1
            camera.zoom = max(0.2, min(3.0, camera.zoom))

        if pr.is_key_pressed(pr.KEY_R):
            if nodes:
                avg_x = sum(n["x"] for n in nodes.values()) / len(nodes)
                avg_y = sum(n["y"] for n in nodes.values()) / len(nodes)
                camera.target = Vector2(avg_x, avg_y)
            else:
                camera.target = Vector2(0, 0)
            camera.zoom = 1.0

        if pr.is_key_pressed(pr.KEY_L):
            layout_enabled = not layout_enabled

        if pr.is_key_pressed(pr.KEY_S):
            search_active = not search_active
            search_text = ""
        if search_active:
            key = pr.get_char_pressed()
            while key > 0:
                if 32 <= key <= 126:
                    search_text += chr(key)
                key = pr.get_char_pressed()
            if pr.is_key_pressed(pr.KEY_BACKSPACE) and search_text:
                search_text = search_text[:-1]
            if pr.is_key_pressed(pr.KEY_ENTER):
                search_active = False

        if pr.is_mouse_button_pressed(pr.MOUSE_BUTTON_LEFT):
            mouse_pos = pr.get_mouse_position()
            world_mouse = pr.get_screen_to_world_2d(mouse_pos, camera)
            best_id = None
            best_dist = NODE_RADIUS * 2
            for nid, n in nodes.items():
                dist = math.hypot(n["x"] - world_mouse.x, n["y"] - world_mouse.y)
                if dist < best_dist:
                    best_dist = dist
                    best_id = nid
            selected_node_id = best_id

        # ---------- Force layout with improved parameters ----------
        if layout_enabled and nodes:
            # Reset forces
            for n in nodes.values():
                n["fx"] = 0.0
                n["fy"] = 0.0

            node_list = list(nodes.items())
            n_count = len(node_list)

            # Repulsion (stronger)
            for i in range(n_count):
                id1, p1 = node_list[i]
                for j in range(i+1, n_count):
                    id2, p2 = node_list[j]
                    dx = p1["x"] - p2["x"]
                    dy = p1["y"] - p2["y"]
                    dist_sq = dx*dx + dy*dy + 1e-5
                    dist = math.sqrt(dist_sq)
                    force = REPULSION_STRENGTH / dist_sq
                    fx = (dx / dist) * force
                    fy = (dy / dist) * force
                    p1["fx"] += fx
                    p1["fy"] += fy
                    p2["fx"] -= fx
                    p2["fy"] -= fy

            # Attraction along edges (weaker)
            for src, tgt in edges:
                if src in nodes and tgt in nodes:
                    p1 = nodes[src]
                    p2 = nodes[tgt]
                    dx = p1["x"] - p2["x"]
                    dy = p1["y"] - p2["y"]
                    dist = math.hypot(dx, dy) + 1e-5
                    force = ATTRACTION_STRENGTH * (dist - DESIRED_EDGE_LENGTH)
                    fx = (dx / dist) * force
                    fy = (dy / dist) * force
                    p1["fx"] -= fx
                    p1["fy"] -= fy
                    p2["fx"] += fx
                    p2["fy"] += fy

            # Euler integration
            for n in nodes.values():
                fx = max(-MAX_FORCE, min(MAX_FORCE, n["fx"]))
                fy = max(-MAX_FORCE, min(MAX_FORCE, n["fy"]))
                n["x"] += fx * TIME_STEP
                n["y"] += fy * TIME_STEP
                n["fx"] *= DAMPING
                n["fy"] *= DAMPING

            # Additional collision resolution (keeps nodes nicely apart)
            resolve_collisions()

            # Keep nodes inside screen margins
            margin = 50
            for n in nodes.values():
                n["x"] = max(margin, min(WINDOW_WIDTH - margin, n["x"]))
                n["y"] = max(margin, min(WINDOW_HEIGHT - margin, n["y"]))

        # ---------- Drawing (unchanged) ----------
        pr.begin_drawing()
        pr.clear_background(SOLARIZED_BASE03)
        pr.begin_mode_2d(camera)

        for src, tgt in edges:
            if src in nodes and tgt in nodes:
                x1, y1 = nodes[src]["x"], nodes[src]["y"]
                x2, y2 = nodes[tgt]["x"], nodes[tgt]["y"]
                pr.draw_line_ex(Vector2(x1, y1), Vector2(x2, y2), 2, EDGE_COLOR)

        search_results = set()
        if search_text:
            search_results = get_nodes_by_tag(search_text)

        for nid, n in nodes.items():
            color = SELECTED_NODE_COLOR if nid == selected_node_id else NODE_COLOR
            if nid in search_results:
                color = pr.PURPLE
            radius = SELECTED_NODE_RADIUS if nid == selected_node_id else NODE_RADIUS
            pr.draw_circle_v(Vector2(n["x"], n["y"]), radius, color)
            pr.draw_circle_lines(int(n["x"]), int(n["y"]), radius, pr.DARKBLUE)

            font_size = 14
            text_x = n["x"] - pr.measure_text(n["label"], font_size) // 2
            text_y = n["y"] - radius - 16
            draw_text_outline(n["label"], int(text_x), int(text_y), font_size, pr.BLACK, pr.WHITE)

        pr.end_mode_2d()

        fps_text = f"FPS: {pr.get_fps()}"
        draw_text_outline(fps_text, 10, 10, 20, pr.WHITE)
        status = "Layout ON" if layout_enabled else "Layout OFF"
        draw_text_outline(status, 10, 40, 16, pr.LIME if layout_enabled else pr.RED)
        help_text = "[L] Layout | [S] Search tags | [R] Reset cam | Middle drag | Scroll zoom"
        draw_text_outline(help_text, 10, WINDOW_HEIGHT - 30, 16, pr.WHITE)

        if search_active:
            bar_bg = (10, 70, 300, 30)
            pr.draw_rectangle_rec(pr.Rectangle(*bar_bg), UI_BG_COLOR)
            pr.draw_rectangle_lines(int(bar_bg[0]), int(bar_bg[1]), int(bar_bg[2]), int(bar_bg[3]), pr.WHITE)
            prompt = f"Search tag: {search_text}_"
            pr.draw_text(prompt, 15, 75, 20, pr.WHITE)

        if selected_node_id and selected_node_id in nodes:
            details = get_node_details(selected_node_id)
            if details:
                panel_w = 280
                panel_h = 150
                panel_x = WINDOW_WIDTH - panel_w - 10
                panel_y = 10
                pr.draw_rectangle(panel_x, panel_y, panel_w, panel_h, UI_BG_COLOR)
                pr.draw_rectangle_lines(panel_x, panel_y, panel_w, panel_h, pr.WHITE)
                draw_text_outline(f"ID: {details['id']}", panel_x+5, panel_y+5, 14, pr.WHITE)
                draw_text_outline(f"Label: {details['label']}", panel_x+5, panel_y+25, 14, pr.WHITE)
                meta_str = str(details["metadata"])[:40]
                if len(str(details["metadata"])) > 40:
                    meta_str += "..."
                draw_text_outline(f"Meta: {meta_str}", panel_x+5, panel_y+45, 12, pr.LIGHTGRAY)
                tags_str = ", ".join(details["tags"]) if details["tags"] else "none"
                draw_text_outline(f"Tags: {tags_str}", panel_x+5, panel_y+65, 12, pr.LIGHTGRAY)

        pr.end_drawing()

    conn.close()
    pr.close_window()

if __name__ == "__main__":
    main()
