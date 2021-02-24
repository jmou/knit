cp -R in/inputs/ out/inputs

for input in in/inputs/*; do
    step=${input#in/inputs/}
    echo in/$step/=_pos:$step:out/ >> steps

    echo _pos=$step
    echo 'process=command:bash -e in/transform.sh < in/input > out/-'
    echo in/transform.sh=$(<inref/transform.sh)
    echo in/input=file:inputs/$step
    echo
done

echo _pos=main
echo process=identity
cat steps
