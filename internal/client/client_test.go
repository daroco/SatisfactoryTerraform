package client_test

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/daroco/satisfacto-form/internal/api"
	"github.com/daroco/satisfacto-form/internal/client"
)

// TestIsNotFound_RealNotFound is the load-bearing drift-detection check
// documented in CLAUDE.md: a 404 from the mod must surface as
// client.IsNotFound(err) == true so the provider's Read() can remove the
// resource from state and plan a recreate.
func TestIsNotFound_RealNotFound(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		_ = json.NewEncoder(w).Encode(api.Error{Message: "no buildable with that tf_id"})
	}))
	defer srv.Close()

	c := client.New(srv.URL, "")
	_, err := c.GetBuildable(context.Background(), "missing-1")
	if err == nil {
		t.Fatal("expected an error, got nil")
	}
	if !client.IsNotFound(err) {
		t.Errorf("IsNotFound(%v) = false, want true", err)
	}

	var nf *client.NotFoundError
	if !errors.As(err, &nf) {
		t.Fatalf("error is not a *client.NotFoundError: %v", err)
	}
	if nf.TFID != "missing-1" {
		t.Errorf("NotFoundError.TFID = %q, want %q", nf.TFID, "missing-1")
	}
}

// TestIsNotFound_OtherErrorsNotMisclassified ensures every other error shape
// the client can produce is NOT mistaken for drift - misclassifying, say, a
// validation or server error as "not found" would make the provider silently
// drop resources from state instead of surfacing a real failure.
func TestIsNotFound_OtherErrorsNotMisclassified(t *testing.T) {
	cases := []struct {
		name   string
		status int
	}{
		{"unprocessable entity", http.StatusUnprocessableEntity},
		{"conflict", http.StatusConflict},
		{"unauthorized", http.StatusUnauthorized},
		{"internal server error", http.StatusInternalServerError},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "application/json")
				w.WriteHeader(tc.status)
				_ = json.NewEncoder(w).Encode(api.Error{Message: "boom"})
			}))
			defer srv.Close()

			c := client.New(srv.URL, "")
			_, err := c.GetBuildable(context.Background(), "x")
			if err == nil {
				t.Fatal("expected an error, got nil")
			}
			if client.IsNotFound(err) {
				t.Errorf("status %d misclassified as IsNotFound", tc.status)
			}
			var apiErr *client.APIError
			if !errors.As(err, &apiErr) {
				t.Fatalf("error is not a *client.APIError: %v", err)
			}
			if apiErr.Status != tc.status {
				t.Errorf("APIError.Status = %d, want %d", apiErr.Status, tc.status)
			}
		})
	}

	t.Run("unreachable server", func(t *testing.T) {
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
		srv.Close() // closed before any request is made

		c := client.New(srv.URL, "")
		_, err := c.GetBuildable(context.Background(), "x")
		if err == nil {
			t.Fatal("expected an error, got nil")
		}
		if client.IsNotFound(err) {
			t.Error("network error misclassified as IsNotFound")
		}
	})

	t.Run("nil error", func(t *testing.T) {
		if client.IsNotFound(nil) {
			t.Error("IsNotFound(nil) = true, want false")
		}
	})
}

func TestAPIError_MessageFromBody(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusUnprocessableEntity)
		_ = json.NewEncoder(w).Encode(api.Error{Message: "clock_speed must be between 0.01 and 2.5"})
	}))
	defer srv.Close()

	c := client.New(srv.URL, "")
	_, err := c.CreateBuildable(context.Background(), api.Buildable{TFID: "x", Class: "Build_SmelterMk1_C"})
	if err == nil {
		t.Fatal("expected an error, got nil")
	}
	if !strings.Contains(err.Error(), "clock_speed must be between 0.01 and 2.5") {
		t.Errorf("error = %q, want it to contain the API's message", err.Error())
	}
}

// TestAPIError_FallsBackToHTTPStatusOnNonJSONBody covers the defensive path
// in Client.do: if a non-2xx body isn't valid api.Error JSON (e.g. an
// upstream proxy or the game crashing mid-response), the client must still
// produce a usable error instead of failing to decode.
func TestAPIError_FallsBackToHTTPStatusOnNonJSONBody(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
		_, _ = w.Write([]byte("<html>not json</html>"))
	}))
	defer srv.Close()

	c := client.New(srv.URL, "")
	_, err := c.GetBuildable(context.Background(), "x")
	if err == nil {
		t.Fatal("expected an error, got nil")
	}
	var apiErr *client.APIError
	if !errors.As(err, &apiErr) {
		t.Fatalf("error is not a *client.APIError: %v", err)
	}
	if apiErr.Message != "500 Internal Server Error" {
		t.Errorf("APIError.Message = %q, want the raw HTTP status as fallback", apiErr.Message)
	}
}

func TestDeleteBuildable_IdempotentOn404(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		_ = json.NewEncoder(w).Encode(api.Error{Message: "gone"})
	}))
	defer srv.Close()

	c := client.New(srv.URL, "")
	if err := c.DeleteBuildable(context.Background(), "already-gone"); err != nil {
		t.Errorf("DeleteBuildable on a 404 should be nil (idempotent), got %v", err)
	}
}

