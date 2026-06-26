require "colorize"


module Volt::CLI


  class Logger

    enum Level
      Debug
      Info
      Warn
      Error
      Progress
    end

    record LogMessage, level : Level, step : String?, text : String, finished : Bool = false

    @@channel = Channel( LogMessage ).new 1000
    @@done = Channel( Nil ).new
    @@worker_fiber : Fiber? = nil
    @@last_progress : LogMessage? = nil

    private def self.ensure_channel
      if @@channel.closed?
        @@channel = Channel( LogMessage ).new( 1000 )
        @@done = Channel( Nil ).new
      end
    end


    def self.start
      return if @@worker_fiber
      ensure_channel
      @@worker_fiber = spawn do
        loop do
          message = @@channel.receive?
          break unless message
          process(message)
        end
        if @@last_progress
          print "\r\e[2K"
          STDOUT.flush
        end
        @@done.send(nil)
      end
    end

    def self.stop
      return unless @@worker_fiber
      @@channel.close
      @@done.receive
      @@worker_fiber = nil
    end

    private def self.process( msg : LogMessage )
      if @@last_progress
        print "\r\e[2K"
      end

      case msg.level
      when Level::Progress
        if msg.finished
          print_progress msg
          print "\n"
          @@last_progress = nil
        else
          print_progress msg
          @@last_progress = msg
        end
        STDOUT.flush
      else
        case msg.level
        when Level::Info
          step_part = msg.step ? "[#{msg.step}] ".colorize( :cyan ).bold : ""
          puts "#{step_part}#{msg.text}"
        when Level::Warn
          step_part = msg.step ? "[#{msg.step}] " : ""
          puts "#{step_part}#{"warning: ".colorize( :yellow ).bold}#{msg.text}"
        when Level::Error
          step_part = msg.step ? "[#{msg.step}] " : ""
          STDERR.puts "#{step_part}#{"error: ".colorize( :red ).bold}#{msg.text}"
        when Level::Debug
          step_part = msg.step ? "[#{msg.step}] " : ""
          puts "#{step_part}#{"debug: ".colorize( :dark_gray )}#{msg.text}"
        end

        if last = @@last_progress
          print_progress( last )
          STDOUT.flush
        end
      end
    end

    private def self.print_progress(msg : LogMessage)
      step_part = msg.step ? "[#{msg.step}] ".colorize( :green ).bold : ""
      print "\r#{step_part}#{msg.text}"
    end

    def self.info( text : String, step : String? = nil )
      ensure_channel
      @@channel.send( LogMessage.new( Level::Info, step, text ) )
    end


    def self.warn( text : String, step : String? = nil )
      ensure_channel
      @@channel.send( LogMessage.new( Level::Warn, step, text ) )
    end


    def self.error( text : String, step : String? = nil )
      ensure_channel
      @@channel.send( LogMessage.new( Level::Error, step, text ) )
    end


    def self.debug( text : String, step : String? = nil )
      ensure_channel
      @@channel.send( LogMessage.new( Level::Debug, step, text ) )
    end


    def self.progress( text : String, step : String? = nil, finished : Bool = false )
      ensure_channel
      @@channel.send( LogMessage.new( Level::Progress, step, text, finished ) )
    end
  end


end
