#!/usr/bin/env python3
"""
Offline layout using Graphviz directly on the SQLite database.
Uses JSON output for reliable parsing.
Usage: ./offline_layout_sqlite.py [--mode sfdp] [--db graph.db] [--output new.db]
"""

import sqlite3
import argparse
import subprocess
import tempfile
import os
import sys
import shutil
import json

def fetch_graph_from_db(db_path):
    """Read nodes and edges from SQLite database."""
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()
    cursor.execute("SELECT id, label, x, y, metadata FROM nodes")
    nodes = [{"id": row[0], "label": row[1], "x": row[2], "y": row[3], "metadata": row[4]} for row in cursor]
    cursor.execute("SELECT source, target FROM edges")
    edges = [{"source": row[0], "target": row[1]} for row in cursor]
    conn.close()
    return nodes, edges

def graph_to_dot(nodes, edges):
    """Convert nodes/edges to DOT format with proper quoting."""
    dot = ["digraph G {"]
    for n in nodes:
        node_id = n["id"]
        label = n.get("label", node_id).replace('"', '\\"')
        # Quote the node ID – Graphviz will handle embedded quotes and braces
        dot.append(f'  "{node_id}" [label="{label}"];')
    for e in edges:
        dot.append(f'  "{e["source"]}" -> "{e["target"]}";')
    dot.append("}")
    return "\n".join(dot)

def run_graphviz_json(dot_data, mode="sfdp"):
    """Run Graphviz and return JSON output."""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".dot", delete=False) as f:
        f.write(dot_data)
        dot_file = f.name
    try:
        cmd = [mode, "-Tjson", dot_file]
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return result.stdout
    finally:
        os.unlink(dot_file)

def parse_json_output(json_text):
    """Extract positions from Graphviz JSON output."""
    positions = {}
    data = json.loads(json_text)
    # JSON structure: {"objects": [{"name": "node_id", "pos": "x,y"}]}
    for obj in data.get("objects", []):
        if "pos" in obj and "name" in obj:
            name = obj["name"]
            # pos is like "x,y" or "x,y!"
            pos = obj["pos"].rstrip("!")
            try:
                x_str, y_str = pos.split(",")
                x = float(x_str)
                y = -float(y_str)   # flip Y
                positions[name] = (x, y)
            except (ValueError, AttributeError):
                continue
    return positions

def update_positions_in_db(db_path, positions, output_db=None):
    """Update node positions in the database."""
    if output_db:
        shutil.copy2(db_path, output_db)
        target_db = output_db
    else:
        target_db = db_path
    conn = sqlite3.connect(target_db)
    cursor = conn.cursor()
    for node_id, (x, y) in positions.items():
        cursor.execute("UPDATE nodes SET x = ?, y = ? WHERE id = ?", (x, y, node_id))
    conn.commit()
    conn.close()
    print(f"Updated {len(positions)} node positions in {target_db}")

def main():
    parser = argparse.ArgumentParser(description="Offline Graphviz layout on SQLite database")
    parser.add_argument("--mode", default="sfdp", choices=["sfdp", "neato", "dot"],
                        help="Graphviz layout engine")
    parser.add_argument("--db", default="graph.db", help="Input SQLite database file")
    parser.add_argument("--output", default=None, help="Output SQLite file (optional; if not given, overwrites input)")
    args = parser.parse_args()

    if not os.path.isfile(args.db):
        print(f"Database file {args.db} not found. Run the visualizer at least once to create it.")
        sys.exit(1)

    print("Fetching graph from database...")
    nodes, edges = fetch_graph_from_db(args.db)
    if not nodes:
        print("No nodes found.")
        return

    print("Converting to DOT...")
    dot_data = graph_to_dot(nodes, edges)

    print(f"Running Graphviz {args.mode} (JSON output)...")
    try:
        json_output = run_graphviz_json(dot_data, args.mode)
    except subprocess.CalledProcessError as e:
        print(f"Graphviz failed: {e.stderr}")
        sys.exit(1)

    print("Parsing JSON positions...")
    positions = parse_json_output(json_output)
    if not positions:
        print("No valid positions extracted. Exiting.")
        return

    print(f"Updating positions in database...")
    update_positions_in_db(args.db, positions, args.output)

    print("Done. Restart the visualizer to see new positions (or press 'R' to reset camera).")

if __name__ == "__main__":
    main()
