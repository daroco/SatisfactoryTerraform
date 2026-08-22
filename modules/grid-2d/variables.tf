variable "from" {
  type        = object({ x = number, y = number })
  description = "One corner of the bounding box, in centimetres (Unreal units)."
}

variable "to" {
  type        = object({ x = number, y = number })
  description = <<-EOT
    The opposite corner of the bounding box, in centimetres. Cells are placed
    on a regular grid starting at `from`; `to` bounds how far that grid
    extends but doesn't have to land exactly on a cell boundary (the last
    row/column is whichever cell covers it, per `ceil()`).
  EOT
}

variable "spacing" {
  type        = number
  description = <<-EOT
    Grid pitch in centimetres, applied to both axes. For foundations this is
    the buildable's own footprint (e.g. 800 for Build_Foundation_8x4_01_C);
    for spacing out repeated buildings it's however far apart you want them
    - the module has no notion of a building's actual in-game footprint (see
    the module README for why).
  EOT

  validation {
    condition     = var.spacing > 0
    error_message = "spacing must be a positive number of centimetres."
  }
}
