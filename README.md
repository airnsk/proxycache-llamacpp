# llama.cpp

![llama](https://raw.githubusercontent.com/ggml-org/llama.brand/refs/heads/master/cover/llama-cpp/cover-llama-cpp-dark.svg)

<div align="center">

<b>LLM inference in C/C++</b>

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Release](https://img.shields.io/github/v/release/ggml-org/llama.cpp?filter=v*&color=brightgreen)](https://github.com/ggml-org/llama.cpp/releases?q=tag:v0)
[![Nightly](https://img.shields.io/github/v/release/ggml-org/llama.cpp?label=nightly&filter=b*&color=orange)](https://github.com/ggml-org/llama.cpp/releases?q=b)
[![Server](https://img.shields.io/github/actions/workflow/status/ggml-org/llama.cpp/server.yml?label=Server)](https://github.com/ggml-org/llama.cpp/actions/workflows/server.yml)
[![Docker](https://img.shields.io/github/actions/workflow/status/ggml-org/llama.cpp/docker.yml?label=Docker)](https://github.com/ggml-org/llama.cpp/actions/workflows/docker.yml)
[![Winget](https://img.shields.io/github/actions/workflow/status/ggml-org/llama.cpp/winget.yml?label=Winget)](https://github.com/ggml-org/llama.cpp/actions/workflows/winget.yml)

[ggml](https://github.com/ggml-org/ggml) / [ops](https://github.com/ggml-org/llama.cpp/blob/master/docs/ops.md) / [maintainer PRs](https://github.com/ggml-org/llama.cpp/issues?q=is%3Apr%20is%3Aopen%20draft%3AFalse%20(author%3Argerganov%20OR%20author%3AKitaitiMakoto%20OR%20author%3Adanbev%20OR%20author%3Aaldehir%20OR%20author%3Amax-krasnyansky%20OR%20author%3ACISC%20OR%20author%3Aggerganov%20OR%20author%3Aam17an%20OR%20author%3Ajhen0409%20OR%20author%3Abartowski1182%20OR%20author%3Anikwen%20OR%20author%3Ahipudding%20OR%20author%3Aravi9%20OR%20author%3AServeurpersoCom%20OR%20author%3Apwilkin%20OR%20author%3Areeselevine%20OR%20author%3Angxson%20OR%20author%3Ajeffbolznv%20OR%20author%3Amarty1885%20OR%20author%3A0cc4m%20OR%20author%3ATitaniumtown%20OR%20author%3Aangt%20OR%20author%3AIMbackK%20OR%20author%3Aarthw%20OR%20author%3AJohannesGaessler%20OR%20author%3AORippler%20OR%20author%3Aruixiang63%20OR%20author%3Axctan%20OR%20author%3Aallozaur%20OR%20author%3Ayomaytk%20OR%20author%3Aaendk%20OR%20author%3Awine99%20OR%20author%3Agaugarg-nv%20OR%20author%3Ataronaeo%20OR%20author%3Aforforever73%20OR%20author%3Alhez%20OR%20author%3Anetrunnereve%20OR%20author%3Afairydreaming)%20sort%3Aupdated-desc) / [dev stats](https://github.com/ggml-org/llama.cpp-dev) / [lib llama API](https://github.com/ggml-org/llama.cpp/issues/9289) / [llama-server REST API](https://github.com/ggml-org/llama.cpp/issues/9291)

</div>

> **This fork** adds a persistent prefix/KV-state cache to `llama-server`: the in-RAM prompt cache
> stays the first level, and a new on-disk level keeps serialized sequence states — including the
> draft/speculative state — across server restarts, so a long prompt that was prefilled once is
> restored from the disk instead of being recomputed. Nothing external is involved: no proxy, no
> sidecar. Without `--cache-disk` the server behaves exactly as upstream.
>
> Details: [DISK-CACHE.md](DISK-CACHE.md) and [docs/persistent-disk-cache/](docs/persistent-disk-cache/).

## Disk cache: the flags

| flag | default | meaning |
| --- | --- | --- |
| `--cache-disk PATH` | disabled | Root directory of the persistent cache. This is the only flag needed to switch the disk level on; the root is shared by all namespaces. |
| `--cache-disk-size N` | 500 GiB | Maximum total size of the cache root, in GiB (`K/M/G/T` suffixes are accepted, a bare number is GiB). `0` means **no limit**, not "off". The limit covers the whole root, but a namespace only measures and evicts its own files. |
| `--cache-disk-min-tokens N` | 1024 | Prompts shorter than this are not stored: a state costs disk space and a write regardless of its length, so a short prompt never pays for itself. `0` stores everything. |
| `--cache-disk-read-mbps N` | 200 | Assumed sequential read speed of the cache disk in decimal MB/s (1 MB = 1000000 bytes); used by the disk-vs-RAM cost model. |
| `--cache-disk-min-gain-ms N` | 1000 | Minimum expected gain in milliseconds for a state to be worth reading from disk; below it the resident state is kept instead. |

Each flag has an environment variable too: `LLAMA_ARG_CACHE_DISK`, `LLAMA_ARG_CACHE_DISK_SIZE`,
`LLAMA_ARG_CACHE_DISK_MIN_TOKENS`, `LLAMA_ARG_CACHE_DISK_READ_MBPS`,
`LLAMA_ARG_CACHE_DISK_MIN_GAIN_MS`.

The disk level sits under the in-RAM cache and cooperates with it: `--cache-ram` (default 8192 MiB)
still holds the first level, `--cache-idle-slots` (default enabled, requires `--cache-ram`) lets an
idle slot be saved, and the disk level is what makes those states survive a restart.

```
llama-server -m model.gguf -ngl 99 -fa on -c 160000 \
  --cache-ram 32768 \
  --cache-disk /mnt/cache --cache-disk-size 500 --cache-disk-min-tokens 1024 \
  --cache-idle-slots --metrics
```

## Disk cache: where and how it is stored

Everything lives under the root given to `--cache-disk`, one directory per namespace:

```
<cache root>/<model-slug>-<fp12>/
    manifest.json      namespace descriptor: the fingerprint and every component it was built from
    index.bin          compact binary index of the entries (little-endian, crc32-protected header)
    states/<id>.meta   self-describing header + the exact token array of that entry
    states/<id>.bin    the payload: [target seq-state][dft][ckpt][spec]
    states/*.tmp       incomplete writes; ignored and deleted when the namespace is opened
```

- `<model-slug>` is the slugified `general.name` from the GGUF (the model file name if that key is
  absent); `<fp12>` is the first 12 hex digits of a SHA-256 over the components below.
- `states/<id>.meta` carries the exact token array of the entry, which is what the lookup uses: the
  entry with the longest common prefix with the request wins, and the surplus is dropped by the
  ordinary slot logic once the state is loaded. A saved state covers the prompt *and* the tokens
  generated after it, so a repeat of that prompt is a hit even though the stored state is longer.
- `states/<id>.bin` is the sequence state streamed in sections: the target state, then the draft
  state (MTP/speculative), the checkpoints, and the speculative state. Writes go to `*.tmp` and are
  renamed into place, so a kill in the middle leaves no half-written entry behind.

## Disk cache: why there can be several directories

The directory name is the identity of the *state layout*, not just of the model, so a new directory
appears whenever anything that shapes the bytes of a stored state changes:

- the container format: `state_seq` magic/version and the disk-cache format version;
- the model: architecture, name, file type, quantization version, size label, and the hparams that
  shape the state (block count, `n_embd`, `n_head`, `n_head_kv`);
- the KV configuration of the context: `-ctk/--cache-type-k`, `-ctv/--cache-type-v`, `-fa`,
  unified KV, the SWA setting;
- checkpointing and speculation settings, including `--spec-type`;
- the LoRA adapters in use.

So two different models always get two directories — and the *same* model does too, if the KV type,
the `--spec-type` or the adapters differ. Their states are not interchangeable, and the cache says
so instead of loading a state that does not fit. Nothing is lost by that: switching back to the
previous flags reuses the old directory, and a namespace only ever evicts its own files, so it can
never throw away another model's cache.

## Quick start

A few options to get `llama.cpp` installed on your machine:

- Visit https://llama.app and follow the instructions
- Run with Docker - see our [Docker documentation](docs/docker.md)
- Download pre-built binaries from the [releases page](https://github.com/ggml-org/llama.cpp/releases)
- Build from source by cloning this repository - check out [our build guide](docs/build.md)

Once installed:

```sh
# Download and run a model directly from Hugging Face
llama cli -hf ggml-org/Qwen3.5-0.8B-GGUF

# Launch OpenAI-compatible API server
llama serve -hf ggml-org/Qwen3.5-0.8B-GGUF
```

<table align="center">
    <tr>
        <td align="center" width=50%>
            <img width="1310" height="888" alt="VLM session with `llama cli`" src="https://github.com/user-attachments/assets/88726b48-1713-48aa-a525-95a02e78afc4" />
            <i>VLM session with <b>llama cli</b></i>
        </td>
        <td align="center">
            <img width="1392" height="958" alt="Built-in web UI against `llama serve` running Qwen 3.6" src="https://github.com/user-attachments/assets/b402f972-2e32-4def-8771-8d849f08cf2e" />
            <i>Built-in web UI against <b>llama serve</b></i>
        </td>
    </tr>
<table>

## Description

The main goal of `llama.cpp` is to enable LLM (and VLM) inference with minimal setup and state-of-the-art performance on
a wide range of hardware - locally and in the cloud.

- Plain C/C++ implementation without any dependencies
- Apple silicon is a first-class citizen - optimized via ARM NEON, Accelerate and Metal frameworks
- AVX, AVX2, AVX512 and AMX support for x86 architectures
- RVV, ZVFH, ZFH, ZICBOP and ZIHINTPAUSE support for RISC-V architectures
- 1.5-bit, 2-bit, 3-bit, 4-bit, 5-bit, 6-bit, and 8-bit integer quantization for faster inference and reduced memory use
- Custom CUDA kernels for running LLMs on NVIDIA GPUs (support for AMD GPUs via HIP and Moore Threads GPUs via MUSA)
- Vulkan and SYCL backend support
- CPU+GPU hybrid inference to partially accelerate models larger than the total VRAM capacity

The `llama.cpp` project is build on top of the [ggml](https://github.com/ggml-org/ggml) library.

## Supported backends

| Backend | Target devices |
| --- | --- |
| [BLAS](docs/build.md#blas-build) | All |
| [BLIS](docs/backend/BLIS.md) | All |
| [CANN](docs/build.md#cann) | Ascend NPU |
| [CUDA](docs/build.md#cuda) | Nvidia GPU |
| [HIP](docs/build.md#hip) | AMD GPU |
| [Hexagon](docs/backend/snapdragon/README.md) | Snapdragon |
| [IBM zDNN](docs/backend/zDNN.md) | IBM Z & LinuxONE |
| [MUSA](docs/build.md#musa) | Moore Threads GPU |
| [Metal](docs/build.md#metal-build) | Apple Silicon |
| [OpenCL](docs/backend/OPENCL.md) | Adreno GPU |
| [OpenVINO [In Progress]](docs/backend/OPENVINO.md) | Intel CPUs, GPUs, and NPUs |
| [RPC](https://github.com/ggml-org/llama.cpp/tree/master/tools/rpc) | All |
| [SYCL](docs/backend/SYCL.md) | Intel GPU |
| [VirtGPU](docs/backend/VirtGPU.md) | VirtGPU APIR |
| [Vulkan](docs/build.md#vulkan) | GPU |
| [WebGPU](docs/build.md#webgpu) | All |
| [ZenDNN](docs/build.md#zendnn) | AMD CPU |

## Documentation

#### Tools

- [cli](tools/cli/README.md)
- [completion](tools/completion/README.md)
- [server](tools/server/README.md)
- [GBNF grammars](grammars/README.md)

#### Development

- [How to build](docs/build.md)
- [Running on Docker](docs/docker.md)
- [Build on Android](docs/android.md)
- [Multi-GPU usage](docs/multi-gpu.md)
- [Performance troubleshooting](docs/development/token_generation_performance_tips.md)
- [GGML tips & tricks](https://github.com/ggml-org/llama.cpp/wiki/GGML-Tips-&-Tricks)
- [XCFramework](docs/xcframework.md)
- [Completions](docs/completions.md)
- [Models](docs/models.md)
- [Release process](docs/release.md)

## Contributing

- Contributors can open PRs
- Collaborators will be invited based on contributions
- Maintainers can push to branches in the `llama.cpp` repo and merge PRs into the `master` branch
- Any help with managing issues, PRs and projects is very appreciated!
- Read the [CONTRIBUTING.md](CONTRIBUTING.md) for more information

## Acknowledgements

- [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) - Single-header HTTP server, used by `llama-server` - MIT license
- [nothings/stb](https://github.com/nothings/stb) - Single-header image format decoder, used by multimodal subsystem - Public domain
- [nlohmann/json](https://github.com/nlohmann/json) - Single-header JSON library, used by various tools/examples - MIT License
- [mackron/miniaudio](https://github.com/mackron/miniaudio) - Single-header audio format decoder, used by multimodal subsystem - Public domain
- [sheredom/subprocess.h](https://github.com/sheredom/subprocess.h) - Single-header process launching solution for C and C++ - Public domain
