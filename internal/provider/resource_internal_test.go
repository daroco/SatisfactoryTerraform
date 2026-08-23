package provider

// White-box tests for the resource implementations: these live in package
// provider (not provider_test) so they can reach unexported types like
// buildingResource/buildingModel directly, and can run without a terraform
// binary on PATH (they call the resource.Resource methods directly instead
// of going through terraform-plugin-testing's resource.Test harness, which
// shells out to terraform and only runs under TF_ACC=1 - see
// provider_test.go for the acceptance-level counterparts).

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/hashicorp/terraform-plugin-framework/resource"
	fwschema "github.com/hashicorp/terraform-plugin-framework/resource/schema"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/planmodifier"
	"github.com/hashicorp/terraform-plugin-framework/tfsdk"
	"github.com/hashicorp/terraform-plugin-framework/types"

	"github.com/daroco/satisfacto-form/internal/api"
	"github.com/daroco/satisfacto-form/internal/client"
)

// requiresReplaceDescription is the exact string every RequiresReplace()
// plan modifier in terraform-plugin-framework (string/float64/int64
// variants) uses for Description/MarkdownDescription. Asserting on it lets
// these tests confirm an attribute carries *a* RequiresReplace modifier
// without depending on the framework's unexported modifier types.
const requiresReplaceDescription = "If the value of this attribute changes, Terraform will destroy and recreate the resource."

func hasFloat64RequiresReplace(mods []planmodifier.Float64) bool {
	for _, m := range mods {
		if m.Description(context.Background()) == requiresReplaceDescription {
			return true
		}
	}
	return false
}

func hasStringRequiresReplace(mods []planmodifier.String) bool {
	for _, m := range mods {
		if m.Description(context.Background()) == requiresReplaceDescription {
			return true
		}
	}
	return false
}

func hasInt64RequiresReplace(mods []planmodifier.Int64) bool {
	for _, m := range mods {
		if m.Description(context.Background()) == requiresReplaceDescription {
			return true
		}
	}
	return false
}

// TestBuildingSchema_ClockSpeedHasNoStaticDefault is a regression test for
// commit da605f4: clock_speed must be Optional+Computed with NO schema-level
// Default. A static Default(1.0) would make terraform-plugin-framework
// assert the post-apply value equals 1.0, which breaks the moment a
// non-manufacturer buildable (splitter, merger, power pole, ...) comes back
// from the mod without a clock_speed key at all (unmarshals to 0) - that's
// exactly the "provider produced inconsistent result" failure mode this
// field's design avoids. See resource_building.go Schema/Create.
func TestBuildingSchema_ClockSpeedHasNoStaticDefault(t *testing.T) {
	r := &buildingResource{}
	var resp resource.SchemaResponse
	r.Schema(context.Background(), resource.SchemaRequest{}, &resp)

	attr, ok := resp.Schema.Attributes["clock_speed"]
	if !ok {
		t.Fatal("schema is missing a clock_speed attribute")
	}
	f64, ok := attr.(fwschema.Float64Attribute)
	if !ok {
		t.Fatalf("clock_speed is %T, want fwschema.Float64Attribute", attr)
	}
	if !f64.Optional {
		t.Error("clock_speed should be Optional (a user can set it explicitly)")
	}
	if !f64.Computed {
		t.Error("clock_speed should be Computed (the API may return a value the user didn't set)")
	}
	if f64.Default != nil {
		t.Errorf("clock_speed has a static schema Default (%#v); it must not, so a nil/omitted "+
			"clock_speed in the API response doesn't conflict with an asserted default", f64.Default)
	}

	// Contrast case: yaw is also Optional+Computed but DOES keep a static
	// Default, since the mod always echoes yaw back. If this ever failed
	// it would mean the two attributes' defaulting strategies got mixed up.
	yaw, ok := resp.Schema.Attributes["yaw"].(fwschema.Float64Attribute)
	if !ok {
		t.Fatal("schema is missing a yaw attribute")
	}
	if yaw.Default == nil {
		t.Error("yaw should keep its static schema Default; this test's contrast with clock_speed relies on that")
	}
}

