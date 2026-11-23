# Upstream source / licenses

- **DaymareOn/hdtSMP64**, `dev`, `src/PluginAPI.h`, and inspected `src/hdtSkinnedMesh/hdtSkinnedMeshBone.{h,cpp}`, `src/hdtSkyrimBone.cpp`: FSMP public plugin API and the explicitly version-gated 3.0.0 ABI adapter. GPL-3.0. https://github.com/DaymareOn/hdtSMP64
- **alandtse/open-shaders**, `dev`, `include/VRAPI/CSinterface001.h`: public revision-4 wind API contract (locally mirrored). LGPL-3.0-or-later. https://github.com/alandtse/open-shaders
- **alandtse/GPUSMPPhysicsNG**, `main`: `shaders/Common/HairWind.hlsli` wind drag, `src/smp/WindDrag.cpp` NIF area distribution, `src/Utils/SkinVertexBlock.h` packed skin half-floats, `src/Features/Wind/WindShelter.{h,cpp}` and `WindShelterQuery.cpp`, `src/Utils/ActorUtils.cpp` world raycast. Ported C++ is GPL-3.0-or-later WITH LicenseRef-Modding-Exception per upstream source file headers. https://github.com/alandtse/GPUSMPPhysicsNG
- **Bullet Physics**: API headers from a compatible user-supplied build or vcpkg; upstream zlib license. https://github.com/bulletphysics/bullet3

All modified source and build files for this project are provided in this source ZIP. Reuse/distribution must comply with applicable GPL/LGPL requirements and notices.
