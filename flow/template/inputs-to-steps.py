import os
import subprocess


def move(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    os.rename(src, dst)


raw_root = 'in/raw/'
with open('steps/all', 'w') as fh:
    print('process=identity', file=fh)
    for dirpath, _, filenames in os.walk(raw_root):
        for filename in filenames:
            filepath = os.path.join(dirpath, filename)
            assert filepath.startswith(raw_root)
            stem = filepath[len(raw_root):]
            extension = filepath.rsplit('.', 1)[-1]

            # convert raw to step and build
            converter = f'in/converters/{extension}'
            driver = open(f'inref/drivers/{extension}').read().strip()
            subprocess.run(f'chmod +x {converter} && '
                           f'{converter} {driver} file:./builds/{stem} < {filepath}',
                           shell=True, check=True)
            move('step', f'steps/{stem}')
            move('build', f'builds/{stem}')

            print(f'in/{stem}/=_pos:{stem}:out/', file=fh)
