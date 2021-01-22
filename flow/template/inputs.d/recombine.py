names = open('@OUT(split.sh,out/names)')
scores = open('@OUT(split.sh,out/scores)')
tac = ['letter'] + list(open('@STDOUT(tac.py)'))

for triple in zip(names, scores, tac):
    print(','.join(x.strip() for x in triple))
