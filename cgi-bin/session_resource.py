import html
import os
import subprocess
import sys
import urllib.parse
from pathlib import Path

_, sid, resource = sys.argv
resource_path = Path('gen/session') / sid / resource

print('Content-Type: text/html')
print()

if os.environ['REQUEST_METHOD'] == 'GET':
    if not resource_path.exists():
        with (Path('gen/session') / sid / 'manifest').open() as f:
            for line in f:
                _, resource_id, this_resource = line.strip().split(' ', 2)
                if this_resource == resource:
                    os.makedirs(resource_path.parent(), exist_ok=True)
                    subprocess.run(f'./kgit cat-file blob {resource_id} > {resource_path}', shell=True, check=True)
                    break

    print('<html><body><form method=post><input type=submit value=save><textarea name=content style="width: 100%; height: 90%">')
    print(html.escape(resource_path.open().read()), end='')
    print('</textarea></form></body></html>')
elif os.environ['REQUEST_METHOD'] == 'POST':
    form = sys.stdin.read(int(os.environ['CONTENT_LENGTH']))
    form = urllib.parse.parse_qs(form)
    content = form['content'][0].replace('\r\n', '\n')
    with resource_path.open('w') as f:
        f.write(content)
    print(f'<html><head><meta http-equiv=refresh content="0; /cgi-bin/session/{sid}"></head></html>')
