module Volt::Core


  # Core stdlib sources embedded at (Crystal) build time — the "linked by
  # default" half of the hybrid distribution. `VOLT_CORE=<dir>` bypasses
  # this list at runtime and reads textual `.vl` sources instead (see
  # `Core.sources`), so the stdlib can be developed without rebuilding.
  #
  # The list is explicit and ordered: declaration order is program order
  # once the nodes are prepended to the user program.
  EMBEDDED_SOURCES = [
    { "String.vl", {{ read_file( "#{__DIR__}/../../../Core/String.vl" ) }} },
    { "IO.vl", {{ read_file( "#{__DIR__}/../../../Core/IO.vl" ) }} },
    { "Int.vl", {{ read_file( "#{__DIR__}/../../../Core/Int.vl" ) }} },
    { "Bool.vl", {{ read_file( "#{__DIR__}/../../../Core/Bool.vl" ) }} },
    { "Float.vl", {{ read_file( "#{__DIR__}/../../../Core/Float.vl" ) }} },
    { "Regex.vl", {{ read_file( "#{__DIR__}/../../../Core/Regex.vl" ) }} },
  ]


end
