# Caustica Scene Pack Format (`.caustica`)

A single-file, self-contained binary bundle for a Caustica scene — the engine
equivalent of a `.blend` file: one artifact that carries the scene document and
every referenced asset (models, textures, materials, environment maps,
prefabs), ready to send to another machine.

## Layout

Little-endian throughout. All offsets are absolute byte offsets from the start
of the file.

```
+---------------------------+ offset 0
| Header (64 bytes)         |
+---------------------------+
| Payload blobs             |  each 16-byte aligned
|   entry 0 data            |
|   entry 1 data            |
|   ...                     |
+---------------------------+ indexOffset
| Central directory         |
|   entryCount: uint32      |
|   entries[]               |
+---------------------------+
| manifest.json payload     |  (also listed in the directory)
+---------------------------+
```

## Header (64 bytes)

| Field       | Type     | Value                                        |
| ----------- | -------- | -------------------------------------------- |
| magic       | char[8]  | `CAUSTICP`                                   |
| version     | uint32   | `1`                                          |
| flags       | uint32   | bit0: little-endian (always 1); bits 1-31: 0 |
| indexOffset | uint64   | offset of the central directory              |
| indexSize   | uint64   | byte size of the central directory           |
| dataOffset  | uint64   | offset of the first payload blob             |
| indexCrc32  | uint32   | CRC-32 (IEEE) of the central directory bytes |
| reserved    | uint8[28]| zero                                         |

## Central directory entry

```
pathLen      uint16   byte length of path
path         uint8[]  UTF-8, pack-root relative, '/' separators
type         uint8    PackEntryType (see below)
compression  uint8    0 = stored, 1 = LZ4 block
crc32        uint32   CRC-32 of the *uncompressed* payload
uncompSize   uint64
compSize     uint64
offset       uint64   absolute offset of the payload blob
reserved     uint8[7] zero
```

Packed size per entry: 2 + pathLen + 1 + 1 + 4 + 8 + 8 + 8 + 7.

### PackEntryType

| Value | Meaning                       |
| ----- | ----------------------------- |
| 0     | other / raw                   |
| 1     | scene JSON (`caustica.scene`) |
| 2     | model (glTF / GLB / OBJ / …)  |
| 3     | texture (DDS / PNG / …)       |
| 4     | material JSON (OpenPBR)       |
| 5     | environment map               |
| 6     | prefab JSON                   |
| 7     | manifest JSON                 |

## Compression

`compression = 1` payloads hold a single raw **LZ4 block** (not the framed
format) of `compSize` bytes that decompresses to exactly `uncompSize` bytes.
The engine contains a built-in block decoder, so no external dependency is
required at runtime. The Python packer uses `lz4.block` when available and
falls back to stored payloads otherwise.

## manifest.json

A regular directory entry (type 7) whose payload is a JSON document:

```json
{
  "format": "caustica.scene-pack",
  "version": 1,
  "generator": "pack_scene.py 1.0",
  "primaryScene": "scenes/kitchen/kitchen.scene.json",
  "entries": [{ "path": "...", "type": 3, "uncompSize": 123, "crc32": 456 }]
}
```

`primaryScene` is the pack-root-relative scene JSON the engine opens when the
`.caustica` file itself is passed as `--scene`.

## Runtime semantics

`PackFileSystem` (an `IFileSystem`) is mounted when a scene path ends in
`.caustica`. Reads resolve in order:

1. exact entry match on the normalized generic path,
2. case-insensitive entry match,
3. suffix match of the requested path against entries (lets absolute resolved
   media paths such as `<packRoot>/models/kitchen/kitchen.gltf` hit the entry
   `models/kitchen/kitchen.gltf`),
4. optional native fallback filesystem.

Reading the pack file's own path returns the primary scene JSON, so
`SceneManager`'s existing `Scene::load(path)` path works unchanged.

## Tooling

```
python support/python/pack_scene.py pack  <scene.json> [-o out.caustica] [--assets DIR] [--lz4]
python support/python/pack_scene.py inspect <file.caustica>
python support/python/pack_scene.py extract <file.caustica> [-o DIR]
```

`pack` walks the reference closure (prefab sources, material slot JSONs and
their texture paths, environment maps, glTF buffers/images) and writes the
pack. `inspect` validates magic, directory CRC, per-entry CRCs and sizes.
`extract` unpacks for diffing.
