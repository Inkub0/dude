# DUDE — Doom3 Unified Development Engine

# Project Overview
**DUDE** is a fork of [dhewm3](https://github.com/dhewm/dhewm3) (itself a GPL source
port of _DOOM 3_) that modernizes the renderer while keeping the classic _DOOM 3_ look
and gameplay faithful. Work in progress:

- A modernized **OpenGL 3.3 core** renderer and a **Vulkan** renderer (1.1 baseline for
  wide hardware support, plus a modern `vulkan-rt` profile targeting ray tracing),
  selectable via the `r_graphicsAPI` cvar.
- ARB assembly shaders translated to a **shared GLSL source tree** (GL + SPIR-V).
- Opt-in enhancements kept strictly separate from the faithful defaults (an
  "Improvements over the classic engine" menu section).

The full plan lives in [docs/vulkan-port.md](docs/vulkan-port.md). DUDE is GPLv3, like
its base — see COPYING.txt. It runs on the same classic _DOOM 3_ / _RoE_ game data.

**License note:** DUDE is a modified version of [dhewm3](https://github.com/dhewm/dhewm3),
based on id Software's Doom 3 GPL source release. All DUDE-authored source in this
repository — including every file under `neo/shaders/` and files without an explicit
license header — is licensed under the GNU GPLv3 (see COPYING.txt). Third-party
components (Dear ImGui, VMA, FSR2, SMAA, stb) keep their original licenses, stated in
their files. **No game assets are included or redistributable here**: the `base/*.pk4`
game data is proprietary id/Bethesda content — bring your own Doom 3 installation.

## Key Technologies
- C++ for core engine components
- OpenGL 3.x for graphics rendering
- Vulkan for alternative graphics API support
- CMake for build system
- Various third-party libraries (ImGui, miniz, etc.)

## Project Structure
```
.
├── .continue/              # Continue-specific configuration
├── base/                   # Base game assets and configurations
├── build.sh                # Build script
├── docs/                   # Documentation files
├── neo/                    # Main engine source code
│   ├── CMakeLists.txt      # CMake build configuration
│   ├── framework/          # Core framework components
│   ├── game/               # Game logic and entities
│   ├── idlib/              # Utility libraries
│   ├── renderer/           # Rendering system
│   ├── shaders/            # Shader files
│   └── sys/                # System-specific code
├── scripts/                # Utility scripts
└── *.md                    # Documentation files
```

## Getting Started

### Prerequisites
- C++ compiler (C++17 or later)
- CMake 3.10 or higher
- OpenGL 3.x compatible graphics hardware
- Vulkan SDK (for Vulkan support)
- SDL2 or similar windowing system

### Installation Instructions
1. Clone the repository
2. Run `./build.sh` to build the project
3. Configure using CMake
4. Build using your preferred build system

## Development Workflow

### Coding Standards
- C++17 compliant code
- Follow id Tech 4 coding conventions
- Modular design with clear separation of concerns
- Memory management practices consistent with engine architecture

### Testing Approach
- Unit testing for core components
- Integration testing for rendering and game systems
- Performance testing for graphics subsystems
- Cross-platform compatibility testing

## Key Concepts

### Domain-Specific Terminology
- **pk4**: Game data archive files (similar to .pk3 in Quake 3)
- **Renderer**: Graphics subsystem handling rendering pipeline
- **Framework**: Core engine components (memory, math, file I/O)
- **Game**: Game logic and entity definitions
- **Shader**: Graphics program for rendering effects

### Core Abstractions
- Entity-component system for game objects
- Modular renderer architecture supporting multiple backends
- Resource management system for assets
- Configuration system for engine settings

## Common Tasks

### Building the Project
1. Ensure dependencies are installed
2. Run `./build.sh` or `cmake . && make`
3. Verify build completes successfully

### Debugging Issues
- Use logging system for debugging information
- Utilize debugger to step through engine components
- Check configuration files for incorrect settings
- Review documentation for known issues

## Troubleshooting

### Common Issues and Solutions
- **Build failures**: Ensure all dependencies are installed and CMake is configured properly
- **Renderer issues**: Verify graphics hardware compatibility and driver versions
- **Asset loading problems**: Check pk4 archive integrity and file paths
- **Performance issues**: Profile rendering pipeline and optimize shaders

# Upstream: dhewm3

DUDE is built on **dhewm3**; everything documented there (game data setup, original
compile instructions, mod support, FAQ) applies to DUDE's faithful baseline too:

- Project + README: https://github.com/dhewm/dhewm3
- Homepage: https://dhewm3.org — mods: https://dhewm3.org/mods.html
- FAQ: https://github.com/dhewm/dhewm3/wiki/FAQ

DUDE-specific documentation lives in this repo under [docs/](docs/) —
[port architecture](docs/port-architecture.md), [Vulkan backend](docs/vulkan-backend.md),
and per-feature plans.
