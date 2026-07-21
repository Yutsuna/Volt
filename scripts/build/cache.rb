# frozen_string_literal: true

require 'json'

module Volt::Build
  class Cache
    attr_reader :path

    def initialize( build_type, cmake_flags, binary_path, build_dir )
      @build_type   = build_type
      @cmake_flags  = cmake_flags
      @binary_path  = binary_path
      @build_dir    = build_dir
      @path         = File.join( @build_dir, '.volt_build_cache.json' )
    end

    def valid?
      return false unless File.exist?( File.join( @build_dir, 'CMakeCache.txt' ) )
      return false unless File.exist?( @path )

      cached_state = JSON.parse( File.read( @path ) ) rescue nil
      return false unless cached_state

      cached_state[ 'build_type' ] == @build_type && cached_state[ 'cmake_flags' ] == @cmake_flags
    end

    def save!
      save_configure!
    end

    def save_configure!
      File.write( @path, JSON.pretty_generate(
        'build_type'  => @build_type,
        'cmake_flags' => @cmake_flags
      ) )
    end
  end
end
