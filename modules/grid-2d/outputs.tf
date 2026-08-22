output "positions" {
  value       = local.positions
  description = <<-EOT
    Map of stable grid-cell key ("ix_iy") to {x, y} in centimetres. Feed
    directly into a resource's for_each, e.g.:

      resource "satisfactory_foundation" "floor" {
        for_each = module.floor.positions
        class    = "Build_Foundation_8x4_01_C"
        x        = each.value.x
        y        = each.value.y
        z        = local.base_z
      }
  EOT
}

output "count" {
  value       = local.count_x * local.count_y
  description = "Total number of cells (count_x * count_y) - handy for sanity-checking a range before applying."
}
