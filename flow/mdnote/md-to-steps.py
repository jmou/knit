import os
import sys

cellbuf = []

def scan_to_cell():
    for line in sys.stdin:
        if line.startswith('# '):
            global cellbuf
            cellbuf = [line]
            return line[2:].strip()

def driver_ref(driver):
    with open(f'inref/drivers/{driver}') as fh:
        return fh.read().strip()

def make_step(name, prereqs, driver, script):
    with open(f'out/scripts/{name}', 'w') as fh:
        fh.write(script)
    step = [
        'process=command:chmod +x in/driver && ./in/driver',
        f'in/driver={driver_ref(driver)}',
        f'in/script=file:scripts/{name}',
    ]
    for prereq in prereqs:
        step.append(f'in/inputs/{prereq}/=_pos:{prereq}:out/')
    return step

def main():
    os.mkdir('out/scripts')  # needed by scan_to_cell
    os.mkdir('out/cells')  # needed to write cellbuf
    steps = {}
    while True:
        name = scan_to_cell()
        if not name:
            break
        prereqs = []
        for line in sys.stdin:
            cellbuf.append(line)
            line = line.strip()
            if line.startswith('- '):
                prereqs.append(line[2:])
            else:
                assert line.startswith('```'), 'expected - prereq or ```driver'
                driver = line[3:]
                break
        script = []
        for line in sys.stdin:
            cellbuf.append(line)
            if line.startswith('```'):
                break
            script.append(line)
        script = ''.join(script)
        steps[name] = make_step(name, prereqs, driver, script)
        with open(f'out/cells/{name}', 'w') as fh:
            fh.write(''.join(cellbuf))
    with open('out/order', 'w') as fh:
        print('\n'.join(steps), file=fh)

    with open('out/plan', 'w') as fh:
        for name, step in steps.items():
            print(f'_pos={name}', file=fh)
            for line in step:
                print(line, file=fh)
            print(file=fh)

        print('_pos=main', file=fh)
        print('process=identity', file=fh)
        print('in/order=file:order', file=fh)
        for name in steps:
            print(f'in/outs/{name}/=_pos:{name}:out/', file=fh)
            print(f'in/cells/{name}=file:cells/{name}', file=fh)
        print(file=fh)


if __name__ == '__main__':
    main()
