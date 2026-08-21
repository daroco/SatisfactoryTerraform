// Package mockserver is an in-memory implementation of the SatisfactoTerraform
// mod API (api/openapi.yaml). It exists so the provider can be developed and
// acceptance-tested without a running game, and doubles as executable
// documentation of the contract the UE mod must implement.
package mockserver

import (
	"encoding/json"
	"net/http"
	"strings"
	"sync"

	"github.com/daroco/satisfacto-form/internal/api"
)

// Server holds the in-memory world state behind the mock API.
type Server struct {
	mu          sync.Mutex
	buildables  map[string]api.Buildable
	connections map[string]api.Connection
	token       string
}

// New returns a mock server. token is optional; when set, every request except
// /health must carry it as a bearer token.
func New(token string) *Server {
	return &Server{
		buildables:  map[string]api.Buildable{},
		connections: map[string]api.Connection{},
		token:       token,
	}
}

// Handler returns the http.Handler serving the API under /api/v1.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/v1/health", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
	})
	mux.HandleFunc("GET /api/v1/world", s.auth(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, api.World{
			SessionName: "mock",
			GameVersion: "mock",
			ModVersion:  "0.1.0",
		})
	}))

	mux.HandleFunc("GET /api/v1/buildables", s.auth(s.listBuildables))
	mux.HandleFunc("POST /api/v1/buildables", s.auth(s.createBuildable))
	mux.HandleFunc("GET /api/v1/buildables/{tf_id}", s.auth(s.getBuildable))
	mux.HandleFunc("PATCH /api/v1/buildables/{tf_id}", s.auth(s.patchBuildable))
	mux.HandleFunc("DELETE /api/v1/buildables/{tf_id}", s.auth(s.deleteBuildable))

	mux.HandleFunc("GET /api/v1/connections", s.auth(s.listConnections))
	mux.HandleFunc("POST /api/v1/connections", s.auth(s.createConnection))
	mux.HandleFunc("GET /api/v1/connections/{tf_id}", s.auth(s.getConnection))
	mux.HandleFunc("DELETE /api/v1/connections/{tf_id}", s.auth(s.deleteConnection))
	return mux
}

func (s *Server) auth(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if s.token != "" && r.Header.Get("Authorization") != "Bearer "+s.token {
			writeErr(w, http.StatusUnauthorized, "missing or invalid bearer token")
			return
		}
		next(w, r)
	}
}

func (s *Server) listBuildables(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]api.Buildable, 0, len(s.buildables))
	for _, b := range s.buildables {
		out = append(out, b)
	}
	writeJSON(w, http.StatusOK, out)
}

