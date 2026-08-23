# A hub-and-spoke factory: a large square floor (room for a lot more
# machines than are wired up below) with a merger/splitter pair at the
# center acting as a single "central input" / "central output" - every
# producer belts into the merger, every consumer belts out of the splitter.
#
# Connector indices for the merger/splitter were determined empirically
# (spawn one, probe connectors 0-3 via POST /connections, read back the
# resulting belt's transform to see which side of the building each index
# sits on - see mod/README.md's "Connections" section for the general
# technique). Confirmed live against a running game session:
#
#   Build_ConveyorAttachmentMerger_C     Build_ConveyorAttachmentSplitter_C
#     0 = input  (back,  -x)               0 = output (front, +x)
#     1 = output (front, +x)               1 = input  (back,  -x)
#     2 = input  (side,  +y)               2 = output (side,  +y)
#     3 = input  (side,  -y)               3 = output (side,  -y)
#
# A merger only has 3 inputs and a splitter only has 3 outputs - that's a
# real in-game constraint (one belt per connector), which is why this
# topology tops out at 3 producers / 3 consumers per hub even though the
# floor has room for many more machines. Scale by adding more hubs, not
# more spokes per hub.
#
# Local dev against the mock: see the mock-stack skill
# (.claude/skills/mock-stack/SKILL.md) - same loop as examples/iron-plate-line.

terraform {
  required_providers {
    satisfactory = {
      source = "daroco/satisfactory"
    }
  }
}

provider "satisfactory" {
  # endpoint defaults to http://localhost:8090 (or SATISFACTORY_ENDPOINT)
}

locals {
  # Centered on the player's location (debug HUD: V(X=-223975.55,
  # Y=-31243.68, Z=5793.27), creative-mode save) rather than world origin,
  # so the hub actually lands where whoever applies this is standing.
  # base_z uses the player's own Z as the foundation top, same as the
  # earlier single-foundation smoke test that landed "literally right on
  # top of me." Update these three values (and re-apply) any time this
  # example moves to a different save/session - the mod's tf_id registry
  # is persisted per save game, so it starts empty on a new one.
  base_z   = 5793.27
  # +200, not +100 - calibrated live by placing a row of constructors at
  # several heights on a foundation and eyeballing which one sat flush
  # (see mod/README.md's "Placement offset" note). Machines appear to be
  # pivoted 2m above their own base, so they need base_z + half the
  # foundation's 4m thickness to land on the surface instead of embedded
  # in it.
  build_z  = local.base_z + 200
  # Conveyor attachments (merger/splitter) are a different mesh/pivot than
  # manufacturers - +200 left them still visibly embedded. Recalibrated
  # separately the same way: +300.
  attachment_z = local.base_z + 300
  center_x = -223975.55
  center_y = -31243.68
}

# 64m x 64m floor (8x8 tiles of an 8x8m foundation) centered on the player -
# plenty of room left over for more machines beyond the 6 wired up here.
module "floor" {
  source  = "../../modules/grid-2d"
  from    = { x = local.center_x - 3200, y = local.center_y - 3200 }
  to      = { x = local.center_x + 3200, y = local.center_y + 3200 }
  spacing = 800
}

resource "satisfactory_foundation" "floor" {
  for_each = module.floor.positions
  class    = "Build_Foundation_8x4_01_C"
  x        = each.value.x
  y        = each.value.y
  z        = local.base_z
}

# --- Central hub: merger (input) -> splitter (output) ---------------------

resource "satisfactory_building" "merger" {
  class = "Build_ConveyorAttachmentMerger_C"
  x     = local.center_x - 400
  y     = local.center_y
  z     = local.attachment_z
}

resource "satisfactory_building" "splitter" {
  class = "Build_ConveyorAttachmentSplitter_C"
  x     = local.center_x + 400
  y     = local.center_y
  z     = local.attachment_z
}

resource "satisfactory_belt" "hub_core" {
  class          = "Build_ConveyorBeltMk1_C"
  from_id        = satisfactory_building.merger.id
  from_connector = 1 # merger output
  to_id          = satisfactory_building.splitter.id
  to_connector   = 1 # splitter input
}

# --- Producers: 3 smelters feeding the merger's 3 inputs -------------------

locals {
  producers = {
    # yaw turns the machine so its output connector faces the hub (adjusted
    # by eye against the live build - every map entry needs the key, since
    # Terraform requires map-of-object entries to share one shape).
    west  = { x = local.center_x - 2400, y = local.center_y, merger_connector = 0, yaw = 90 }     # merger back input
    south = { x = local.center_x - 400, y = local.center_y + 2400, merger_connector = 2, yaw = 0 } # merger +y input
    north = { x = local.center_x - 400, y = local.center_y - 2400, merger_connector = 3, yaw = 0 } # merger -y input
  }
}

resource "satisfactory_building" "producer" {
  for_each = local.producers
  class    = "Build_SmelterMk1_C"
  recipe   = "Recipe_IngotIron_C"
  x        = each.value.x
  y        = each.value.y
  z        = local.build_z
  yaw      = each.value.yaw
}

resource "satisfactory_belt" "inbound" {
  for_each       = local.producers
  class          = "Build_ConveyorBeltMk1_C"
  from_id        = satisfactory_building.producer[each.key].id
  from_connector = 1 # smelter output
  to_id          = satisfactory_building.merger.id
  to_connector   = each.value.merger_connector
}

# --- Consumers: 3 constructors fed by the splitter's 3 outputs -------------

locals {
  consumers = {
    # yaw: see the note on local.producers.
    east  = { x = local.center_x + 2400, y = local.center_y, splitter_connector = 0, yaw = 90 }    # splitter front output
    south = { x = local.center_x + 400, y = local.center_y + 2400, splitter_connector = 2, yaw = 0 } # splitter +y output
    north = { x = local.center_x + 400, y = local.center_y - 2400, splitter_connector = 3, yaw = 0 } # splitter -y output
  }
}

resource "satisfactory_building" "consumer" {
  for_each = local.consumers
  class    = "Build_ConstructorMk1_C"
  recipe   = "Recipe_IronPlate_C"
  x        = each.value.x
  y        = each.value.y
  z        = local.build_z
  yaw      = each.value.yaw
}

resource "satisfactory_belt" "outbound" {
  for_each       = local.consumers
  class          = "Build_ConveyorBeltMk1_C"
  from_id        = satisfactory_building.splitter.id
  from_connector = each.value.splitter_connector
  to_id          = satisfactory_building.consumer[each.key].id
  to_connector   = 0 # constructor input
}

output "foundation_count" {
  value = module.floor.count
}
