# Realtime Graph Visualizer (C Implementation)

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C](https://img.shields.io/badge/language-C-blue.svg)](https://en.wikipedia.org/wiki/C_(programming_language))
[![Raylib](https://img.shields.io/badge/graphics-raylib-green.svg)](https://www.raylib.com/)
[![SQLite](https://img.shields.io/badge/database-SQLite-blue.svg)](https://www.sqlite.org/)

A high‑performance, real‑time graph visualizer with an interactive force‑directed layout, HTTP API, and persistent storage. This **C version** is the main implementation – the original Python prototype has been fully rewritten in C for better speed and lower resource usage.

![Good View](blob/good_view.jpg)  
*Interactive graph layout with tag search, collision handling, and an “infinite canvas” mode.*

---

## ✨ Features

- **Force‑directed layout** – real‑time physical simulation (repulsion, attraction, damping).
- **Interactive UI** – pan with middle mouse, zoom with scroll, select nodes by clicking.
- **Tag search** – highlight nodes that contain a specific tag (press `S` to activate search).
- **Toggleable options** – layout animation, collision resolution, infinite canvas (cage) mode.
- **HTTP API** – RESTful endpoints to manage nodes, edges, and tags dynamically.
- **Persistent storage** – SQLite (in‑memory by default, easily switched to a file).
- **Multi‑threaded** – HTTP server runs on a separate thread; graph updates are queued and applied safely.
- **Node metadata** – store arbitrary JSON metadata per node.
- **Real‑time updates** – add, delete or modify elements via the API while the visualisation runs.

---

## 📸 Screenshots

| Simple example showing many nodes | Node detail panel and search |
|-----------------------------------|------------------------------|
| ![Simple Example](blob/simple_example.jpg) | ![Node panel](blob/good_view.jpg) |

---

## 🚀 Getting Started

### Dependencies

Make sure the following libraries are installed:

- [raylib](https://www.raylib.com/) – graphics and input handling
- [SQLite3](https://www.sqlite.org/) – node/edge/tag storage
- [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) – HTTP server
- [cJSON](https://github.com/DaveGamble/cJSON) – JSON parsing
- [uthash](https://troydhanson.github.io/uthash/) – hash tables (header‑only)
- pthreads – threading support

On **NixOS** or with **nix**, you can use the provided `shell.nix`:

```bash
nix-shell
```

### Build

The project includes a simple `build.sh` script:

```bash
chmod +x build.sh
./build.sh
```

Or compile manually (example with `gcc`):

```bash
gcc -o graphviz main.c -lraylib -lsqlite3 -lmicrohttpd -lcjson -lpthread -lm
```

### Run

```bash
./graphviz
```

The visualisation window will open. The HTTP server starts automatically on port `5000`.

---

## 🎮 Keyboard & Mouse Controls

| Action               | Control                          |
|----------------------|----------------------------------|
| **Pan**              | Middle mouse button + drag       |
| **Zoom**             | Mouse wheel                      |
| **Reset camera**     | `R`                              |
| **Toggle layout**    | `L`                              |
| **Toggle cage mode** | `C` (bounding box vs. infinite)  |
| **Toggle collisions**| `X`                              |
| **Search by tag**    | `S` (then type, press Enter)     |
| **Select node**      | Left click                       |

> **Cage mode OFF** – nodes can move anywhere (infinite canvas).  
> **Cage mode ON** – nodes stay inside the window boundaries.

---

## 🌐 HTTP API

The server listens on `http://localhost:5000`. All endpoints expect and return JSON.

### Nodes

| Method | Endpoint       | Description                      |
|--------|----------------|----------------------------------|
| `POST` | `/nodes`       | Create a new node                |
| `GET`  | `/nodes`       | List all nodes (filter by `?tag=`) |
| `DELETE` | `/nodes/{id}` | Delete a node and its edges      |

**POST /nodes** example payload:

```json
{
  "id": "node123",
  "label": "My Node",
  "metadata": { "type": "server", "value": 42 },
  "tags": ["database", "critical"]
}
```

**GET /nodes?tag=database** – returns nodes having that tag.

### Edges

| Method | Endpoint                     | Description                 |
|--------|------------------------------|-----------------------------|
| `POST` | `/edges`                     | Create an edge between two nodes |
| `DELETE` | `/edges?source=A&target=B` | Delete a specific edge       |

**POST /edges** payload:

```json
{
  "source": "node123",
  "target": "node456"
}
```

### Tags

| Method | Endpoint                    | Description                  |
|--------|-----------------------------|------------------------------|
| `POST` | `/nodes/{id}/tags`          | Add tags to an existing node |

**POST /nodes/node123/tags** payload:

```json
{
  "tags": ["newtag", "updated"]
}
```

### Graph dump

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET`  | `/graph` | Returns all nodes and edges in one object |

---

## 🗄️ Database

By default, SQLite uses an **in‑memory** database (`file::memory:?cache=shared`).  
To persist data across runs, change `DB_URI` in `main.c` to a file path (e.g. `"graph.db"`).

Schema:

- `nodes(id, label, x, y, metadata)`
- `edges(source, target)`
- `node_tags(node_id, tag)`

All modifications via the HTTP API are written to the database **and** applied to the live visualisation.

---

## 🧪 Example Workflow

```bash
# Add two nodes
curl -X POST http://localhost:5000/nodes \
  -H "Content-Type: application/json" \
  -d '{"id":"A","label":"Alpha","tags":["group1"]}'

curl -X POST http://localhost:5000/nodes \
  -H "Content-Type: application/json" \
  -d '{"id":"B","label":"Beta","tags":["group1"]}'

# Connect them
curl -X POST http://localhost:5000/edges \
  -H "Content-Type: application/json" \
  -d '{"source":"A","target":"B"}'

# Search for "group1" in the visualiser (press S, type group1)
```

The graph will immediately update with the new nodes and edge, and the layout simulation will adjust.

---

## 🧠 Architecture

- **Main thread** – runs the raylib graphics loop, processes user input, and updates the force layout.
- **HTTP thread** – runs libmicrohttpd, parses JSON, and pushes messages into a thread‑safe queue.
- **Message queue** – holds `ADD_NODE`, `ADD_EDGE`, `DELETE_NODE`, etc. messages to avoid concurrent modification of the graph data.
- **In‑memory structures** – nodes stored with uthash hash tables; edges stored in a hash set and a linear array for fast iteration.
- **Force layout** – classical Fruchterman‑Reingold style with repulsion, attraction, damping, and optional collision resolution.

---

## 📦 Project Structure

```
.
├── main.c          – Single‑file implementation (graphics, HTTP, DB, layout)
├── build.sh        – Quick build script (clang)
├── shell.nix       – Nix development environment
├── blob/           – Screenshot images
│   ├── good_view.jpg
│   └── simple_example.jpg
└── README.md       – This file
```

---

## 🤝 Contributing

Contributions are welcome! Please open an issue or pull request on the [GitHub repository](https://github.com/haller33/RealtimeGraphVisualizer).

---

## 📄 License

This project is licensed under the MIT License.  
See the [LICENSE](LICENSE) file for details.

---

## 🙏 Acknowledgements

- [Raylib](https://www.raylib.com/) – simple and powerful graphics library.
- [cJSON](https://github.com/DaveGamble/cJSON) – lightweight JSON parser.
- [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) – embedded HTTP server.
- [uthash](https://troydhanson.github.io/uthash/) – convenient hash table macros.
- The original Python prototype – inspiration for the features.

---

*Built with ❤️ in C*
