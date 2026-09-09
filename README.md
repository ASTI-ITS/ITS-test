# ESP-IDF CI with Docker Compose

This repository demonstrates an automated Continuous Integration (CI) pipeline for ESP-IDF firmware projects using **Docker Compose** and **GitHub Actions**.

## 🚀 Quick Start (Local Build)

You can build the firmware locally using Docker without installing the ESP-IDF toolchain directly on your machine:

```bash
# Default build for ESP32
docker compose -f docker-compose.ci.yml up --abort-on-container-exit --exit-code-from idf-builder

# Or specify a different target (e.g., esp32s3, esp32c3)
IDF_TARGET=esp32s3 docker compose -f docker-compose.ci.yml up --abort-on-container-exit --exit-code-from idf-builder
```

Compiled binaries will be generated in `./build/`:
- `build/bootloader/bootloader.bin`
- `build/partition_table/partition-table.bin`
- `build/app-firmware.bin`
- `build/flasher_args.json`

## 🛠️ GitHub Actions CI

The workflow in `.github/workflows/ci.yml`:
1. Runs on pinned `ubuntu-22.04` runner.
2. Leverages `actions/cache` for `.ccache` compiler caching across commits.
3. Executes `docker-compose.ci.yml` in an isolated container environment.
4. Archives and uploads the compiled firmware binaries as downloadable build artifacts.
5. Cleans up containers and volumes automatically.
