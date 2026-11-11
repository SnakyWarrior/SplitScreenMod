#include "Elf.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <fmt/format.h>

namespace splitscreen {

bool Module::isCode(uintptr_t p) const {
    for (auto const& s : segments)
        if (s.exec && p >= s.start && p < s.end) return true;
    return false;
}

std::optional<Module> findModule(char const* name) {
    struct Ctx {
        char const*           name;
        std::optional<Module> result;
    } ctx{name, std::nullopt};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) -> int {
            auto& c = *static_cast<Ctx*>(data);
            if (!info->dlpi_name) return 0;
            std::string_view path(info->dlpi_name), want(c.name);
            if (path.size() < want.size() || path.substr(path.size() - want.size()) != want) return 0;
            Module m;
            m.path = info->dlpi_name;
            m.base = info->dlpi_addr;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                auto const& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_LOAD) continue;
                Segment s;
                s.start = info->dlpi_addr + ph.p_vaddr;
                s.end   = s.start + ph.p_memsz;
                s.read  = ph.p_flags & PF_R;
                s.write = ph.p_flags & PF_W;
                s.exec  = ph.p_flags & PF_X;
                m.segments.push_back(s);
            }
            c.result = std::move(m);
            return 1;
        },
        &ctx
    );
    return ctx.result;
}

static bool looksLikeTypeName(std::string_view s) {
    if (s.empty() || s.size() > 200) return false;
    if (!(std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == 'N')) return false;
    for (char c : s)
        if (static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) > 0x7E) return false;
    return true;
}

std::vector<TypeName> findTypeNames(Module const& m, std::string_view needle) {
    std::vector<TypeName> out;
    for (auto const& s : m.segments) {
        // Type names live in .rodata. The game links .rodata into its first, executable segment (r-x), so every
        // readable segment that is not writable is searched.
        if (!s.read || s.write) continue;
        auto const* begin = reinterpret_cast<char const*>(s.start);
        size_t      size  = s.end - s.start;
        for (size_t at = 0; at + needle.size() <= size;) {
            auto const* hit = static_cast<char const*>(memmem(begin + at, size - at, needle.data(), needle.size()));
            if (!hit) break;
            at = static_cast<size_t>(hit - begin) + 1;
            // the start of the string: just after the previous NUL
            char const* start = hit;
            while (start > begin && start[-1] != '\0' && hit - start < 200) --start;
            if (start > begin && start[-1] != '\0') continue;
            std::string_view str(start, strnlen(start, 256));
            if (!looksLikeTypeName(str)) continue;
            auto addr = reinterpret_cast<uintptr_t>(start);
            bool dup  = false;
            for (auto const& t : out) dup |= t.address == addr;
            if (!dup) out.push_back({addr, std::string(str)});
        }
    }
    return out;
}

// Calls f(slot address) for each pointer-sized word in the module's writable (relocated) segments equal to value.
template <class F>
static void forEachPointerTo(Module const& m, uintptr_t value, F&& f) {
    for (auto const& s : m.segments) {
        if (!s.read || !s.write) continue;
        for (uintptr_t p = (s.start + 7) & ~uintptr_t(7); p + 8 <= s.end; p += 8)
            if (*reinterpret_cast<uintptr_t const*>(p) == value) f(p, s);
    }
}

uintptr_t findTypeInfo(Module const& m, uintptr_t name) {
    uintptr_t found = 0;
    forEachPointerTo(m, name, [&](uintptr_t p, Segment const& s) {
        // type_info: { vptr, name, ... }; the vptr is non-null
        if (!found && p >= s.start + 8 && *reinterpret_cast<uintptr_t const*>(p - 8) != 0) found = p - 8;
    });
    return found;
}

std::vector<uintptr_t*> findVtables(Module const& m, uintptr_t typeInfo) {
    std::vector<uintptr_t*> out;
    forEachPointerTo(m, typeInfo, [&](uintptr_t p, Segment const& s) {
        // vtable: { offset_to_top (0 for the primary vtable), type_info*, slot 0, ... }
        if (p < s.start + 8 || p + 16 > s.end) return;
        if (*reinterpret_cast<intptr_t const*>(p - 8) != 0) return;
        out.push_back(reinterpret_cast<uintptr_t*>(p + 8));
    });
    return out;
}

// True if p points into an executable segment of any loaded library (e.g. __cxa_pure_virtual in libc++).
static bool isCodeAnywhere(uintptr_t p) {
    struct Ctx {
        uintptr_t p;
        bool      found;
    } ctx{p, false};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) -> int {
            auto& c = *static_cast<Ctx*>(data);
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                auto const& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) continue;
                uintptr_t start = info->dlpi_addr + ph.p_vaddr;
                if (c.p >= start && c.p < start + ph.p_memsz) return c.found = true, 1;
            }
            return 0;
        },
        &ctx
    );
    return ctx.found;
}

size_t countSlots(Module const& m, uintptr_t* vtable, size_t max) {
    size_t n = 0;
    while (n < max && (m.isCode(vtable[n]) || isCodeAnywhere(vtable[n]))) ++n;
    return n;
}

