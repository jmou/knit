echo digraph {
find "$1" -name '*.unit' -type f | while read unit; do
    echo "\"$unit\" [shape=box,style=filled,fillcolor=white,label=\"${unit#$1}\"];"
    grep ^input_ "$unit" | cut -d= -f2- | grep ^unit: | cut -d: -f2- | while read dep; do
        echo "\"$unit\" -> \"$dep\";"
    done
done
echo }
