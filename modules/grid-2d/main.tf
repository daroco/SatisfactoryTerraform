locals {
  count_x = max(1, ceil((var.to.x - var.from.x) / var.spacing))
  count_y = max(1, ceil((var.to.y - var.from.y) / var.spacing))

  # Flatten a nested for over (ix, iy) into a list of {key, x, y}, then fold
  # that into the map for_each actually wants. Keys are stable ("ix_iy"),
  # not list indices - see README for why that matters.
  positions = {
    for cell in flatten([
      for ix in range(local.count_x) : [
        for iy in range(local.count_y) : {
          key = "${ix}_${iy}"
          x   = var.from.x + ix * var.spacing
          y   = var.from.y + iy * var.spacing
        }
      ]
    ]) : cell.key => { x = cell.x, y = cell.y }
  }
}
