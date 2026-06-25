require "./Volt/__all__"

Volt::CLI::Logger.start

Volt::CLI::Logger.info("Starting compilation pipeline...", "volt")
sleep 0.2

10.times do |i|
  step_label = "#{i + 1}/10"

  Volt::CLI::Logger.progress("Processing node classification...", step_label)
  sleep 0.15

  if i == 3
    Volt::CLI::Logger.warn("Deprecated syntax usage in module AST parser", step_label)
    sleep 0.15
  end

  if i == 7
    Volt::CLI::Logger.info("Tree reduction computation complete", step_label)
    sleep 0.15
  end
end

Volt::CLI::Logger.progress("Compilation finished successfully", "10/10", finished: true)

Volt::CLI::Logger.stop
