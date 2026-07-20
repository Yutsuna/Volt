# frozen_string_literal: true

require 'etc'
require 'fileutils'


module Volt
  module Build


    $LOAD_PATH.unshift( File.expand_path( __dir__ ) )

    autoload :Logger,   'logger'
    autoload :Options,  'options'
    autoload :Cache,    'cache'
    autoload :Pipeline, 'pipeline'

    module SystemInfo
      class << self
        def total_cpus
          @total_cpus ||= Etc.nprocessors
        end

        def total_memory_gb
          @total_memory_gb ||= fetch_system_memory_gb
        end

        def max_safe_parallel_jobs
          mem_jobs = ( total_memory_gb / 3.0 ).floor
          cpu_jobs = ( total_cpus / 2.0 ).ceil
          [ mem_jobs, cpu_jobs, 4 ].min.clamp( 1, total_cpus )
        end

        def ninja_threads_per_job( active_jobs_count )
          count = [ active_jobs_count, 1 ].max
          [ ( total_cpus.to_f / count ).floor, 1 ].max
        end

        private

        def fetch_system_memory_gb
          if File.exist?( '/proc/meminfo' )
            mem_line = File.readlines( '/proc/meminfo' ).find { |l| l.start_with?( 'MemTotal:' ) }
            if mem_line && ( m = mem_line.match( /(\d+)/ ) )
              return m[ 1 ].to_i / 1_048_576.0
            end
          end

          pages = Etc.sysconf( Etc::SC_PHYS_PAGES ) rescue 2_097_152
          page_size = Etc.sysconf( Etc::SC_PAGE_SIZE ) rescue 4096
          ( pages * page_size ) / ( 1024.0**3 )
        rescue
          8.0
        end
      end
    end

    class Scheduler
      class << self
        def run!( parse_result )
          return Options.show_usage if parse_result[ :help ]

          clean_build_dir if parse_result[ :clean_first ]

          jobs = parse_result[ :jobs ]
          return if jobs.empty?

          if jobs.size == 1
            execute_single_job( jobs.first )
          else
            execute_parallel_jobs( jobs )
          end
        end

        private

        def clean_build_dir
          Logger.info "Cleaning build directory..."
          FileUtils.rm_rf( 'build' )
        end

        def execute_single_job( job_options )
          threads = SystemInfo.total_cpus
          pipeline = Pipeline.new( job_options, threads_per_job: threads )
          pipeline.execute!
        end

        def execute_parallel_jobs( jobs )
          max_parallel = SystemInfo.max_safe_parallel_jobs
          Logger.info "Running #{jobs.size} build jobs in parallel (Pool limit: #{max_parallel} worker processes)..."

          queue = jobs.dup
          active_pids = {}
          failed_variants = []

          begin
            while queue.any? || active_pids.any?
              while queue.any? && active_pids.size < max_parallel
                job = queue.shift
                threads = SystemInfo.ninja_threads_per_job( [ queue.size + active_pids.size + 1, max_parallel ].min )

                pid = Process.fork do
                  pipeline = Pipeline.new( job, threads_per_job: threads )
                  pipeline.execute!
                  exit 0
                rescue StandardError => e
                  Logger.warn "Job exception for #{job[:variant_name]}: #{e.message}"
                  exit 1
                end

                active_pids[ pid ] = job
              end

              begin
                pid, status = Process.wait2( -1, Process::WNOHANG )
                if pid
                  finished_job = active_pids.delete( pid )
                  if finished_job && !status.success?
                    failed_variants << finished_job[ :variant_name ]
                  end
                else
                  sleep 0.05
                end
              rescue Errno::ECHILD
                break
              end
            end
          rescue Interrupt
            Logger.warn "Build interrupted by user. Terminating worker processes..."
            raise
          ensure
            kill_active_workers( active_pids.keys )
          end

          if failed_variants.any?
            Logger.fatal!( "Parallel execution failed for variant(s): #{failed_variants.join( ', ' )}" )
          else
            Logger.ok "All #{jobs.size} parallel jobs completed successfully."
          end
        end

        def kill_active_workers( pids )
          pids.each do |pid|
            Process.kill( 'TERM', pid ) rescue nil
          end
          pids.each do |pid|
            Process.waitpid( pid, Process::WNOHANG ) rescue nil
          end
        end
      end
    end

    def self.run!
      parse_result = Options.parse
      Scheduler.run!( parse_result )
    end


  end
end
