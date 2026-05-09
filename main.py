#!/usr/bin/env python3
"""
Graph Visualizer with Raylib + HTTP API + SQLite.

Usage:
    uv run --with raylib --with flask script.py

Endpoints and interactive controls remain the same as described.
"""

import json
import math
import random
import sqlite3
import threading
import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Set, Tuple

import pyray as pr
from pyray import Camera2D, Vector2
from flask import Flask, request, jsonify

# ----------------------------- Configuration ---------------------------------
WINDOW_WIDTH = 1280
WINDOW_HEIGHT = 720
FPS_TARGET = 60

# Force-directed layout parameters
REPULSION_STRENGTH = 800.0
ATTRACTION_STRENGTH = 0.01
DESIRED_EDGE_LENGTH = 150.0
DAMPING = 0.85
ITERATIONS_PER_FRAME = 5
MAX_FORCE = 20.0
TIME_STEP = 0.1

# Visual
NODE_RADIUS = 12
SELECTED_NODE_RADIUS = 16
EDGE_COLOR = pr.LIGHTGRAY
NODE_COLOR = pr.SKYBLUE
SELECTED_NODE_COLOR = pr.GOLD
TEXT_COLOR = pr.BLACK
# UI_BG_COLOR = pr.Fade(pr.BLACK, 0.78)
UI_BG_COLOR = pr.Color(30, 30, 40, 200)   # RGBA: near black, ~78% opacity
faded_white = pr.Color(245, 245, 245, 128)   # RAYWHITE values + 50% alpha

# ----------------------------- Database Setup --------------------------------
DB_URI = "file::memory:?cache=shared"

def init_database() -> sqlite3.Connection:
    """Create and initialize SQLite database (in-memory, shared cache)."""
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

# ----------------------------- Force-Directed Layout -------------------------
def apply_forces(conn: sqlite3.Connection, width: int, height: int):
    """Run one iteration of force-directed layout on all nodes."""
    cur = conn.cursor()
    cur.execute("SELECT id, x, y FROM nodes")
    nodes = {row["id"]: {"x": row["x"], "y": row["y"], "fx": 0.0, "fy": 0.0}
             for row in cur.fetchall()}
    if not nodes:
        return

    node_ids = list(nodes.keys())
    n = len(node_ids)

    # Repulsion between all pairs (O(N^2) – fine for demo)
    for i in range(n):
        for j in range(i + 1, n):
            id1, id2 = node_ids[i], node_ids[j]
            p1, p2 = nodes[id1], nodes[id2]
            dx = p1["x"] - p2["x"]
            dy = p1["y"] - p2["y"]
            dist_sq = dx * dx + dy * dy + 1e-5
            dist = math.sqrt(dist_sq)
            force = REPULSION_STRENGTH / dist_sq
            fx = (dx / dist) * force
            fy = (dy / dist) * force
            p1["fx"] += fx
            p1["fy"] += fy
            p2["fx"] -= fx
            p2["fy"] -= fy

    # Attraction along edges
    cur.execute("SELECT source, target FROM edges")
    for edge in cur.fetchall():
        src, tgt = edge["source"], edge["target"]
        if src in nodes and tgt in nodes:
            p1, p2 = nodes[src], nodes[tgt]
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

    # Update positions with Euler integration
    for node_id, p in nodes.items():
        # Clamp force to avoid explosions
        fx = max(-MAX_FORCE, min(MAX_FORCE, p["fx"]))
        fy = max(-MAX_FORCE, min(MAX_FORCE, p["fy"]))
        p["x"] += fx * TIME_STEP
        p["y"] += fy * TIME_STEP
        # Damping
        p["fx"] *= DAMPING
        p["fy"] *= DAMPING
        # Keep inside screen with margins
        margin = 50
        p["x"] = max(margin, min(width - margin, p["x"]))
        p["y"] = max(margin, min(height - margin, p["y"]))

    # Write back positions
    for node_id, p in nodes.items():
        cur.execute("UPDATE nodes SET x = ?, y = ? WHERE id = ?",
                    (p["x"], p["y"], node_id))
    conn.commit()

# ----------------------------- HTTP API Server ------------------------------
app = Flask(__name__)

