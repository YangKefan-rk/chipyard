#ifndef CHIPYARD_DIFFTEST_S_BOOM_HARTBREAKER_ELF_H
#define CHIPYARD_DIFFTEST_S_BOOM_HARTBREAKER_ELF_H

#include "rvwmo/memory_image.h"
#include <elf.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace difftest_s::native {

// Resolve a terminal address from the independently loaded ELF. Never derive
// the expected address or value from a DUT sample or the native HTIF response.
struct HartBreakerTerminalAddresses {
  uint64_t tohost = 0;
  uint64_t parked_wfi = 0;
};

inline HartBreakerTerminalAddresses resolve_hartbreaker_terminal(const std::string &path,
                                                                 const GlobalMem &memory) {
  constexpr size_t max_file = 64 * 1024 * 1024;
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() < static_cast<std::streamoff>(sizeof(Elf64_Ehdr)) ||
      file.tellg() > static_cast<std::streamoff>(max_file))
    throw std::invalid_argument("HartBreaker terminal requires a bounded readable ELF");
  std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
    throw std::invalid_argument("HartBreaker terminal ELF could not be read completely");
  auto within = [&bytes](uint64_t offset, uint64_t size) {
    return offset <= bytes.size() && size <= bytes.size() - offset;
  };
  auto read_section = [&bytes, &within](uint64_t offset) {
    Elf64_Shdr section{};
    if (!within(offset, sizeof(section)))
      throw std::invalid_argument("HartBreaker terminal ELF has a malformed section table");
    std::memcpy(&section, bytes.data() + offset, sizeof(section));
    return section;
  };
  Elf64_Ehdr header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_RISCV ||
      header.e_shentsize != sizeof(Elf64_Shdr) || !header.e_shnum ||
      !within(header.e_shoff, uint64_t(header.e_shnum) * sizeof(Elf64_Shdr)))
    throw std::invalid_argument("HartBreaker terminal requires a valid RV64 ELF section table");

  uint64_t address = 0;
  for (uint16_t index = 0; index < header.e_shnum; ++index) {
    const auto symbols = read_section(header.e_shoff + uint64_t(index) * sizeof(Elf64_Shdr));
    if (symbols.sh_type != SHT_SYMTAB)
      continue;
    if (symbols.sh_link >= header.e_shnum || symbols.sh_entsize != sizeof(Elf64_Sym) || symbols.sh_size % sizeof(Elf64_Sym) ||
        !within(symbols.sh_offset, symbols.sh_size))
      throw std::invalid_argument("HartBreaker terminal has a malformed ELF symbol table");
    const auto strings = read_section(header.e_shoff + uint64_t(symbols.sh_link) * sizeof(Elf64_Shdr));
    if (strings.sh_type != SHT_STRTAB || !within(strings.sh_offset, strings.sh_size))
      throw std::invalid_argument("HartBreaker terminal has a malformed ELF string table");
    for (uint64_t offset = 0; offset + sizeof(Elf64_Sym) <= symbols.sh_size; offset += sizeof(Elf64_Sym)) {
      Elf64_Sym symbol{};
      std::memcpy(&symbol, bytes.data() + symbols.sh_offset + offset, sizeof(symbol));
      if (symbol.st_name >= strings.sh_size)
        continue;
      const char *name = reinterpret_cast<const char *>(bytes.data() + strings.sh_offset + symbol.st_name);
      const size_t left = strings.sh_size - symbol.st_name;
      if (!std::memchr(name, '\0', left) || std::strcmp(name, "tohost"))
        continue;
      if (symbol.st_shndx == SHN_UNDEF || symbol.st_shndx >= header.e_shnum ||
          (symbol.st_value & 7) || symbol.st_size != 8 || address)
        throw std::invalid_argument("HartBreaker terminal has ambiguous or invalid tohost symbol");
      const auto section = read_section(header.e_shoff + uint64_t(symbol.st_shndx) * sizeof(Elf64_Shdr));
      if (!(section.sh_flags & SHF_ALLOC) || !(section.sh_flags & SHF_WRITE))
        throw std::invalid_argument("HartBreaker tohost is not allocated writable memory");
      address = symbol.st_value;
    }
  }
  uint64_t initial = 0;
  if (!address || !memory.read(address, &initial, sizeof(initial)) || initial)
    throw std::invalid_argument("HartBreaker tohost is absent or not initialized to zero in reference RAM");
  if (header.e_phentsize != sizeof(Elf64_Phdr) || !header.e_phnum ||
      !within(header.e_phoff, uint64_t(header.e_phnum) * sizeof(Elf64_Phdr)))
    throw std::invalid_argument("HartBreaker terminal requires valid executable ELF segments");
  const std::array<uint8_t, 8> parked_loop{{0x73, 0, 0x50, 0x10, 0x6f, 0xf0, 0xdf, 0xff}};
  uint64_t parked_wfi = 0;
  for (uint16_t index = 0; index < header.e_phnum; ++index) {
    Elf64_Phdr segment{};
    std::memcpy(&segment, bytes.data() + header.e_phoff + uint64_t(index) * sizeof(segment), sizeof(segment));
    if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_X))
      continue;
    if (!within(segment.p_offset, segment.p_filesz))
      throw std::invalid_argument("HartBreaker terminal has an out-of-bounds executable segment");
    for (uint64_t offset = 0; offset + parked_loop.size() <= segment.p_filesz; offset += 2) {
      if (std::memcmp(bytes.data() + segment.p_offset + offset, parked_loop.data(), parked_loop.size()))
        continue;
      if (parked_wfi || segment.p_paddr > UINT64_MAX - offset)
        throw std::invalid_argument("HartBreaker terminal parking loop is ambiguous or outside RAM");
      parked_wfi = segment.p_paddr + offset;
    }
  }
  std::array<uint8_t, 8> initialized_loop{};
  if (!parked_wfi || !memory.read(parked_wfi, initialized_loop.data(), initialized_loop.size()) ||
      initialized_loop != parked_loop)
    throw std::invalid_argument("HartBreaker WFI parking loop is not independently initialized");
  return {address, parked_wfi};
}

} // namespace difftest_s::native
#endif
