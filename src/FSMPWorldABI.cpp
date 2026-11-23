// FSMP system ABI bridge: read-only access validated against the *installed*
// DLL and its exact matching, adjacent PDB. No fixed DLL hash, PDB GUID/RVA
// allowlist, guessed getter RVA, or FSMP hooks. Reject incompatible layouts.
#include "FSMPWorldABI.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <DbgHelp.h>
#include <filesystem>
#include <cstring>
#include <cwchar>
#include <limits>
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <cstdio>
#include <system_error>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <vector>

namespace osfsmp {
namespace {
constexpr std::uintptr_t kSystems = 0x228;
constexpr std::uintptr_t kSystemBones = 0x38;
constexpr std::uintptr_t kSystemWorld = 0x28;
constexpr std::uintptr_t kSystemRoot = 0x98;
constexpr std::uintptr_t kBoneRig = 0x30;
constexpr std::uintptr_t kBoneNode = 0x418;
constexpr unsigned kMaxSystems = 4096;
constexpr unsigned kMaxBonesPerSystem = 16384;
// DbgHelp SYMBOL_INFO::Tag uses CodeView SymTagEnum; SymTagFunction == 5.
// Not every Windows SDK exposes the SymTagFunction enumerator without cvconst.h.
constexpr ULONG kFunctionSymbolTag = 5;
// The matching user-supplied PDB exposes the getter as an S_PUB32 record.
// DbgHelp represents those as SymTagPublicSymbol (10), not SymTagFunction.
constexpr ULONG kPublicSymbolTag = 10;
constexpr std::string_view kWorldGetterDecorated = "?get@SkyrimPhysicsWorld@hdt@@SAPEAV12@XZ";
using WorldGetter = void* (*)();
WorldGetter g_getWorld = nullptr;
// Keep SEH outside functions owning C++ containers (MSVC C2712).
void* CallGetter(WorldGetter fn) noexcept {
    __try {return fn();}
    __except(EXCEPTION_EXECUTE_HANDLER) {return nullptr;}
}
void* SafeBodyUser(btRigidBody* b) noexcept {
    __try {return b->getUserPointer();}
    __except(EXCEPTION_EXECUTE_HANDLER) {return nullptr;}
}
struct VectorView { const std::uintptr_t* begin; const std::uintptr_t* end; const std::uintptr_t* capacity; };
static_assert(sizeof(VectorView)==24);

template<class T> bool Read(T& out,const void* from) noexcept {
    __try { std::memcpy(&out,from,sizeof(T)); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool ReadVector(const std::byte* base, std::size_t offset, VectorView& view, unsigned maxSize) noexcept {
    if(!Read(view,base+offset))return false;
    if(!view.begin && !view.end && !view.capacity)return true;
    if(!view.begin || !view.end || !view.capacity)return false;
    const auto begin=reinterpret_cast<std::uintptr_t>(view.begin);
    const auto end=reinterpret_cast<std::uintptr_t>(view.end);
    const auto cap=reinterpret_cast<std::uintptr_t>(view.capacity);
    return !(begin%alignof(void*) || end<begin || cap<end ||
             (end-begin)%sizeof(void*) || (end-begin)/sizeof(void*)>maxSize ||
             (cap-begin)/sizeof(void*)>maxSize*2ull);
}
// DbgHelp type enumeration can report both a forward declaration and an
// actual class definition. Always check the complete record, never the first
// matching name. An append-only increase in an FSMP class's size is allowed,
// but all fields that our bridge dereferences must stay at their verified
// offsets and have the expected sizes. Bullet itself must be *exactly* sized.
struct TypeRequirement {
    const char* name;
    const wchar_t* member;      // nullptr = size-only requirement
    ULONG offset;
    ULONG64 minimumSize;
    ULONG64 maximumSize;        // 0 = no upper bound for append-only changes
    ULONG64 memberSize;         // 0 = don't enforce member type size
    ULONG requiredTag{};        // e.g. 18 = SymTagBaseClass (inheritance)
    bool matched{};
};

bool CheckType(HANDLE session, DWORD64 base, ULONG typeId,
               const TypeRequirement& req) {
    ULONG64 total = 0;
    if (!SymGetTypeInfo(session, base, typeId, TI_GET_LENGTH, &total) ||
        total < req.minimumSize || (req.maximumSize && total != req.maximumSize))
        return false;
    if (!req.member) return true;
    ULONG count = 0;
    if (!SymGetTypeInfo(session, base, typeId, TI_GET_CHILDRENCOUNT, &count) ||
        !count || count > 2048) return false;
    const auto bytes = sizeof(TI_FINDCHILDREN_PARAMS) + sizeof(ULONG) * (count - 1);
    auto storage = std::make_unique<std::byte[]>(bytes);
    auto* children = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(storage.get());
    children->Count = count;
    children->Start = 0;
    if (!SymGetTypeInfo(session, base, typeId, TI_FINDCHILDREN, children)) return false;
    for (ULONG i = 0; i < count; ++i) {
        PWSTR name = nullptr;
        if (!SymGetTypeInfo(session, base, children->ChildId[i], TI_GET_SYMNAME, &name) || !name)
            continue;
        const bool equal = std::wcscmp(name, req.member) == 0;
        LocalFree(name);
        if (!equal) continue;
        if (req.requiredTag) {
            ULONG tag = 0;
            if (!SymGetTypeInfo(session, base, children->ChildId[i], TI_GET_SYMTAG, &tag) ||
                tag != req.requiredTag) return false;
        }
        ULONG offset = 0;
        if (!SymGetTypeInfo(session, base, children->ChildId[i], TI_GET_OFFSET, &offset) ||
            offset != req.offset) return false;
        if (!req.memberSize) return true;
        ULONG memberType = 0;
        ULONG64 size = 0;
        return SymGetTypeInfo(session, base, children->ChildId[i], TI_GET_TYPEID, &memberType) &&
               SymGetTypeInfo(session, base, memberType, TI_GET_LENGTH, &size) &&
               size == req.memberSize;
    }
    return false;
}

struct TypeChecker {
    HANDLE session;
    DWORD64 base;
    // Layout-dependent fields read by this bridge and by Plugin.cpp's
    // FSMPBoneVerifiedLayout prefix. FSMP classes may grow at the end, not
    // shift any checked fields or base classes. Bullet must be *exactly* sized
    // and retain key force, velocity and constraint member offsets.
    // Explicit element type and bound: MSVC does not allow CTAD for a non-static data member.
    std::array<TypeRequirement, 22> required{
        TypeRequirement{"btVector3", nullptr, 0, 0x10, 0x10, 0},
        TypeRequirement{"btTransform", nullptr, 0, 0x40, 0x40, 0},
        TypeRequirement{"btCollisionObject", nullptr, 0, 0x180, 0x180, 0},
        TypeRequirement{"btRigidBody", nullptr, 0, 0x310, 0x310, 0},
        TypeRequirement{"btRigidBody", L"btCollisionObject", 0, 0x310, 0x310, 0, 18},
        TypeRequirement{"btRigidBody", L"m_linearVelocity", 0x1B0, 0x310, 0x310, 0x10},
        TypeRequirement{"btRigidBody", L"m_inverseMass", 0x1D0, 0x310, 0x310, 4},
        TypeRequirement{"btRigidBody", L"m_totalForce", 0x220, 0x310, 0x310, 0x10},
        TypeRequirement{"btRigidBody", L"m_constraintRefs", 0x270, 0x310, 0x310, 0x20},
        TypeRequirement{"btRigidBody", L"m_invMass", 0x2D0, 0x310, 0x310, 0x10},
        TypeRequirement{"hdt::SkinnedMeshWorld", L"btDiscreteDynamicsWorldMt", 0, 0x290, 0, 0, 18},
        TypeRequirement{"hdt::SkinnedMeshWorld", L"m_systems", static_cast<ULONG>(kSystems), 0x290, 0, 24},
        TypeRequirement{"hdt::SkinnedMeshSystem", L"m_bones", static_cast<ULONG>(kSystemBones), 0x98, 0, 24},
        TypeRequirement{"hdt::SkinnedMeshSystem", L"m_world", static_cast<ULONG>(kSystemWorld), 0x98, 0, 8},
        TypeRequirement{"hdt::SkyrimSystem", L"hdt::SkinnedMeshSystem", 0, 0xC0, 0, 0, 18},
        TypeRequirement{"hdt::SkyrimSystem", L"m_skeleton", static_cast<ULONG>(kSystemRoot), 0xC0, 0, 8},
        TypeRequirement{"hdt::SkinnedMeshBone", L"m_name", 0x18, 0x410, 0, 8},
        TypeRequirement{"hdt::SkinnedMeshBone", L"m_windFactor", 0x2C, 0x410, 0, 4},
        TypeRequirement{"hdt::SkinnedMeshBone", L"m_rig", static_cast<ULONG>(kBoneRig), 0x410, 0, 0x310},
        TypeRequirement{"hdt::SkyrimBone", L"hdt::SkinnedMeshBone", 0, 0x430, 0, 0, 18},
        TypeRequirement{"hdt::SkyrimBone", L"m_node", static_cast<ULONG>(kBoneNode), 0x430, 0, 8},
        TypeRequirement{"hdt::SkyrimPhysicsWorld", L"hdt::SkinnedMeshWorld", 0, 0x400, 0, 0, 18},
    };
    bool AllMatched() const {
        return std::all_of(required.begin(), required.end(),
                           [](const auto& req) { return req.matched; });
    }
    std::string Missing() const {
        std::string missing;
        for (const auto& req : required) {
            if (req.matched) continue;
            if (!missing.empty()) missing += ", ";
            missing += req.name;
            if (req.member) {
                // The fields are known ASCII C++ identifiers.
                missing += "::";
                for (const wchar_t* q = req.member; *q; ++q)
                    missing += static_cast<char>(*q);
            }
        }
        return missing;
    }
};

BOOL CALLBACK VerifyPdbType(SYMBOL_INFO* sym, ULONG, void* context) {
    auto* checker = static_cast<TypeChecker*>(context);
    for (auto& req : checker->required) {
        if (req.matched || std::strcmp(sym->Name, req.name) != 0) continue;
        if (CheckType(checker->session, checker->base, sym->TypeIndex, req))
            req.matched = true;
    }
    return TRUE;
}

// Check that the *loaded PE* embeds an RSDS record, and compare its GUID and
// age with the PDB that DbgHelp actually loaded. This avoids accepting a
// similarly named or stale PDB and removes any need to enumerate approved
// GUIDs. All memory reads are guarded in Read(...), above.
struct RsdsIdentity {
    GUID guid{};
    DWORD age{};
};
bool ReadPeIdentity(const std::byte* module, RsdsIdentity& id) noexcept {
    if (!module) return false;
    IMAGE_DOS_HEADER dos{};
    if (!Read(dos, module) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof dos) || dos.e_lfanew > 0x10000)
        return false;
    IMAGE_NT_HEADERS64 nt{};
    if (!Read(nt, module + dos.e_lfanew) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    const auto imageSize = nt.OptionalHeader.SizeOfImage;
    const auto& debug = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!debug.VirtualAddress || debug.Size < sizeof(IMAGE_DEBUG_DIRECTORY) ||
        debug.VirtualAddress >= imageSize || debug.Size > imageSize - debug.VirtualAddress)
        return false;
    constexpr std::size_t recordSize = 4 + 16 + 4;
    bool found = false;
    for (std::size_t i = 0; i < debug.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        IMAGE_DEBUG_DIRECTORY entry{};
        if (!Read(entry, module + debug.VirtualAddress + i * sizeof(entry))) return false;
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData < recordSize ||
            entry.AddressOfRawData >= imageSize ||
            recordSize > imageSize - entry.AddressOfRawData) continue;
        struct CodeViewRecord { char magic[4]; GUID guid; DWORD age; } record{};
        static_assert(sizeof(CodeViewRecord) == recordSize);
        if (!Read(record, module + entry.AddressOfRawData)) return false;
        if (std::memcmp(record.magic, "RSDS", 4) != 0 || record.age == 0) continue;
        if (found) return false; // ambiguous debug identities
        id.guid = record.guid;
        id.age = record.age;
        found = true;
    }
    return found;
}

bool IsLocalMatchingPdb(const std::filesystem::path& expected,
                        const wchar_t* loaded) {
    if (!loaded || !loaded[0]) return false;
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(expected,
                                                       std::filesystem::path(loaded), error);
    return !error && equivalent;
}

std::string IdentityText(const RsdsIdentity& id) {
    char buf[96]{};
    std::snprintf(buf, sizeof buf,
                  "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX / age %lu",
                  static_cast<unsigned long>(id.guid.Data1), id.guid.Data2, id.guid.Data3,
                  id.guid.Data4[0], id.guid.Data4[1], id.guid.Data4[2], id.guid.Data4[3],
                  id.guid.Data4[4], id.guid.Data4[5], id.guid.Data4[6], id.guid.Data4[7],
                  static_cast<unsigned long>(id.age));
    return buf;
}
struct SymbolLookup {
    DWORD64 base{};
    DWORD64 addr{};
    unsigned distinctAddresses{};
    unsigned enumerated{};
    unsigned matchingNames{};
    unsigned rejectedTags{};
};

bool IsCallableSymbol(const SYMBOL_INFO* sym) noexcept {
    return sym && (sym->Tag == kFunctionSymbolTag || sym->Tag == kPublicSymbolTag);
}

void RecordGetter(SymbolLookup& lookup, DWORD64 address) noexcept {
    if (address < lookup.base) return;
    if (!lookup.distinctAddresses) {
        lookup.addr = address;
        lookup.distinctAddresses = 1;
    } else if (lookup.addr != address) {
        // More than one distinct target must fail closed. Two symbol records
        // for the same RVA are normal in an MSVC public/private PDB.
        lookup.distinctAddresses = 2;
    }
}

// DbgHelp can present a public symbol in at least three forms:
//  * ?get@SkyrimPhysicsWorld@hdt@@SAPEAV12@XZ (decorated, exact type)
//  * hdt::SkyrimPhysicsWorld::get (undecorated, no signature)
//  * hdt::SkyrimPhysicsWorld * __cdecl hdt::SkyrimPhysicsWorld::get(void)
// UnDecorateSymbolName only accepts the FIRST form. Calling it on the second
// silently returns zero; that was the v0.3.1 regression.
bool IsGetterName(const std::string_view raw) {
    if (raw == kWorldGetterDecorated) return true;
    if (raw == "hdt::SkyrimPhysicsWorld::get" ||
        raw == "hdt::SkyrimPhysicsWorld::get(void)" ||
        raw == "hdt::SkyrimPhysicsWorld::get()") return true;

    char undecorated[1024]{};
    std::string_view text = raw;
    if (!raw.empty() && raw.front() == '?' &&
        UnDecorateSymbolName(raw.data(), undecorated, sizeof undecorated, UNDNAME_COMPLETE)) {
        text = undecorated;
    }
    constexpr std::string_view qualified = "hdt::SkyrimPhysicsWorld::get";
    const auto pos = text.find(qualified);
    if (pos == std::string_view::npos) return false;
    const auto suffix = text.substr(pos + qualified.size());
    if (suffix != "(void)" && suffix != "()") return false;
    // For demangled signatures containing a return type, reject incompatible
    // return types; the exact decorated spelling above is even stricter.
    const auto prefix = text.substr(0, pos);
    return prefix.find("hdt::SkyrimPhysicsWorld *") != std::string_view::npos ||
           prefix.find("hdt::SkyrimPhysicsWorld*") != std::string_view::npos;
}

BOOL CALLBACK FindGetter(SYMBOL_INFO* sym, ULONG, void* arg) {
    auto* lookup = static_cast<SymbolLookup*>(arg);
    if (!sym) return TRUE;
    ++lookup->enumerated;
    if (sym->Address < lookup->base) return TRUE;
    const std::string_view raw(sym->Name, sym->NameLen);
    if (!IsGetterName(raw)) return TRUE;
    ++lookup->matchingNames;
    if (!IsCallableSymbol(sym)) {
        ++lookup->rejectedTags;
        return TRUE;
    }
    RecordGetter(*lookup, sym->Address);
    return TRUE;
}

// Named lookup and enumeration need separate paths: SymFromName may not find
// decorated S_PUB32 names when DbgHelp is presenting undecorated symbols.
// Also search in explicit publics-only mode as the final fallback. The PDB
// GUID, type layouts, and executable RVA are still validated independently.
void FindWorldGetter(HANDLE session, DWORD64 base, SymbolLookup& out,
                     bool& directDecorated, bool& directUndecorated,
                     bool& publicsFallback, ULONG& directTag) {
    char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;
    auto probe = [&](const char* name, bool& result) {
        result = SymFromName(session, name, sym) != FALSE;
        if (!result) return;
        directTag = sym->Tag;
        if (IsGetterName(std::string_view(sym->Name, sym->NameLen)) &&
            IsCallableSymbol(sym)) RecordGetter(out, sym->Address);
    };
    probe(kWorldGetterDecorated.data(), directDecorated);
    probe("hdt::SkyrimPhysicsWorld::get", directUndecorated);
    if (out.distinctAddresses == 1) return;

    // The pattern is only an optimization. DbgHelp also stores local/private
    // names with various spellings, so retry with a full module enumeration.
    SymEnumSymbols(session, base, "*SkyrimPhysicsWorld*get*", FindGetter, &out);
    if (out.distinctAddresses == 1) return;
    SymEnumSymbols(session, base, "*", FindGetter, &out);
    if (out.distinctAddresses == 1) return;

    // DbgHelp can preferentially expose private, undecorated PDB records.
    // Asking explicitly for public symbols exposes the S_PUB32 getter that is
    // present in both user-supplied PDBs. Restore the process-wide options.
    const DWORD previousOptions = SymGetOptions();
    SymSetOptions((previousOptions | SYMOPT_PUBLICS_ONLY) & ~SYMOPT_UNDNAME);
    publicsFallback = true;
    bool found = false;
    probe(kWorldGetterDecorated.data(), found);
    if (out.distinctAddresses != 1) {
        SymEnumSymbols(session, base, "*SkyrimPhysicsWorld*get*", FindGetter, &out);
        if (out.distinctAddresses != 1)
            SymEnumSymbols(session, base, "*", FindGetter, &out);
    }
    SymSetOptions(previousOptions);
}
// Some FSMP PDBs expose the getter through a CodeView S_PUB32 record,
// while the Windows DbgHelp symbol API fails to enumerate that record even
// though the *same DbgHelp session* successfully validates its type records.
// Read the public function's CodeView record from the already-validated,
// matching local PDB. Unlike an RVA table this follows future rebuilds.
//
// MSF7 keeps PDB data in numbered blocks. Parse its stream directory rather
// than byte-searching raw disk blocks: a symbol record may cross block
// boundaries. All offsets, sizes, and PE section mappings are bounded and
// checked. Reject ambiguity or any unsupported format (fail closed).
std::optional<DWORD64> FindGetterInPdbPublics(
    const std::filesystem::path& pdb, const std::byte* module,
    std::string& explanation)
{
    explanation = "PDB public symbol not located";
    std::error_code fsError;
    const auto fileBytes = std::filesystem::file_size(pdb, fsError);
    constexpr std::uint64_t kMaximumPdbBytes = 512ull * 1024ull * 1024ull;
    if (fsError || fileBytes < 64 || fileBytes > kMaximumPdbBytes) {
        explanation = "PDB size unavailable or outside supported bounds";
        return std::nullopt;
    }
    std::ifstream file(pdb, std::ios::binary);
    if (!file) { explanation = "Cannot read matching local PDB"; return std::nullopt; }
    std::vector<unsigned char> data(static_cast<std::size_t>(fileBytes));
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        explanation = "Could not read complete matching PDB";
        return std::nullopt;
    }
    const auto u16 = [](const std::vector<unsigned char>& bytes, std::size_t at) -> std::uint16_t {
        return static_cast<std::uint16_t>(bytes[at]) |
               (static_cast<std::uint16_t>(bytes[at + 1]) << 8);
    };
    const auto u32 = [](const std::vector<unsigned char>& bytes, std::size_t at) -> std::uint32_t {
        return static_cast<std::uint32_t>(bytes[at]) |
               (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
               (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
    };
    static constexpr char kMsf7Magic[32] =
        "Microsoft C/C++ MSF 7.00\r\n\x1a" "DS\0\0";
    if (std::memcmp(data.data(), kMsf7Magic, 32) != 0) {
        explanation = "Unsupported PDB MSF format";
        return std::nullopt;
    }
    const auto blockSize = u32(data, 32);
    const auto blockCount = u32(data, 40);
    const auto dirBytes = u32(data, 44);
    const auto blockMapAddress = u32(data, 52);
    if (blockSize < 512 || blockSize > 65536 || (blockSize & (blockSize - 1)) ||
        blockCount == 0 || blockCount > data.size() / blockSize ||
        dirBytes < 4 || dirBytes > 16 * 1024 * 1024 ||
        blockMapAddress >= blockCount) {
        explanation = "Invalid MSF7 superblock";
        return std::nullopt;
    }
    const auto dirBlocks = (static_cast<std::size_t>(dirBytes) + blockSize - 1) / blockSize;
    const auto blockMapStart = static_cast<std::size_t>(blockMapAddress) * blockSize;
    if (dirBlocks > (data.size() - blockMapStart) / 4) {
        explanation = "Invalid MSF7 block map";
        return std::nullopt;
    }
    std::vector<unsigned char> directory;
    directory.reserve(dirBlocks * blockSize);
    for (std::size_t i = 0; i < dirBlocks; ++i) {
        const auto id = u32(data, blockMapStart + i * 4);
        if (id >= blockCount) {
            explanation = "Invalid MSF7 directory block";
            return std::nullopt;
        }
        const auto start = static_cast<std::size_t>(id) * blockSize;
        directory.insert(directory.end(), data.begin() + start, data.begin() + start + blockSize);
    }
    directory.resize(dirBytes);
    const std::size_t count = u32(directory, 0);
    if (count > 131072 || count > (directory.size() - 4) / 4) {
        explanation = "Invalid MSF7 stream directory";
        return std::nullopt;
    }
    std::vector<std::uint32_t> streamSizes;
    streamSizes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) streamSizes.push_back(u32(directory, 4 + 4 * i));
    std::size_t position = 4 + count * 4;
    // Reconstruct streams only as needed. Their block lists are read in MSF
    // directory order; the sought CodeView record is verified in each stream.
    std::vector<std::vector<std::uint32_t>> blocks(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (streamSizes[i] == 0xffffffffu) continue; // absent stream
        const auto n = (static_cast<std::size_t>(streamSizes[i]) + blockSize - 1) / blockSize;
        if (n > (directory.size() - position) / 4) {
            explanation = "Invalid MSF7 stream block list";
            return std::nullopt;
        }
        blocks[i].reserve(n);
        for (std::size_t j = 0; j < n; ++j) {
            const auto id = u32(directory, position + j * 4);
            if (id >= blockCount) {
                explanation = "Invalid MSF7 stream block";
                return std::nullopt;
            }
            blocks[i].push_back(id);
        }
        position += n * 4;
    }
    if (position != directory.size()) {
        explanation = "Unexpected MSF7 directory trailing data";
        return std::nullopt;
    }

    // Resolve a CodeView section-relative address using the loaded PE rather
    // than assuming .text has a fixed RVA. Require executable image memory.
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!Read(dos, module) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof dos) || dos.e_lfanew > 0x10000 ||
        !Read(nt, module + dos.e_lfanew) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections == 0 || nt.FileHeader.NumberOfSections > 96 ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
        explanation = "Invalid loaded FSMP PE headers";
        return std::nullopt;
    }
    const std::size_t sectionHeaders =
        static_cast<std::size_t>(dos.e_lfanew) +
        offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    const std::size_t sectionHeadersBytes =
        static_cast<std::size_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (sectionHeaders > nt.OptionalHeader.SizeOfHeaders ||
        sectionHeadersBytes > nt.OptionalHeader.SizeOfHeaders - sectionHeaders) {
        explanation = "Invalid loaded FSMP PE section headers";
        return std::nullopt;
    }
    auto sectionRva = [&](std::uint16_t segment, std::uint32_t offset) -> std::optional<std::uint32_t> {
        if (!segment || segment > nt.FileHeader.NumberOfSections) return std::nullopt;
        IMAGE_SECTION_HEADER header{};
        if (!Read(header, module + sectionHeaders + (segment - 1) * sizeof header) ||
            !(header.Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            offset >= header.Misc.VirtualSize ||
            header.VirtualAddress >= nt.OptionalHeader.SizeOfImage ||
            offset >= nt.OptionalHeader.SizeOfImage - header.VirtualAddress)
            return std::nullopt;
        return header.VirtualAddress + offset;
    };

    // Reject any symbol address that is not the *start* of an entry in the
    // x64 exception directory. Both provided getter functions satisfy this.
    const auto& exception =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const std::size_t pdataBegin = exception.VirtualAddress;
    const std::size_t pdataSize = exception.Size;
    if (!pdataBegin || pdataSize < sizeof(RUNTIME_FUNCTION) ||
        pdataBegin > nt.OptionalHeader.SizeOfImage ||
        pdataSize > nt.OptionalHeader.SizeOfImage - pdataBegin) {
        explanation = "FSMP PE has no usable x64 function table";
        return std::nullopt;
    }
    auto isFunctionEntry = [&](std::uint32_t rva) {
        for (std::size_t p = 0; p + sizeof(RUNTIME_FUNCTION) <= pdataSize; p += sizeof(RUNTIME_FUNCTION)) {
            RUNTIME_FUNCTION fn{};
            if (!Read(fn, module + pdataBegin + p)) return false;
            if (fn.BeginAddress == rva && fn.EndAddress > rva) return true;
        }
        return false;
    };
    DWORD64 chosenAddress = 0;
    unsigned matches = 0;
    const auto target = kWorldGetterDecorated;
    for (std::size_t si = 0; si < count; ++si) {
        if (streamSizes[si] == 0xffffffffu || streamSizes[si] < target.size() + 15) continue;
        std::vector<unsigned char> stream;
        stream.reserve(blocks[si].size() * blockSize);
        for (const auto block : blocks[si]) {
            const auto start = static_cast<std::size_t>(block) * blockSize;
            stream.insert(stream.end(), data.begin() + start, data.begin() + start + blockSize);
        }
        stream.resize(streamSizes[si]);
        for (std::size_t at = 14; at + target.size() < stream.size(); ++at) {
            if (stream[at] != '?' ||
                std::memcmp(stream.data() + at, target.data(), target.size()) != 0 ||
                stream[at + target.size()] != 0) continue;
            const auto start = at - 14;
            const auto recordBytes = u16(stream, start);
            const auto kind = u16(stream, start + 2);
            // S_PUB32: uint16 length; uint16 0x110e; uint32 flags;
            // uint32 sectionOffset; uint16 segment; NUL-terminated name.
            if (kind != 0x110e || recordBytes < 12 + target.size() + 1 ||
                recordBytes > stream.size() - start - 2) continue;
            const auto flags = u32(stream, start + 4);
            if (!(flags & 2u)) continue; // CV_PUBSYMFLAGS_FUNCTION
            const auto sectionOffset = u32(stream, start + 8);
            const auto segment = u16(stream, start + 12);
            const auto rva = sectionRva(segment, sectionOffset);
            if (!rva || !isFunctionEntry(*rva)) continue;
            const auto address = reinterpret_cast<DWORD64>(module) + *rva;
            if (!matches) chosenAddress = address;
            if (chosenAddress != address) {
                explanation = "Ambiguous valid S_PUB32 getter addresses";
                return std::nullopt;
            }
            ++matches;
        }
    }
    if (!matches) return std::nullopt;
    explanation = "S_PUB32 record (matching PDB, PE function-entry verified)";
    return chosenAddress;
}

} // namespace

