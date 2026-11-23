# Windows build: x64 Native Tools Command Prompt for VS 2022

**Source-only experimental package.** Windows compilation and in-game behavior are not verified here. This project targets **Skyrim 1.7.104** and FSMP builds whose **matching local PDB** proves the required private ABI layout and Bullet 3.24.0 compatibility. It does not promise universal compatibility with every future FSMP build. Consult `README.md` before installing.

## Prerequisites

- Visual Studio 2022 with C++ Desktop Development workload and MSVC x64 tools.
- CMake, Ninja, vcpkg with `x64-windows-static-md` triplet.
- Your own checked-out **CommonLibSSE-NG** (ideally recent alandtse `ng`) e.g. `D:\Modding\CommonLibSSE-NG`.
- Open Shaders revision-4 build-9 and a compatible **hdtSMP64.dll + hdtsmp64.pdb** pair at runtime, **not at build time**.

There is **no need** to clone the FSMP, Open Shaders, or GPUSMPPhysicsNG repositories. Our ZIP includes the required API headers and ported mathematics. Bullet is a **header dependency**; CMake does not link a separate Bullet runtime. It is important to use headers and compiler flags compatible with your installed FSMP. DaymareOn `dev` uses a customized Bullet 3.25 overlay with `BT_USE_SSE_IN_API`.

## In x64 Native Tools Command Prompt for VS 2022 (CMD, not PowerShell)

Unpack the ZIP in `D:\Downloads\OpenShadersFSMPWind`. Adjust paths to your installed folders:

```bat
cd /d D:\Downloads\OpenShadersFSMPWind
set "COMMONLIB_DIR=D:\Modding\CommonLibSSE-NG"
set "VCPKG_DIR=D:\Modding\vcpkg"
where cl
where ninja
where cmake
```

Configure (using your own CommonLib checkout, not downloading another one):

```bat
cmake -S . -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCommonLibSSEPath_NG="%COMMONLIB_DIR%" ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_DIR%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
```

Build:

```bat
cmake --build build --parallel 4
```

Stage install files:

```bat
cmake --install build --prefix "%CD%\staging"
dir staging\SKSE\Plugins\
```

This project intentionally does not install anything directly into Skyrim, modify any FSMP/Open Shaders DLL, or alter your existing source dependency folders. CommonLib's source checkout may still be **compiled again inside this project's build folder**, unless its own prebuilt matching library mechanism is configured.

**Expected output (after a successful Ninja build):**

```text
build\SKSE\Plugins\OpenShadersFSMPWind.dll
staging\SKSE\Plugins\OpenShadersFSMPWind.dll
staging\SKSE\Plugins\OpenShadersFSMPWind.ini
```

## Visual Studio solution generator alternative

```bat
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64 ^
  -DCommonLibSSEPath_NG="%COMMONLIB_DIR%" ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_DIR%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build-vs --config Release --parallel 4
cmake --install build-vs --config Release --prefix "%CD%\staging"
```

## Bullet header override (optional)

To supply your own **FSMP-compatible** Bullet headers, add:

```bat
  -DBULLET_INCLUDE_PATH="D:\Modding\BulletCompatible\include\bullet"
```

Use that option in the configure invocation above (with a continuation `^` on the previous line). If vcpkg headers exist, the source CMake's fallback takes them. **Do not link another Bullet simulation runtime** just to use `btRigidBody` methods.

## Portable C++ tests (independent of CommonLib and SKSE)

```bat
cl /nologo /std:c++latest /EHsc /Isrc src\WindMath.cpp tests\WindMathTest.cpp /Fe:WindMathTest.exe
WindMathTest.exe
cl /nologo /std:c++latest /EHsc /Isrc src\WindMath.cpp src\WindArea.cpp src\WindShelter.cpp tests\WindAreaTest.cpp /Fe:WindAreaTest.exe
WindAreaTest.exe
```

These test force math, 4-weight triangle distribution, half-float decoding, area clamps, and shelter weights. They **do not prove** the Windows DLL builds or works in Skyrim.

## Installation and first-test checklist

