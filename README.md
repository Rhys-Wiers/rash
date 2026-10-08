# rash

A small Unix shell written in C++ with pipes, file redirection, and command history.

## Build and run

With CMake:

```bash
cmake -B build && cmake --build build
./build/rash
```

Or directly:

```bash
g++ -std=c++17 -o rash rash.cpp
./rash
```

Type `quit` or press Ctrl+D to exit.

## Features

**Commands and pipes**

```
> ls -a
> seq 1 100 | sort -n | wc -l
```

**Redirection**

- `cmd < file` reads input from a file
- `cmd > file` writes output to a file (fails if the file exists)
- `cmd >! file` writes output to a file, overwriting it

**History**

- `history` prints the last 100 successful commands (newest is `[1]`)
- `!n` re-runs entry `n`

Failed commands and `history` itself are not saved. History is not kept between runs.

**Ctrl+C** stops the running command and returns to the prompt.

## Limitations

- Tokens must be separated by spaces (`ls > out`, not `ls>out`)
- No quoting, `cd`, background jobs (`&`), `>>`, or `2>`
- Arguments after a redirect are ignored
- `!n` must be the whole line