bool InitializeWorldABI(std::string& failure, std::string* diagnostics) {
    g_getWorld = nullptr;
    failure.clear();
    if (diagnostics) diagnostics->clear();
    auto* module = GetModuleHandleW(L"hdtsmp64.dll");
    if (!module) { failure = "FSMP module unavailable"; return false; }
    wchar_t dllPath[32768]{};
    const auto copied = GetModuleFileNameW(module, dllPath, 32768);
    if (!copied || copied >= 32768) {
        failure = "Could not locate the loaded FSMP DLL";
        return false;
    }
    std::filesystem::path pdb(dllPath);
    pdb.replace_extension(L".pdb");
    std::error_code error;
    if (!std::filesystem::is_regular_file(pdb, error) || error) {
        failure = "Matching hdtsmp64.pdb must be installed beside hdtsmp64.dll";
        return false;
    }
    RsdsIdentity peIdentity{};
    if (!ReadPeIdentity(reinterpret_cast<const std::byte*>(module), peIdentity)) {
        failure = "FSMP DLL does not contain a unique valid CodeView RSDS identity";
        return false;
    }
    // Use an isolated DbgHelp session to avoid interfering with other plugins.
    // DbgHelp's global options are process-wide; restore them before return.
    const HANDLE session = reinterpret_cast<HANDLE>(
        reinterpret_cast<std::uintptr_t>(module) ^ 0x57534D5057444255ull);
    const auto oldOptions = SymGetOptions();
    SymSetOptions(oldOptions | SYMOPT_EXACT_SYMBOLS | SYMOPT_NO_PROMPTS);
    bool ok = false;
    if (!SymInitializeW(session, pdb.parent_path().c_str(), FALSE)) {
        failure = "DbgHelp could not initialize FSMP PDB session";
    } else {
        const DWORD64 base = reinterpret_cast<DWORD64>(module);
        const auto loaded = SymLoadModuleExW(session, nullptr, dllPath, nullptr, base, 0,
                                             nullptr, 0);
        IMAGEHLP_MODULEW64 info{};
        info.SizeOfStruct = sizeof(info);
        if (!loaded || !SymGetModuleInfoW64(session, base, &info) ||
            info.PdbUnmatched || !info.LoadedPdbName[0]) {
            failure = "FSMP PDB missing, mismatched or not loaded by DbgHelp";
        } else if (!IsLocalMatchingPdb(pdb, info.LoadedPdbName)) {
            failure = "DbgHelp loaded a different PDB; require hdtsmp64.pdb beside the loaded DLL";
        } else if (std::memcmp(&peIdentity.guid, &info.PdbSig70, sizeof(GUID)) != 0 ||
                   peIdentity.age != info.PdbAge) {
            failure = "Loaded FSMP PDB RSDS GUID/age does not match the loaded DLL";
        } else {
            TypeChecker checker{session, base};
            const bool layout = SymEnumTypes(session, base, VerifyPdbType, &checker);
            if (!layout || !checker.AllMatched()) {
                failure = "FSMP PDB ABI layout mismatch: " + checker.Missing();
            } else {
                SymbolLookup lookup{base};
                bool directDecorated = false;
                bool directUndecorated = false;
                bool publicSearch = false;
                ULONG directTag = 0;
                FindWorldGetter(session, base, lookup, directDecorated,
                                directUndecorated, publicSearch, directTag);
                bool pdbPublicsFallback = false;
                std::string pdbPublicsDetail;
                // Use the direct CodeView fallback only after the normal
                // DbgHelp lookups found *nothing*. Never mask ambiguity.
                if (lookup.distinctAddresses == 0) {
                    if (const auto directPdb = FindGetterInPdbPublics(
                            pdb, reinterpret_cast<const std::byte*>(module),
                            pdbPublicsDetail)) {
                        RecordGetter(lookup, *directPdb);
                        pdbPublicsFallback = lookup.distinctAddresses == 1;
                    }
                }
                if (lookup.distinctAddresses != 1) {
                    failure = "Cannot uniquely locate SkyrimPhysicsWorld::get in matching PDB "
                        "(distinct candidates=" + std::to_string(lookup.distinctAddresses) +
                        ", decorated=" + (directDecorated ? "found" : "missing") +
                        ", qualified=" + (directUndecorated ? "found" : "missing") +
                        ", publicFallback=" + (publicSearch ? "yes" : "no") +
                        ", enumerated=" + std::to_string(lookup.enumerated) +
                        ", nameMatches=" + std::to_string(lookup.matchingNames) +
                        ", rejectedTags=" + std::to_string(lookup.rejectedTags) +
                        ", directTag=" + (directDecorated || directUndecorated ?
                            std::to_string(directTag) : "n/a") +
                        ", pdbPublics=" + (pdbPublicsDetail.empty() ? "not tried" : pdbPublicsDetail) + ")";
                } else {
                    MEMORY_BASIC_INFORMATION mem{};
                    const auto* address = reinterpret_cast<const void*>(lookup.addr);
                    constexpr DWORD executablePages = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                    if (lookup.addr < base || lookup.addr >= base + info.ImageSize ||
                        !VirtualQuery(address, &mem, sizeof mem) ||
                        mem.State != MEM_COMMIT || (mem.Protect & PAGE_GUARD) ||
                        !(mem.Protect & executablePages)) {
                        failure = "FSMP world getter does not point into executable image memory";
                    } else {
                        g_getWorld = reinterpret_cast<WorldGetter>(lookup.addr);
                        if (diagnostics) {
                            *diagnostics = "RSDS=" + IdentityText(peIdentity) +
                                ", verified type layouts (including Bullet), getter RVA=0x";
                            char addrText[32]{};
                            std::snprintf(addrText, sizeof addrText, "%llX",
                                          static_cast<unsigned long long>(lookup.addr - base));
                            *diagnostics += addrText;
                            if (pdbPublicsFallback)
                                *diagnostics += ", resolved via verified PDB S_PUB32 fallback";
                        }
                        ok = true;
                    }
                }
            }
        }
        SymCleanup(session);
    }
    SymSetOptions(oldOptions);
    return ok;
}