1. Back up your Skyrim installation/modlist/save. Place the PDB produced by the **same FSMP compilation** beside the installed `hdtSMP64.dll`, named `hdtsmp64.pdb`. This plugin no longer requires a particular build/GUID. It verifies the pair and layouts on startup. DLL FileVersion alone is **not** an ABI check.
2. Ensure Open Shaders exposes revision-4 build-9 wind sampling. No GPUSMPPhysicsNG dependency or ESP required.
3. Turn **FSMP's native wind OFF** (`wind.enabled=false` in its `userConfigs.json`, or via its menu). This prevents ambient double force.
4. Copy `staging\SKSE\Plugins\OpenShadersFSMPWind.dll` and `.ini` into the game Data `SKSE\Plugins` folder or the mod manager mod root.
5. Launch the game, move near an actor with SMP hair/cloth. Check the SKSE plugin log (`OpenShadersFSMPWind.log`) for FSMP and Open Shaders connections, "systems", "nodeMatched", "worldBodies", "NIFarea" and "nonzeroForces" counts. Missing NIF CPU buffers or unmatched nodes cause the strict default to apply zero additional wind; only enable `AllowUnmeasuredAreaFallback=1` to test those cases.
6. Test in wind/explosions/shouts and behind a wall; observe no movement when `<wind-factor>0</wind-factor>` applies. Test equip/unequip/reload/reset transitions and monitor for crashes.

## Troubleshooting

- **CMake cannot find CommonLib:** `CommonLibSSEPath_NG` must point to the directory with its `CMakeLists.txt`; it is not the general `D:\Modding` parent.
- **Compiler cannot find Bullet:** Check the vcpkg manifest bootstrap and correct `-DBULLET_INCLUDE_PATH`. DaymareOn's patched Bullet 3.25 overlay is the ideal header source.
- **Build error in CommonLib:** Your selected CommonLib checkout may require a different vcpkg baseline/package versions. Use its own matching dependency setup; this project does not force-update it.
- **`hdtSMP64.dll` ABI/PDB mismatch:** This project's adapter is **matching-PDB/layout-gated**, not version-gated. The log identifies the missing file, mismatch, field, Bullet ABI, or getter. Never bypass an ABI failure.
- **No wind / no NIF area:** Mesh might lack retained CPU vertex/index data, actor/node association may be ambiguous, or its owning reference's Havok world could be unavailable. Check log. Set `AllowUnmeasuredAreaFallback=1` only as a diagnostic/quality tradeoff.
- **Cannot find SKSE plugin DLL:** Skyrim 1.7.104, matching SKSE64 and Address Library required. Check the game's SKSE logs.
- **Runtime crash:** Stop using the plugin and keep the logs; the FSMP private ABI mapping and Bullet header compatibility require validation against the *exact* installed DLL before production use.

## v0.3.1 matching-PDB requirements

Copy the PDB built alongside **your currently installed** `hdtsmp64.dll` into
the same directory, named `hdtsmp64.pdb`. The plugin verifies the loaded
binary's RSDS GUID/age against that PDB and checks the required private FSMP
and Bullet layouts dynamically. There is **no version/GUID allowlist**. Function
addresses, including `SkyrimPhysicsWorld::get()`, are resolved from the matching
PDB. See `PDB-COMPATIBILITY.md` for precise compatibility checks and errors.

The two DLL/PDB pairs previously supplied have matching RSDS records and
expected layout data. A future build that preserves the checked ABI may be
accepted without recompiling this plugin; incompatible updates fail closed.

The built `OpenShadersFSMPWind.dll` uses Windows' `DbgHelp.dll` and `version.dll`;
no FSMP or BannerPhysics source rebuild is needed.

Portable new test:

```bat
cl /nologo /std:c++latest /EHsc /Isrc tests\WindComponentsTest.cpp /Fe:WindComponentsTest.exe
WindComponentsTest.exe
```

**Note:** If you disable native FSMP wind to avoid duplicate wind,
BannerPhysics's native-wind-dependent twisting is also disabled; the plugin
does not implement BannerPhysics twisting itself.
