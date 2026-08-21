// terraform-provider-satisfactory: manage Satisfactory factories as code via
// the SatisfactoTerraform mod's HTTP API.
package main

import (
	"context"
	"flag"
	"log"

	"github.com/hashicorp/terraform-plugin-framework/providerserver"

	"github.com/daroco/satisfacto-form/internal/provider"
)

// version is set by goreleaser at build time.
var version = "dev"

func main() {
	var debug bool
	flag.BoolVar(&debug, "debug", false, "run with support for debuggers like delve")
	flag.Parse()

	err := providerserver.Serve(context.Background(), provider.New(version), providerserver.ServeOpts{
		Address: "registry.terraform.io/daroco/satisfactory",
		Debug:   debug,
	})
	if err != nil {
		log.Fatal(err)
	}
}
