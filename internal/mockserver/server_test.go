package mockserver_test

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/daroco/satisfacto-form/internal/api"
	"github.com/daroco/satisfacto-form/internal/client"
	"github.com/daroco/satisfacto-form/internal/mockserver"
)

func newTestClient(t *testing.T) *client.Client {
	t.Helper()
	srv := httptest.NewServer(mockserver.New("").Handler())
	t.Cleanup(srv.Close)
	return client.New(srv.URL, "")
}

// newTestServerAndClient is like newTestClient but also hands back the raw
// httptest.Server, for tests that need to hit the mock without going
// through the typed client (e.g. auth header checks, world/health).
func newTestServerAndClient(t *testing.T, token string) (*httptest.Server, *client.Client) {
	t.Helper()
	srv := httptest.NewServer(mockserver.New(token).Handler())
	t.Cleanup(srv.Close)
	return srv, client.New(srv.URL, token)
}

func TestBuildableLifecycle(t *testing.T) {
	c := newTestClient(t)
	ctx := context.Background()

	if err := c.Health(ctx); err != nil {
		t.Fatalf("health: %v", err)
	}

	b := api.Buildable{
		TFID:      "b-1",
		Class:     "Build_ConstructorMk1_C",
		Transform: api.Transform{X: 100, Y: 200, Z: 0, Yaw: 90},
		Recipe:    "Recipe_IronPlate_C",
	}
	created, err := c.CreateBuildable(ctx, b)
	if err != nil {
		t.Fatalf("create: %v", err)
	}
	if created.ClockSpeed != 1.0 {
		t.Errorf("clock_speed default = %v, want 1.0", created.ClockSpeed)
	}

	if _, err := c.CreateBuildable(ctx, b); err == nil {
		t.Error("duplicate tf_id should 409")
	}

	clock := 1.5
	patched, err := c.PatchBuildable(ctx, "b-1", api.BuildablePatch{ClockSpeed: &clock})
	if err != nil {
		t.Fatalf("patch: %v", err)
	}
	if patched.ClockSpeed != 1.5 || patched.Recipe != "Recipe_IronPlate_C" {
		t.Errorf("patch result = %+v", patched)
	}

	if err := c.DeleteBuildable(ctx, "b-1"); err != nil {
		t.Fatalf("delete: %v", err)
	}
	_, err = c.GetBuildable(ctx, "b-1")
	if !client.IsNotFound(err) {
		t.Errorf("get after delete: want NotFound, got %v", err)
	}
	// Idempotent destroy.
	if err := c.DeleteBuildable(ctx, "b-1"); err != nil {
		t.Errorf("second delete should be nil, got %v", err)
	}
}

func TestBuildableValidation(t *testing.T) {
	c := newTestClient(t)
	ctx := context.Background()

	cases := []api.Buildable{
		{TFID: "", Class: "Build_SmelterMk1_C"},                                  // missing tf_id
		{TFID: "x", Class: "Smelter"},                                            // bad class name
		{TFID: "x", Class: "Build_SmelterMk1_C", Recipe: "IronIngot"},            // bad recipe name
		{TFID: "x", Class: "Build_SmelterMk1_C", ClockSpeed: 9.0},                // clock out of range
	}
	for _, b := range cases {
		if _, err := c.CreateBuildable(ctx, b); err == nil {
			t.Errorf("create %+v: want validation error", b)
		}
	}
}

func TestConnectionLifecycle(t *testing.T) {
	c := newTestClient(t)
	ctx := context.Background()

	for _, id := range []string{"m-1", "m-2"} {
		if _, err := c.CreateBuildable(ctx, api.Buildable{TFID: id, Class: "Build_SmelterMk1_C"}); err != nil {
			t.Fatalf("create %s: %v", id, err)
		}
	}

	conn := api.Connection{
		TFID:  "c-1",
		Class: "Build_ConveyorBeltMk1_C",
		From:  api.ConnectionEndpoint{BuildableTFID: "m-1", Connector: 0},
		To:    api.ConnectionEndpoint{BuildableTFID: "m-2", Connector: 0},
	}
	if _, err := c.CreateConnection(ctx, conn); err != nil {
		t.Fatalf("create connection: %v", err)
	}

	// Unknown endpoint must be rejected.
	bad := conn
	bad.TFID = "c-2"
	bad.To.BuildableTFID = "nope"
	if _, err := c.CreateConnection(ctx, bad); err == nil {
		t.Error("connection to unknown buildable should 422")
	}

	// Deleting a buildable with an attached connection is allowed - matches
	// the real mod/game (dismantling a connected buildable isn't blocked,
	// the belt just dangles) - but the connection it was part of should no
	// longer resolve afterward, since one of its endpoints is now gone.
	if err := c.DeleteBuildable(ctx, "m-1"); err != nil {
		t.Fatalf("delete buildable with attached connection: %v", err)
	}
	if _, err := c.GetConnection(ctx, "c-1"); !client.IsNotFound(err) {
		t.Errorf("connection referencing a deleted buildable should now 404, got: %v", err)
	}
	if err := c.DeleteBuildable(ctx, "m-2"); err != nil {
		t.Errorf("delete remaining buildable: %v", err)
	}
}

