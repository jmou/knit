exec > out/plan
mkdir steps

for inpath in inref/inputs/*; do
    name="${inpath#inref/inputs/}"

    if [[ ! -f "steps/driver__$name" ]]; then
        touch "steps/driver__$name"
        echo "_pos=driver__$name"
        echo 'process=command:cp in/drivers/$(<in/mode) out/_'
        echo in/mode=$(<$inpath/mode)
        for driverpath in inref/drivers/*; do
            driver=${driverpath#inref/drivers/}
            echo in/drivers/$driver=$(<$driverpath)
        done
        echo
    fi

    touch steps/$name
    echo _pos=$name
    echo 'process=command:chmod +x in/driver && ./in/driver in/script'
    echo in/driver=_pos:driver__$name:out/_
    echo in/script=$(<$inpath/script)
    # TODO this should depend on the driver. would need another subflow?
    grep -Eo '\bin/inputs/(\w+)' "in/inputs/$name/script" | cut -d/ -f3- | while read -r dep; do
        echo "in/inputs/$dep=_pos:$dep:out/_"
    done
    echo
done

echo _pos=main
echo process=identity
for step in steps/*; do
    name=${step#steps/}
    echo "in/$name=_pos:$name:out/_"
done
echo
