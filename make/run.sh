mkdir -p in/inputs
cd in/inputs
bash -ex ../recipe
while read -r target; do
    mv $target ../../out/
done < ../targets
