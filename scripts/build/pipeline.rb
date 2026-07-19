# frozen_string_literal: true

require 'etc'
require 'fileutils'
require 'json'


module Volt::Build


  class Pipeline

    def initialize( options )
      @options      = options
      @binary_path  = determine_binary_path
      @cache        = Cache.new( @options[ :build_type ], @options[ :cmake_flags ], @binary_path )
    end

    def execute!
      steps.each { |step| break if send( step ) == :halt }
    end

    private

    def steps
      return [ :clean ]               if @options[ :clean ]
      return [ :configure, :format ]  if @options[ :format ]
      return [ :configure, :tidy ]    if @options[ :tidy ]
      [ :validate, :configure, :build, :persist, :epilogue ]
    end

    def clean
      Logger.info "Cleaning build directory..."
      FileUtils.rm_rf( 'build' )
      :halt
    end

    def validate
      return :continue unless @cache.valid?

      Logger.ok "No changes detected. Build is up-to-date (Cached)."
      epilogue
    end

    def format
      run_tool 'format', "Formatting codebase with clang-format (incremental)..."
    end

    def tidy
      run_tool 'tidy', "Linting codebase with clang-tidy (parallel, incremental)..."
    end

    def run_tool( target, message )
      Logger.info message
      system( 'cmake', '--build', 'build', '--target', target ) or Logger.fatal!( "Target '#{target}' failed." )
      Logger.ok "#{target.capitalize} completed."
      :halt
    end

    def configure
      return :continue unless configure_required?

      Logger.info "Configuring CMake (Mode: #{@options[:build_type]})..."
      FileUtils.mkdir_p('build')

      definitions = @options[:cmake_flags].map { |k, v| "-D#{k}=#{v}" }.join(' ')
      cmd = "cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=#{@options[:build_type]} #{definitions}"

      system(cmd) ? :continue : Logger.fatal!("CMake configuration failed.")
    end

    def build
      Logger.info "Building targets..."
      system("cmake --build build") ? :continue : Logger.fatal!("Build failed.")
    end

    def persist
      FileUtils.mkdir_p('build')
      @cache.save!
      Logger.ok "Build completed."
      :continue
    end

    def epilogue
      test    if @options[ :test ]
      run_bin if @options[ :run ]
      :halt
    end

    def test
      jobs = Etc.nprocessors
      Logger.info "Running test suites (ctest, #{jobs} jobs)..."
      system( 'ctest', '--test-dir', 'build', '--output-on-failure', '--parallel', jobs.to_s ) or Logger.fatal!( "Tests failed." )
      Logger.ok "All tests passed."
    end

    def run_bin
      Logger.fatal!("Executable not found at #{@binary_path}") unless File.exist?(@binary_path)

      Logger.info "Executing: #{@binary_path} #{@options[:run_args].join(' ')}"
      puts "--------------------------------------------------"
      exec(@binary_path, *@options[:run_args])
    end

    def determine_binary_path
      postfix = @options[:build_type] == 'Debug' ? '_d' : ''
      File.join('build', 'bin', "Volt#{postfix}")
    end

    def configure_required?
      return true unless File.exist?('build/CMakeCache.txt')
      return true unless File.exist?(Cache::PATH)

      cached_state = JSON.parse(File.read(Cache::PATH), symbolize_names: true) rescue nil
      return true unless cached_state

      cached_state[:build_type] != @options[:build_type] ||
        cached_state[:cmake_flags] != @options[:cmake_flags]
    end
  end


end
