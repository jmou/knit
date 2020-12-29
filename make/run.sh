mkdir -p in/inputs
cd in/inputs
bash -ex ../recipe
while read -r target; do
    if [ -f $target ]; then
        mv $target ../../out/
    else
        echo "warn: target '$target' not written" >&2
    fi
done < ../targets
