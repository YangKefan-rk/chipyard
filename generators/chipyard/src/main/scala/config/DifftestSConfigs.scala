package chipyard

import freechips.rocketchip.rocket.EnableDifftestRocketCache
import freechips.rocketchip.subsystem.{InSubsystem, TilesLocated}
import org.chipsalliance.cde.config.Config

class WithDifftestSObservationExport extends Config(
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

class WithDifftestSBoomObservations extends Config((site, here, up) => {
  case TilesLocated(InSubsystem) => up(TilesLocated(InSubsystem), site).map {
    case tile: boom.v4.common.BoomTileAttachParams =>
      tile.copy(tileParams = tile.tileParams.copy(
        core = tile.tileParams.core.copy(enableDifftest = true)
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
  new WithDifftestSBoomObservations ++
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

class QuadSmallBoomV4DifftestSConfig extends Config(
  new WithDifftestSObservationExport ++
  new chipyard.clocking.WithSingleClockBroadcastClockGenerator(freqMHz = 1000) ++
  new chipyard.config.WithNoTileClockGaters ++
  new chipyard.config.WithNoTileResetSetters ++
  new boom.v4.common.WithSynchronousBoomTiles ++
  new WithDifftestSBoomObservations ++
  new boom.v4.common.WithNSmallBooms(4) ++
  new chipyard.config.AbstractConfig
)
