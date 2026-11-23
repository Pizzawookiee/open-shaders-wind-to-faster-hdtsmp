# PDB-verified FSMP compatibility policy (v0.3.1)

This plugin **no longer requires one of two pinned FSMP DLL hashes or RSDS
GUIDs**. It accepts compatible FSMP updates without needing a source change
when ALL of the following checks succeed:

1. FSMP loads and supplies plugin interface major version 2 and Bullet version
   **3.24.0**. Bullet headers used to compile this wind DLL must match the
   binary layout (in particular `sizeof(btRigidBody) == 0x310`).
2. The installed DLL contains a unique CodeView `RSDS` record. The adjacent
   `hdtsmp64.pdb` is the **actual PDB DbgHelp loads**, and its GUID/age equals
   the **loaded DLL**'s RSDS GUID/age. Other matching PDB copies somewhere on
   the symbol path do not satisfy the adjacent-file requirement.
3. DbgHelp finds complete type information for the required physics classes.
   All referenced field offsets and sizes are checked: `m_systems`, `m_bones`,
   `m_world`, `m_skeleton`, `m_name`, `m_windFactor`, `m_rig`, and `m_node`.
   `btRigidBody`, `btCollisionObject`, `btVector3`, and `btTransform` retain
   exact expected sizes; important Bullet force/velocity/constraint offsets
   and FSMP base-class inheritance offsets are checked. Appending unrelated
   data to certain FSMP classes is allowed if the checked offsets remain intact.
4. The expected zero-argument static getter `SkyrimPhysicsWorld::get()` is
   uniquely resolved from symbols, and its RVA is in executable image memory.
   Function RVAs are *not* hardcoded.
5. During physics PreStep, the bridge checks that every candidate rigid body
   is accounted for in the authoritative `m_systems` collection, that each
   inline `btRigidBody` has its expected owner user-pointer, and that the
   owning system points back to the active world. A failed snapshot skips wind.

## Expected diagnostic messages

Successful startup:

```text
FSMP PDB-verified ABI compatible: FileVersion=..., RSDS=... / age ..., verified type layouts (including Bullet), getter RVA=0x...
FSMP system-level private ABI ready; subscribed to PreStep
```

Failure examples:

```text
FSMP private system ABI unavailable: Matching hdtsmp64.pdb must be installed beside hdtsmp64.dll
FSMP private system ABI unavailable: Loaded FSMP PDB RSDS GUID/age does not match the loaded DLL
FSMP private system ABI unavailable: FSMP PDB ABI layout mismatch: hdt::SkinnedMeshBone::m_rig
FSMP private system ABI unavailable: Cannot uniquely locate SkyrimPhysicsWorld::get in matching PDB ...
```

## Limitations

Matching PDB metadata + field layouts strongly reduces but **cannot eliminate**
private-ABI risk. A mod update can change semantics, synchronization, pointer
lifetimes or calling conventions without moving checked fields. The public
interface check and dynamic getter name help, but Windows compilation and
in-game testing on a future updated binary are still essential.

Only the ABI gate and diagnostics changed in v0.3.1: 0.5-second staggered
shelter, separate ambient/transient raycasts, native impulse default OFF,
world-object system detection and BannerPhysics isolation remain unchanged.