def get_db_conn():
    """Return a new SQLite connection (shared in-memory)."""
    return sqlite3.connect(DB_URI, uri=True, check_same_thread=False)

@app.route("/nodes", methods=["POST"])
def add_node():
    data = request.json
    node_id = str(data["id"])
    label = data.get("label", node_id)
    metadata = json.dumps(data.get("metadata", {}))
    tags = data.get("tags", [])
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
        return jsonify({"status": "ok"}), 201
    except Exception as e:
        return jsonify({"error": str(e)}), 400
    finally:
        conn.close()

# (Other API endpoints remain the same as in your original script)
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
            FROM nodes n
            JOIN node_tags t ON n.id = t.node_id
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
    return jsonify({"status": "ok"}), 200

def run_api_server():
    """Start Flask API server in a background thread."""
    app.run(host="0.0.0.0", port=5000, threaded=True, debug=False)

# ----------------------------- Raylib Visualization -------------------------
def draw_text_outline(text, x, y, font_size, color, outline_color=pr.BLACK):
    """Draw text with black outline for readability."""
    pr.draw_text(text, x + 1, y + 1, font_size, outline_color)
    pr.draw_text(text, x, y, font_size, color)

def fade_color(color, alpha):
    return pr.Color(color.r, color.g, color.b, int(alpha * 255))