// TestConnectionErrorContract drives the 404/409/422 contract (CLAUDE.md:
// "Error contract: 404 unknown tf_id, 409 duplicate tf_id, 422 validation")
// specifically through /api/v1/connections, which TestConnectionLifecycle
// only partly exercises.
func TestConnectionErrorContract(t *testing.T) {
	c := newTestClient(t)
	ctx := context.Background()

	for _, id := range []string{"m-1", "m-2"} {
		if _, err := c.CreateBuildable(ctx, api.Buildable{TFID: id, Class: "Build_SmelterMk1_C"}); err != nil {
			t.Fatalf("create %s: %v", id, err)
		}
	}
	base := api.Connection{
		TFID:  "c-1",
		Class: "Build_ConveyorBeltMk1_C",
		From:  api.ConnectionEndpoint{BuildableTFID: "m-1", Connector: 0},
		To:    api.ConnectionEndpoint{BuildableTFID: "m-2", Connector: 0},
	}

	// 404: reading or deleting an unknown connection.
	if _, err := c.GetConnection(ctx, "nope"); !client.IsNotFound(err) {
		t.Errorf("get unknown connection: want NotFound, got %v", err)
	}
	if err := c.DeleteConnection(ctx, "nope"); err != nil {
		t.Errorf("delete unknown connection should be idempotent-nil, got %v", err)
	}

	// 422: missing tf_id, bad class, negative connector index.
	badCases := []api.Connection{
		{TFID: "", Class: "Build_ConveyorBeltMk1_C", From: base.From, To: base.To},                                          // missing tf_id
		{TFID: "c-2", Class: "ConveyorBelt", From: base.From, To: base.To},                                                  // bad class name
		{TFID: "c-2", Class: "Build_ConveyorBeltMk1_C", From: api.ConnectionEndpoint{BuildableTFID: "m-1", Connector: -1}, To: base.To}, // negative connector
	}
	for _, bad := range badCases {
		if _, err := c.CreateConnection(ctx, bad); err == nil {
			t.Errorf("create %+v: want validation error", bad)
		} else if client.IsNotFound(err) {
			t.Errorf("create %+v: validation error misclassified as NotFound", bad)
		}
	}

	// Happy path, then 409 on a duplicate tf_id.
	if _, err := c.CreateConnection(ctx, base); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := c.CreateConnection(ctx, base); err == nil {
		t.Error("duplicate connection tf_id should 409")
	}

	// 404 again: after deleting, both Get and a second Delete report gone.
	if err := c.DeleteConnection(ctx, "c-1"); err != nil {
		t.Fatalf("delete: %v", err)
	}
	if _, err := c.GetConnection(ctx, "c-1"); !client.IsNotFound(err) {
		t.Errorf("get after delete: want NotFound, got %v", err)
	}
}

// TestBuildableErrorContract fills the gaps TestBuildableLifecycle and
// TestBuildableValidation don't cover for PATCH: unknown tf_id (404) and an
// out-of-range/invalid patch (422).
func TestBuildableErrorContract(t *testing.T) {
	c := newTestClient(t)
	ctx := context.Background()

	badRecipe := "IronIngot" // no Recipe_ prefix
	if _, err := c.PatchBuildable(ctx, "nope", api.BuildablePatch{Recipe: &badRecipe}); !client.IsNotFound(err) {
		t.Errorf("patch unknown tf_id: want NotFound, got %v", err)
	}

	if _, err := c.CreateBuildable(ctx, api.Buildable{TFID: "b-1", Class: "Build_SmelterMk1_C"}); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := c.PatchBuildable(ctx, "b-1", api.BuildablePatch{Recipe: &badRecipe}); err == nil {
		t.Error("patch with invalid recipe class should 422")
	} else if client.IsNotFound(err) {
		t.Error("422 misclassified as NotFound")
	}
	tooHot := 9.0
	if _, err := c.PatchBuildable(ctx, "b-1", api.BuildablePatch{ClockSpeed: &tooHot}); err == nil {
		t.Error("patch with out-of-range clock_speed should 422")
	}
}

