#!/usr/bin/env sh

curl -X POST http://localhost:5000/nodes \
     -H "Content-Type: application/json" \
     -d '{"id":"A","label":"Node A","color":"#ff0000","tags":["important","data"]}'

curl -X POST http://localhost:5000/nodes \
     -H "Content-Type: application/json" \
     -d '{"id":"B","label":"Node B","color":"#00ff00","metadata":{"type":"server"}}'

curl -X POST http://localhost:5000/edges \
     -H "Content-Type: application/json" \
     -d '{"source":"A","target":"B"}'
