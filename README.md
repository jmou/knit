So far little attention has been paid to portability so it is likely not to run
on non-Linux machines (like macOS).

# Implementation

The knit prototype is implemented as a collection of commands that build on top
of each other, similar to git's plumbing and porcelain. To visualize how
commands invoke each other:

```
$ ./commands | grep -v -e cgi-bin -e kgit -e test | dot -Tx11
```

# Units

Unit files are steps in a data process. They specify any inputs, which can be
files in the same directory or other units (dependencies), and a command to run.
For a very basic data pipeline, see `flow/basic`.

# Tests

There is a test which gives some idea of how knit may be invoked. It has
examples of programmatically building unit files.

```
$ ./test
```

# Usage

Once one or more unit files have been created, a target unit and its
dependencies can be run:

```
$ ./run-flow flow/basic/tac.unit
5a823a84b094d77e166ac371daa20478192cd977
```

The output is a *manifest* of all steps that contribute to this run.

There are still only low-level commands for inspecting manifests:

```
$ ./kgit cat-file blob 5a823a84b094d77e166ac371daa20478192cd977
c0fce37b37ce86f022c79975e034dc32f058a0c4 785686d78e0f28356e15e1c4b3d54cee9299b14f 01e79c32a8c99c557f0757da7cb6d65b3414466d
fea1ac9a8bb9b31b15aba64d80710f4c38b5fdc2 4a85e19c36c06e471122a3a521192460e0f8957c 01e79c32a8c99c557f0757da7cb6d65b3414466d
0478d9264db30f03bf92744b779aa39a5e86758a 292dad9b01d56c78f653bf2d78cf0ee7793024b8 2d6976f9b54866fa6afeb9080bfd843098f107bb
$ ./kgit cat-file blob 0478d9264db30f03bf92744b779aa39a5e86758a
_source=unit:flow/basic/tac.unit
command=tac $input_data
input_data=tree:fea1ac9a8bb9b31b15aba64d80710f4c38b5fdc2
$ ./kgit cat-file blob 292dad9b01d56c78f653bf2d78cf0ee7793024b8
command=tac $input_data
input_data=01e79c32a8c99c557f0757da7cb6d65b3414466d
epoch=b9d4f935bd16e427ddfcd61d1efc2f715813b56c
$ ./kgit cat-file blob 2d6976f9b54866fa6afeb9080bfd843098f107bb
3
2
1
```

Each line is a step. The left column is a *tree* that represents a graph of
dependencies to execute. The middle column is a *job* which has the minimum
information needed to run a single step. The right column is the *result*.

# Web

There is a very minimal web UI:

```
$ python3 -m http.server --cgi
$ firefox http://localhost:8000/cgi-bin/unit/flow/basic/tac.unit
```
