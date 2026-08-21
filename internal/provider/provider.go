// Package provider implements terraform-provider-satisfactory against the
// SatisfactoTerraform mod API.
package provider

import (
	"context"
	"os"

	"github.com/hashicorp/terraform-plugin-framework/datasource"
	"github.com/hashicorp/terraform-plugin-framework/provider"
	"github.com/hashicorp/terraform-plugin-framework/provider/schema"
	"github.com/hashicorp/terraform-plugin-framework/resource"
	"github.com/hashicorp/terraform-plugin-framework/types"

	"github.com/daroco/satisfacto-form/internal/client"
)

// New returns the provider factory for the given version string.
func New(version string) func() provider.Provider {
	return func() provider.Provider {
		return &satisfactoryProvider{version: version}
	}
}

type satisfactoryProvider struct {
	version string
}

type providerModel struct {
	Endpoint types.String `tfsdk:"endpoint"`
	Token    types.String `tfsdk:"token"`
}

func (p *satisfactoryProvider) Metadata(_ context.Context, _ provider.MetadataRequest, resp *provider.MetadataResponse) {
	resp.TypeName = "satisfactory"
	resp.Version = p.version
}

func (p *satisfactoryProvider) Schema(_ context.Context, _ provider.SchemaRequest, resp *provider.SchemaResponse) {
	resp.Schema = schema.Schema{
		Description: "Manage Satisfactory factories through the SatisfactoTerraform mod's HTTP API.",
		Attributes: map[string]schema.Attribute{
			"endpoint": schema.StringAttribute{
				Optional:    true,
				Description: "Base URL of the mod API. Defaults to SATISFACTORY_ENDPOINT or http://localhost:8090.",
			},
			"token": schema.StringAttribute{
				Optional:    true,
				Sensitive:   true,
				Description: "Bearer token if the mod API has one configured. Defaults to SATISFACTORY_TOKEN.",
			},
		},
	}
}

func (p *satisfactoryProvider) Configure(ctx context.Context, req provider.ConfigureRequest, resp *provider.ConfigureResponse) {
	var cfg providerModel
	resp.Diagnostics.Append(req.Config.Get(ctx, &cfg)...)
	if resp.Diagnostics.HasError() {
		return
	}

	endpoint := cfg.Endpoint.ValueString()
	if endpoint == "" {
		endpoint = os.Getenv("SATISFACTORY_ENDPOINT")
	}
	if endpoint == "" {
		endpoint = "http://localhost:8090"
	}
	token := cfg.Token.ValueString()
	if token == "" {
		token = os.Getenv("SATISFACTORY_TOKEN")
	}

	c := client.New(endpoint, token)
	resp.ResourceData = c
	resp.DataSourceData = c
}

func (p *satisfactoryProvider) Resources(_ context.Context) []func() resource.Resource {
	return []func() resource.Resource{
		newFoundationResource,
		newBuildingResource,
		newBeltResource,
		newPowerLineResource,
	}
}

func (p *satisfactoryProvider) DataSources(_ context.Context) []func() datasource.DataSource {
	return nil
}
