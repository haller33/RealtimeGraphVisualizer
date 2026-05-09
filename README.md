# Realtime Graph Visualizer

Just a graph visualizer realtime using Python + Raylib + API + SQLite

## ✨ Overview

This project is a real‑time, interactive graph visualizer that combines:

* A **Raylib**‑powered 2D rendering window where nodes and edges are drawn and can be panned/zoomed.
* An **HTTP API** (built with Flask) to add, remove and query nodes, edges and tags.
* A **SQLite in‑memory database** that persists all changes, while the visualisation runs from fast in‑memory hash tables.
* An **anti‑clustering force‑directed layout** that keeps nodes well spaced for readability.

## 🚀 Features

- **Real‑time updates** – add nodes and edges via simple `curl` commands and see them appear instantly.
- **Interactive 2D view** – pan with middle‑click, zoom with scroll wheel, left‑click to select a node.
- **Tagging system** – assign multiple tags to any node; search for nodes by tag (press `S`).
- **Force‑directed layout** – nodes automatically arrange themselves; press `L` to freeze/unfreeze the layout.
- **Metadata storage** – each node carries a JSON metadata field, shown when selected.
- **Solarized Dark theme** – a calm, developer‑friendly colour scheme.
- **Non‑blocking architecture** – a queue decouples the API threads from the drawing thread, ensuring smooth animation even under load.

## 📦 Requirements

- Python 3.12 or later
- [uv](https://github.com/astral-sh/uv) (recommended) or `pip`
- The following Python packages (automatically installed by `uv`):
  - `raylib` – 2D graphics and input
  - `flask` – REST API
- On **NixOS** you need a shell that provides X11/OpenGL libraries (see the included `shell.nix`).

## 🔧 Installation & Running

### Using `uv` (recommended)

```bash
# Clone the repository
git clone https://github.com/haller33/RealtimeGraphVisualizer
cd RealtimeGraphVisualizer

# Run the visualizer (uv will fetch Python and all dependencies)
uv run --with raylib --with flask python main.py
```

### Using `pip`

```bash
# Create a virtual environment
python -m venv venv
source venv/bin/activate

# Install dependencies
pip install raylib flask

# Run the program
python main.py
```

After startup you will see two sample nodes (`Node Alpha` and `Node Beta`) connected by an edge.  
Your terminal will print:

```
API server running on http://localhost:5000
```

Keep this terminal open while you use the visualizer.

## 📡 API Endpoints

All endpoints expect and return JSON unless noted otherwise.

| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | `/nodes` | Add a node. Example: `{"id":"A","label":"Node A","tags":["test"],"metadata":{"color":"blue"}}` |
| `POST` | `/edges` | Add an edge. Example: `{"source":"A","target":"B"}` |
| `POST` | `/nodes/<id>/tags` | Add tags to an existing node. Example: `{"tags":["newtag","important"]}` |
| `GET`  | `/nodes` | List all nodes (metadata is returned as a JSON object). |
| `GET`  | `/nodes?tag=foo` | List nodes that contain the given tag. |
| `GET`  | `/graph` | Get the full graph: `{"nodes":[...], "edges":[...]}`. |
| `DELETE`| `/nodes/<id>` | Delete a node and all edges connected to it. |
| `DELETE`| `/edges?source=A&target=B` | Delete a specific edge. |

### Example: adding a graph via `curl`

```bash
# Add two nodes with tags
curl -X POST http://localhost:5000/nodes \
     -H "Content-Type: application/json" \
     -d '{"id":"A","label":"Node A","tags":["important","data"]}'

curl -X POST http://localhost:5000/nodes \
     -H "Content-Type: application/json" \
     -d '{"id":"B","label":"Node B","metadata":{"type":"server"}}'

# Connect them
curl -X POST http://localhost:5000/edges \
     -H "Content-Type: application/json" \
     -d '{"source":"A","target":"B"}'
```

## 🖱️ Interactive Controls (Raylib Window)

| Control | Action |
|---------|--------|
| **Middle‑click + drag** | Pan the view |
| **Scroll wheel** | Zoom in / out |
| **Left‑click** on a node | Select the node – its metadata and tags appear in the top‑right panel |
| `R` | Reset camera – centers on the average position of all nodes |
| `L` | Toggle force‑directed layout (freeze/unfreeze) |
| `S` | Activate search bar – type a tag name, press **Enter** to highlight all nodes with that tag (they turn purple) |

## 🧪 Testing

A stress test script is provided:

```bash
chmod +x stress_test.sh
./stress_test.sh 200 20   # 200 requests, 20 parallel
```

You can also run the full API test suite:

```bash
chmod +x test_api.sh
./test_api.sh
```

## 🧬 Architecture Notes

* The program uses **two threads**:
  1. **Flask API** – receives HTTP requests, writes to the SQLite database, and pushes changes into a `queue.Queue`.
  2. **Raylib drawing** – consumes the queue and updates fast in‑memory dictionaries (`nodes`, `edges`, `node_tags`). The drawing loop never touches the database, making it smooth and responsive.
* The layout is a **spring‑embedder** (force‑directed) with:
  * Stronger repulsion to prevent clustering.
  * Weaker edge attraction to keep the graph readable.
  * An extra collision‑resolution step that guarantees no two nodes overlap.

## 💡 Tips

* After adding many nodes via the API, press **`R`** to re‑center the camera.
* If nodes fly away, press **`L`** to freeze the layout, pan/zoom to the graph, then press **`L`** again.
* The `metadata` field can store any JSON data – it will be displayed when you click on a node.

## 📄 License

This project is open‑source under the [MIT License](LICENSE).

## 🤝 Contributing

Issues and pull requests are welcome! For major changes, please open an issue first to discuss what you would like to change.
