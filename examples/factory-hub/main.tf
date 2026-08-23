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
  base_z   = 20000
  build_z  = local.base_z + 100
  center_x = 3200
  center_y = 3200
}

# 64m x 64m floor (8x8 tiles of an 8x8m foundation) - plenty of room left
# over for more machines beyond the 6 wired up here.
module "floor" {
  source  = "../../modules/grid-2d"
  from    = { x = 0, y = 0 }
  to      = { x = 6400, y = 6400 }
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
  z     = local.build_z
}

resource "satisfactory_building" "splitter" {
  class = "Build_ConveyorAttachmentSplitter_C"
  x     = local.center_x + 400
  y     = local.center_y
  z     = local.build_z
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
    west  = { x = 800, y = local.center_y, merger_connector = 0 } # merger back input
    south = { x = local.center_x - 400, y = 5600, merger_connector = 2 } # merger +y input
    north = { x = local.center_x - 400, y = 800, merger_connector = 3 } # merger -y input
  }
}

resource "satisfactory_building" "producer" {
  for_each = local.producers
  class    = "Build_SmelterMk1_C"
  recipe   = "Recipe_IngotIron_C"
  x        = each.value.x
  y        = each.value.y
  z        = local.build_z
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
    east  = { x = 5600, y = local.center_y, splitter_connector = 0 } # splitter front output
    south = { x = local.center_x + 400, y = 5600, splitter_connector = 2 } # splitter +y output
    north = { x = local.center_x + 400, y = 800, splitter_connector = 3 } # splitter -y output
  }
}

resource "satisfactory_building" "consumer" {
  for_each = local.consumers
  class    = "Build_ConstructorMk1_C"
  recipe   = "Recipe_IronPlate_C"
  x        = each.value.x
  y        = each.value.y
  z        = local.build_z
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