func TestDeleteConnection_IdempotentOn404(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		_ = json.NewEncoder(w).Encode(api.Error{Message: "gone"})
	}))
	defer srv.Close()

	c := client.New(srv.URL, "")
	if err := c.DeleteConnection(context.Background(), "already-gone"); err != nil {
		t.Errorf("DeleteConnection on a 404 should be nil (idempotent), got %v", err)
	}
}

func TestDeleteBuildable_NonNotFoundErrorPropagates(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusConflict)
		_ = json.NewEncoder(w).Encode(api.Error{Message: "buildable still has connection c-1 attached"})
	}))
	defer srv.Close()

	c := client.New(srv.URL, "")
	err := c.DeleteBuildable(context.Background(), "x")
	if err == nil {
		t.Fatal("expected the 409 to propagate, got nil")
	}
	if client.IsNotFound(err) {
		t.Error("409 should not be treated as not-found")
	}
}

func TestClient_BearerTokenHeader(t *testing.T) {
	var gotAuth string
	var gotAuthPresent bool
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotAuth = r.Header.Get("Authorization")
		_, gotAuthPresent = r.Header["Authorization"]
		w.WriteHeader(http.StatusOK)
	}))
	defer srv.Close()

	c := client.New(srv.URL, "s3cr3t")
	if err := c.Health(context.Background()); err != nil {
		t.Fatalf("health: %v", err)
	}
	if gotAuth != "Bearer s3cr3t" {
		t.Errorf("Authorization header = %q, want %q", gotAuth, "Bearer s3cr3t")
	}

	c2 := client.New(srv.URL, "")
	if err := c2.Health(context.Background()); err != nil {
		t.Fatalf("health: %v", err)
	}
	if gotAuthPresent {
		t.Error("Authorization header should be absent when no token is configured")
	}
}

func TestClient_ContentTypeOnlyOnRequestsWithBody(t *testing.T) {
	var getContentType, postContentType string
	var getHadContentType bool
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case http.MethodGet:
			getContentType = r.Header.Get("Content-Type")
			_, getHadContentType = r.Header["Content-Type"]
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(api.Buildable{TFID: "x", Class: "Build_SmelterMk1_C"})
		case http.MethodPost:
			postContentType = r.Header.Get("Content-Type")
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(api.Buildable{TFID: "x", Class: "Build_SmelterMk1_C"})
		}
	}))
	defer srv.Close()

	c := client.New(srv.URL, "")
	if _, err := c.GetBuildable(context.Background(), "x"); err != nil {
		t.Fatalf("get: %v", err)
	}
	if getHadContentType {
		t.Errorf("GET (no body) should not set Content-Type, got %q", getContentType)
	}

	if _, err := c.CreateBuildable(context.Background(), api.Buildable{TFID: "x", Class: "Build_SmelterMk1_C"}); err != nil {
		t.Fatalf("create: %v", err)
	}
	if postContentType != "application/json" {
		t.Errorf("POST Content-Type = %q, want application/json", postContentType)
	}
}

func TestClient_RequestPathsAreBuiltUnderAPIv1(t *testing.T) {
	var gotPath string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.Path
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(api.Buildable{TFID: "b-1", Class: "Build_SmelterMk1_C"})
	}))
	defer srv.Close()

	// A trailing slash on the endpoint must not produce a doubled slash.
	c := client.New(srv.URL+"/", "")
	if _, err := c.GetBuildable(context.Background(), "b-1"); err != nil {
		t.Fatalf("get: %v", err)
	}
	if gotPath != "/api/v1/buildables/b-1" {
		t.Errorf("request path = %q, want /api/v1/buildables/b-1", gotPath)
	}
}

func TestHealth(t *testing.T) {
	t.Run("ok", func(t *testing.T) {
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusOK)
		}))
		defer srv.Close()
		if err := client.New(srv.URL, "").Health(context.Background()); err != nil {
			t.Errorf("Health() = %v, want nil", err)
		}
	})

	t.Run("error", func(t *testing.T) {
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusInternalServerError)
		}))
		defer srv.Close()
		if err := client.New(srv.URL, "").Health(context.Background()); err == nil {
			t.Error("Health() = nil, want an error")
		}
	})
}

// TestClient_UnreachableServerErrorIsHelpful checks the wrapped error
// message that GetBuildable/etc. produce when the game/mod simply isn't
// there - this is the message a user sees, so it should mention the mod.
func TestClient_UnreachableServerErrorIsHelpful(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	srv.Close()

	c := client.New(srv.URL, "")
	err := c.Health(context.Background())
	if err == nil {
		t.Fatal("expected an error, got nil")
	}
	if !strings.Contains(err.Error(), "SatisfactoTerraform mod") {
		t.Errorf("error = %q, want it to mention the mod for user-friendliness", err.Error())
	}
	if client.IsNotFound(err) {
		t.Error("a network-level failure should not be classified as IsNotFound")
	}
}
