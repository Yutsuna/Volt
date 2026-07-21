require_relative 'system_info'


module Volt
  module Build


    class Scheduler
      class << self
        def run!( parse_result )
          return Options.show_usage if parse_result[ :help ]

          if parse_result[ :only_clean ]
            clean_build_dir
            return
          end

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

                pid = Volt::ProcessManager.fork_child do
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
                result = Volt::ProcessManager.wait_any( Process::WNOHANG )
                if result
                  pid, status = result
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
            Volt::ProcessManager::Termination.terminate_all!
          end

          if failed_variants.any?
            Logger.fatal!( "Parallel execution failed for variant(s): #{failed_variants.join( ', ' )}" )
          else
            Logger.ok "All #{jobs.size} parallel jobs completed successfully."
          end
        end
      end
    end


  end
end
