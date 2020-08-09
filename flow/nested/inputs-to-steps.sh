echo process=identity > steps/all
for inpath in inref/inputs/*; do
    name="${inpath#inref/inputs/}"

    echo 'process=command:cp in/drivers/$(<in/mode) out/_' > "steps/driver__$name"
    echo in/mode=$(<$inpath/mode) >> "steps/driver__$name"
    for driverpath in inref/drivers/*; do
        driver=${driverpath#inref/drivers/}
        echo in/drivers/$driver=$(<$driverpath) >> "steps/driver__$name"
    done

    echo 'process=command:chmod +x in/driver && ./in/driver in/script' > "steps/$name"
    echo in/driver=_pos:driver__$name:out/_ >> "steps/$name"
    echo in/script=$(<$inpath/script) >> "steps/$name"
    # TODO this should depend on the driver. would need another subflow?
    grep -Eo '\bin/inputs/(\w+)' "in/inputs/$name/script" | cut -d/ -f3- | while read -r dep; do
        echo "in/inputs/$dep=_pos:$dep:out/_" >> "steps/$name"
    done

    echo "in/$name=_pos:$name:out/_" >> steps/all
done
