# udpperf_dpdk

## Build
```bash
meson setup build
ninja -C build
```

## Run

### Client example

```bash
sudo ./build/src/client/client -l 0-4 -- -T30 -s800 -P2
```

- `-T`: number of seconds to run the traffic (default 10).
- `-s`: per-millisecond packet budget (default 800).
- `-P`: number of ports to use (default 1, requires `(lcore_count - 1) % P == 0`).

