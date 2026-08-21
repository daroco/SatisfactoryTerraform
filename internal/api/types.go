// Package api defines the wire types shared by the Terraform provider client
// and the mock server. The authoritative contract is api/openapi.yaml; the
// SatisfactoTerraform UE mod implements the same shapes in C++.
package api

// Transform is a world position in centimetres (Unreal units) plus yaw in degrees.
type Transform struct {
	X   float64 `json:"x"`
	Y   float64 `json:"y"`
	Z   float64 `json:"z"`
	Yaw float64 `json:"yaw"`
}

// Buildable is a machine or foundation placed in the world.
type Buildable struct {
	TFID       string    `json:"tf_id"`
	Class      string    `json:"class"`
	Transform  Transform `json:"transform"`
	Recipe     string    `json:"recipe,omitempty"`
	ClockSpeed float64   `json:"clock_speed,omitempty"`
}

// BuildablePatch carries the in-place-mutable fields of a Buildable.
type BuildablePatch struct {
	Recipe     *string  `json:"recipe,omitempty"`
	ClockSpeed *float64 `json:"clock_speed,omitempty"`
}

// ConnectionEndpoint identifies one end of a belt or power line by the
// buildable it attaches to and the index of the connection component on it.
type ConnectionEndpoint struct {
	BuildableTFID string `json:"buildable_tf_id"`
	Connector     int64  `json:"connector"`
}

// Connection is a belt or power line between two buildables.
type Connection struct {
	TFID  string             `json:"tf_id"`
	Class string             `json:"class"`
	From  ConnectionEndpoint `json:"from"`
	To    ConnectionEndpoint `json:"to"`
}

// Error is the body of every non-2xx response.
type Error struct {
	Message string `json:"message"`
}

// World is the response of GET /world.
type World struct {
	SessionName string `json:"session_name"`
	GameVersion string `json:"game_version"`
	ModVersion  string `json:"mod_version"`
}
