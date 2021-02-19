mkdir out/steps
cp -R in/inputs/ out/inputs

echo process=identity > out/steps/all

for input in out/inputs/*; do
    step=${input#out/inputs/}
    echo in/$step/=_pos:$step:out/ >> out/steps/all

    echo "process=command:bash -e in/transform.sh < in/input > out/-" > out/steps/$step
    echo in/transform.sh=$(<inref/transform.sh) >> out/steps/$step
    echo in/input=file:$input >> out/steps/$step
done
