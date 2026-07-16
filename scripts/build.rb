#!/usr/bin/env ruby
# frozen_string_literal: true

require_relative 'build/run'

Volt::Build.run! if __FILE__ == $0
