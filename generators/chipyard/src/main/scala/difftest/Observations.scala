package chipyard.difftest

import chisel3._
import chisel3.util.experimental.BoringUtils
import freechips.rocketchip.diplomacy.{InModuleBody, ModuleValue}
import freechips.rocketchip.prci.SynchronousCrossing
import freechips.rocketchip.rocket.{DCacheObservation, RocketCoreObservation}
import freechips.rocketchip.tile.RocketTile
import org.chipsalliance.cde.config.Field

import chipyard.ChipyardSystem
import chipyard.iobinders.{OverrideLazyIOBinder, Port}

case object DifftestSObservationKey extends Field[Boolean](false)

class DifftestSSource extends Bundle {
  val clock = Clock()
  val reset = Bool()
  val epoch = UInt(64.W)
  val cycle = UInt(64.W)
}

class DifftestSRocketObservation(coreType: RocketCoreObservation, cacheType: DCacheObservation) extends Bundle {
  val source = new DifftestSSource
  val core = coreType.cloneType
  val cache = cacheType.cloneType
  val coreRequestor = UInt(32.W)
  val cacheRequestors = UInt(32.W)
}

class DifftestSBoomObservation(coreWidth: Int, robEntries: Int, lsuWidth: Int) extends Bundle {
  val source = new DifftestSSource
  val core = new boom.v4.difftest.DifftestCoreObservation(coreWidth, robEntries)
  val lsu = new boom.v4.difftest.DifftestLsuObservation(lsuWidth, coreWidth)
  val cache = new boom.v4.difftest.DifftestCacheObservation(lsuWidth)
}

case class DifftestSRocketPort(getIO: () => DifftestSRocketObservation, tileId: Int)
    extends Port[DifftestSRocketObservation]

case class DifftestSBoomPort(getIO: () => DifftestSBoomObservation, tileId: Int,
                            sramCaptureBoundary: Boolean = false)
    extends Port[DifftestSBoomObservation]

/** Private DUT observations. The source clock is exported; no receive-time
  * timestamp, cross-hart phase, watermark, or memory-order rank is inferred.
  */
trait CanHaveDifftestSObservations { this: ChipyardSystem =>
  val difftestSPorts: Option[ModuleValue[Seq[Port[_]]]] = Option.when(p(DifftestSObservationKey)) {
    require(clusterAttachParams.isEmpty, "Difftest-S observation export does not support clustered tiles")
    require(tileAttachParams.nonEmpty, "Difftest-S observations require at least one tile")
    require(tileCrossingTypes.forall(_.isInstanceOf[SynchronousCrossing]),
      "Difftest-S observation export requires synchronous tile crossings")

    val boomControls = tile_prci_domains.toSeq.flatMap { case (tileId, domain) => domain.element match {
      case boomTile: boom.v4.common.BoomTile =>
        Some(tileId -> domain.element_reset_domain {
          InModuleBody {
            // A duplex parent wire is a legal WiringTransform sink. Targeting
            // the child's Input directly gives that sink source flow.
            val control = Wire(new boom.v4.difftest.DifftestControl)
              .suggestName(s"difftest_s_control_$tileId")
            boomTile.module.difftest_control.get := control
            control
          }
        })
      case _ => None
    }}.toMap

    InModuleBody {
      val sourceClock = BoringUtils.bore(totalTiles.values.head.module.clock)
      val sourceReset = BoringUtils.bore(totalTiles.values.head.module.reset)
      val cycle = withClockAndReset(sourceClock, sourceReset) {
        val count = RegInit(0.U(64.W))
        when (!sourceReset.asBool) {
          assert(!count.andR, "Difftest-S source cycle exhausted")
          count := count + 1.U
        }
        count
      }

      def source(port: DifftestSSource, clock: Clock, reset: Reset): Unit = {
        port.clock := BoringUtils.bore(clock)
        port.reset := BoringUtils.bore(reset).asBool
        // This export describes one cold-reset run; a new run requires a new host session.
        port.epoch := 1.U
        port.cycle := cycle
      }

      totalTiles.toSeq.map { case (tileId, tile) => tile match {
        case rocket: RocketTile =>
          require(!rocket.rocketParams.core.clockGate && !rocket.rocketParams.dcache.get.clockGate,
            "Difftest-S Rocket observation export requires ungated core and cache clocks")
          val core = rocket.module.core.io.difftest.getOrElse(
            throw new IllegalArgumentException("Difftest-S Rocket core observations are not enabled"))
          val cache = rocket.dcache.module.io.difftest.getOrElse(
            throw new IllegalArgumentException("Difftest-S Rocket cache observations are not enabled"))
          val port = IO(Output(new DifftestSRocketObservation(core, cache)))
            .suggestName(s"difftest_s_rocket_$tileId")
          source(port.source, rocket.module.clock, rocket.module.reset)
          port.core := BoringUtils.bore(core)
          port.cache := BoringUtils.bore(cache)
          port.coreRequestor := BoringUtils.bore(rocket.module.difftestCoreRequestor.get)
          port.cacheRequestors := BoringUtils.bore(rocket.module.difftestCacheRequestors.get)
          DifftestSRocketPort(() => port, tileId)

        case boomTile: boom.v4.common.BoomTile =>
          val core = boomTile.boomParams.core
          require(core.enableDifftest, "Difftest-S BOOM observations are not enabled")
          require(!core.clockGate, "Difftest-S BOOM observation export requires an ungated core clock")
          val port = IO(Output(new DifftestSBoomObservation(core.decodeWidth, core.numRobEntries, core.lsuWidth)))
            .suggestName(s"difftest_s_boom_$tileId")
          source(port.source, boomTile.module.clock, boomTile.module.reset)
          port.core := BoringUtils.bore(boomTile.module.difftest_core.get)
          port.lsu := BoringUtils.bore(boomTile.module.difftest_lsu.get)
          port.cache := BoringUtils.bore(boomTile.module.difftest_cache.get)
          val control = boomControls(tileId).getWrappedValue
          BoringUtils.bore(port.source.epoch, Seq(control.epoch))
          BoringUtils.bore(port.source.cycle, Seq(control.cycle))
          DifftestSBoomPort(() => port, tileId,
            core.difftestSramCaptureBoundary && core.numDCacheBanks == 1 && !core.dcacheSinglePorted)

        case other =>
          throw new IllegalArgumentException(s"Difftest-S observation export does not support ${other.getClass.getName}")
      }}
    }
  }
}

class WithDifftestSObservationPunchthrough extends OverrideLazyIOBinder({
  (system: CanHaveDifftestSObservations) => InModuleBody {
    val ports = system.difftestSPorts.toSeq.flatMap(_.getWrappedValue).map {
      case rocket: DifftestSRocketPort =>
        val port = IO(Output(chiselTypeOf(rocket.getIO())))
          .suggestName(s"difftest_s_rocket_${rocket.tileId}")
        port := rocket.getIO()
        DifftestSRocketPort(() => port, rocket.tileId)
      case boom: DifftestSBoomPort =>
        val port = IO(Output(chiselTypeOf(boom.getIO())))
          .suggestName(s"difftest_s_boom_${boom.tileId}")
        port := boom.getIO()
        DifftestSBoomPort(() => port, boom.tileId, boom.sramCaptureBoundary)
    }
    (ports, Nil)
  }
})
