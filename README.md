# OS-Jackfruit: Multi-Container Runtime

Authors:
- Mohammed Ibrahim Maqsood Khan (`PES1UG24C578`)
- Nitesh Harsur (`PES1UG24CS584`)

## Overview

OS-Jackfruit is a two-part Linux systems project:

- `src/engine.c`: a user-space runtime and supervisor that starts multiple containers with Linux namespaces, captures logs with a bounded-buffer subsystem, tracks lifecycle state, and talks to the kernel monitor over `ioctl`.
- `kernel/monitor.c`: a loadable kernel module that tracks registered container PID namespaces, samples memory usage, issues soft-limit warnings, and kills containers that cross their hard limit.

The shared ABI lives in `include/osj_uapi.h`.

## Build

On Linux:

```bash
make engine
make tests
make module
```

To load the kernel module:

```bash
sudo insmod kernel/monitor.ko
ls -l /dev/osj_monitor
```

## Engine Usage

Run the runtime:

```bash
./bin/engine
```

Supported commands:

```text
start <name> <soft-limit-mb> <hard-limit-mb> -- <command> [args...]
stop <container-id|name>
list
help
quit
```

Example:

```bash
start web 64 128 -- /bin/sh -c 'while true; do echo hello; sleep 1; done'
start hog 32 48 -- ./tests/test_memory_overflow
list
stop web
quit
```

## Tests

- `tests/test_multi_container.sh`: starts multiple containers and lists them while they run.
- `tests/test_logging.sh`: validates that stdout and stderr are captured in the per-container log file.
- `tests/test_memory_overflow.c`: helper program that keeps allocating memory so the kernel module can enforce the hard limit.

## Notes

- This project is Linux-only. The runtime depends on Linux namespaces, `epoll`, and the kernel `ioctl` interface exposed by the monitor module.
- For namespace and mount operations, run the engine with sufficient privileges on Linux.
