module Volt::Core


  # Core stdlib sources embedded at (Crystal) build time — the "linked by
  # default" half of the hybrid distribution. `VOLT_CORE=<dir>` bypasses
  # this list at runtime and reads textual `.vl` sources instead (see
  # `Std.sources`), so the stdlib can be developed without rebuilding.
  #
  # The list is built at compile time by scanning `Std/**/*.vl` (sorted,
  # so declaration order is stable and deterministic) — dropping a new
  # file into `Std/` embeds it automatically, no manual registration.
  {% begin %}
    EMBEDDED_SOURCES = [
      {% for path in `find #{__DIR__}/../../../Std -name "*.vl" | sort`.stringify.split( "\n" ).reject( &.empty? ) %}
        { {{ path.split( "/Std/" ).last }}, {{ read_file( path ) }} },
      {% end %}
    ]
  {% end %}


end