std::optional<uint32_t> constantReturn(uintptr_t fn) {
    auto const* code = reinterpret_cast<uint32_t const*>(fn);
    if (code[1] != 0xD65F03C0) return std::nullopt;                // ret
    if (code[0] == 0x2A1F03E0) return 0u;                          // mov w0, wzr
    if ((code[0] & 0xFFE0001F) == 0x52800000) return (code[0] >> 5) & 0xFFFF; // movz w0, #imm
    return std::nullopt;
}

std::string describeFunction(Module const& m, uintptr_t fn) {
    std::string out;
    Dl_info     info{};
    if (dladdr(reinterpret_cast<void*>(fn), &info) && info.dli_sname
        && reinterpret_cast<uintptr_t>(info.dli_saddr) == fn) {
        int   status = 0;
        char* plain  = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        out          = status == 0 && plain ? plain : info.dli_sname;
        std::free(plain);
        out += "  ";
    }
    if (!m.isCode(fn)) return out + "(outside the game)";
    if (auto c = constantReturn(fn)) return out + fmt::format("return {}", *c);
    auto const* code = reinterpret_cast<uint32_t const*>(fn);
    if ((code[0] & 0xFC000000) == 0x14000000) return out + "tail call";
    return out + fmt::format("{:08X} {:08X} {:08X}", code[0], code[1], code[2]);
}

// The number of entries in the dynamic symbol table, from DT_HASH (nchain) or DT_GNU_HASH (the highest symbol
// index in its buckets and chains).
static size_t dynamicSymbolCount(uintptr_t hash, uintptr_t gnuHash) {
    if (hash) return reinterpret_cast<uint32_t const*>(hash)[1];
    if (!gnuHash) return 0;
    auto const* h          = reinterpret_cast<uint32_t const*>(gnuHash);
    uint32_t    nbuckets   = h[0];
    uint32_t    symoffset  = h[1];
    uint32_t    bloomSize  = h[2];
    auto const* buckets    = reinterpret_cast<uint32_t const*>(gnuHash + 16 + bloomSize * sizeof(uintptr_t));
    auto const* chains     = buckets + nbuckets;
    uint32_t    last       = 0;
    for (uint32_t b = 0; b < nbuckets; ++b) last = std::max(last, buckets[b]);
    if (last < symoffset) return symoffset;
    while (!(chains[last - symoffset] & 1)) ++last;
    return last + 1;
}

std::vector<Symbol> exportedFunctions(Module const& m) {
    std::vector<Symbol> out;
    struct Ctx {
        Module const*        m;
        std::vector<Symbol>* out;
    } ctx{&m, &out};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) -> int {
            auto& c = *static_cast<Ctx*>(data);
            if (info->dlpi_addr != c.m->base || !info->dlpi_name || c.m->path != info->dlpi_name) return 0;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                auto const& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_DYNAMIC) continue;
                uintptr_t symtab = 0, strtab = 0, hash = 0, gnuHash = 0;
                for (auto const* d = reinterpret_cast<ElfW(Dyn) const*>(info->dlpi_addr + ph.p_vaddr); d->d_tag != DT_NULL;
                     ++d) {
                    // Android's loader leaves these as addresses relative to the load base
                    uintptr_t v = d->d_un.d_ptr;
                    if (v < info->dlpi_addr) v += info->dlpi_addr;
                    if (d->d_tag == DT_SYMTAB) symtab = v;
                    else if (d->d_tag == DT_STRTAB) strtab = v;
                    else if (d->d_tag == DT_HASH) hash = v;
                    else if (d->d_tag == DT_GNU_HASH) gnuHash = v;
                }
                if (!symtab || !strtab) return 1;
                size_t n    = dynamicSymbolCount(hash, gnuHash);
                auto*  syms = reinterpret_cast<ElfW(Sym) const*>(symtab);
                for (size_t k = 0; k < n; ++k) {
                    auto const& sym = syms[k];
                    if (ELF64_ST_TYPE(sym.st_info) != STT_FUNC || sym.st_shndx == SHN_UNDEF || !sym.st_value) continue;
                    c.out->push_back({reinterpret_cast<char const*>(strtab + sym.st_name), info->dlpi_addr + sym.st_value});
                }
                return 1;
            }
            return 1;
        },
        &ctx
    );
    return out;
}

bool isPureVirtual(uintptr_t fn) {
    Dl_info info{};
    return fn && dladdr(reinterpret_cast<void*>(fn), &info) && info.dli_sname
        && reinterpret_cast<uintptr_t>(info.dli_saddr) == fn && std::strcmp(info.dli_sname, "__cxa_pure_virtual") == 0;
}

bool writeSlot(uintptr_t* slot, uintptr_t value) {
    long      pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t page     = reinterpret_cast<uintptr_t>(slot) & ~(uintptr_t(pageSize) - 1);
    if (mprotect(reinterpret_cast<void*>(page), pageSize, PROT_READ | PROT_WRITE) != 0) return false;
    __atomic_store_n(slot, value, __ATOMIC_SEQ_CST);
    mprotect(reinterpret_cast<void*>(page), pageSize, PROT_READ);
    return true;
}

} // namespace splitscreen
