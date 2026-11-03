#ifndef DIFFTEST_S_BOOM_SEMANTIC_EXPORT_H
#define DIFFTEST_S_BOOM_SEMANTIC_EXPORT_H

#include "rvwmo/event.h"
#include <cstdint>
#include <string>
#include <vector>

namespace rvwmo { class HostSession; }

namespace difftest_s::native {

// Only a source-closed checker-accepted tap. Feedback never supplies a REF
// value, a memory-order edge or a successful checker certificate.
class BoomSemanticExport {
public:
  explicit BoomSemanticExport(const char *elf);
  void observe(const rvwmo::RuntimeEvent &event);
  void publish(const rvwmo::HostSession &session);

private:
  std::string output_, case_id_, run_id_, elf_sha256_, backend_sha256_, layout_sha256_;
  uint64_t last_cycle_ = 0;
  bool published_ = false;
  std::vector<rvwmo::RuntimeEvent> observed_;
};

} // namespace difftest_s::native
#endif
