mkdir out/steps

echo process=identity > out/steps/all
for inpath in inref/inputs/*; do
    name="${inpath#inref/inputs/}"

    echo 'process=command:cp in/drivers/$(<in/mode) out/_' > "out/steps/driver__$name"
    echo in/mode=$(<$inpath/mode) >> "out/steps/driver__$name"
    for driverpath in inref/drivers/*; do
        driver=${driverpath#inref/drivers/}
        echo in/drivers/$driver=$(<$driverpath) >> "out/steps/driver__$name"
    done

    echo 'process=command:chmod +x in/driver && ./in/driver in/script' > "out/steps/$name"
    echo in/driver=_pos:driver__$name:out/_ >> "out/steps/$name"
    echo in/script=$(<$inpath/script) >> "out/steps/$name"
    # TODO this should depend on the driver. would need another subflow?
    grep -Eo '\bin/inputs/(\w+)' "in/inputs/$name/script" | cut -d/ -f3- | while read -r dep; do
        echo "in/inputs/$dep=_pos:$dep:out/_" >> "out/steps/$name"
    done

    echo "in/$name=_pos:$name:out/_" >> out/steps/all
done
