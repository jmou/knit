# TODO refactor as a frontend

import os
import re
import shutil
import subprocess


def create(path, mode):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return open(path, mode)


visited = set()
plan = open('out/plan', 'w')

# write params
if os.path.isdir('in/param'):
    shutil.copytree('in/param', 'out/param')
    print('_pos=_param', file=plan)
    print('process=identity', file=plan)
    for dirpath, _, filenames in os.walk('out/param'):
        for filename in filenames:
            filepath = os.path.join(dirpath, filename)
            assert filepath.startswith('out/param/')
            inpath = 'in/' + filepath[10:]
            print(f'{inpath}=file:{filepath[4:]}', file=plan)
    print(file=plan)
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
    with create(f'out/builds/{target}', 'wb') as fh:
        fh.write(build)
    print(f'_pos={target}', file=plan)
    # TODO remove stdout if empty
    print('process=command:chmod +x in/driver && '
          'in/driver in/build > out/-', file=plan)
    driver = open(f'inref/drivers/{extension}').read().strip()
    print(f'in/driver={driver}', file=plan)
    print(f'in/build=file:builds/{target}', file=plan)
    for req in reqs:
        print(req, file=plan)
        match = re.fullmatch(r'in/[^=]*=_pos:([^:]*):.*', req)
        if match:
            frontier.append(match[1])
    print(file=plan)

# copy outputs to main
print('_pos=main', file=plan)
print('process=identity', file=plan)
for step in visited:
    print(f'in/{step}/=_pos:{step}:out/', file=plan)
print(file=plan)
