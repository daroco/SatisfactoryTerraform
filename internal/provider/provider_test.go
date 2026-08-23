package provider_test

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"regexp"
	"strings"
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

// checkDifferentID asserts the resource's current id differs from the one
// previously captured in *avoid (a RequiresReplace attribute forced a
// recreate rather than an in-place update), then updates *avoid to the new
// id so a later step in the same test can chain another such assertion.
func checkDifferentID(name string, avoid *string) resource.TestCheckFunc {
	return func(s *terraform.State) error {
		rs, ok := s.RootModule().Resources[name]
		if !ok {
			return fmt.Errorf("resource %s not in state", name)
		}
		if rs.Primary.ID == *avoid {
			return fmt.Errorf("id stayed %s: change should have forced replacement", *avoid)
		}
		*avoid = rs.Primary.ID
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

// TestAccBuildingReplaceOnCoordinateAndClassChange verifies the
// RequiresReplace plan modifiers on x/class actually force a recreate
// end to end (schema-level coverage of every RequiresReplace attribute
// lives in resource_internal_test.go; this confirms the behavior survives
// a real plan/apply cycle through terraform-plugin-testing).
func TestAccBuildingReplaceOnCoordinateAndClassChange(t *testing.T) {
	startMock(t)

	base := `
resource "satisfactory_building" "smelter" {
  class = "Build_SmelterMk1_C"
  x     = 0
  y     = 0
  z     = 100
}
`
	movedX := `
resource "satisfactory_building" "smelter" {
  class = "Build_SmelterMk1_C"
  x     = 500
  y     = 0
  z     = 100
}
`
	newClass := `
resource "satisfactory_building" "smelter" {
  class = "Build_SmelterMk2_C"
  x     = 500
  y     = 0
  z     = 100
}
`
	var id string
	resource.Test(t, resource.TestCase{
		ProtoV6ProviderFactories: protoFactories,
		Steps: []resource.TestStep{
			{
				Config: base,
				Check:  captureID("satisfactory_building.smelter", &id),
			},
			{
				// x changed: RequiresReplace, expect a new id.
				Config: movedX,
				Check:  checkDifferentID("satisfactory_building.smelter", &id),
			},
			{
				// class changed: RequiresReplace again.
				Config: newClass,
				Check:  checkDifferentID("satisfactory_building.smelter", &id),
			},
		},
	})
}

// TestAccFoundationLifecycle covers satisfactory_foundation on its own -
// previously it only appeared bundled into TestAccFullLine. It checks a
// clean create, a stable no-op re-apply, and in-game-dismantle drift
// detection (404 on Read -> state removal -> non-empty recreate plan, per
// CLAUDE.md's error contract).
func TestAccFoundationLifecycle(t *testing.T) {
	c := startMock(t)

	config := `
resource "satisfactory_foundation" "pad" {
  class = "Build_Foundation_8x4_01_C"
  x     = 0
  y     = 0
  z     = 0
}
`
	var id string
	resource.Test(t, resource.TestCase{
		ProtoV6ProviderFactories: protoFactories,
		Steps: []resource.TestStep{
			{
				Config: config,
				Check: resource.ComposeTestCheckFunc(
					resource.TestCheckResourceAttr("satisfactory_foundation.pad", "yaw", "0"),
					resource.TestCheckResourceAttrSet("satisfactory_foundation.pad", "id"),
					captureID("satisfactory_foundation.pad", &id),
				),
			},
			{
				// Re-applying identical config must be a no-op.
				Config:   config,
				PlanOnly: true,
			},
			{
				// Simulate dismantling it in-game.
				PreConfig: func() {
					if err := c.DeleteBuildable(context.Background(), id); err != nil {
						t.Fatalf("out-of-band delete: %v", err)
					}
				},
				Config:             config,
				PlanOnly:           true,
				ExpectNonEmptyPlan: true,
			},
		},
	})
}

// TestAccBeltDriftAndReplace covers the belt/power-line-shaped
// connectionResource, which TestAccFullLine only exercises for a clean
// create + no-op plan: this adds out-of-band-delete drift detection and a
// RequiresReplace check on a connector index change.
func TestAccBeltDriftAndReplace(t *testing.T) {
	c := startMock(t)

	config := `
resource "satisfactory_building" "a" {
  class = "Build_SmelterMk1_C"
  x     = 0
  y     = 0
  z     = 0
}

resource "satisfactory_building" "b" {
  class = "Build_SmelterMk1_C"
  x     = 1000
  y     = 0
  z     = 0
}

resource "satisfactory_belt" "line" {
  class          = "Build_ConveyorBeltMk1_C"
  from_id        = satisfactory_building.a.id
  from_connector = 0
  to_id          = satisfactory_building.b.id
  to_connector   = 0
}
`
	movedConnector := `
resource "satisfactory_building" "a" {
  class = "Build_SmelterMk1_C"
  x     = 0
  y     = 0
  z     = 0
}

resource "satisfactory_building" "b" {
  class = "Build_SmelterMk1_C"
  x     = 1000
  y     = 0
  z     = 0
}

resource "satisfactory_belt" "line" {
  class          = "Build_ConveyorBeltMk1_C"
  from_id        = satisfactory_building.a.id
  from_connector = 0
  to_id          = satisfactory_building.b.id
  to_connector   = 1
}
`
	var beltID string
	resource.Test(t, resource.TestCase{
		ProtoV6ProviderFactories: protoFactories,
		Steps: []resource.TestStep{
			{
				Config: config,
				Check: resource.ComposeTestCheckFunc(
					resource.TestCheckResourceAttrSet("satisfactory_belt.line", "id"),
					captureID("satisfactory_belt.line", &beltID),
				),
			},
			{
				// to_connector changed: RequiresReplace, expect a new id.
				Config: movedConnector,
				Check:  checkDifferentID("satisfactory_belt.line", &beltID),
			},
			{
				// Simulate dismantling the belt in-game.
				PreConfig: func() {
					if err := c.DeleteConnection(context.Background(), beltID); err != nil {
						t.Fatalf("out-of-band delete: %v", err)
					}
				},
				Config:             movedConnector,
				PlanOnly:           true,
				ExpectNonEmptyPlan: true,
			},
		},
	})
}

// TestAccBuildingNonManufacturerClockSpeedOmitted is the acceptance-level
// counterpart to TestBuildingCreate_DefaultsUnsetClockSpeedTo1 in
// resource_internal_test.go. That test drives buildingResource.Create
// directly; this one goes through a full terraform apply + plan cycle via
// terraform-plugin-testing, which is the only way to actually catch a
// "provider produced inconsistent result" plan-time error if one ever
// regressed.
//
// It deliberately does NOT use startMock()/internal/mockserver: that mock
// unconditionally defaults ClockSpeed to 1.0 for every class (see
// internal/mockserver/server_test.go's
// TestBuildableClockSpeedDefaultIgnoresClass for why - a documented,
// out-of-scope fidelity gap vs. the real mod), so it can never produce the
// "clock_speed key absent from the response" shape this test needs. Instead
// this spins up a tiny purpose-built fake API that omits clock_speed from
// its create/read responses, mirroring the real mod's BuildableToJson for a
// buildable that isn't a manufacturer.
func TestAccBuildingNonManufacturerClockSpeedOmitted(t *testing.T) {
	var created api.Buildable
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet && r.URL.Path == "/api/v1/health":
			w.WriteHeader(http.StatusOK)
		case r.Method == http.MethodPost && r.URL.Path == "/api/v1/buildables":
			var posted api.Buildable
			_ = json.NewDecoder(r.Body).Decode(&posted)
			// Omit clock_speed, like the real mod does for a
			// non-manufacturer buildable (see the ClockSpeed comment on
			// api.Buildable and resource_building.go's Schema/Create).
			created = api.Buildable{TFID: posted.TFID, Class: posted.Class, Transform: posted.Transform}
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(created)
		case r.Method == http.MethodGet && strings.HasPrefix(r.URL.Path, "/api/v1/buildables/"):
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(created)
		case r.Method == http.MethodDelete:
			w.WriteHeader(http.StatusNoContent)
		default:
			http.NotFound(w, r)
		}
	}))
	defer srv.Close()
	t.Setenv("SATISFACTORY_ENDPOINT", srv.URL)

	config := `
resource "satisfactory_building" "splitter" {
  class = "Build_SplitterMk1_C"
  x     = 0
  y     = 0
  z     = 0
}
`
	resource.Test(t, resource.TestCase{
		ProtoV6ProviderFactories: protoFactories,
		Steps: []resource.TestStep{
			{
				Config: config,
				Check: resource.ComposeTestCheckFunc(
					// The fake API echoed back an omitted (zero-value)
					// clock_speed, even though Create() sent 1.0 - proving
					// the provider stores the API's actual response
					// rather than fighting it.
					resource.TestCheckResourceAttr("satisfactory_building.splitter", "clock_speed", "0"),
					resource.TestCheckResourceAttrSet("satisfactory_building.splitter", "id"),
				),
			},
			{
				// The actual regression this test guards: re-planning
				// must NOT produce a "provider produced inconsistent
				// result" error or a perpetual diff, despite clock_speed
				// having no static schema Default.
				Config:   config,
				PlanOnly: true,
			},
		},
	})
}
