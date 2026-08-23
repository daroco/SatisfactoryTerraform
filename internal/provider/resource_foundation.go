package provider

import (
	"context"

	"github.com/google/uuid"
	"github.com/hashicorp/terraform-plugin-framework/path"
	"github.com/hashicorp/terraform-plugin-framework/resource"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/float64default"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/float64planmodifier"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/planmodifier"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/stringplanmodifier"
	"github.com/hashicorp/terraform-plugin-framework/types"

	"github.com/daroco/satisfacto-form/internal/api"
	"github.com/daroco/satisfacto-form/internal/client"
)

type foundationResource struct {
	client *client.Client
}

func newFoundationResource() resource.Resource { return &foundationResource{} }

type foundationModel struct {
	ID    types.String  `tfsdk:"id"`
	Class types.String  `tfsdk:"class"`
	X     types.Float64 `tfsdk:"x"`
	Y     types.Float64 `tfsdk:"y"`
	Z     types.Float64 `tfsdk:"z"`
	Yaw   types.Float64 `tfsdk:"yaw"`
}

func (r *foundationResource) Metadata(_ context.Context, req resource.MetadataRequest, resp *resource.MetadataResponse) {
	resp.TypeName = req.ProviderTypeName + "_foundation"
}

func (r *foundationResource) Schema(_ context.Context, _ resource.SchemaRequest, resp *resource.SchemaResponse) {
	resp.Schema = schema.Schema{
		Description: "A foundation (or other passive structural buildable) placed in the world. Every change replaces it.",
		Attributes: map[string]schema.Attribute{
			"id": schema.StringAttribute{
				Computed:      true,
				Description:   "Terraform-assigned stable ID, persisted in the save game by the mod.",
				PlanModifiers: []planmodifier.String{stringplanmodifier.UseStateForUnknown()},
			},
			"class": schema.StringAttribute{
				Required:      true,
				Description:   "Buildable class name, e.g. Build_Foundation_8x4_01_C.",
				PlanModifiers: []planmodifier.String{stringplanmodifier.RequiresReplace()},
			},
			"x": schema.Float64Attribute{
				Required:      true,
				Description:   "World X in centimetres.",
				PlanModifiers: []planmodifier.Float64{float64planmodifier.RequiresReplace()},
			},
			"y": schema.Float64Attribute{
				Required:      true,
				Description:   "World Y in centimetres.",
				PlanModifiers: []planmodifier.Float64{float64planmodifier.RequiresReplace()},
			},
			"z": schema.Float64Attribute{
				Required:      true,
				Description:   "World Z in centimetres.",
				PlanModifiers: []planmodifier.Float64{float64planmodifier.RequiresReplace()},
			},
			"yaw": schema.Float64Attribute{
				Optional:      true,
				Computed:      true,
				Default:       float64default.StaticFloat64(0),
				Description:   "Rotation around the vertical axis in degrees.",
				PlanModifiers: []planmodifier.Float64{float64planmodifier.RequiresReplace()},
			},
		},
	}
}

func (r *foundationResource) Configure(_ context.Context, req resource.ConfigureRequest, resp *resource.ConfigureResponse) {
	r.client = configureClient(req, resp)
}

func (r *foundationResource) Create(ctx context.Context, req resource.CreateRequest, resp *resource.CreateResponse) {
	var plan foundationModel
	resp.Diagnostics.Append(req.Plan.Get(ctx, &plan)...)
	if resp.Diagnostics.HasError() {
		return
	}
	plan.ID = types.StringValue(uuid.NewString())
	created, err := r.client.CreateBuildable(ctx, api.Buildable{
		TFID:  plan.ID.ValueString(),
		Class: plan.Class.ValueString(),
		Transform: api.Transform{
			X:   plan.X.ValueFloat64(),
			Y:   plan.Y.ValueFloat64(),
			Z:   plan.Z.ValueFloat64(),
			Yaw: plan.Yaw.ValueFloat64(),
		},
	})
	if err != nil {
		resp.Diagnostics.AddError("Failed to place foundation", err.Error())
		return
	}
	// See preserveWithinEpsilon (resource_building.go): keep plan values
	// when the mod's echo differs only by float round-trip noise.
	next := foundationFromAPI(created)
	next.X = preserveWithinEpsilon(plan.X, next.X)
	next.Y = preserveWithinEpsilon(plan.Y, next.Y)
	next.Z = preserveWithinEpsilon(plan.Z, next.Z)
	next.Yaw = preserveWithinEpsilon(plan.Yaw, next.Yaw)
	resp.Diagnostics.Append(resp.State.Set(ctx, next)...)
}

func foundationFromAPI(b api.Buildable) foundationModel {
	return foundationModel{
		ID:    types.StringValue(b.TFID),
		Class: types.StringValue(b.Class),
		X:     types.Float64Value(b.Transform.X),
		Y:     types.Float64Value(b.Transform.Y),
		Z:     types.Float64Value(b.Transform.Z),
		Yaw:   types.Float64Value(b.Transform.Yaw),
	}
}

func (r *foundationResource) Read(ctx context.Context, req resource.ReadRequest, resp *resource.ReadResponse) {
	var state foundationModel
	resp.Diagnostics.Append(req.State.Get(ctx, &state)...)
	if resp.Diagnostics.HasError() {
		return
	}
	b, err := r.client.GetBuildable(ctx, state.ID.ValueString())
	if client.IsNotFound(err) {
		resp.State.RemoveResource(ctx)
		return
	}
	if err != nil {
		resp.Diagnostics.AddError("Failed to read foundation", err.Error())
		return
	}
	// See preserveWithinEpsilon (resource_building.go): absorb float32
	// save/load quantization so reloads don't read as replace-drift.
	next := foundationFromAPI(b)
	next.X = preserveWithinEpsilon(state.X, next.X)
	next.Y = preserveWithinEpsilon(state.Y, next.Y)
	next.Z = preserveWithinEpsilon(state.Z, next.Z)
	next.Yaw = preserveWithinEpsilon(state.Yaw, next.Yaw)
	resp.Diagnostics.Append(resp.State.Set(ctx, next)...)
}

// Update is never called: every attribute forces replacement.
func (r *foundationResource) Update(_ context.Context, _ resource.UpdateRequest, _ *resource.UpdateResponse) {
}

func (r *foundationResource) Delete(ctx context.Context, req resource.DeleteRequest, resp *resource.DeleteResponse) {
	var state foundationModel
	resp.Diagnostics.Append(req.State.Get(ctx, &state)...)
	if resp.Diagnostics.HasError() {
		return
	}
	if err := r.client.DeleteBuildable(ctx, state.ID.ValueString()); err != nil {
		resp.Diagnostics.AddError("Failed to dismantle foundation", err.Error())
	}
}

func (r *foundationResource) ImportState(ctx context.Context, req resource.ImportStateRequest, resp *resource.ImportStateResponse) {
	resource.ImportStatePassthroughID(ctx, path.Root("id"), req, resp)
}
