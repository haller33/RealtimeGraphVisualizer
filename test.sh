# Add two nodes with tags
curl -X POST http://localhost:5000/nodes \
     -H "Content-Type: application/json" \
     -d '{"id":"A","label":"Node A","tags":["important","data"]}'

curl -X POST http://localhost:5000/nodes \
     -H "Content-Type: application/json" \
     -d '{"id":"B","label":"Node B","metadata":{"type":"server"}}'

# Connect them with an edge
curl -X POST http://localhost:5000/edges \
     -H "Content-Type: application/json" \
     -d '{"source":"A","target":"B"}'
