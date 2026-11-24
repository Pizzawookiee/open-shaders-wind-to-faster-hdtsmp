Applies Open Shaders' published wind to Faster HDT-SMP Bullet bodies, with NIF
area-weighted aerodynamic drag, a rig-specific anchor, and independent ambient
and transient Havok shelter. Intended for Skyrim **1.7.104**, Open Shaders wind API revision 4/build 9,
FSMP public API 2.x and Bullet **3.24.0** with SSE-aligned headers.

Compared to the Open Shaders wind in the GPU-SMP repo https://github.com/alandtse/GPUSMPPhysicsNG this does not run on GPU and is compatible with latest versions of Faster HDT-SMP, Banners HDT-SMP and Settle.

Compatible with FSMP Clipping Fix, but Banners HDT-SMP doesn't work with that mod currently.

Also works with latest Improved Camera beta.

Don't use with GPU-SMP as that has its own open shaders wind.

## Building

Follow `BUILD-WINDOWS.md` then install the resulting DLL and INI. Disable logging options in INI to avoid large log file.

Avoid **double ambient wind**: normally turn FSMP's native wind off.