// TestBuildingSchema_ReplaceVsInPlace pins down the CLAUDE.md convention:
// "Only recipe and clock_speed update in place; everything else
// RequiresReplace."
func TestBuildingSchema_ReplaceVsInPlace(t *testing.T) {
	r := &buildingResource{}
	var resp resource.SchemaResponse
	r.Schema(context.Background(), resource.SchemaRequest{}, &resp)

	forceReplace := []string{"class", "x", "y", "z", "yaw"}
	for _, name := range forceReplace {
		attr := resp.Schema.Attributes[name]
		switch a := attr.(type) {
		case fwschema.StringAttribute:
			if !hasStringRequiresReplace(a.PlanModifiers) {
				t.Errorf("%s: want a RequiresReplace plan modifier, has none", name)
			}
		case fwschema.Float64Attribute:
			if !hasFloat64RequiresReplace(a.PlanModifiers) {
				t.Errorf("%s: want a RequiresReplace plan modifier, has none", name)
			}
		default:
			t.Fatalf("%s: unexpected attribute type %T", name, attr)
		}
	}

	inPlace := []string{"recipe", "clock_speed"}
	for _, name := range inPlace {
		attr := resp.Schema.Attributes[name]
		switch a := attr.(type) {
		case fwschema.StringAttribute:
			if hasStringRequiresReplace(a.PlanModifiers) {
				t.Errorf("%s: should update in place, but has a RequiresReplace plan modifier", name)
			}
		case fwschema.Float64Attribute:
			if hasFloat64RequiresReplace(a.PlanModifiers) {
				t.Errorf("%s: should update in place, but has a RequiresReplace plan modifier", name)
			}
		default:
			t.Fatalf("%s: unexpected attribute type %T", name, attr)
		}
	}
}

// TestFoundationSchema_EverythingReplaces mirrors the building test above
// for satisfactory_foundation, which (per resource_foundation.go's Update,
// a deliberate no-op) has no in-place-updatable attributes at all.
func TestFoundationSchema_EverythingReplaces(t *testing.T) {
	r := &foundationResource{}
	var resp resource.SchemaResponse
	r.Schema(context.Background(), resource.SchemaRequest{}, &resp)

	for _, name := range []string{"class", "x", "y", "z", "yaw"} {
		attr := resp.Schema.Attributes[name]
		switch a := attr.(type) {
		case fwschema.StringAttribute:
			if !hasStringRequiresReplace(a.PlanModifiers) {
				t.Errorf("%s: want a RequiresReplace plan modifier, has none", name)
			}
		case fwschema.Float64Attribute:
			if !hasFloat64RequiresReplace(a.PlanModifiers) {
				t.Errorf("%s: want a RequiresReplace plan modifier, has none", name)
			}
		default:
			t.Fatalf("%s: unexpected attribute type %T", name, attr)
		}
	}
}

// TestConnectionSchema_EverythingReplaces covers satisfactory_belt and
// satisfactory_power_line (both backed by connectionResource, whose Update
// is also a deliberate no-op).
func TestConnectionSchema_EverythingReplaces(t *testing.T) {
	for _, factory := range []func() resource.Resource{newBeltResource, newPowerLineResource} {
		r := factory().(*connectionResource)
		var resp resource.SchemaResponse
		r.Schema(context.Background(), resource.SchemaRequest{}, &resp)

		strAttrs := []string{"class", "from_id", "to_id"}
		for _, name := range strAttrs {
			a, ok := resp.Schema.Attributes[name].(fwschema.StringAttribute)
			if !ok {
				t.Fatalf("%s%s: %s is not a StringAttribute", "connection", r.suffix, name)
			}
			if !hasStringRequiresReplace(a.PlanModifiers) {
				t.Errorf("%s%s: %s wants a RequiresReplace plan modifier, has none", "connection", r.suffix, name)
			}
		}
		intAttrs := []string{"from_connector", "to_connector"}
		for _, name := range intAttrs {
			a, ok := resp.Schema.Attributes[name].(fwschema.Int64Attribute)
			if !ok {
				t.Fatalf("%s%s: %s is not an Int64Attribute", "connection", r.suffix, name)
			}
			if !hasInt64RequiresReplace(a.PlanModifiers) {
				t.Errorf("%s%s: %s wants a RequiresReplace plan modifier, has none", "connection", r.suffix, name)
			}
		}
	}
}

