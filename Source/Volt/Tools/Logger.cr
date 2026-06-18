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

    #--------------------------------------------------------------------------

    def verbose ( value : Bool ) : Nil
      @@verbose = value
    end

    #--------------------------------------------------------------------------

    def log ( message : String, prefix : String = "Volt", color : String = EAnsiColor::CYAN) : Nil
      return nil unless @@verbose
      tag = "#{color}#{EAnsiColor::BOLD}[#{prefix}]#{EAnsiColor::RESET}"
      STDOUT.puts "#{tag} #{message}"
      STDOUT.flush
    end

    #--------------------------------------------------------------------------

    def ok ( message    : String ) : Nil
      log message, "  OK  ",    EAnsiColor::GREEN
    end

    def warn ( message  : String ) : Nil
      log message, "  WARN  ",  EAnsiColor::YELLOW
    end

    def error ( message : String ) : Nil
      log message, "  ERR  ",   EAnsiColor::RED
    end

    def step ( message  : String ) : Nil
      log message, "  >>  ",    EAnsiColor::BLUE
    end

    def info ( message  : String ) : Nil
      return nil unless @@verbose
      STDOUT.puts( "#{EAnsiColor::GREY}#{message}#{EAnsiColor::RESET}" )
      STDOUT.flush
    end

    def command ( message : String ) : Nil
      return nil unless @@verbose
      STDOUT.print( "\r#{EAnsiColor::GREY}#{message}#{EAnsiColor::RESET}" )
      STDOUT.flush
    end

    #--------------------------------------------------------------------------

  end


end
