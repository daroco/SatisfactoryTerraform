package provider

import (
	"context"

	"github.com/google/uuid"
	"github.com/hashicorp/terraform-plugin-framework/path"
	"github.com/hashicorp/terraform-plugin-framework/resource"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/int64planmodifier"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/planmodifier"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/stringdefault"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/stringplanmodifier"
	"github.com/hashicorp/terraform-plugin-framework/types"

	"github.com/daroco/satisfacto-form/internal/api"
	"github.com/daroco/satisfacto-form/internal/client"
)

// connectionResource implements both satisfactory_belt and
// satisfactory_power_line: the API shape is identical, only the buildable
// class differs (conveyor marks vs. Build_PowerLine_C).
type connectionResource struct {
	client       *client.Client
	suffix       string // "_belt" or "_power_line"
	description  string
	classDesc    string
	defaultClass string // "" means class is required
}

func newBeltResource() resource.Resource {
	return &connectionResource{
		suffix:      "_belt",
		description: "A conveyor belt between two factory connectors.",
		classDesc:   "Belt class, Build_ConveyorBeltMk1_C through Build_ConveyorBeltMk6_C.",
	}
}

func newPowerLineResource() resource.Resource {
	return &connectionResource{
		suffix:       "_power_line",
		description:  "A power line between two power connectors.",
		classDesc:    "Wire class; defaults to Build_PowerLine_C.",
		defaultClass: "Build_PowerLine_C",
	}
}

type connectionModel struct {
	ID            types.String `tfsdk:"id"`
	Class         types.String `tfsdk:"class"`
	FromID        types.String `tfsdk:"from_id"`
	FromConnector types.Int64  `tfsdk:"from_connector"`
	ToID          types.String `tfsdk:"to_id"`
	ToConnector   types.Int64  `tfsdk:"to_connector"`
}

func (r *connectionResource) Metadata(_ context.Context, req resource.MetadataRequest, resp *resource.MetadataResponse) {
	resp.TypeName = req.ProviderTypeName + r.suffix
}

func (r *connectionResource) Schema(_ context.Context, _ resource.SchemaRequest, resp *resource.SchemaResponse) {
	classAttr := schema.StringAttribute{
		Description:   r.classDesc,
		PlanModifiers: []planmodifier.String{stringplanmodifier.RequiresReplace()},
	}
	if r.defaultClass != "" {
		classAttr.Optional = true
		classAttr.Computed = true
		classAttr.Default = stringdefault.StaticString(r.defaultClass)
	} else {
		classAttr.Required = true
	}
	replaceStr := []planmodifier.String{stringplanmodifier.RequiresReplace()}
	replaceInt := []planmodifier.Int64{int64planmodifier.RequiresReplace()}
	resp.Schema = schema.Schema{
		Description: r.description + " Every change replaces it.",
		Attributes: map[string]schema.Attribute{
			"id": schema.StringAttribute{
				Computed:      true,
				Description:   "Terraform-assigned stable ID, persisted in the save game by the mod.",
				PlanModifiers: []planmodifier.String{stringplanmodifier.UseStateForUnknown()},
			},
			"class": classAttr,
			"from_id": schema.StringAttribute{
				Required:      true,
				Description:   "id of the buildable the source end attaches to.",
				PlanModifiers: replaceStr,
			},
			"from_connector": schema.Int64Attribute{
				Required:      true,
				Description:   "Zero-based connector index on the source buildable.",
				PlanModifiers: replaceInt,
			},
			"to_id": schema.StringAttribute{
				Required:      true,
				Description:   "id of the buildable the destination end attaches to.",
				PlanModifiers: replaceStr,
			},
			"to_connector": schema.Int64Attribute{
				Required:      true,
				Description:   "Zero-based connector index on the destination buildable.",
				PlanModifiers: replaceInt,
			},
		},
	}
}

func (r *connectionResource) Configure(_ context.Context, req resource.ConfigureRequest, resp *resource.ConfigureResponse) {
	r.client = configureClient(req, resp)
}

func connectionFromAPI(c api.Connection) connectionModel {
	return connectionModel{
		ID:            types.StringValue(c.TFID),
		Class:         types.StringValue(c.Class),
		FromID:        types.StringValue(c.From.BuildableTFID),
		FromConnector: types.Int64Value(c.From.Connector),
		ToID:          types.StringValue(c.To.BuildableTFID),
		ToConnector:   types.Int64Value(c.To.Connector),
	}
}

func (r *connectionResource) Create(ctx context.Context, req resource.CreateRequest, resp *resource.CreateResponse) {
	var plan connectionModel
	resp.Diagnostics.Append(req.Plan.Get(ctx, &plan)...)
	if resp.Diagnostics.HasError() {
		return
	}
	created, err := r.client.CreateConnection(ctx, api.Connection{
		TFID:  uuid.NewString(),
		Class: plan.Class.ValueString(),
		From: api.ConnectionEndpoint{
			BuildableTFID: plan.FromID.ValueString(),
			Connector:     plan.FromConnector.ValueInt64(),
		},
		To: api.ConnectionEndpoint{
			BuildableTFID: plan.ToID.ValueString(),
			Connector:     plan.ToConnector.ValueInt64(),
		},
	})
	if err != nil {
		resp.Diagnostics.AddError("Failed to build connection", err.Error())
		return
	}
	resp.Diagnostics.Append(resp.State.Set(ctx, connectionFromAPI(created))...)
}

func (r *connectionResource) Read(ctx context.Context, req resource.ReadRequest, resp *resource.ReadResponse) {
	var state connectionModel
	resp.Diagnostics.Append(req.State.Get(ctx, &state)...)
	if resp.Diagnostics.HasError() {
		return
	}
	c, err := r.client.GetConnection(ctx, state.ID.ValueString())
	if client.IsNotFound(err) {
		resp.State.RemoveResource(ctx)
		return
	}
	if err != nil {
		resp.Diagnostics.AddError("Failed to read connection", err.Error())
		return
	}
	resp.Diagnostics.Append(resp.State.Set(ctx, connectionFromAPI(c))...)
}

// Update is never called: every attribute forces replacement.
func (r *connectionResource) Update(_ context.Context, _ resource.UpdateRequest, _ *resource.UpdateResponse) {
}

func (r *connectionResource) Delete(ctx context.Context, req resource.DeleteRequest, resp *resource.DeleteResponse) {
	var state connectionModel
	resp.Diagnostics.Append(req.State.Get(ctx, &state)...)
	if resp.Diagnostics.HasError() {
		return
	}
	if err := r.client.DeleteConnection(ctx, state.ID.ValueString()); err != nil {
		resp.Diagnostics.AddError("Failed to dismantle connection", err.Error())
	}
}

func (r *connectionResource) ImportState(ctx context.Context, req resource.ImportStateRequest, resp *resource.ImportStateResponse) {
	resource.ImportStatePassthroughID(ctx, path.Root("id"), req, resp)
}
