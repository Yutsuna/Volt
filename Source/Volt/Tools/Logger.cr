module Volt


  module EAnsiColor
    RED     = "\e[31m"
    GREEN   = "\e[32m"
    YELLOW  = "\e[33m"
    CYAN    = "\e[36m"
    BLUE    = "\e[34m"
    GREY    = "\e[90m"
    BOLD    = "\e[1m"
    RESET   = "\e[0m"
  end


  module FLog

    extend self

    @@verbose = false
    @@in_progress = false

    #--------------------------------------------------------------------------

    def verbose( value : Bool ) : Nil
      @@verbose = value
    end

    #--------------------------------------------------------------------------

    def log( message : String, prefix : String, color : String ) : Nil
      clear_line
      tag = "#{color}#{EAnsiColor::BOLD}[#{prefix}]#{EAnsiColor::RESET}"
      STDOUT.puts "#{tag} #{message}"
      STDOUT.flush
    end

    #--------------------------------------------------------------------------

    def command( message : String ) : Nil
      return unless @@verbose
      clear_line
      print "#{EAnsiColor::GREY}#{message.ljust( 30, '.' )}#{EAnsiColor::RESET}"
      STDOUT.flush
      @@in_progress = true
    end

    def command_done( result : String = "done" ) : Nil
      return unless @@verbose
      if @@in_progress
        puts " #{EAnsiColor::GREEN}#{result}#{EAnsiColor::RESET}"
        @@in_progress = false
      end
    end

    #--------------------------------------------------------------------------

    def ok( msg )
      return unless @@verbose
      log msg, "  OK  ", EAnsiColor::GREEN
    end

    def error( msg )
      log msg, " ERR  ", EAnsiColor::RED
    end

    def warn( msg )
      log msg, " WARN ", EAnsiColor::YELLOW
    end

    def step( msg )
      return unless @@verbose
      log msg, " >>   ", EAnsiColor::BLUE
    end

    def info( msg )
      return unless @@verbose
      log msg, " INFO ", EAnsiColor::CYAN
    end

    #--------------------------------------------------------------------------

    private def clear_line
      if @@in_progress
        print "\r\e[K"
        @@in_progress = false
      end
    end

    #--------------------------------------------------------------------------

  end


end
