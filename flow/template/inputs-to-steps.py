import os
import re
import subprocess


def create(path, mode):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return open(path, mode)


visited = set()

# write params
if os.path.isdir('in/param'):
    with open('steps/_param', 'w') as fh:
        print('process=identity', file=fh)
        for dirpath, _, filenames in os.walk('in/param'):
            for filename in filenames:
                filepath = os.path.join(dirpath, filename)
                assert filepath.startswith('in/param/')
                inpath = 'in/' + filepath[9:]
                print(f'{inpath}=file:./{filepath}', file=fh)
    visited.add('_param')

frontier = [t.strip() for t in open('in/targets')]
raw_root = 'in/raw/'
while frontier:
    target = frontier.pop()
    if target in visited:
        continue
    extension = target.rsplit('.', 1)[-1]

    # convert raw to reqs and build
    converter = f'in/converters/{extension}'
    output = subprocess.check_output(
        f'chmod +x {converter} && {converter} < in/raw/{target}',
        shell=True)
    # delimited by blank line
    if output.startswith(b'\n'):
        reqs = []
        build = output[1:]
    else:
        reqs, build = output.split(b'\n\n', 1)
        reqs = reqs.decode('utf-8').split('\n')

    # write the step and build
    visited.add(target)
    with create(f'builds/{target}', 'wb') as fh:
        fh.write(build)
    with create(f'steps/{target}', 'w') as fh:
        # TODO remove stdout if empty
        print('process=command:chmod +x in/driver && '
              './in/driver in/build > out/-', file=fh)
        driver = open(f'inref/drivers/{extension}').read().strip()
        print(f'in/driver={driver}', file=fh)
        print(f'in/build=file:./builds/{target}', file=fh)
        for req in reqs:
            print(req, file=fh)
            match = re.fullmatch(r'in/[^=]*=_pos:([^:]*):.*', req)
            if match:
                frontier.append(match[1])

# copy outputs to all
with open('steps/all', 'w') as fh:
    print('process=identity', file=fh)
    for step in visited:
        print(f'in/{step}/=_pos:{step}:out/', file=fh)
