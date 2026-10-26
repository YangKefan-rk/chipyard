package chipyard.difftest

import chisel3._
import chisel3.experimental.IntParam
import chisel3.util.{Cat, HasBlackBoxInline}
import freechips.rocketchip.util.ElaborationArtefacts

import chipyard.harness.{HarnessBinder, HasHarnessInstantiators}

private case class NativeField(path: String, signal: UInt, offset: Int) {
  val width: Int = signal.getWidth
}

private object NativePayload {
  private def leaves(path: String, signal: Data): Seq[(String, UInt)] = signal match {
    case vector: Vec[_] => vector.zipWithIndex.flatMap { case (element, index) => leaves(s"$path[$index]", element) }
    case record: Record => record.elements.toSeq.flatMap { case (name, element) => leaves(s"$path.$name", element) }
    case bits: Bits => Seq(path -> bits.asUInt)
    case other => throw new IllegalArgumentException(s"Unsupported Difftest-S native field $path: ${other.getClass.getName}")
  }

  def pack(kind: Int, hart: Int, rosterSize: Int, sramCaptureBoundary: Boolean,
           records: (String, Data)*): UInt = {
    var offset = 0
    val fields = records.flatMap { case (path, signal) => leaves(path, signal) }.map { case (path, signal) =>
      require(signal.getWidth > 0 && signal.getWidth <= 64, s"Difftest-S native field $path has unsupported width")
      val field = NativeField(path, signal, offset)
      offset += field.width
      field
    }
    require(fields.nonEmpty)
    val label = s"${kind}_$hart"
    val entries = fields.map(f => s"  {\"${f.path}\", ${f.offset}, ${f.width}}").mkString(",\n")
    val header = s"""#pragma once
        |#include "native_probe.h"
        |namespace difftest_s::native {
        |inline constexpr NativeFieldSpec fields_$label[] = {
        |$entries
        |};
        |inline constexpr NativeProbeLayout layout_$label = {
        |  1, $kind, $hart, $rosterSize, $offset,
        |  sizeof(fields_$label) / sizeof(fields_$label[0]), fields_$label, $sramCaptureBoundary
        |};
        |}
        |""".stripMargin
    val jsonFields = fields.map(f => s"{\"path\":\"${f.path}\",\"offset\":${f.offset},\"width\":${f.width}}").mkString(",")
    ElaborationArtefacts.add(s"difftest-s-layout-$label.h", header)
    ElaborationArtefacts.add(s"difftest-s-layout-$label.json",
      s"{\"abi\":1,\"kind\":$kind,\"hart\":$hart,\"harts\":$rosterSize,\"bits\":$offset,\"sram_capture_boundary\":$sramCaptureBoundary,\"fields\":[$jsonFields]}")
    Cat(fields.reverse.map(_.signal))
  }
}

/** One synchronous source snapshot per positive edge. DPI sees pre-NBA values,
  * the same values consumed by the native pipeline at that edge. transport_tick
  * survives reset and frames the roster only; it is never an observation rank.
  */
private class DifftestSNativeProbe(kind: Int, hart: Int, rosterSize: Int, width: Int)
    extends BlackBox(Map("KIND" -> IntParam(kind), "HART" -> IntParam(hart),
      "HARTS" -> IntParam(rosterSize), "PAYLOAD_BITS" -> IntParam(width))) with HasBlackBoxInline {
  val io = IO(new Bundle {
    val clock = Input(Clock())
    val reset = Input(Bool())
    val epoch = Input(UInt(64.W))
    val cycle = Input(UInt(64.W))
    val payload = Input(UInt(width.W))
  })
  setInline("DifftestSNativeProbe.sv", """
    |module DifftestSNativeProbe #(
    |  parameter int KIND = 0, HART = 0, HARTS = 0, PAYLOAD_BITS = 1
    |)(input clock, input reset, input [63:0] epoch, input [63:0] cycle,
    |  input [PAYLOAD_BITS-1:0] payload);
    |  import "DPI-C" function void difftest_s_native_sample(
    |    input int abi_version, input int kind, input int hart, input int harts,
    |    input longint unsigned transport_tick, input bit reset,
    |    input longint unsigned epoch, input longint unsigned source_cycle,
    |    input int payload_bits, input bit [PAYLOAD_BITS-1:0] words);
    |  import "DPI-C" function void difftest_s_native_finish();
    |  longint unsigned transport_tick = 0;
    |  always @(posedge clock) begin
    |    difftest_s_native_sample(1, KIND, HART, HARTS, transport_tick,
    |      reset, epoch, cycle, PAYLOAD_BITS, payload);
    |    transport_tick = transport_tick + 1;
    |  end
    |  final difftest_s_native_finish();
    |endmodule
    |""".stripMargin)
}

class WithDifftestSNativeSimulation extends HarnessBinder({
  case (harness: HasHarnessInstantiators, port: DifftestSBoomPort, chipId: Int) =>
    require(chipId == 0, "Difftest-S native transport currently supports one chip")
    val rosterSize = harness.chipParameters(chipId)(freechips.rocketchip.subsystem.NumTiles)
    val observation = port.io
    val payload = NativePayload.pack(2, port.tileId, rosterSize, port.sramCaptureBoundary,
      "core" -> observation.core, "lsu" -> observation.lsu, "cache" -> observation.cache)
    val probe = Module(new DifftestSNativeProbe(2, port.tileId, rosterSize, payload.getWidth))
    probe.io.clock := observation.source.clock
    probe.io.reset := observation.source.reset
    probe.io.epoch := observation.source.epoch
    probe.io.cycle := observation.source.cycle
    probe.io.payload := payload

  case (harness: HasHarnessInstantiators, port: DifftestSRocketPort, chipId: Int) =>
    require(chipId == 0, "Difftest-S native transport currently supports one chip")
    val rosterSize = harness.chipParameters(chipId)(freechips.rocketchip.subsystem.NumTiles)
    val observation = port.io
    val payload = NativePayload.pack(1, port.tileId, rosterSize, false,
      "core" -> observation.core, "cache" -> observation.cache,
      "coreRequestor" -> observation.coreRequestor, "cacheRequestors" -> observation.cacheRequestors)
    val probe = Module(new DifftestSNativeProbe(1, port.tileId, rosterSize, payload.getWidth))
    probe.io.clock := observation.source.clock
    probe.io.reset := observation.source.reset
    probe.io.epoch := observation.source.epoch
    probe.io.cycle := observation.source.cycle
    probe.io.payload := payload
})
