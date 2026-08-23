# A minimal iron plate line: a foundation pad, a smelter feeding a
# constructor by belt, and a power line between them.
#
# Local dev against the mock:
#   go build -o /tmp/terraform-provider-satisfactory .
#   go run ./cmd/mockserver &
#   TF_CLI_CONFIG_FILE=<dev_overrides rc>  terraform apply
# Against the real game: run Satisfactory with the SatisfactoTerraform mod
# loaded and just `terraform apply`.

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
  # Grid-ish layout in centimetres; one foundation is 800 x 800.
  # Buildings sit 200cm above the foundation's own z, not 100 - calibrated
  # live (see mod/README.md's "Placement offset" note); a machine placed at
  # base_z + 100 lands partway embedded in the foundation.
  base_z = 20000
}

resource "satisfactory_foundation" "pad" {
  count = 4
  class = "Build_Foundation_8x4_01_C"
  x     = 800 * count.index
  y     = 0
  z     = local.base_z
}

resource "satisfactory_building" "smelter" {
  class  = "Build_SmelterMk1_C"
  x      = 200
  y      = 0
  z      = local.base_z + 200
  recipe = "Recipe_IngotIron_C"
}

resource "satisfactory_building" "constructor" {
  class       = "Build_ConstructorMk1_C"
  x           = 2200
  y           = 0
  z           = local.base_z + 200
  recipe      = "Recipe_IronPlate_C"
  clock_speed = 1.0
}

resource "satisfactory_belt" "ingots" {
  class          = "Build_ConveyorBeltMk1_C"
  from_id        = satisfactory_building.smelter.id
  from_connector = 1 # smelter output
  to_id          = satisfactory_building.constructor.id
  to_connector   = 0 # constructor input
}

resource "satisfactory_power_line" "power" {
  from_id        = satisfactory_building.smelter.id
  from_connector = 0
  to_id          = satisfactory_building.constructor.id
  to_connector   = 0
}

output "building_ids" {
  value = {
    smelter     = satisfactory_building.smelter.id
    constructor = satisfactory_building.constructor.id
  }
}
