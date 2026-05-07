# Proto Files for HTTP Cache

This folder defines protobuf schema used by the cache module.

## Generate C++ source

Run from repository root:

```bash
protoc \
  --cpp_out=src/net/http \
  --proto_path=proto \
  proto/http_cache.proto
```

Generated files:
- `src/net/http/http_cache.pb.h`
- `src/net/http/http_cache.pb.cc`

Then add `src/net/http/http_cache.pb.cc` into `hpserver_lib` sources in `CMakeLists.txt`.
