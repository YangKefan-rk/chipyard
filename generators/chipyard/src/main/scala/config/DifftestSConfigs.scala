package chipyard

import freechips.rocketchip.rocket.{EnableDifftestRocketCache, EnableSfuzzRocketFunctional, RocketDCacheConfig}
import freechips.rocketchip.subsystem.{InSubsystem, TilesLocated}
import org.chipsalliance.cde.config.Config

class WithDifftestSObservationExport extends Config(
  new chipyard.difftest.WithDifftestSNativeSimulation ++
  new chipyard.difftest.WithDifftestSObservationPunchthrough ++
  new Config((site, here, up) => {
    case chipyard.difftest.DifftestSObservationKey => true
  })
)

class WithDifftestSRocketObservations extends Config((site, here, up) => {
  case EnableDifftestRocketCache => true
})

class DualRocketDifftestSConfig extends Config(
  new WithDifftestSObservationExport ++
  new chipyard.clocking.WithSingleClockBroadcastClockGenerator(freqMHz = 1000) ++
  new chipyard.config.WithNoTileClockGaters ++
  new chipyard.config.WithNoTileResetSetters ++
  new freechips.rocketchip.rocket.WithSynchronousCDCs ++
  new WithDifftestSRocketObservations ++
  new DualRocketConfig
)

class WithDifftestSBoomObservations(sramCaptureBoundary: Boolean = false) extends Config((site, here, up) => {
  case TilesLocated(InSubsystem) => up(TilesLocated(InSubsystem), site).map {
    case tile: boom.v4.common.BoomTileAttachParams =>
      tile.copy(tileParams = tile.tileParams.copy(
        core = tile.tileParams.core.copy(
          enableDifftest = true, difftestSramCaptureBoundary = sramCaptureBoundary)
      ))
    case other => other
  }
})

class DualSmallBoomV4DifftestSConfig extends Config(
  new WithDifftestSObservationExport ++
  new chipyard.clocking.WithSingleClockBroadcastClockGenerator(freqMHz = 1000) ++
  new chipyard.config.WithNoTileClockGaters ++
  new chipyard.config.WithNoTileResetSetters ++
  new boom.v4.common.WithSynchronousBoomTiles ++
  new WithDifftestSBoomObservations(sramCaptureBoundary = true) ++
  new DualSmallBoomV4Config
)

class QuadRocketDifftestSConfig extends Config(
  new WithDifftestSObservationExport ++
  new chipyard.clocking.WithSingleClockBroadcastClockGenerator(freqMHz = 1000) ++
  new chipyard.config.WithNoTileClockGaters ++
  new chipyard.config.WithNoTileResetSetters ++
  new freechips.rocketchip.rocket.WithSynchronousCDCs ++
  new WithDifftestSRocketObservations ++
  new freechips.rocketchip.rocket.WithNBigCores(4) ++
  new chipyard.config.AbstractConfig
)

// Preserve QuadRocketDifftestSConfig's original native layout by default.
// Enabling this variant appends passive L1 observations to the existing
// four-hart Difftest-S native source; it does not modify cache behavior.
class WithSfuzzRocketFunctionalProbe extends Config((site, here, up) => {
  case EnableSfuzzRocketFunctional => true
})

class QuadRocketSFuzzDifftestSConfig extends Config(
  new WithSfuzzRocketFunctionalProbe ++
  new QuadRocketDifftestSConfig
)

class WithDifftestSRocketStoreMerging extends RocketDCacheConfig(_.copy(
  dataECC = Some("parity"), dataECCBytes = 2
))

class QuadRocketMergeDifftestSConfig extends Config(
  new WithDifftestSRocketStoreMerging ++
  new QuadRocketDifftestSConfig
)

class QuadSmallBoomV4DifftestSConfig extends Config(
  new WithDifftestSObservationExport ++
  new chipyard.clocking.WithSingleClockBroadcastClockGenerator(freqMHz = 1000) ++
  new chipyard.config.WithNoTileClockGaters ++
  new chipyard.config.WithNoTileResetSetters ++
  new boom.v4.common.WithSynchronousBoomTiles ++
  new WithDifftestSBoomObservations(sramCaptureBoundary = true) ++
  new boom.v4.common.WithNSmallBooms(4) ++
  new chipyard.config.AbstractConfig
)
