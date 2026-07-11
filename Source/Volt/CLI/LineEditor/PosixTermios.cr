lib LibC
  struct Winsize
    ws_row    : UShort
    ws_col    : UShort
    ws_xpixel : UShort
    ws_ypixel : UShort
  end

  # Platform-dependent ioctl request for the terminal window size.
  {% if flag?(:darwin) || flag?(:bsd) %}
    TIOCGWINSZ = 0x40087468
  {% else %}
    TIOCGWINSZ = 0x5413
  {% end %}

  fun ioctl( fd : Int, request : ULong, ... ) : Int
end


module Volt::CLI


  class Terminal

    @original : LibC::Termios = LibC::Termios.new
    @active : Bool = false
    STDIN_FD = 0

    # Linux c_cc indices, missing from Crystal's LibC bindings.
    VTIME = 5
    VMIN  = 6

    def enable_raw_mode : Nil
      return if @active

      if LibC.tcgetattr(STDIN_FD, pointerof(@original)) == 0
        @active = true

        raw = @original
        raw.c_lflag &= ~(LibC::ECHO | LibC::ICANON | LibC::ISIG)
        raw.c_cc[VMIN]  = 1_u8
        raw.c_cc[VTIME] = 0_u8

        LibC.tcsetattr(STDIN_FD, 0, pointerof(raw))
      end
    end

    def restore : Nil
      return unless @active
      @active = false if LibC.tcsetattr(STDIN_FD, 0, pointerof(@original)) == 0
    end

    # Current terminal width in columns, falling back to 80 when no tty answers.
    def self.width : Int32
      { 0, 1, 2 }.each do |fd|
        winsize = uninitialized LibC::Winsize
        if LibC.ioctl( fd, LibC::TIOCGWINSZ, pointerof(winsize) ) == 0 && winsize.ws_col > 0
          return winsize.ws_col.to_i32
        end
      end
      80
    end
  end


end
