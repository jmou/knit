import os
import subprocess


def create(path, mode):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return open(path, mode)


allsteps = []
raw_root = 'in/raw/'
for dirpath, _, filenames in os.walk(raw_root):
    for filename in filenames:
        filepath = os.path.join(dirpath, filename)
        assert filepath.startswith(raw_root)
        stem = filepath[len(raw_root):]
        extension = filepath.rsplit('.', 1)[-1]

        # convert raw to reqs and build
        converter = f'in/converters/{extension}'
        output = subprocess.check_output(
            f'chmod +x {converter} && {converter} < {filepath}',
            shell=True)
        # delimited by blank line
        if output.startswith(b'\n'):
            reqs = b''
            build = output[1:]
        else:
            reqs, build = output.split(b'\n\n', 1)
            reqs += b'\n'

        # write the step and build
        allsteps.append(stem)
        with create(f'steps/{stem}', 'w') as fh:
            print('process=command:chmod +x in/driver && ./in/driver in/build', file=fh)
            driver = open(f'inref/drivers/{extension}').read().strip()
            print(f'in/driver={driver}', file=fh)
            print(f'in/build=file:./builds/{stem}', file=fh)
            fh.write(reqs.decode('utf-8'))
        with create(f'builds/{stem}', 'wb') as fh:
            fh.write(build)

# copy outputs to all
with open('steps/all', 'w') as fh:
    print('process=identity', file=fh)
    for step in allsteps:
        print(f'in/{step}/=_pos:{step}:out/', file=fh)
