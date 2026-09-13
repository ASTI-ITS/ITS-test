# ESP-IDF CI with Docker Compose

This repository demonstrates an automated Continuous Integration (CI) pipeline for ESP-IDF firmware projects using **Docker Compose** and **GitHub Actions**.

---

## 📁 Repository Structure

```text
ITS-test/
├── .github/
│   └── workflows/
│       └── ci.yml               # GitHub Actions CI workflow
├── docker-compose.ci.yml        # Docker Compose CI build service
├── Makefile
├── esp-idf-projects/            #Project Files
│   ├── hello_world/
│   ├── rtos_hq/
│   ├── rtos_task_test/
│   └── sample-project/
├── .gitignore
└── README.md
```

---
## 🚀 Build Instructions

### Makefile Option: Build docker with MakeFile Command

sdkconfig must be set first to specified chip or else flash and monitor wont execute 

(`CONFIG_IDF_TARGET_ESP32=y and CONFIG_IDF_TARGET="esp32"`):

```bash
# Build, flash, and  monitor rtos_hq project
make bd_rtos_hq: --abort-on-container-exit --exit-code-from idf-builder

# Or run directly from sample-project/
make bd_rtos_task_test --abort-on-container-exit --exit-code-from idf-builder

# Specify a different target chip (e.g., esp32, esp32c3)
make bd_hello_world --abort-on-container-exit --exit-code-from idf-builder

# Specify a different target chip (e.g., esp32, esp32c3)
make bd_sample-project --abort-on-container-exit --exit-code-from idf-builder
```

## 🚀 Build Instructions

### Option 1: Build with Docker Compose (No Local Toolchain Required)

Build the firmware inside an isolated Espressif Docker container (`espressif/idf:v5.3`):

```bash
# Default build for ESP32-S3
docker compose -f docker-compose.ci.yml up --abort-on-container-exit --exit-code-from idf-builder

# Or run directly from sample-project/
cd sample-project
docker compose -f ../docker-compose.ci.yml up --abort-on-container-exit --exit-code-from idf-builder

# Specify a different target chip (e.g., esp32, esp32c3)
IDF_TARGET=esp32 docker compose -f docker-compose.ci.yml up --abort-on-container-exit --exit-code-from idf-builder
```

### Option 2: Native Local Build (ESP-IDF Installed)

If you have ESP-IDF installed locally on your host or WSL:

```bash
cd sample-project

# Activate ESP-IDF environment (adjust path if needed)
. $IDF_PATH/export.sh

# Set target chip (matches CI default: esp32s3)
idf.py set-target esp32s3

# Build application
idf.py build

# Flash and monitor (replace /dev/ttyUSB0 with your serial port)
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## 📦 Build Artifacts

Compiled binaries are generated in `./sample-project/build/`:
- `sample-project/build/bootloader/bootloader.bin`
- `sample-project/build/partition_table/partition-table.bin`
- `sample-project/build/sample-project.bin`
- `sample-project/build/flasher_args.json`

---

## 🛠️ GitHub Actions CI

The workflow configured in [`.github/workflows/ci.yml`](.github/workflows/ci.yml):
1. Runs on an `ubuntu-22.04` runner.
2. Utilizes `actions/cache` for `.ccache` compiler caching across commits.
3. Builds the firmware in an isolated container environment using `docker-compose.ci.yml`.
4. Validates compilation and verifies that all `.bin` firmware artifacts are generated.
5. Uploads the firmware binaries as downloadable build artifacts (`esp-idf-firmware-esp32s3-<sha>`).
6. Cleans up containers and volumes automatically upon completion.

---

## 📝 Commit Style

Use the following format when writing commit messages:

```text
type(scope?): subject
```

- `scope` is optional
- Multiple scopes supported (use `,` as delimiter)

### Example

```text
feature(credentials): add VC issuance endpoint
fix(mqtt,websocket): resolve disconnection on idle timeout
refactor(models): normalize Trip field naming
```

### Types

| Type       | Description                                |
| ---------- | ------------------------------------------ |
| `feature`  | New feature or functionality               |
| `fix`      | Bug fix                                    |
| `refactor` | Code restructuring without behavior change |
| `enhance`  | Improvement to existing feature            |
| `debug`    | Debugging-related changes                  |
| `test`     | Adding or updating tests                   |
| `upgrade`  | Dependency or version upgrades             |

