#!/usr/bin/env ruby
# frozen_string_literal: true

require_relative 'process/termination'
require_relative 'process/manager'

Volt::ProcessManager::Termination.setup!

require_relative 'build/run'

Volt::Build.run! if __FILE__ == $0
