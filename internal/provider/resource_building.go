package provider

import (
	"context"
	"fmt"
	"math"

	"github.com/google/uuid"
	"github.com/hashicorp/terraform-plugin-framework/path"
	"github.com/hashicorp/terraform-plugin-framework/resource"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/float64default"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/float64planmodifier"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/planmodifier"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/stringdefault"
	"github.com/hashicorp/terraform-plugin-framework/resource/schema/stringplanmodifier"
	"github.com/hashicorp/terraform-plugin-framework/types"

	"github.com/daroco/satisfacto-form/internal/api"
	"github.com/daroco/satisfacto-form/internal/client"
)

type buildingResource struct {
	client *client.Client
}

func newBuildingResource() resource.Resource { return &buildingResource{} }

type buildingModel struct {
	ID         types.String  `tfsdk:"id"`
	Class      types.String  `tfsdk:"class"`
	X          types.Float64 `tfsdk:"x"`
	Y          types.Float64 `tfsdk:"y"`
	Z          types.Float64 `tfsdk:"z"`
	Yaw        types.Float64 `tfsdk:"yaw"`
	Recipe     types.String  `tfsdk:"recipe"`
	ClockSpeed types.Float64 `tfsdk:"clock_speed"`
}

func (r *buildingResource) Metadata(_ context.Context, req resource.MetadataRequest, resp *resource.MetadataResponse) {
	resp.TypeName = req.ProviderTypeName + "_building"
}

