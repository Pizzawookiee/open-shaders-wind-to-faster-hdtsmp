Applies Open Shaders' published wind to Faster HDT-SMP Bullet bodies, with NIF
area-weighted aerodynamic drag, a rig-specific anchor, and independent ambient
and transient Havok shelter. Intended for Skyrim **1.7.104**, Open Shaders wind API revision 4/build 9,
FSMP public API 2.x and Bullet **3.24.0** with SSE-aligned headers.

Compared to the Open Shaders wind in the GPU-SMP repo https://github.com/alandtse/GPUSMPPhysicsNG this does not run on GPU and is compatible with latest versions of Faster HDT-SMP, Banners HDT-SMP and Settle.

Also works with latest Improved Camera beta.

Not compatible with FSMP Clipping Fix yet, since as far as I know FSMP Clipping fix doesn't work with Banners HDT-SMP yet.

Not compatible with GPU-SMP.

## Building

Follow `BUILD-WINDOWS.md` then install the resulting DLL and INI. Disable logging options in INI to avoid large log file.

Avoid **double ambient wind**: normally turn FSMP's native wind off.

## FSMP startup diagnostics

The plugin logs a one-time warning to `OpenShadersFSMPWind.log` at the SKSE `DataLoaded` event if it has not received FSMP `MSG_STARTUP`. This is distinct from receiving a startup event but failing public or private ABI verification. A startup event carrying a null interface logs its own error.
