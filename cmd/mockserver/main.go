// mockserver runs the in-memory mock of the SatisfactoTerraform mod API for
// local provider development: `go run ./cmd/mockserver` then `terraform apply`.
package main

import (
	"flag"
	"log"
	"net/http"
	"os"

	"github.com/daroco/satisfacto-form/internal/mockserver"
)

func main() {
	addr := flag.String("addr", ":8090", "listen address")
	flag.Parse()

	srv := mockserver.New(os.Getenv("SATISFACTORY_TOKEN"))
	log.Printf("mock SatisfactoTerraform API listening on %s", *addr)
	log.Fatal(http.ListenAndServe(*addr, srv.Handler()))
}
