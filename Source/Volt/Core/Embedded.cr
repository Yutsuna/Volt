module Volt::Core


  # Core stdlib sources embedded at (Crystal) build time — the "linked by
  # default" half of the hybrid distribution. `VOLT_CORE=<dir>` bypasses
  # this list at runtime and reads textual `.vl` sources instead (see
  # `Std.sources`), so the stdlib can be developed without rebuilding.
  #
  # The list is built at compile time by scanning `Lib/**/*.vl` (sorted,
  # so declaration order is stable and deterministic) — dropping a new
  # file into `Lib/` embeds it automatically, no manual registration.
  {% begin %}
    EMBEDDED_SOURCES = [
      {% for path in `find #{__DIR__}/../../../Lib/ -name "*.vl" | sort`.stringify.split( "\n" ).reject( &.empty? ) %}
        { {{ path.split( "/Lib/" ).last }}, {{ read_file( path ) }} },
      {% end %}
    ]
  {% end %}


end