func (s *Server) createBuildable(w http.ResponseWriter, r *http.Request) {
	var b api.Buildable
	if err := json.NewDecoder(r.Body).Decode(&b); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	if b.TFID == "" {
		writeErr(w, http.StatusUnprocessableEntity, "tf_id is required")
		return
	}
	if !validClass(b.Class, "Build_") {
		writeErr(w, http.StatusUnprocessableEntity, "class must be a buildable class name like Build_ConstructorMk1_C")
		return
	}
	if b.Recipe != "" && !validClass(b.Recipe, "Recipe_") {
		writeErr(w, http.StatusUnprocessableEntity, "recipe must be a recipe class name like Recipe_IronPlate_C")
		return
	}
	if b.ClockSpeed == 0 {
		b.ClockSpeed = 1.0
	}
	if b.ClockSpeed < 0.01 || b.ClockSpeed > 2.5 {
		writeErr(w, http.StatusUnprocessableEntity, "clock_speed must be between 0.01 and 2.5")
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, exists := s.buildables[b.TFID]; exists {
		writeErr(w, http.StatusConflict, "buildable with tf_id "+b.TFID+" already exists")
		return
	}
	s.buildables[b.TFID] = b
	writeJSON(w, http.StatusCreated, b)
}

func (s *Server) getBuildable(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	defer s.mu.Unlock()
	b, ok := s.buildables[r.PathValue("tf_id")]
	if !ok {
		writeErr(w, http.StatusNotFound, "no buildable with that tf_id")
		return
	}
	writeJSON(w, http.StatusOK, b)
}

func (s *Server) patchBuildable(w http.ResponseWriter, r *http.Request) {
	var p api.BuildablePatch
	if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	b, ok := s.buildables[r.PathValue("tf_id")]
	if !ok {
		writeErr(w, http.StatusNotFound, "no buildable with that tf_id")
		return
	}
	if p.Recipe != nil {
		if *p.Recipe != "" && !validClass(*p.Recipe, "Recipe_") {
			writeErr(w, http.StatusUnprocessableEntity, "recipe must be a recipe class name like Recipe_IronPlate_C")
			return
		}
		b.Recipe = *p.Recipe
	}
	if p.ClockSpeed != nil {
		if *p.ClockSpeed < 0.01 || *p.ClockSpeed > 2.5 {
			writeErr(w, http.StatusUnprocessableEntity, "clock_speed must be between 0.01 and 2.5")
			return
		}
		b.ClockSpeed = *p.ClockSpeed
	}
	s.buildables[b.TFID] = b
	writeJSON(w, http.StatusOK, b)
}

func (s *Server) deleteBuildable(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("tf_id")
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.buildables[id]; !ok {
		writeErr(w, http.StatusNotFound, "no buildable with that tf_id")
		return
	}
	for _, c := range s.connections {
		if c.From.BuildableTFID == id || c.To.BuildableTFID == id {
			writeErr(w, http.StatusConflict, "buildable still has connection "+c.TFID+" attached")
			return
		}
	}
	delete(s.buildables, id)
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) listConnections(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]api.Connection, 0, len(s.connections))
	for _, c := range s.connections {
		out = append(out, c)
	}
	writeJSON(w, http.StatusOK, out)
}

func (s *Server) createConnection(w http.ResponseWriter, r *http.Request) {
	var c api.Connection
	if err := json.NewDecoder(r.Body).Decode(&c); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	if c.TFID == "" {
		writeErr(w, http.StatusUnprocessableEntity, "tf_id is required")
		return
	}
	if !validClass(c.Class, "Build_") {
		writeErr(w, http.StatusUnprocessableEntity, "class must be a class name like Build_ConveyorBeltMk1_C or Build_PowerLine_C")
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, exists := s.connections[c.TFID]; exists {
		writeErr(w, http.StatusConflict, "connection with tf_id "+c.TFID+" already exists")
		return
	}
	for _, end := range []api.ConnectionEndpoint{c.From, c.To} {
		if _, ok := s.buildables[end.BuildableTFID]; !ok {
			writeErr(w, http.StatusUnprocessableEntity, "endpoint references unknown buildable "+end.BuildableTFID)
			return
		}
		if end.Connector < 0 {
			writeErr(w, http.StatusUnprocessableEntity, "connector index must be >= 0")
			return
		}
	}
	s.connections[c.TFID] = c
	writeJSON(w, http.StatusCreated, c)
}

func (s *Server) getConnection(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	defer s.mu.Unlock()
	c, ok := s.connections[r.PathValue("tf_id")]
	if !ok {
		writeErr(w, http.StatusNotFound, "no connection with that tf_id")
		return
	}
	writeJSON(w, http.StatusOK, c)
}

func (s *Server) deleteConnection(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.connections[r.PathValue("tf_id")]; !ok {
		writeErr(w, http.StatusNotFound, "no connection with that tf_id")
		return
	}
	delete(s.connections, r.PathValue("tf_id"))
	w.WriteHeader(http.StatusNoContent)
}

// validClass approximates the game's class naming: prefix + name + "_C".
func validClass(class, prefix string) bool {
	return strings.HasPrefix(class, prefix) && strings.HasSuffix(class, "_C") && len(class) > len(prefix)+2
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, api.Error{Message: msg})
}