def main():
    # Initialize Raylib
    pr.init_window(WINDOW_WIDTH, WINDOW_HEIGHT, b"Graph Visualizer")
    pr.set_target_fps(FPS_TARGET)

    # Camera for pan/zoom
    camera = Camera2D()
    camera.offset = Vector2(WINDOW_WIDTH // 2, WINDOW_HEIGHT // 2)
    camera.target = Vector2(0, 0)
    camera.zoom = 1.0

    # Database connection for main thread
    conn = init_database()
    # Insert a sample node if empty
    cur = conn.cursor()
    cur.execute("SELECT COUNT(*) FROM nodes")
    if cur.fetchone()[0] == 0:
        cur.execute(
            "INSERT INTO nodes (id, label, x, y, metadata) VALUES (?, ?, ?, ?, ?)",
            ("sample1", "Sample Node 1", WINDOW_WIDTH // 2, WINDOW_HEIGHT // 2, "{}")
        )
        cur.execute(
            "INSERT INTO nodes (id, label, x, y, metadata) VALUES (?, ?, ?, ?, ?)",
            ("sample2", "Sample Node 2", WINDOW_WIDTH // 2 + 150, WINDOW_HEIGHT // 2 + 100, "{}")
        )
        cur.execute("INSERT INTO edges (source, target) VALUES (?, ?)", ("sample1", "sample2"))
        conn.commit()

    # UI state
    search_active = False
    search_text = ""
    selected_node_id = None
    layout_enabled = True

    # Start HTTP API server in a daemon thread
    api_thread = threading.Thread(target=run_api_server, daemon=True)
    api_thread.start()
    print("API server running on http://localhost:5000")

    # Helper functions remain unchanged...
    def get_node_details(node_id):
        cur = conn.cursor()
        cur.execute("SELECT id, label, x, y, metadata FROM nodes WHERE id = ?", (node_id,))
        node = cur.fetchone()
        if not node:
            return None
        cur.execute("SELECT tag FROM node_tags WHERE node_id = ?", (node_id,))
        tags = [row["tag"] for row in cur.fetchall()]
        return {
            "id": node["id"],
            "label": node["label"],
            "x": node["x"],
            "y": node["y"],
            "metadata": json.loads(node["metadata"]),
            "tags": tags,
        }

    def get_nodes_by_tag(tag):
        cur = conn.cursor()
        cur.execute("""
            SELECT n.id FROM nodes n
            JOIN node_tags t ON n.id = t.node_id
            WHERE t.tag = ?
        """, (tag,))
        return {row["id"] for row in cur.fetchall()}

    # Main loop
    while not pr.window_should_close():
        # Input handling
        if pr.is_mouse_button_down(pr.MOUSE_BUTTON_MIDDLE):
            delta = pr.get_mouse_delta()
            camera.target.x -= delta.x / camera.zoom
            camera.target.y -= delta.y / camera.zoom
        scroll = pr.get_mouse_wheel_move()
        if scroll != 0:
            camera.zoom += scroll * 0.1
            camera.zoom = max(0.2, min(3.0, camera.zoom))

        if pr.is_key_pressed(pr.KEY_R):
            camera.target = Vector2(0, 0)
            camera.zoom = 1.0

        if pr.is_key_pressed(pr.KEY_L):
            layout_enabled = not layout_enabled

        # Search input
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

        # Node selection
        if pr.is_mouse_button_pressed(pr.MOUSE_BUTTON_LEFT):
            mouse_pos = pr.get_mouse_position()
            world_mouse = pr.get_screen_to_world_2d(mouse_pos, camera)
            cur.execute("SELECT id, x, y FROM nodes")
            best_id = None
            best_dist = NODE_RADIUS * 2
            for row in cur.fetchall():
                dist = math.hypot(row["x"] - world_mouse.x, row["y"] - world_mouse.y)
                if dist < best_dist:
                    best_dist = dist
                    best_id = row["id"]
            selected_node_id = best_id

        # Update layout
        if layout_enabled:
            for _ in range(ITERATIONS_PER_FRAME):
                apply_forces(conn, WINDOW_WIDTH, WINDOW_HEIGHT)

        # ----- Drawing -----
        pr.begin_drawing()
        pr.clear_background(pr.DARKGRAY)
        pr.begin_mode_2d(camera)

        # Draw edges
        cur.execute("SELECT source, target FROM edges")
        edges = cur.fetchall()
        cur.execute("SELECT id, x, y FROM nodes")
        node_pos = {row["id"]: (row["x"], row["y"]) for row in cur.fetchall()}
        for edge in edges:
            src, tgt = edge["source"], edge["target"]
            if src in node_pos and tgt in node_pos:
                x1, y1 = node_pos[src]
                x2, y2 = node_pos[tgt]
                pr.draw_line_ex(Vector2(x1, y1), Vector2(x2, y2), 2, EDGE_COLOR)

        # Draw nodes
        search_results = set()
        if search_text:
            search_results = get_nodes_by_tag(search_text)

        for node_id, (x, y) in node_pos.items():
            color = SELECTED_NODE_COLOR if node_id == selected_node_id else NODE_COLOR
            if node_id in search_results:
                color = pr.PURPLE
            radius = SELECTED_NODE_RADIUS if node_id == selected_node_id else NODE_RADIUS
            pr.draw_circle_v(Vector2(x, y), radius, color)
            pr.draw_circle_lines(int(x), int(y), radius, pr.DARKBLUE)

            cur.execute("SELECT label FROM nodes WHERE id = ?", (node_id,))
            label = cur.fetchone()["label"]
            font_size = 14
            text_x = x - pr.measure_text(label, font_size) // 2
            text_y = y - radius - 16
            draw_text_outline(label, int(text_x), int(text_y), font_size, pr.BLACK, pr.WHITE)

        pr.end_mode_2d()

        # UI Overlay
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

        if selected_node_id:
            details = get_node_details(selected_node_id)
            if details:
                panel_w = 280
                panel_h = 150
                panel_x = WINDOW_WIDTH - panel_w - 10
                panel_y = 10
                pr.draw_rectangle(panel_x, panel_y, panel_w, panel_h, UI_BG_COLOR)
                pr.draw_rectangle_lines(panel_x, panel_y, panel_w, panel_h, pr.WHITE)
                draw_text_outline(f"ID: {details['id']}", panel_x + 5, panel_y + 5, 14, pr.WHITE)
                draw_text_outline(f"Label: {details['label']}", panel_x + 5, panel_y + 25, 14, pr.WHITE)
                meta = details["metadata"]
                meta_str = str(meta)[:40] + ("..." if len(str(meta)) > 40 else "")
                draw_text_outline(f"Meta: {meta_str}", panel_x + 5, panel_y + 45, 12, pr.LIGHTGRAY)
                tags_str = ", ".join(details["tags"]) if details["tags"] else "none"
                draw_text_outline(f"Tags: {tags_str}", panel_x + 5, panel_y + 65, 12, pr.LIGHTGRAY)

        pr.end_drawing()

    conn.close()
    pr.close_window()

if __name__ == "__main__":
    main()
