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

    def self.show_usage
      puts <<~USAGE
        Volt Build Tool

        Usage: volt-build [options] [-- [run_args]]

        Commands:
          clean         Clean the build directory and cache.
          help, -h      Show this help message.

        Build Types:
          debug         Build in Debug mode (default, executable: Volt_d).
          release       Build in Release mode (executable: Volt).

        Sanitizers & Features:
          asan          Enable AddressSanitizer (VOLT_ENABLE_ASAN=ON).
          ubsan         Enable UndefinedBehaviorSanitizer (VOLT_ENABLE_UBSAN=ON).
          tsan          Enable ThreadSanitizer (VOLT_ENABLE_TSAN=ON).
          testing       Enable Testing (VOLT_ENABLE_TESTING=ON).

        Execution:
          run           Run the built binary after a successful build.
          --            Pass all subsequent arguments directly to the Volt binary.

        Examples:
          volt-build clean
          volt-build release run
          volt-build debug asan run -- --verbose
      USAGE
    end

    private

    def self.default_options
      {
        build_type: 'Debug',
        run: false,
        clean: false,
        cmake_flags: FLAGS_MAP.values.each_with_object({}) { |var, h| h[var] = 'OFF' },
        run_args: [],
        help: false
      }
    end

    def self.parse_args(options)
      args = ARGV.dup
      until args.empty?
        arg = args.shift.downcase

        case arg
        when 'help', '-h', '--help'
          options[:help] = true
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
