package api_test

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/daroco/satisfacto-form/internal/api"
)

// TestBuildable_UnmarshalMissingClockSpeed pins down the exact fidelity gap
// the provider's clock_speed handling is built around (see
// internal/provider/resource_building.go Schema/Create): the mod's
// BuildableToJson only emits "clock_speed" when the buildable
// Cast<AFGBuildableManufacturer>s successfully (constructors, smelters,
// ...). For every other buildable class (splitters, mergers, power poles,
// foundations, ...) the key is simply absent from the response body. This
// test confirms that decoding such a body leaves ClockSpeed at Go's float64
// zero value rather than erroring or silently defaulting to 1.0 - if this
// ever changed (e.g. someone added a custom UnmarshalJSON), the provider's
// "no static schema Default, apply the default in Create() instead" design
// would need to be revisited.
func TestBuildable_UnmarshalMissingClockSpeed(t *testing.T) {
	body := `{"tf_id":"b-1","class":"Build_SplitterMk1_C","transform":{"x":1,"y":2,"z":3,"yaw":90}}`
	var b api.Buildable
	if err := json.Unmarshal([]byte(body), &b); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if b.ClockSpeed != 0 {
		t.Errorf("ClockSpeed = %v, want 0 (zero value) when the key is absent", b.ClockSpeed)
	}
	if b.TFID != "b-1" || b.Class != "Build_SplitterMk1_C" {
		t.Errorf("unexpected decode: %+v", b)
	}
	if b.Transform.Yaw != 90 {
		t.Errorf("Transform.Yaw = %v, want 90", b.Transform.Yaw)
	}
}

// TestBuildable_MarshalOmitsZeroValueFields exercises the other side of the
// same contract: encoding/json's `omitempty` on Recipe and ClockSpeed means
// a freshly zero-valued Buildable (as mockserver hands back for non-clocked
// classes, or as the real mod does) never puts those keys on the wire at
// all - it's not that the mod writes `"clock_speed":0`, the key is missing
// entirely. That distinction is what makes the schema-level Computed (no
// static Default) + Create()-level Go default in resource_building.go
// necessary.
func TestBuildable_MarshalOmitsZeroValueFields(t *testing.T) {
	b := api.Buildable{
		TFID:  "b-1",
		Class: "Build_SplitterMk1_C",
	}
	buf, err := json.Marshal(b)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	s := string(buf)
	if strings.Contains(s, "recipe") {
		t.Errorf("marshaled JSON should omit empty recipe, got %s", s)
	}
	if strings.Contains(s, "clock_speed") {
		t.Errorf("marshaled JSON should omit zero-value clock_speed, got %s", s)
	}
}

func TestBuildable_RoundTrip(t *testing.T) {
	want := api.Buildable{
		TFID:       "b-2",
		Class:      "Build_ConstructorMk1_C",
		Transform:  api.Transform{X: 100.5, Y: -200, Z: 0, Yaw: 270},
		Recipe:     "Recipe_IronPlate_C",
		ClockSpeed: 1.5,
	}
	buf, err := json.Marshal(want)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	var got api.Buildable
	if err := json.Unmarshal(buf, &got); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if got != want {
		t.Errorf("round trip = %+v, want %+v", got, want)
	}
}

// TestBuildablePatch_NilVsZeroPointer verifies the pointer-field semantics
// PatchBuildable relies on: a nil field means "leave alone" (omitted from
// the wire), while a non-nil pointer - even one pointing at a zero value -
// means "set it", and must appear on the wire. This is what lets the
// provider's Update() send only the fields that actually changed.
func TestBuildablePatch_NilVsZeroPointer(t *testing.T) {
	recipe := ""
	patch := api.BuildablePatch{Recipe: &recipe} // explicitly clearing the recipe
	buf, err := json.Marshal(patch)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	s := string(buf)
	if !strings.Contains(s, `"recipe":""`) {
		t.Errorf("expected explicit empty recipe on the wire, got %s", s)
	}
	if strings.Contains(s, "clock_speed") {
		t.Errorf("unset ClockSpeed pointer should be omitted entirely, got %s", s)
	}

	var got api.BuildablePatch
	if err := json.Unmarshal(buf, &got); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if got.Recipe == nil || *got.Recipe != "" {
		t.Errorf("Recipe = %v, want non-nil pointer to empty string", got.Recipe)
	}
	if got.ClockSpeed != nil {
		t.Errorf("ClockSpeed = %v, want nil (field absent)", got.ClockSpeed)
	}
}

func TestConnection_RoundTrip(t *testing.T) {
	want := api.Connection{
		TFID:  "c-1",
		Class: "Build_ConveyorBeltMk1_C",
		From:  api.ConnectionEndpoint{BuildableTFID: "m-1", Connector: 0},
		To:    api.ConnectionEndpoint{BuildableTFID: "m-2", Connector: 1},
	}
	buf, err := json.Marshal(want)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	var got api.Connection
	if err := json.Unmarshal(buf, &got); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if got != want {
		t.Errorf("round trip = %+v, want %+v", got, want)
	}
}

func TestError_RoundTrip(t *testing.T) {
	want := api.Error{Message: "tf_id is required"}
	buf, err := json.Marshal(want)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	var got api.Error
	if err := json.Unmarshal(buf, &got); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if got != want {
		t.Errorf("round trip = %+v, want %+v", got, want)
	}
}

func TestWorld_Unmarshal(t *testing.T) {
	body := `{"session_name":"my save","game_version":"1.0","mod_version":"0.1.0"}`
	var w api.World
	if err := json.Unmarshal([]byte(body), &w); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	want := api.World{SessionName: "my save", GameVersion: "1.0", ModVersion: "0.1.0"}
	if w != want {
		t.Errorf("World = %+v, want %+v", w, want)
	}
}
