#ifndef DIFFTEST_S_BOOM_SEMANTIC_LONG_EXPORT_H
#define DIFFTEST_S_BOOM_SEMANTIC_LONG_EXPORT_H

#include "rvwmo/event.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace difftest_s::native {

// The caller must supply the terminal checker's verified source watermarks.
// No case-local manifest is published before that terminal Pass.
class BoomSemanticLongExport {
public:
  BoomSemanticLongExport(std::string output, std::string case_id, std::string run_id,
      std::string elf_sha256, std::string backend_sha256, std::string layout_sha256,
      uint64_t first_cycle, uint64_t max_cycle_budget, uint64_t chunk_cycles, uint64_t max_pair_gap_cycles);
  ~BoomSemanticLongExport();
  BoomSemanticLongExport(const BoomSemanticLongExport &) = delete;
  BoomSemanticLongExport &operator=(const BoomSemanticLongExport &) = delete;

  void observe(const rvwmo::RuntimeEvent &event);
  void publish(const std::map<uint32_t, rvwmo::SourcePosition> &verified_watermarks,
      uint64_t checked_completed_cycle);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace difftest_s::native
#endif
