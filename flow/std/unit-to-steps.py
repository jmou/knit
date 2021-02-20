# This is essentially unit-to-plan but it can run in a nested flow. Some
# differences to note:
# - Does not include identity steps to annotate file and input value sources.
# - Because identity steps for inline inputs are missing, they do not work
#   in the shell implementation.

import os
import shutil
import sys

pos = 0

def copy_input(path):
    os.makedirs(os.path.dirname(f'out/{path}'), exist_ok=True)
    return shutil.copy(path, f'out/{path}')

def interpret_input(inkey, invalue):
    if ':' not in invalue:
        return [f'{inkey}={invalue}']
    input_type, input_path = invalue.split(':', 1)
    if input_type == 'file':
        if inkey.endswith('/') and input_path.endswith('/'):  # directory
            lines = []
            for dirpath, _, filenames in os.walk(input_path):
                for filename in filenames:
                    filepath = os.path.join(dirpath, filename)
                    assert filepath.startswith(input_path)
                    outkey = inkey + filepath[len(input_path):]
                    value = f'{input_type}:{copy_input(filepath)}'
                    lines.append(f'{outkey}={value}')
            return lines
        value = f'{input_type}:{copy_input(input_path)}'
        return [f'{inkey}={value}']
    elif input_type == 'inline':
        return [f'{inkey}={invalue}']
    elif input_type == '_pos':
        return [f'{inkey}={invalue}']
    elif input_type == 'param':
        return [f'{inkey}=_pos:_param:{input_path}']
    else:
        raise Exception('unknown input type', input_type)

def add_step(step, source, process, inputs):
    with open(f'out/steps/{step}', 'w') as fh:
        print(f'_source={source}', file=fh)
        print(f'process={process}', file=fh)
        for inline in inputs:
            inkey, invalue = inline.rstrip('\n').split('=', 1)
            for outline in interpret_input(inkey, invalue):
                print(outline, file=fh)

def translate(unit, appendsuffix=True):
    global pos
    process = None
    inputs = []
    with open(unit) as fh:
        for line in fh:
            if line.startswith('#') or line.isspace():
                continue
            key, value = line.rstrip().split('=', 1)
            if key == 'process':
                process = value
            elif key.startswith('in/') or key.startswith('inref/'):
                if value.startswith('unit:'):
                    _, unit_path, artifact_path = value.split(':', 2)
                    dep = translate(unit_path)
                    value = f'_pos:{dep}:{artifact_path}'
                elif value.startswith('_pos:'):
                    print(f'warning: passing through _pos input in {unit}',
                          file=sys.stderr)
                inputs += [f'{key}={value}\n']
            else:
                raise Exception('unsupported key', key)

    mypos = f'{root_pos}@{pos}' if appendsuffix else root_pos
    add_step(mypos, f'unit:{unit}', process, inputs)
    pos += 1
    return mypos


_, root_pos, unit = sys.argv
os.mkdir('out/steps')
os.symlink('in/flow', 'flow')  # always rooted at flow/
translate(unit, appendsuffix=False)

# write params
if os.path.isdir('in/param'):
    shutil.copytree('in/param', 'out/param')
    with open('out/steps/_param', 'w') as fh:
        print('process=identity', file=fh)
        for dirpath, _, filenames in os.walk('out/param'):
            for filename in filenames:
                filepath = os.path.join(dirpath, filename)
                assert filepath.startswith('out/param/')
                inpath = 'in/' + filepath[10:]
                print(f'{inpath}=file:{filepath}', file=fh)
