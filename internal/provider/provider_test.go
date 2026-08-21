package provider_test

import (
	"context"
	"fmt"
	"net/http/httptest"
	"regexp"
	"testing"

	"github.com/hashicorp/terraform-plugin-framework/providerserver"
	"github.com/hashicorp/terraform-plugin-go/tfprotov6"
	"github.com/hashicorp/terraform-plugin-testing/helper/resource"
	"github.com/hashicorp/terraform-plugin-testing/terraform"

	"github.com/daroco/satisfacto-form/internal/api"
	"github.com/daroco/satisfacto-form/internal/client"
	"github.com/daroco/satisfacto-form/internal/mockserver"
	"github.com/daroco/satisfacto-form/internal/provider"
)

// captureID stores the resource's current id in *dst for later steps.
func captureID(name string, dst *string) resource.TestCheckFunc {
	return func(s *terraform.State) error {
		rs, ok := s.RootModule().Resources[name]
		if !ok {
			return fmt.Errorf("resource %s not in state", name)
		}
		*dst = rs.Primary.ID
		return nil
	}
}

// checkSameID asserts the resource kept the id captured earlier (in-place update).
func checkSameID(name string, want *string) resource.TestCheckFunc {
	return func(s *terraform.State) error {
		rs, ok := s.RootModule().Resources[name]
		if !ok {
			return fmt.Errorf("resource %s not in state", name)
		}
		if rs.Primary.ID != *want {
			return fmt.Errorf("id changed from %s to %s: update replaced instead of patching", *want, rs.Primary.ID)
		}
		return nil
	}
}

// startMock spins up a fresh in-memory world and points the provider at it via
// the endpoint env var so test configs need no provider block arguments.
func startMock(t *testing.T) *client.Client {
	t.Helper()
	srv := httptest.NewServer(mockserver.New("").Handler())
	t.Cleanup(srv.Close)
	t.Setenv("SATISFACTORY_ENDPOINT", srv.URL)
	return client.New(srv.URL, "")
}

var protoFactories = map[string]func() (tfprotov6.ProviderServer, error){
	"satisfactory": providerserver.NewProtocol6WithError(provider.New("test")()),
}

func TestAccBuilding(t *testing.T) {
	c := startMock(t)

	config := `
resource "satisfactory_building" "smelter" {
  class  = "Build_SmelterMk1_C"
  x      = 0
  y      = 0
  z      = 100
  recipe = "Recipe_IngotIron_C"
}
`
	updated := `
resource "satisfactory_building" "smelter" {
  class       = "Build_SmelterMk1_C"
  x           = 0
  y           = 0
  z           = 100
  recipe      = "Recipe_IngotIron_C"
  clock_speed = 1.5
}
`
	var firstID string
	resource.Test(t, resource.TestCase{
		ProtoV6ProviderFactories: protoFactories,
		Steps: []resource.TestStep{
			{
				Config: config,
				Check: resource.ComposeTestCheckFunc(
					resource.TestCheckResourceAttr("satisfactory_building.smelter", "clock_speed", "1"),
					resource.TestCheckResourceAttrSet("satisfactory_building.smelter", "id"),
					captureID("satisfactory_building.smelter", &firstID),
				),
			},
			{
				// clock_speed changes in place: same id afterwards.
				Config: updated,
				Check: resource.ComposeTestCheckFunc(
					resource.TestCheckResourceAttr("satisfactory_building.smelter", "clock_speed", "1.5"),
					checkSameID("satisfactory_building.smelter", &firstID),
				),
			},
			{
				// Simulate the pioneer dismantling it in-game: refresh must
				// detect the loss and plan a recreate.
				PreConfig: func() {
					if err := c.DeleteBuildable(context.Background(), firstID); err != nil {
						t.Fatalf("out-of-band delete: %v", err)
					}
				},
				Config:             updated,
				PlanOnly:           true,
				ExpectNonEmptyPlan: true,
			},
		},
	})
}

func TestAccBuildingInvalidClass(t *testing.T) {
	startMock(t)
	resource.Test(t, resource.TestCase{
		ProtoV6ProviderFactories: protoFactories,
		Steps: []resource.TestStep{
			{
				Config: `
resource "satisfactory_building" "bad" {
  class = "NotABuildable"
  x     = 0
  y     = 0
  z     = 0
}
`,
				ExpectError: regexp.MustCompile(`class must be a buildable class name`),
			},
		},
	})
}

func TestAccFullLine(t *testing.T) {
	startMock(t)

	config := `
resource "satisfactory_foundation" "pad" {
  class = "Build_Foundation_8x4_01_C"
  x     = 0
  y     = 0
  z     = 0
}

resource "satisfactory_building" "smelter" {
  class  = "Build_SmelterMk1_C"
  x      = 0
  y      = 0
  z      = 100
  recipe = "Recipe_IngotIron_C"
}

resource "satisfactory_building" "constructor" {
  class  = "Build_ConstructorMk1_C"
  x      = 1000
  y      = 0
  z      = 100
  recipe = "Recipe_IronPlate_C"
}

resource "satisfactory_belt" "smelter_to_constructor" {
  class          = "Build_ConveyorBeltMk1_C"
  from_id        = satisfactory_building.smelter.id
  from_connector = 1
  to_id          = satisfactory_building.constructor.id
  to_connector   = 0
}

resource "satisfactory_power_line" "power" {
  from_id        = satisfactory_building.smelter.id
  from_connector = 0
  to_id          = satisfactory_building.constructor.id
  to_connector   = 0
}
`
	resource.Test(t, resource.TestCase{
		ProtoV6ProviderFactories: protoFactories,
		Steps: []resource.TestStep{
			{
				Config: config,
				Check: resource.ComposeTestCheckFunc(
					resource.TestCheckResourceAttrSet("satisfactory_belt.smelter_to_constructor", "id"),
					resource.TestCheckResourceAttr("satisfactory_power_line.power", "class", "Build_PowerLine_C"),
					resource.TestCheckResourceAttrPair(
						"satisfactory_belt.smelter_to_constructor", "from_id",
						"satisfactory_building.smelter", "id",
					),
				),
			},
			{
				// Re-applying the same config must be a no-op.
				Config:   config,
				PlanOnly: true,
			},
		},
	})
}

func TestAccBuildingImport(t *testing.T) {
	c := startMock(t)
	if _, err := c.CreateBuildable(context.Background(), api.Buildable{
		TFID:       "imported-1",
		Class:      "Build_SmelterMk1_C",
		Transform:  api.Transform{X: 5, Y: 6, Z: 7},
		Recipe:     "Recipe_IngotIron_C",
		ClockSpeed: 1.0,
	}); err != nil {
		t.Fatalf("seed: %v", err)
	}

	resource.Test(t, resource.TestCase{
		ProtoV6ProviderFactories: protoFactories,
		Steps: []resource.TestStep{
			{
				Config: `
resource "satisfactory_building" "smelter" {
  class  = "Build_SmelterMk1_C"
  x      = 5
  y      = 6
  z      = 7
  recipe = "Recipe_IngotIron_C"
}
`,
				ResourceName:  "satisfactory_building.smelter",
				ImportState:   true,
				ImportStateId: "imported-1",
				ImportStateCheck: func(states []*terraform.InstanceState) error {
					if len(states) != 1 {
						return fmt.Errorf("expected 1 state, got %d", len(states))
					}
					if got := states[0].Attributes["class"]; got != "Build_SmelterMk1_C" {
						return fmt.Errorf("imported class = %q", got)
					}
					return nil
				},
			},
		},
	})
}
