module Volt
  module Backend


    class FLinker

      CC    = "cc"
      MOLD  = "mold"

      def initialize ( @reporter : Diagnostic::FReporter )
      end

      #--------------------------------------------------------------------------

      def build ( ir : String, output : String ) : Bool
        ll_path  = "#{output}.ll"
        obj_path = "#{output}.o"

        File.write( ll_path, ir )

        return false unless run( "llc", [ "-filetype=obj", "-relocation-model=pic", ll_path, "-o", obj_path ] )

        cc_args = [ obj_path, "-o", output ]

        if Process.find_executable MOLD
          cpu_count = System.cpu_count
          FLog.info("Linking with mold with #{cpu_count} threads...")
          cc_args << "-fuse-ld=mold"
          cc_args << "-Wl,--thread-count=#{System.cpu_count}"
        end

        return false unless run( CC, cc_args )

        File.delete? obj_path
        true
      end

      #--------------------------------------------------------------------------

      private def run ( command : String, args : Array(String) ) : Bool
        FLog.command "#{command} #{args.join(" ")}"
        error = IO::Memory.new
        status = Process.run(command, args, error: error, output: Process::Redirect::Close)

        if status.success?
          return true
        end

        FLog.command_done " failed."

        @reporter.error("#{command} failed: #{error.to_s.strip}")
        false
      end

    end


  end
end