// TestBuildingCreate_DefaultsUnsetClockSpeedTo1 exercises the Go-level
// default resource_building.go's Create() applies (see the comment there
// and in Schema) by driving the real Create() method against a purpose-
// built fake server that omits clock_speed from its response - exactly as
// the real mod does for a non-manufacturer buildable, and as
// internal/mockserver's shared mock currently does NOT (it unconditionally
// defaults ClockSpeed to 1.0 for every class - see
// internal/mockserver/server_test.go's
// TestBuildableClockSpeedDefaultIgnoresClass for that documented gap),
// which is why this test needs its own fake instead of internal/mockserver.
//
// This is a unit test, not an acceptance test: it calls buildingResource.Create
// directly, so it runs under plain `go test` with no terraform binary
// required. It asserts two things: (1) the request body sent to the API has
// clock_speed=1.0 even though the config left it unset, and (2) Create()
// completes without error and stores whatever the API actually returned
// (here, an omitted/zero clock_speed) - i.e. nothing in Create() tries to
// force the state back to 1.0 or otherwise fights the API response, which
// is what would risk a "provider produced inconsistent result" error once
// terraform core compares the applied value against the plan.
func TestBuildingCreate_DefaultsUnsetClockSpeedTo1(t *testing.T) {
	var sawClockSpeedKey bool
	var gotClockSpeed float64
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		if req.Method != http.MethodPost || req.URL.Path != "/api/v1/buildables" {
			http.NotFound(w, req)
			return
		}
		raw, err := io.ReadAll(req.Body)
		if err != nil {
			t.Fatalf("read request body: %v", err)
		}
		var b api.Buildable
		if err := json.Unmarshal(raw, &b); err != nil {
			t.Fatalf("unmarshal request body: %v", err)
		}
		var generic map[string]json.RawMessage
		if err := json.Unmarshal(raw, &generic); err != nil {
			t.Fatalf("unmarshal request body as map: %v", err)
		}
		_, sawClockSpeedKey = generic["clock_speed"]
		gotClockSpeed = b.ClockSpeed

		// Simulate the mod's BuildableToJson for a class that fails
		// Cast<AFGBuildableManufacturer>: the response simply doesn't
		// carry a clock_speed field. Leaving api.Buildable.ClockSpeed at
		// its zero value achieves that, since the field is
		// `json:"clock_speed,omitempty"`.
		out := api.Buildable{TFID: b.TFID, Class: b.Class, Transform: b.Transform}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusCreated)
		_ = json.NewEncoder(w).Encode(out)
	}))
	defer srv.Close()

	r := &buildingResource{client: client.New(srv.URL, "")}
	var schemaResp resource.SchemaResponse
	r.Schema(context.Background(), resource.SchemaRequest{}, &schemaResp)

	ctx := context.Background()
	plan := tfsdk.Plan{Schema: schemaResp.Schema}
	diags := plan.Set(ctx, &buildingModel{
		ID:    types.StringUnknown(),
		Class: types.StringValue("Build_SplitterMk1_C"),
		X:     types.Float64Value(0),
		Y:     types.Float64Value(0),
		Z:     types.Float64Value(0),
		Yaw:   types.Float64Value(0),
		// Recipe/ClockSpeed left unknown, matching what terraform core
		// hands the provider for an Optional+Computed attribute the user
		// didn't set in config and that has no static Default.
		Recipe:     types.StringUnknown(),
		ClockSpeed: types.Float64Unknown(),
	})
	if diags.HasError() {
		t.Fatalf("plan.Set: %v", diags)
	}

	createReq := resource.CreateRequest{Plan: plan}
	createResp := &resource.CreateResponse{State: tfsdk.State{Schema: schemaResp.Schema}}
	r.Create(ctx, createReq, createResp)
	if createResp.Diagnostics.HasError() {
		t.Fatalf("Create(): %v", createResp.Diagnostics)
	}

	if !sawClockSpeedKey {
		t.Error("request body omitted clock_speed entirely; Create() should always send an explicit value")
	}
	if gotClockSpeed != 1.0 {
		t.Errorf("clock_speed sent to the API = %v, want 1.0 (the Go-level default from Create)", gotClockSpeed)
	}

	var result buildingModel
	if diags := createResp.State.Get(ctx, &result); diags.HasError() {
		t.Fatalf("createResp.State.Get: %v", diags)
	}
	if result.ClockSpeed.IsUnknown() || result.ClockSpeed.IsNull() {
		t.Error("state clock_speed should be fully known after Create, not null/unknown")
	}
	// The fake API in this test echoed back an omitted clock_speed (zero
	// value), exactly like the real mod would for a non-manufacturer
	// buildable. Create() must store that as-is rather than forcing 1.0:
	// the whole point of dropping the static schema Default is to let the
	// API's actual response - whatever it is - become the final state.
	if got := result.ClockSpeed.ValueFloat64(); got != 0 {
		t.Errorf("state clock_speed = %v, want 0 (verbatim from the API response, not re-defaulted)", got)
	}
}
