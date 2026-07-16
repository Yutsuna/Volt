# frozen_string_literal: true

module Volt::Build


  class Options

    FLAGS_MAP = {
      'asan'    => 'VOLT_ENABLE_ASAN',
      'ubsan'   => 'VOLT_ENABLE_UBSAN',
      'tsan'    => 'VOLT_ENABLE_TSAN',
      'testing' => 'VOLT_ENABLE_TESTING'
    }

    def self.parse
      options = default_options
      parse_args(options)
      options
    end

    private

    def self.default_options
      {
        build_type: 'Debug',
        run: false,
        clean: false,
        cmake_flags: FLAGS_MAP.values.each_with_object({}) { |var, h| h[var] = 'OFF' },
        run_args: []
      }
    end

    def self.parse_args(options)
      args = ARGV.dup
      until args.empty?
        arg = args.shift.downcase

        case arg
        when 'debug'
          options[:build_type] = 'Debug'
        when 'release'
          options[:build_type] = 'Release'
        when 'run'
          options[:run] = true
        when 'clean'
          options[:clean] = true
        when *FLAGS_MAP.keys
          options[:cmake_flags][FLAGS_MAP[arg]] = 'ON'
        when '--'
          options[:run_args].concat(args)
          break
        else
          options[:run_args] << arg
        end
      end
    end

  end


end
