#!/usr/bin/env bash
# test_api_debug.sh – prints full response for failed endpoints

API_URL="http://localhost:5000"

curl -s -X POST "$API_URL/nodes" -H "Content-Type: application/json" \
     -d '{"id":"A","label":"Node A","tags":["test"],"metadata":{}}' && echo

curl -s -X POST "$API_URL/nodes" -H "Content-Type: application/json" \
     -d '{"id":"B","label":"Node B"}' && echo

curl -s -X POST "$API_URL/edges" -H "Content-Type: application/json" \
     -d '{"source":"A","target":"B"}' && echo

echo "=== GET /graph ==="
curl -v "$API_URL/graph"
echo

echo "=== GET /nodes?tag=test ==="
curl -v "$API_URL/nodes?tag=test"
echo

echo "=== GET /nodes ==="
curl -v "$API_URL/nodes"