// TestBuildableClockSpeedDefaultIgnoresClass documents a known, deliberate
// mock/real-mod fidelity gap: the real mod only reports (and the provider
// only meaningfully applies) clock_speed for buildables that successfully
// Cast<AFGBuildableManufacturer> - splitters, mergers, power poles, etc.
// never get the field. This mock, by contrast, defaults ClockSpeed to 1.0
// for *any* class unconditionally (see createBuildable in server.go around
// the `if b.ClockSpeed == 0` check), so it can never reproduce the
// "field omitted for a non-manufacturer" response the provider's Create()
// default-handling in resource_building.go is actually guarding against.
// That gap is out of scope here (tracked, not yet fixed); this test just
// pins down the mock's current behavior so a future fix is a deliberate,
// visible change rather than a silent one. The field-omission scenario
// itself is covered independently at the api marshaling layer
// (internal/api/types_test.go) and at the provider layer with a
// purpose-built fake server (internal/provider/provider_test.go).
func TestBuildableClockSpeedDefaultIgnoresClass(t *testing.T) {
	c := newTestClient(t)
	ctx := context.Background()

	created, err := c.CreateBuildable(ctx, api.Buildable{TFID: "s-1", Class: "Build_SplitterMk1_C"})
	if err != nil {
		t.Fatalf("create: %v", err)
	}
	if created.ClockSpeed != 1.0 {
		t.Errorf("mock ClockSpeed default = %v, want 1.0 even for a non-manufacturer class (known fidelity gap)", created.ClockSpeed)
	}
}

// TestListBuildablesAndConnections exercises the list endpoints directly
// over HTTP: the typed client (internal/client) doesn't expose list
// methods, only the individual CRUD ones the provider needs.
func TestListBuildablesAndConnections(t *testing.T) {
	srv, c := newTestServerAndClient(t, "")
	ctx := context.Background()

	// Empty world: both lists come back as an empty JSON array, not null.
	assertListLength(t, srv.URL+"/api/v1/buildables", 0)
	assertListLength(t, srv.URL+"/api/v1/connections", 0)

	for _, id := range []string{"m-1", "m-2"} {
		if _, err := c.CreateBuildable(ctx, api.Buildable{TFID: id, Class: "Build_SmelterMk1_C"}); err != nil {
			t.Fatalf("create %s: %v", id, err)
		}
	}
	assertListLength(t, srv.URL+"/api/v1/buildables", 2)

	if _, err := c.CreateConnection(ctx, api.Connection{
		TFID:  "c-1",
		Class: "Build_ConveyorBeltMk1_C",
		From:  api.ConnectionEndpoint{BuildableTFID: "m-1", Connector: 0},
		To:    api.ConnectionEndpoint{BuildableTFID: "m-2", Connector: 0},
	}); err != nil {
		t.Fatalf("create connection: %v", err)
	}
	assertListLength(t, srv.URL+"/api/v1/connections", 1)
}

func assertListLength(t *testing.T, url string, want int) {
	t.Helper()
	resp, err := http.Get(url)
	if err != nil {
		t.Fatalf("GET %s: %v", url, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET %s: status %d", url, resp.StatusCode)
	}
	var items []json.RawMessage
	if err := json.NewDecoder(resp.Body).Decode(&items); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(items) != want {
		t.Errorf("GET %s: got %d items, want %d", url, len(items), want)
	}
}

func TestWorldEndpoint(t *testing.T) {
	srv, _ := newTestServerAndClient(t, "")
	resp, err := http.Get(srv.URL + "/api/v1/world")
	if err != nil {
		t.Fatalf("GET /world: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200", resp.StatusCode)
	}
	var w api.World
	if err := json.NewDecoder(resp.Body).Decode(&w); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if w.ModVersion == "" {
		t.Error("mod_version should not be empty")
	}
}

// TestAuth covers the bearer-token gate: every endpoint except /health must
// reject a missing or wrong token with 401, and accept the right one.
func TestAuth(t *testing.T) {
	srv, _ := newTestServerAndClient(t, "s3cr3t")

	// /health is exempt from auth.
	resp, err := http.Get(srv.URL + "/api/v1/health")
	if err != nil {
		t.Fatalf("GET /health: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Errorf("/health status = %d, want 200 even without a token", resp.StatusCode)
	}

	// Everything else requires it.
	req, _ := http.NewRequest(http.MethodGet, srv.URL+"/api/v1/buildables", nil)
	resp, err = http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("GET /buildables (no token): %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Errorf("no token: status = %d, want 401", resp.StatusCode)
	}

	req, _ = http.NewRequest(http.MethodGet, srv.URL+"/api/v1/buildables", nil)
	req.Header.Set("Authorization", "Bearer wrong")
	resp, err = http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("GET /buildables (wrong token): %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Errorf("wrong token: status = %d, want 401", resp.StatusCode)
	}

	req, _ = http.NewRequest(http.MethodGet, srv.URL+"/api/v1/buildables", nil)
	req.Header.Set("Authorization", "Bearer s3cr3t")
	resp, err = http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("GET /buildables (right token): %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Errorf("right token: status = %d, want 200", resp.StatusCode)
	}

	// The typed client goes through the same path and should work end to end.
	tokenedClient := client.New(srv.URL, "s3cr3t")
	if err := tokenedClient.Health(context.Background()); err != nil {
		t.Errorf("Health() with client token: %v", err)
	}
}
