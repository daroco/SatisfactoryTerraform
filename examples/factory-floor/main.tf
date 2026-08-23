# A range/grid placement example: tile a rectangular floor with foundations,
# then space a row of smelters across it - both via the modules/grid-2d
# pattern (see its README for why for_each + stable keys, not count).
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
  # Buildings sit 200cm above the foundation's own z, not 100 - calibrated
  # live (see mod/README.md's "Placement offset" note).
  base_z = 20000
}

# Tile a 32m x 16m floor with 8x8m foundations (4 x 2 = 8 tiles).
module "floor" {
  source  = "../../modules/grid-2d"
  from    = { x = 0, y = 0 }
  to      = { x = 3200, y = 1600 }
  spacing = 800 # one Build_Foundation_8x4_01_C tile
}

resource "satisfactory_foundation" "floor" {
  for_each = module.floor.positions
  class    = "Build_Foundation_8x4_01_C"
  x        = each.value.x
  y        = each.value.y
  z        = local.base_z
}

# Space a row of smelters across part of that floor, 5m apart - fixed
# spacing you choose, not footprint-aware auto-packing (see module README).
module "smelter_row" {
  source  = "../../modules/grid-2d"
  from    = { x = 200, y = 200 }
  to      = { x = 2800, y = 201 } # single row: to.y just past from.y
  spacing = 500
}

resource "satisfactory_building" "smelter_row" {
  for_each = module.smelter_row.positions
  class    = "Build_SmelterMk1_C"
  recipe   = "Recipe_IngotIron_C"
  x        = each.value.x
  y        = each.value.y
  z        = local.base_z + 200
}

output "foundation_count" {
  value = module.floor.count
}

output "smelter_count" {
  value = module.smelter_row.count
}
