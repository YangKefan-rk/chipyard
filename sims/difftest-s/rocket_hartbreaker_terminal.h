#ifndef CHIPYARD_DIFFTEST_S_ROCKET_HARTBREAKER_TERMINAL_H
#define CHIPYARD_DIFFTEST_S_ROCKET_HARTBREAKER_TERMINAL_H

#include <array>
#include <cstring>
#include <cstdint>
#include <elf.h>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace difftest_s::native {

// A runner may enable this protocol only for this exact independently loaded
// HartBreaker final block. Addresses and bytes come from ELF, never DUT data.
struct HartbreakerTerminalContract {
  uint64_t counter_amo_pc = 0;
  uint64_t park_pc = 0;
  uint64_t tohost_store_pc = 0;
  uint64_t counter_address = 0;
  uint64_t tohost_address = 0;
};

namespace hartbreaker_terminal_detail {
inline bool extent(size_t size, uint64_t offset, uint64_t length) {
  return offset <= size && length <= size - offset;
}
template <typename T>
T object(const std::vector<uint8_t> &elf, uint64_t offset) {
  if (!extent(elf.size(), offset, sizeof(T)))
    throw std::invalid_argument("HartBreaker terminal ELF object is truncated");
  T result;
  std::memcpy(&result, elf.data() + offset, sizeof(result));
  return result;
}
inline uint64_t symbol(const std::vector<uint8_t> &elf, const Elf64_Ehdr &header, const char *name) {
  bool found = false;
  uint64_t value = 0;
  for (unsigned i = 0; i < header.e_shnum; ++i) {
    const auto section = object<Elf64_Shdr>(elf, header.e_shoff + uint64_t{i} * sizeof(Elf64_Shdr));
    if (section.sh_type != SHT_SYMTAB)
      continue;
    if (section.sh_entsize != sizeof(Elf64_Sym) || section.sh_size % sizeof(Elf64_Sym) ||
        section.sh_link >= header.e_shnum || !extent(elf.size(), section.sh_offset, section.sh_size))
      throw std::invalid_argument("HartBreaker terminal ELF symbol table is malformed");
    const auto strings = object<Elf64_Shdr>(elf,
        header.e_shoff + uint64_t{section.sh_link} * sizeof(Elf64_Shdr));
    if (strings.sh_type != SHT_STRTAB || !extent(elf.size(), strings.sh_offset, strings.sh_size))
      throw std::invalid_argument("HartBreaker terminal ELF symbol strings are malformed");
    for (uint64_t offset = 0; offset < section.sh_size; offset += sizeof(Elf64_Sym)) {
      const auto entry = object<Elf64_Sym>(elf, section.sh_offset + offset);
      if (entry.st_name >= strings.sh_size)
        throw std::invalid_argument("HartBreaker terminal ELF symbol name is out of range");
      const char *text = reinterpret_cast<const char *>(elf.data() + strings.sh_offset + entry.st_name);
      const auto limit = strings.sh_size - entry.st_name;
      if (!std::memchr(text, '\0', limit))
        throw std::invalid_argument("HartBreaker terminal ELF symbol name is unterminated");
      if (entry.st_shndx != SHN_UNDEF && std::strcmp(text, name) == 0) {
        if (found && value != entry.st_value)
          throw std::invalid_argument("HartBreaker terminal ELF has conflicting symbols");
        found = true;
        value = entry.st_value;
      }
    }
  }
  if (!found)
    throw std::invalid_argument(std::string("HartBreaker terminal ELF lacks symbol ") + name);
  return value;
}
} // namespace hartbreaker_terminal_detail

template <typename Memory>
HartbreakerTerminalContract hartbreaker_terminal_contract(const Memory &memory, const char *path) {
  using namespace hartbreaker_terminal_detail;
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    throw std::invalid_argument("HartBreaker terminal ELF could not be opened");
  const auto length = file.tellg();
  if (length < static_cast<std::streamoff>(sizeof(Elf64_Ehdr)) || length > (std::streamoff{1} << 29))
    throw std::invalid_argument("HartBreaker terminal ELF size is unsupported");
  std::vector<uint8_t> elf(static_cast<size_t>(length));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(elf.data()), elf.size()))
    throw std::invalid_argument("HartBreaker terminal ELF read failed");
  const auto header = object<Elf64_Ehdr>(elf, 0);
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_RISCV ||
      header.e_phentsize != sizeof(Elf64_Phdr) || header.e_shentsize != sizeof(Elf64_Shdr) ||
      !header.e_shnum || !extent(elf.size(), header.e_phoff, uint64_t{header.e_phnum} * sizeof(Elf64_Phdr)) ||
      !extent(elf.size(), header.e_shoff, uint64_t{header.e_shnum} * sizeof(Elf64_Shdr)))
    throw std::invalid_argument("HartBreaker terminal contract requires a bounded RV64 little-endian ELF");

  constexpr std::array<uint32_t, 13> expected = {
      0xfe8d0d93, 0x00100e13, 0x07cdad2f, 0x000dbd03, 0x00400e13,
      0x01ae0663, 0x10500073, 0xffdff06f, 0x00200e37, 0x01fe0e33,
      0x00100d13, 0x01ae3023, 0xffdff06f};
  std::optional<uint64_t> matched;
  for (unsigned i = 0; i < header.e_phnum; ++i) {
    const auto segment = object<Elf64_Phdr>(elf, header.e_phoff + uint64_t{i} * sizeof(Elf64_Phdr));
    if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_X))
      continue;
    if (!extent(elf.size(), segment.p_offset, segment.p_filesz) ||
        segment.p_vaddr != segment.p_paddr || segment.p_filesz > segment.p_memsz)
      throw std::invalid_argument("HartBreaker terminal executable segment is unsupported");
    if (segment.p_filesz < expected.size() * 4)
      continue;
    for (uint64_t off = 0; off <= segment.p_filesz - expected.size() * 4; off += 2) {
      bool matches = true;
      for (size_t word = 0; word < expected.size(); ++word)
        matches = matches && object<uint32_t>(elf, segment.p_offset + off + word * 4) == expected[word];
      if (!matches)
        continue;
      if (segment.p_paddr > UINT64_MAX - off || matched)
        throw std::invalid_argument("HartBreaker terminal block address is ambiguous");
      matched = segment.p_paddr + off;
    }
  }
  if (!matched)
    throw std::invalid_argument("HartBreaker terminal ELF has no exact quad-hart completion block");
  for (size_t index = 0; index < expected.size(); ++index) {
    std::array<uint8_t, 4> bytes{};
    if (!memory.read(*matched + index * 4, bytes.data(), bytes.size()))
      throw std::invalid_argument("HartBreaker terminal block is absent from independent initialized memory");
    uint32_t raw = 0;
    for (unsigned byte = 0; byte < 4; ++byte)
      raw |= uint32_t{bytes[byte]} << (8 * byte);
    if (raw != expected[index])
      throw std::invalid_argument("HartBreaker terminal loaded bytes differ from the checked ELF");
  }
  const uint64_t counter = symbol(elf, header, "semaphore_1");
  const uint64_t tohost = symbol(elf, header, "tohost");
  if (counter != UINT64_C(0x80180000) || tohost != UINT64_C(0x80200000))
    throw std::invalid_argument("HartBreaker quadrocket terminal addresses differ from its initialized base");
  return {*matched + 8, *matched + 24, *matched + 44, counter, tohost};
}

} // namespace difftest_s::native
#endif