bool ReadActiveSystems(const std::unordered_set<btRigidBody*>& candidates,
                       std::vector<PhysicsSystem>& out,
                       SystemSnapshotReport* report) noexcept {
    out.clear();
    if (report) {
        *report = {};
        report->candidates = candidates.size();
    }
    const auto reject = [&](const char* stage, std::uintptr_t suspect = 0) noexcept {
        if (report) {
            report->stage = stage;
            report->suspect = suspect;
        }
        out.clear();
        return false;
    };
    if (!g_getWorld) return reject("getter unavailable");
    void* world = CallGetter(g_getWorld);
    if (!world) return reject("getter returned null");
    VectorView systems{};
    if (!ReadVector(static_cast<const std::byte*>(world), kSystems, systems, kMaxSystems))
        return reject("m_systems vector header invalid", reinterpret_cast<std::uintptr_t>(world));
    std::unordered_set<btRigidBody*> seen;
    const size_t n = (reinterpret_cast<std::uintptr_t>(systems.end) -
                      reinterpret_cast<std::uintptr_t>(systems.begin)) / sizeof(void*);
    if (report) report->systems = n;
    for (size_t i = 0; i < n; ++i) {
        std::uintptr_t ptr{};
        if (!Read(ptr, systems.begin + i) || !ptr)
            return reject("m_systems element unreadable/null", i);
        const auto* sys = reinterpret_cast<const std::byte*>(ptr);
        std::uintptr_t ownerWorld{}, root{};
        if (!Read(ownerWorld, sys + kSystemWorld) ||
            ownerWorld != reinterpret_cast<std::uintptr_t>(world))
            return reject("system m_world mismatch/unreadable", ptr);
        // Every entry is guaranteed to have the SkinnedMeshSystem base, but
        // m_skeleton belongs specifically to the derived SkyrimSystem at
        // offset 0x98 (exactly sizeof(SkinnedMeshSystem)). A base-only or
        // temporarily rootless entry must not invalidate independent systems,
        // and MUST NOT be inspected as SkyrimBone / receive wind forces.
        // Validate the base-class vector header before deciding to skip it.
        VectorView bones{};
        if (!ReadVector(sys, kSystemBones, bones, kMaxBonesPerSystem))
            return reject("system m_bones vector invalid", ptr);
        if (!Read(root, sys + kSystemRoot))
            return reject("system m_skeleton unreadable", ptr);
        if (!root) {
            if (report) {
                ++report->rootlessSystems;
                if (!report->firstRootless) report->firstRootless = ptr;
            }
            // No trustworthy scene root: do not dereference this system's
            // bone objects, infer a root, or apply forces to its bodies.
            continue;
        }
        PhysicsSystem group{};
        group.identity = ptr;
        group.root = reinterpret_cast<RE::NiNode*>(root);
        const size_t boneCount = (reinterpret_cast<std::uintptr_t>(bones.end) -
                                  reinterpret_cast<std::uintptr_t>(bones.begin)) / sizeof(void*);
        for (size_t b = 0; b < boneCount; ++b) {
            std::uintptr_t bone{};
            if (!Read(bone, bones.begin + b) || !bone)
                return reject("system bone pointer unreadable/null", ptr);
            auto* body = reinterpret_cast<btRigidBody*>(bone + kBoneRig);
            // Not all Bullet objects in a PreStep event necessarily belong to
            // a SkinnedMeshSystem. Only explicitly registered bodies are used.
            if (!candidates.contains(body)) continue;
            std::uintptr_t node{};
            if (!Read(node, reinterpret_cast<const std::byte*>(bone) + kBoneNode) || !node)
                return reject("registered bone m_node invalid", bone);
            void* user = SafeBodyUser(body);
            if (user != reinterpret_cast<void*>(bone))
                return reject("registered bone userPointer mismatch", bone);
            if (!seen.insert(body).second)
                return reject("registered body appears in multiple systems", bone);
            if (report) report->registered = seen.size();
            if (!group.anchor && body->isStaticOrKinematicObject()) group.anchor = body;
            group.bones.push_back({reinterpret_cast<void*>(bone), body,
                                   reinterpret_cast<RE::NiNode*>(node)});
        }
        if (!group.bones.empty()) {
            if (!group.anchor) group.anchor = group.bones.front().body;
            out.push_back(std::move(group));
        }
    }
    // A filtered PreStep object that does not belong to ANY registered FSMP
    // system is not an authoritative bone. Ignore it rather than disabling
    // the wind for unrelated, fully verified systems.
    if (report) {
        report->registered = seen.size();
        report->unmatched = candidates.size() - seen.size();
    }
    return true;
}
} // namespace osfsmp
