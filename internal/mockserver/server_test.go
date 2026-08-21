package mockserver_test

import (
	"context"
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

	// A buildable with an attached connection must not be deletable.
	if err := c.DeleteBuildable(ctx, "m-1"); err == nil {
		t.Error("deleting buildable with attached connection should 409")
	}

	if err := c.DeleteConnection(ctx, "c-1"); err != nil {
		t.Fatalf("delete connection: %v", err)
	}
	if err := c.DeleteBuildable(ctx, "m-1"); err != nil {
		t.Errorf("delete buildable after connection removed: %v", err)
	}
}
