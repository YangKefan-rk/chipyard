#include "native_probe.h"
#include "VTestDriver.h"
#include <verilated.h>
#include <memory>

int main(int argc, char **argv) {
  auto context = std::make_unique<VerilatedContext>();
  context->threads(1);
  context->commandArgs(argc, argv);
  auto model = std::make_unique<VTestDriver>(context.get(), "");
  while (!context->gotFinish()) {
    model->eval();
    if (difftest_s::native::native_terminal_drain_complete())
      break;
    if (!model->eventsPending())
      break;
    context->time(model->nextTimeSlot());
  }
  model->final();
  context->statsPrintSummary();
  return difftest_s::native::native_exit_status();
}
