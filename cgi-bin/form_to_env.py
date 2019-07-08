import os
import sys
import urllib.parse

form = sys.stdin.read(int(os.environ['CONTENT_LENGTH']))
form = urllib.parse.parse_qs(form)
target = form.pop('_target')[0]
with open(form.pop('_resource')[0], 'w') as f:
    for k, v in form.items():
        print(f'{k}={v[0]}', file=f)
print(target)
