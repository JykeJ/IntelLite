# Building IntelLite

IntelLite is an SKSE plugin for Skyrim Special Edition / Anniversary Edition. This build
produces `IntelLite.dll`, a native SKSE plugin that uses SKSE, Address Library, and the
SkyrimNet runtime.

## Prerequisites

- Visual Studio 2022 with the **Desktop development with C++** workload (MSVC v143).
- CMake >= 3.21.
- vcpkg: bootstrap it, then set the `VCPKG_ROOT` environment variable to your vcpkg
  folder (for example `C:\path\to\vcpkg`).
- git.

The `ninja` preset additionally needs Ninja (bundled with Visual Studio) and must be run
from a Visual Studio Developer Command Prompt. The `vs2022` preset needs no special
shell.

## Dependency: CommonLibSSE-NG

From the repo root:

```
git clone -b ng --recurse-submodules https://github.com/alandtse/CommonLibVR.git refs/CommonLibVR
```

The `openvr` submodule is required. You can clone it elsewhere and pass
`-DINTELLITE_COMMONLIB_DIR="<that path>"` to CMake instead.

## Build (presets)

Visual Studio 2022:

```
cmake --preset vs2022
cmake --build --preset vs2022
```

Ninja (from a Developer Command Prompt):

```
cmake --preset ninja
cmake --build --preset ninja
```

## Build (manual equivalent)

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build
```

Under PowerShell, set the environment variable first with `$env:VCPKG_ROOT = "C:\path\to\vcpkg"`.

## Output

`build/` (or `build/<preset>/`) contains `IntelLite.dll` plus a `.pdb` for local
debugging.

## Installing

Copy `IntelLite.dll` into `<game>/Data/SKSE/Plugins/` (or into an MO2 mod's
`SKSE/Plugins/`), together with the contents of the repo's `mod/` tree.

## Optional deploy

Configure with `-DINTELLITE_MOD_DIR="C:/path/to/mods/IntelLite"` (an MO2 mod folder
root):

```
cmake --preset vs2022 -DINTELLITE_MOD_DIR="C:/path/to/mods/IntelLite"
```

The build then copies the DLL (and its PDB when present) into `<mod>/SKSE/Plugins/` and
the repo's `mod/` tree into `<mod>/`. An empty value (the default) means no deploy.

## Note

`IntelLiteData.esp` (the records the plugin uses) ships with the mod release; it is
authored content, not produced by this build.
