#pragma once

// Finding the game's classes in libminecraftpe.so without fixed addresses or byte patterns.
//
// Android's game library keeps C++ RTTI (Itanium ABI), so every polymorphic class has its type name as a string
// ("11AppPlatform"), a type_info object that points to that string, and a vtable that points to the type_info.
// Following those pointers gives each class's vtable, which survives game updates far better than byte patterns.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace splitscreen {

struct Segment {
    uintptr_t start = 0;
    uintptr_t end   = 0;
    bool      read  = false;
    bool      write = false; // .data.rel.ro is in a PF_W segment (made read-only after relocation)
    bool      exec  = false;
};

struct Module {
    std::string          path;
    uintptr_t            base = 0;
    std::vector<Segment> segments;

    [[nodiscard]] bool isCode(uintptr_t p) const;
};

// The loaded library whose path ends with `name`, e.g. "libminecraftpe.so".
std::optional<Module> findModule(char const* name);

struct TypeName {
    uintptr_t   address; // the string in .rodata
    std::string name;    // mangled, e.g. "19AppPlatform_android"
};

// Every RTTI type name that contains `needle` (e.g. "AppPlatform" finds "11AppPlatform", "19AppPlatform_android").
std::vector<TypeName> findTypeNames(Module const& m, std::string_view needle);

// The type_info object whose name pointer is `name`, or 0.
uintptr_t findTypeInfo(Module const& m, uintptr_t name);

// The address points (first virtual function slot) of the primary vtables that use `typeInfo`.
std::vector<uintptr_t*> findVtables(Module const& m, uintptr_t typeInfo);

// Number of slots from `vtable` that hold code pointers (into this module, or exported functions elsewhere).
size_t countSlots(Module const& m, uintptr_t* vtable, size_t max);

// A short description of the code at `fn`: its exported symbol if it has one, and what its first instructions do
// ("return 0", "return 1", "tail call", ...).
std::string describeFunction(Module const& m, uintptr_t fn);

// If the function only returns a constant (mov w0,#n; ret), that constant.
std::optional<uint32_t> constantReturn(uintptr_t fn);

// Writes `value` into a vtable slot (the page is read-only after relocation).
bool writeSlot(uintptr_t* slot, uintptr_t value);

} // namespace splitscreen
