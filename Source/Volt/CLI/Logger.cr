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

    @@channel = Channel(LogMessage).new(1000)
    @@done = Channel(Nil).new
    @@worker_fiber : Fiber? = nil
    @@last_progress : LogMessage? = nil

    private def self.ensure_channel
      if @@channel.closed?
        @@channel = Channel(LogMessage).new(1000)
        @@done = Channel(Nil).new
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

    private def self.process(msg : LogMessage)
      if @@last_progress
        print "\r\e[2K"
      end

      if msg.level == Level::Progress
        if msg.finished
          print_progress(msg)
          print "\n"
          @@last_progress = nil
        else
          print_progress(msg)
          @@last_progress = msg
        end
        STDOUT.flush
      else
        formatted = format_message(msg)
        if msg.level == Level::Error
          STDERR.puts formatted
        else
          puts formatted
        end

        if last = @@last_progress
          print_progress(last)
          STDOUT.flush
        end
      end
    end

    private def self.format_message(msg : LogMessage) : String
      symbol = case msg.level
               when Level::Info  then "•".colorize(:cyan)
               when Level::Warn  then "⚠".colorize(:yellow).bold
               when Level::Error then "✗".colorize(:red).bold
               when Level::Debug then "⚙".colorize(:dark_gray)
               else                   "".colorize
               end

      step_part = msg.step ? " [#{msg.step}]".colorize(:dark_gray).bold : ""
      "#{symbol}#{step_part} #{msg.text}"
    end

    private def self.print_progress(msg : LogMessage)
      symbol = "➜".colorize(:green).bold
      step_part = msg.step ? " [#{msg.step}]".colorize(:dark_gray).bold : ""
      print "\r#{symbol}#{step_part} #{msg.text}"
    end

    def self.info(text : String, step : String? = nil)
      ensure_channel
      @@channel.send(LogMessage.new(Level::Info, step, text))
    end

    def self.warn(text : String, step : String? = nil)
      ensure_channel
      @@channel.send(LogMessage.new(Level::Warn, step, text))
    end

    def self.error(text : String, step : String? = nil)
      ensure_channel
      @@channel.send(LogMessage.new(Level::Error, step, text))
    end

    def self.debug(text : String, step : String? = nil)
      ensure_channel
      @@channel.send(LogMessage.new(Level::Debug, step, text))
    end

    def self.progress(text : String, step : String? = nil, finished : Bool = false)
      ensure_channel
      @@channel.send(LogMessage.new(Level::Progress, step, text, finished))
    end
  end


end