func (r *buildingResource) Schema(_ context.Context, _ resource.SchemaRequest, resp *resource.SchemaResponse) {
	resp.Schema = schema.Schema{
		Description: "A production machine (constructor, smelter, assembler, ...) placed in the world.",
		Attributes: map[string]schema.Attribute{
			"id": schema.StringAttribute{
				Computed:      true,
				Description:   "Terraform-assigned stable ID, persisted in the save game by the mod.",
				PlanModifiers: []planmodifier.String{stringplanmodifier.UseStateForUnknown()},
			},
			"class": schema.StringAttribute{
				Required:      true,
				Description:   "Buildable class name, e.g. Build_ConstructorMk1_C or Build_SmelterMk1_C.",
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
			"recipe": schema.StringAttribute{
				Optional:    true,
				Computed:    true,
				Default:     stringdefault.StaticString(""),
				Description: "Recipe class name, e.g. Recipe_IronPlate_C. Changeable in place.",
			},
			"clock_speed": schema.Float64Attribute{
				Optional: true,
				Computed: true,
				Description: "Clock speed as a fraction (1.0 = 100%). Changeable in place. " +
					"Only meaningful for manufacturer buildings (constructors, smelters, ...); " +
					"the mod omits this field entirely for other classes (splitters, power poles, " +
					"...), so it can't have a static default without breaking those - defaults to " +
					"1.0 on create for classes that do support it (see Create).",
			},
		},
	}
}

func (r *buildingResource) Configure(_ context.Context, req resource.ConfigureRequest, resp *resource.ConfigureResponse) {
	r.client = configureClient(req, resp)
}

func (m buildingModel) toAPI() api.Buildable {
	return api.Buildable{
		TFID:  m.ID.ValueString(),
		Class: m.Class.ValueString(),
		Transform: api.Transform{
			X:   m.X.ValueFloat64(),
			Y:   m.Y.ValueFloat64(),
			Z:   m.Z.ValueFloat64(),
			Yaw: m.Yaw.ValueFloat64(),
		},
		Recipe:     m.Recipe.ValueString(),
		ClockSpeed: m.ClockSpeed.ValueFloat64(),
	}
}

func buildingFromAPI(b api.Buildable) buildingModel {
	return buildingModel{
		ID:         types.StringValue(b.TFID),
		Class:      types.StringValue(b.Class),
		X:          types.Float64Value(b.Transform.X),
		Y:          types.Float64Value(b.Transform.Y),
		Z:          types.Float64Value(b.Transform.Z),
		Yaw:        types.Float64Value(b.Transform.Yaw),
		Recipe:     types.StringValue(b.Recipe),
		ClockSpeed: types.Float64Value(b.ClockSpeed),
	}
}

func (r *buildingResource) Create(ctx context.Context, req resource.CreateRequest, resp *resource.CreateResponse) {
	var plan buildingModel
	resp.Diagnostics.Append(req.Plan.Get(ctx, &plan)...)
	if resp.Diagnostics.HasError() {
		return
	}
	plan.ID = types.StringValue(uuid.NewString())
	if plan.ClockSpeed.IsUnknown() || plan.ClockSpeed.IsNull() {
		// No schema-level Default (see Schema) since the mod omits
		// clock_speed entirely for non-manufacturer classes - applying
		// that default here instead of asserting it at plan time avoids
		// a "provider produced inconsistent result" error when the
		// created buildable turns out not to support it.
		plan.ClockSpeed = types.Float64Value(1.0)
	}
	created, err := r.client.CreateBuildable(ctx, plan.toAPI())
	if err != nil {
		resp.Diagnostics.AddError("Failed to spawn building", err.Error())
		return
	}
	resp.Diagnostics.Append(resp.State.Set(ctx, buildingFromAPI(created))...)
}

func (r *buildingResource) Read(ctx context.Context, req resource.ReadRequest, resp *resource.ReadResponse) {
	var state buildingModel
	resp.Diagnostics.Append(req.State.Get(ctx, &state)...)
	if resp.Diagnostics.HasError() {
		return
	}
	b, err := r.client.GetBuildable(ctx, state.ID.ValueString())
	if client.IsNotFound(err) {
		// Dismantled in-game: drop from state so the next plan recreates it.
		resp.State.RemoveResource(ctx)
		return
	}
	if err != nil {
		resp.Diagnostics.AddError("Failed to read building", err.Error())
		return
	}
	next := buildingFromAPI(b)
	next.X = preserveWithinEpsilon(state.X, next.X)
	next.Y = preserveWithinEpsilon(state.Y, next.Y)
	next.Z = preserveWithinEpsilon(state.Z, next.Z)
	next.Yaw = preserveWithinEpsilon(state.Yaw, next.Yaw)
	resp.Diagnostics.Append(resp.State.Set(ctx, next)...)
}

func (r *buildingResource) Update(ctx context.Context, req resource.UpdateRequest, resp *resource.UpdateResponse) {
	var plan buildingModel
	resp.Diagnostics.Append(req.Plan.Get(ctx, &plan)...)
	if resp.Diagnostics.HasError() {
		return
	}
	recipe := plan.Recipe.ValueString()
	clock := plan.ClockSpeed.ValueFloat64()
	updated, err := r.client.PatchBuildable(ctx, plan.ID.ValueString(), api.BuildablePatch{
		Recipe:     &recipe,
		ClockSpeed: &clock,
	})
	if err != nil {
		resp.Diagnostics.AddError("Failed to update building", err.Error())
		return
	}
	resp.Diagnostics.Append(resp.State.Set(ctx, buildingFromAPI(updated))...)
}

func (r *buildingResource) Delete(ctx context.Context, req resource.DeleteRequest, resp *resource.DeleteResponse) {
	var state buildingModel
	resp.Diagnostics.Append(req.State.Get(ctx, &state)...)
	if resp.Diagnostics.HasError() {
		return
	}
	if err := r.client.DeleteBuildable(ctx, state.ID.ValueString()); err != nil {
		resp.Diagnostics.AddError("Failed to dismantle building", err.Error())
	}
}

func (r *buildingResource) ImportState(ctx context.Context, req resource.ImportStateRequest, resp *resource.ImportStateResponse) {
	resource.ImportStatePassthroughID(ctx, path.Root("id"), req, resp)
}

// transformEpsilon (in the transform's own units: centimetres for x/y/z,
// degrees for yaw) absorbs the float32 quantization the game's save/load
// round-trip applies to actor transforms. Confirmed live (issue #10): a
// building created at x = -224375.55 reads back as -224375.55 within the
// same session, but -224375.546875 after a quit/relaunch - a pure precision
// artifact, not real movement. Without this, every non-integer coordinate
// becomes permanent replace-drift after the first reload, since x/y/z/yaw
// all RequiresReplace. 0.5 is far above the worst quantization error at
// map-scale coordinates (float32 ULP at ~500km is ~0.06cm) and far below
// any difference that could mean an actually-moved buildable.
const transformEpsilon = 0.5

// preserveWithinEpsilon keeps the state's value when the API's reading
// differs only by float32 round-trip noise, so Read doesn't manufacture
// drift; a genuinely different value (in-game movement, manual state edits)
// still comes through.
func preserveWithinEpsilon(state, api types.Float64) types.Float64 {
	if !state.IsNull() && !state.IsUnknown() &&
		math.Abs(state.ValueFloat64()-api.ValueFloat64()) < transformEpsilon {
		return state
	}
	return api
}

// configureClient extracts the shared API client from provider configure data.
func configureClient(req resource.ConfigureRequest, resp *resource.ConfigureResponse) *client.Client {
	if req.ProviderData == nil {
		return nil
	}
	c, ok := req.ProviderData.(*client.Client)
	if !ok {
		resp.Diagnostics.AddError(
			"Unexpected provider data",
			fmt.Sprintf("expected *client.Client, got %T", req.ProviderData),
		)
		return nil
	}
	return c
}
