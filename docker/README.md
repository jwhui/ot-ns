# OTNS v3 Docker Playground

## Build Locally

From the repository root (with the `openthread` submodule initialized):

```bash
docker build -f docker/Dockerfile -t openthread/otns-v3 .
```

## Run Interactive Daemon and Web Visualizer

The container runs as the non-root `otns` user and binds to `0.0.0.0:8999` inside the container so Docker port forwarding works. Publish port `8999` on host loopback only (`127.0.0.1:8999:8999`), then open `http://127.0.0.1:8999/`:

```bash
docker run --rm -it -p 127.0.0.1:8999:8999 openthread/otns-v3
```

To persist PCAP, KPI, replay, and topology outputs in a host directory, pre-create the directory so it is writable by the container user (or pass `--user "$(id -u):$(id -g)"`):

```bash
mkdir -p "$(pwd)/otns_out"
docker run --rm -it \
  --user "$(id -u):$(id -g)" \
  -p 127.0.0.1:8999:8999 \
  -v "$(pwd)/otns_out:/data" \
  openthread/otns-v3
```

If you pass custom `otns` flags after the image name, they replace the default `CMD`. Include `--bind 0.0.0.0 --output-dir /data` so host port forwarding and `/data` persistence keep working.

## Run Python SDK Examples in Container

```bash
docker run --rm --entrypoint python3 openthread/otns-v3 \
  /opt/otns/python/examples/simple_mesh.py
```
